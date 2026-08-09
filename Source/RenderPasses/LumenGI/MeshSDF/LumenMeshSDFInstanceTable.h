// =====================================================================================
//  LumenGI - S6-B2 Mesh SDF instance table (CPU side, header-only)
//  -------------------------------------------------------------------------------------
//  Maps scene instances to the S6-B1/B2 Mesh SDF atlas (LumenMeshSDFAtlas.h):
//  the host registers scene instances (meshID + world affine + layer mask), the
//  table drives atlas page allocation / page-reference counting and tracks a
//  per-instance dirty flag for uploads. Sampling queries forward to the atlas
//  (inverse transform + atlas sampling coordinates) and report residency/miss.
//
//  SCOPE / CONSTRAINTS
//  -------------------------------------------------------------------------------------
//  * PURE C++17, header-only, standard library only. Includes LumenMeshSDFAtlas.h
//    (READ-ONLY) for LumenMeshSDFAtlas and its types/constants. No Falcor include,
//    no CMake target (root pass integrates).
//  * Deterministic (no randomness), NOT thread-safe (single render-loop thread).
//  * Syntax check: cl /Zs /std:c++17 /EHsc LumenMeshSDFInstanceTable.h
//
//  INSTANCE <-> ATLAS ID MAPPING
//  -------------------------------------------------------------------------------------
//  * addInstance() allocates a stable table instance ID and registers a matching
//    instance in the atlas (which places every mip's pages, deduplicating shared
//    mesh content). instanceToAtlas(id) returns the current atlas mapping
//    (atlas instance index, meshID, mip-0 base page slot, residency, world AABB).
//  * removeInstance() detaches the atlas instance: page reference counts drop and
//    pages are freed when the last referencing instance of a mesh goes away.
//  * setInstanceTransform() re-registers the atlas instance with the new affine:
//    pages are keyed by MESH content, so when the mesh's pages are still
//    referenced by other instances the base page slots stay identical and only
//    the instance-table entry (inverse rows + bounds) changes -> marked dirty for
//    re-upload. For the LAST remaining instance of a mesh the pages churn
//    (freed + re-allocated, generation bump -> host re-uploads page contents,
//    exactly the atlas eviction/reload contract). This is a deliberate trade-off
//    until the atlas grows an in-place transform update API (see RESIDUAL RISKS).
//  * Instance handles stay valid across transform changes; the atlas index behind
//    them may change (the old atlas slot is left as a dead slot, GPU sees
//    meshID = 0xFFFFFFFF and returns NoInstance).
//
//  LAYER MASK
//  -------------------------------------------------------------------------------------
//  Each instance carries a layerMask (Static / Dynamic). The table stores and
//  filters by it (instancesForLayer), so S6-B3's GDF compose can select static
//  vs. dynamic instances and S6-B4's trace can decide detail-vs-global lookups.
//  The mask has no effect on page allocation.
//
//  DIRTY TRACKING (S6-A2 host contract)
//  -------------------------------------------------------------------------------------
//  add / remove / transform / layerMask changes set the instance's dirty flag and
//  bump dirtyVersion(). The host clears flags after uploading the GPU instance
//  table (atlas().getInstanceTable() / getPageTableBuffer()). touchInstance()
//  re-places evicted mips (host re-uploads page contents on generation change).
// =====================================================================================
#pragma once

#include "LumenMeshSDFAtlas.h" // READ-ONLY: LumenMeshSDFAtlas + types/constants

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LumenGI
{
namespace MeshSDF
{

// -------------------------------------------------------------------------------------
// Layer masks (Static / Dynamic layers; S6-B3/B4 filter on these)
// -------------------------------------------------------------------------------------

constexpr uint32_t kLumenMeshSDFLayerMaskStatic = 1u << 0;  ///< Baked static geometry.
constexpr uint32_t kLumenMeshSDFLayerMaskDynamic = 1u << 1; ///< Moving / dynamic geometry.
constexpr uint32_t kLumenMeshSDFLayerMaskAll = kLumenMeshSDFLayerMaskStatic | kLumenMeshSDFLayerMaskDynamic;

// -------------------------------------------------------------------------------------
// Host input types
// -------------------------------------------------------------------------------------

/// Host-facing scene instance description: which mesh + the object-to-world affine
/// (the atlas derives the inverse rows / world AABB / non-uniform-scale flag from
/// it) + which GDF layer(s) the instance belongs to.
struct LumenMeshSDFSceneInstanceDesc
{
    uint32_t meshID = kLumenMeshSDFAtlasInvalidID;
    LumenMeshSDFAtlasInstanceDesc transform;
    uint32_t layerMask = kLumenMeshSDFLayerMaskAll;
};

/// Result of instanceToAtlas(): the current instance -> atlas mapping.
struct LumenMeshSDFInstanceAtlasMapping
{
    uint32_t atlasInstanceID = kLumenMeshSDFAtlasInvalidID; ///< Atlas instance index.
    uint32_t meshID = kLumenMeshSDFAtlasInvalidID;
    uint32_t layerMask = 0;
    uint32_t basePageMip0 = kLumenMeshSDFAtlasInvalidPage; ///< Mip-0 base page slot (NotResident if not resident).
    bool resident = false;                                ///< ALL mips resident (atlas definition).
    std::array<float, 3> worldBoundsMin = {0.f, 0.f, 0.f};
    std::array<float, 3> worldBoundsMax = {0.f, 0.f, 0.f};
};

// -------------------------------------------------------------------------------------
// The instance table (header-only class; all methods inline)
// -------------------------------------------------------------------------------------

/// Maps scene instances to the Mesh SDF atlas. See the file header for the
/// ID mapping, layer-mask and dirty-tracking semantics. NOT thread-safe.
class LumenMeshSDFInstanceTable
{
public:
    /// Atlas geometry / budget parameters are forwarded to LumenMeshSDFAtlas
    /// (defaults match the frozen atlas defaults).
    explicit LumenMeshSDFInstanceTable(
        uint32_t pagesPerSide = kLumenMeshSDFAtlasDefaultPagesPerSide,
        uint64_t memoryBudgetBytes = 0,
        uint64_t pageBudget = 0,
        uint32_t minResidencyFrames = kLumenMeshSDFAtlasDefaultMinResidencyFrames)
        : mAtlas(pagesPerSide, memoryBudgetBytes, pageBudget, minResidencyFrames)
    {
    }

    // -- Meshes ----------------------------------------------------------------

    /// Register a volume (deduplicated by content identity; see the atlas).
    uint32_t registerMesh(const LumenMeshSDFAtlasMeshDesc& desc) { return mAtlas.registerMesh(desc); }

    /// Registered (deduplicated) mesh count.
    uint32_t meshCount() const { return mAtlas.meshCount(); }

    /// Base 96-byte volume descriptor for a mesh (host applies per-instance
    /// residency with applyResidencyToDescriptor()).
    bool getVolumeDescriptor(uint32_t meshID, LumenMeshSDFVolumeDescriptor& out) const
    {
        return mAtlas.getVolumeDescriptor(meshID, out);
    }

    // -- Instances -------------------------------------------------------------

    /// Add a scene instance: registers it in the atlas (pages placed / shared)
    /// and returns a STABLE table instance ID, or kLumenMeshSDFAtlasInvalidID on
    /// bad input / capacity overflow. Check isResident() afterwards (an instance
    /// can be registered but non-resident when the atlas is full / budget-bound).
    uint32_t addInstance(const LumenMeshSDFSceneInstanceDesc& desc)
    {
        if (mInstances.size() >= kLumenMeshSDFAtlasMaxInstances)
            return kLumenMeshSDFAtlasInvalidID;
        const uint32_t atlasID = mAtlas.registerInstance(desc.meshID, desc.transform);
        if (atlasID == kLumenMeshSDFAtlasInvalidID)
            return kLumenMeshSDFAtlasInvalidID;

        Record r;
        r.active = true;
        r.meshID = desc.meshID;
        r.layerMask = desc.layerMask;
        r.atlasInstanceID = atlasID;
        r.transform = desc.transform;
        r.dirty = true;
        mInstances.push_back(r);
        ++mVersion;
        return uint32_t(mInstances.size() - 1u);
    }

    /// Remove an instance: page reference counts drop; pages are freed when the
    /// last referencing instance of a mesh goes away (atlas removeInstance).
    /// Returns false for an invalid or already-removed instance.
    bool removeInstance(uint32_t instanceID)
    {
        if (instanceID >= mInstances.size())
            return false;
        Record& r = mInstances[instanceID];
        if (!r.active)
            return false;
        mAtlas.removeInstance(r.atlasInstanceID);
        r.active = false;
        r.atlasInstanceID = kLumenMeshSDFAtlasInvalidID;
        r.dirty = true;
        ++mVersion;
        return true;
    }

    /// Change an instance's transform. Marks the instance dirty (GPU instance-table
    /// entry must be re-uploaded). Page allocations are unchanged while the mesh's
    /// pages are still referenced by any instance (see file header). Returns false
    /// on invalid input / capacity overflow (the old transform is best-effort
    /// restored).
    bool setInstanceTransform(uint32_t instanceID, const LumenMeshSDFAtlasInstanceDesc& transform)
    {
        if (instanceID >= mInstances.size())
            return false;
        Record& r = mInstances[instanceID];
        if (!r.active)
            return false;

        const LumenMeshSDFAtlasInstanceDesc oldTransform = r.transform;
        mAtlas.removeInstance(r.atlasInstanceID); // page refcount--
        const uint32_t newAtlasID = mAtlas.registerInstance(r.meshID, transform); // page refcount++ (mesh dedup)
        if (newAtlasID == kLumenMeshSDFAtlasInvalidID)
        {
            r.atlasInstanceID = mAtlas.registerInstance(r.meshID, oldTransform);
            r.dirty = true;
            ++mVersion;
            return false;
        }
        r.atlasInstanceID = newAtlasID;
        r.transform = transform;
        r.dirty = true;
        ++mVersion;
        return true;
    }

    /// Change an instance's layer mask (Static / Dynamic). Marks dirty.
    bool setInstanceLayerMask(uint32_t instanceID, uint32_t layerMask)
    {
        if (instanceID >= mInstances.size())
            return false;
        Record& r = mInstances[instanceID];
        if (!r.active)
            return false;
        r.layerMask = layerMask;
        r.dirty = true;
        ++mVersion;
        return true;
    }

    /// Current layer mask of an instance (0 when invalid).
    uint32_t getInstanceLayerMask(uint32_t instanceID) const
    {
        if (instanceID >= mInstances.size() || !mInstances[instanceID].active)
            return 0;
        return mInstances[instanceID].layerMask;
    }

    /// LRU touch + re-residency (see atlas::touchInstance; re-places evicted mips
    /// - host must re-upload page contents when getMipGeneration() changed).
    bool touchInstance(uint32_t instanceID)
    {
        const uint32_t atlasID = atlasIDOf(instanceID);
        if (atlasID == kLumenMeshSDFAtlasInvalidID)
            return false;
        return mAtlas.touchInstance(atlasID);
    }

    /// Active (registered, non-removed) instance count.
    uint32_t instanceCount() const
    {
        uint32_t n = 0;
        for (const Record& r : mInstances)
            if (r.active)
                ++n;
        return n;
    }

    // -- Queries ---------------------------------------------------------------

    /// The current instance -> atlas mapping (atlas instance index, mesh, mip-0
    /// page slot, residency, layer mask, world AABB).
    LumenMeshSDFInstanceAtlasMapping instanceToAtlas(uint32_t instanceID) const
    {
        LumenMeshSDFInstanceAtlasMapping out;
        if (instanceID >= mInstances.size() || !mInstances[instanceID].active)
            return out;
        const Record& r = mInstances[instanceID];
        out.atlasInstanceID = r.atlasInstanceID;
        out.meshID = r.meshID;
        out.layerMask = r.layerMask;
        out.basePageMip0 = mAtlas.pageSlot(r.atlasInstanceID, 0);
        out.resident = mAtlas.isResident(r.atlasInstanceID);
        const std::vector<LumenMeshSDFAtlasInstance>& table = mAtlas.getInstanceTable();
        if (r.atlasInstanceID < table.size())
        {
            out.worldBoundsMin = table[r.atlasInstanceID].boundsMin;
            out.worldBoundsMax = table[r.atlasInstanceID].boundsMax;
        }
        return out;
    }

    /// True when ALL mips of the instance are resident in the atlas.
    bool isResident(uint32_t instanceID) const
    {
        const uint32_t atlasID = atlasIDOf(instanceID);
        return atlasID != kLumenMeshSDFAtlasInvalidID && mAtlas.isResident(atlasID);
    }

    /// True when the instance's `mip` is resident.
    bool isResident(uint32_t instanceID, uint32_t mip) const
    {
        const uint32_t atlasID = atlasIDOf(instanceID);
        return atlasID != kLumenMeshSDFAtlasInvalidID && mAtlas.isResident(atlasID, mip);
    }

    /// Base page slot of the instance's `mip` brick grid (kLumenMeshSDFAtlasInvalidPage
    /// when not resident / invalid instance).
    uint32_t pageSlot(uint32_t instanceID, uint32_t mip) const
    {
        const uint32_t atlasID = atlasIDOf(instanceID);
        return atlasID == kLumenMeshSDFAtlasInvalidID ? kLumenMeshSDFAtlasInvalidPage : mAtlas.pageSlot(atlasID, mip);
    }

    /// Sample the atlas at a world position for an instance (CPU mirror of the
    /// GPU sampling path): inverse transform to object, output-space voxel
    /// coords, page lookup + trilinear. Distances are OUTPUT units. All miss
    /// reasons come from the atlas (bounds / residency / invalid mip / corrupt).
    LumenMeshSDFAtlasSampleResult worldToAtlasSample(uint32_t instanceID, const float worldPos[3], uint32_t mip = 0)
    {
        LumenMeshSDFAtlasSampleResult miss;
        const uint32_t atlasID = atlasIDOf(instanceID);
        if (atlasID == kLumenMeshSDFAtlasInvalidID)
        {
            miss.reason = AtlasMissReason::NoInstance;
            return miss;
        }
        return mAtlas.sample(atlasID, worldPos, mip);
    }

    /// Continuous mip-0 voxel coordinates for a world position (CPU mirror of the
    /// atlas sampling math; exact under any affine transform). {0,0,0} on invalid
    /// instance.
    std::array<float, 3> worldToAtlasVoxel(uint32_t instanceID, const float worldPos[3]) const
    {
        const uint32_t atlasID = atlasIDOf(instanceID);
        if (atlasID == kLumenMeshSDFAtlasInvalidID)
            return {0.f, 0.f, 0.f};
        return mAtlas.worldToVoxel(atlasID, worldPos);
    }

    /// Active instances whose layerMask overlaps `layerMask` (for S6-B3 GDF
    /// compose / S6-B4 trace filtering).
    std::vector<uint32_t> instancesForLayer(uint32_t layerMask) const
    {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < mInstances.size(); ++i)
        {
            const Record& r = mInstances[i];
            if (r.active && (r.layerMask & layerMask) != 0)
                out.push_back(uint32_t(i));
        }
        return out;
    }

    // -- Dirty tracking --------------------------------------------------------

    /// True when the instance changed since its flag was last cleared (add /
    /// remove / transform / layerMask).
    bool isDirty(uint32_t instanceID) const
    {
        return instanceID < mInstances.size() && mInstances[instanceID].active && mInstances[instanceID].dirty;
    }

    void clearDirty(uint32_t instanceID)
    {
        if (instanceID < mInstances.size())
            mInstances[instanceID].dirty = false;
    }

    void clearDirtyAll()
    {
        for (Record& r : mInstances)
            r.dirty = false;
    }

    /// IDs of all active dirty instances (host upload list).
    std::vector<uint32_t> dirtyInstanceIDs() const
    {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < mInstances.size(); ++i)
            if (mInstances[i].active && mInstances[i].dirty)
                out.push_back(uint32_t(i));
        return out;
    }

    /// Monotonic change counter; any mutation bumps it (cheap dirty check).
    uint64_t dirtyVersion() const { return mVersion; }

    // -- Frame / atlas access --------------------------------------------------

    void endFrame() { mAtlas.endFrame(); }

    /// The underlying atlas (page tables / instance table / volumes upload).
    LumenMeshSDFAtlas& atlas() { return mAtlas; }
    const LumenMeshSDFAtlas& atlas() const { return mAtlas; }

    /// GPU instance-table upload (atlas-indexed; root pass uploads this or
    /// atlas().getInstanceTable(), which are the same buffer).
    const std::vector<LumenMeshSDFAtlasInstance>& getInstanceTable() const { return mAtlas.getInstanceTable(); }

    LumenMeshSDFAtlasStats getStats() const { return mAtlas.getStats(); }

    /// Reset the table and the atlas (atlas keeps its size / budgets).
    void reset()
    {
        mAtlas.reset();
        mInstances.clear();
        mVersion = 0;
    }

private:
    struct Record
    {
        bool active = false;
        uint32_t meshID = kLumenMeshSDFAtlasInvalidID;
        uint32_t layerMask = kLumenMeshSDFLayerMaskAll;
        uint32_t atlasInstanceID = kLumenMeshSDFAtlasInvalidID;
        LumenMeshSDFAtlasInstanceDesc transform;
        bool dirty = true;
    };

    /// Table instance ID -> current atlas instance ID (invalid for removed/invalid).
    uint32_t atlasIDOf(uint32_t instanceID) const
    {
        if (instanceID >= mInstances.size())
            return kLumenMeshSDFAtlasInvalidID;
        const Record& r = mInstances[instanceID];
        return r.active ? r.atlasInstanceID : kLumenMeshSDFAtlasInvalidID;
    }

    LumenMeshSDFAtlas mAtlas;
    std::vector<Record> mInstances;
    uint64_t mVersion = 0;
};

} // namespace MeshSDF
} // namespace LumenGI
