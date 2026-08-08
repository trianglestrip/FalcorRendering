/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/
// =====================================================================================
//  LumenGI - S6-B2 Mesh SDF atlas (CPU side)
//  -------------------------------------------------------------------------------------
//  Packs the mip chains of many S6-B1 volumes (Agent G's LumenMeshSDFVolumeDescriptor)
//  into two shared 3D atlases with per-(instance, mip) page residency:
//
//      * fine atlas   : R16Float 3D texture, hosts mip 0 of High-quality volumes
//      * coarse atlas : R8Snorm  3D texture, hosts mips >= 1 and Low-quality mip 0
//
//  Mixed formats across mips force two texture objects (same reasoning as S6-B1);
//  the page table entry already says which atlas a page lives in, so a single
//  sampling path covers both.
//
//  FROZEN LAYOUT (mirrored byte-for-byte in LumenMeshSDFAtlas.slang; keep in sync)
//  -------------------------------------------------------------------------------------
//      kLumenMeshSDFAtlasPageSize = 32        texels per page side (32^3 texels/page)
//      default atlas = 8^3 = 512 page slots per atlas (256 texels per side)
//      fine page    = 32^3 * 2 B = 64 KiB ; full fine atlas   (8^3) = 32 MiB
//      coarse page  = 32^3 * 1 B = 32 KiB ; full coarse atlas (8^3) = 16 MiB
//      page table   : uint32[instance][kLumenMeshSDFMaxMipCount] - entry is the
//                     BASE page slot of the mip's brick grid, or
//                     kLumenMeshSDFNotResident when the mip is not resident.
//      instance table: LumenMeshSDFAtlasInstance, 84 bytes (GPU-mirrored)
//
//  A mip is tiled into a brick grid: mip dims d(m) = max(1, ceil(d(m-1)/2)),
//  bricks per axis = ceil(d(m) / 32), bricks of one mip are allocated CONTIGUOUSLY
//  starting at the base slot (z-major, x-fastest brick index:
//  brickIdx = z*bx*by + y*bx + x). The sampler derives the brick grid from the
//  volume resolution, so the page table entry only needs the base slot.
//
//  MESH DEDUP / PAGE SHARING
//  -------------------------------------------------------------------------------------
//  registerMesh() deduplicates by content identity (contentHash + resolution +
//  quality + pooling + mipCount, FNV-1a64). Instances of meshes with identical
//  content share one meshID, so their per-mip page groups are shared; PageGroup
//  keeps a reference count and page residency is tracked per group.
//
//  RESIDENCY / BUDGETS (hard caps; degrade to miss, never crash)
//  -------------------------------------------------------------------------------------
//  * Structural cap: resident pages per atlas <= pagesPerSide^3 (hard; enforced by
//    the allocator - a mip that cannot be placed becomes non-resident).
//  * Optional byte budget + page budget (0 = unlimited). Every successful group
//    placement is followed by a budget pass that evicts LRU groups (oldest touch
//    first, ties by smallest base page) until both budgets hold. If nothing is
//    evictable (all groups inside the min-residency window) the atlas stays
//    temporarily over budget and overBudgetCount is incremented - never a failure.
//  * Eviction (LRU with min residency, mirroring LumenSurfaceCache) frees the
//    group's slots and clears the referencing instances' page-table entries:
//    their mip becomes non-resident and samples return NotResident. touchInstance()
//    re-places missing mips (host must re-upload the page contents; the page
//    generation, getMipGeneration(), detects stale pages after eviction).
//  * Instance residency = ALL mips resident. If any mip cannot be placed (atlas
//    full with nothing evictable, or budget denied), the instance is registered
//    as NON-RESIDENT: every sample of it returns NotResident. Stable degradation.
//
//  SAMPLING MATH (canonical; mirrored in LumenMeshSDFAtlas.slang)
//  -------------------------------------------------------------------------------------
//  Instance transform is stored as the INVERSE affine (world -> object):
//      pObj = A * world + t            (A = invRows rows, t = row .w components)
//  The .msdf volume lives in output space: pOut = pObj * normalizationScale.
//  Continuous mip-0 voxel coords (voxel center convention, S6-B1):
//      u = (pOut - bboxMin) * invVoxelSize - 0.5
//  Mip-m coords uMip = u / 2^m. Cells are clamped to [0, d(m)-1] exactly like
//  Agent G's trilinear sampler, then each integer corner voxel maps to a page:
//      brick = voxel / 32 ; local = voxel % 32 ; (integer division)
//      pageSlot = baseSlot + brickIdx ; texel = pageSlot * 32 + local
//  (Note texel == voxel in texel coords: the atlas is a pure tiling of the mip.)
//  Decode: fine atlas Load is already float (R16Float); coarse atlas Load is
//  R8_SNORM-converted (code/127) and is scaled by quantRange - identical to S6-B1.
//  The final distance is flipped to the math convention via signConvention.
//
//  NON-UNIFORM SCALE
//  -------------------------------------------------------------------------------------
//  The voxel-coordinate mapping stays EXACT under any affine transform (it is a
//  coordinate map). What breaks under non-uniform scale is the DISTANCE: an SDF
//  is not an SDF after anisotropic scaling, so sampled output-space distances are
//  only an approximation of world distances there. The instance flags bit
//  kLumenMeshSDFInstanceFlagNonUniformScale marks such instances (S6-B4 may choose
//  HWRT fallback for them). worldScalePerOutput = max column norm of the forward
//  linear part / normalizationScale gives a conservative world-per-output factor
//  (exact for similarities). worldBounds is the AABB of the 8 transformed output
//  bbox corners - exact for axis-aligned scaling, conservative under rotation.
//
//  SCOPE / CONSTRAINTS
//  -------------------------------------------------------------------------------------
//  PURE C++17, header-only, no Falcor includes (only ../LumenGIStats.h for the
//  optional toResourceStats() report, same pattern as LumenSurfaceCache.h).
//  Deterministic by construction: no randomness, fixed tie-breaks, ascending
//  scans. NOT thread-safe (single render-loop thread, like LumenSurfaceCache).
// =====================================================================================
#pragma once

#include "LumenMeshSDF.h" // Agent G: LumenMeshSDFVolumeDescriptor, constants, enums
#include "../LumenGIStats.h" // LumenGIResourceStats (toResourceStats)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace LumenGI
{
namespace MeshSDF
{

// -------------------------------------------------------------------------------------
// Frozen atlas constants (mirror LumenMeshSDFAtlas.slang; keep both in sync)
// -------------------------------------------------------------------------------------

/// Texels per page side. Page = 32 x 32 x 32 texels. Frozen.
constexpr uint32_t kLumenMeshSDFAtlasPageSize = 32;

/// log2(kLumenMeshSDFAtlasPageSize) - brick/local split is a shift when needed.
constexpr uint32_t kLumenMeshSDFAtlasPageSizeBits = 5;

/// Default atlas geometry: 8^3 page slots per atlas (256 texels per side).
constexpr uint32_t kLumenMeshSDFAtlasDefaultPagesPerSide = 8;

/// Default atlas size in texels per side (pagesPerSide * page size).
constexpr uint32_t kLumenMeshSDFAtlasDefaultAtlasSizeTexels =
    kLumenMeshSDFAtlasDefaultPagesPerSide * kLumenMeshSDFAtlasPageSize;

/// Hard cap on registered instances; sizes the GPU page table buffer
/// (4096 * 12 * 4 B = 192 KiB). Frozen.
constexpr uint32_t kLumenMeshSDFAtlasMaxInstances = 4096;

/// GPU bytes of one resident page.
constexpr uint64_t kLumenMeshSDFAtlasBytesPerFinePage =
    uint64_t(kLumenMeshSDFAtlasPageSize) * kLumenMeshSDFAtlasPageSize * kLumenMeshSDFAtlasPageSize * 2; ///< R16Float
constexpr uint64_t kLumenMeshSDFAtlasBytesPerCoarsePage =
    uint64_t(kLumenMeshSDFAtlasPageSize) * kLumenMeshSDFAtlasPageSize * kLumenMeshSDFAtlasPageSize * 1; ///< R8Snorm

/// Default min-residency window (frames; same default as LumenSurfaceCache).
constexpr uint32_t kLumenMeshSDFAtlasDefaultMinResidencyFrames = 60;

/// Sentinel: invalid mesh/instance ID (mirror Slang).
constexpr uint32_t kLumenMeshSDFAtlasInvalidID = 0xFFFFFFFFu;

/// Sentinel: invalid page slot (reuses Agent G's not-resident value).
constexpr uint32_t kLumenMeshSDFAtlasInvalidPage = kLumenMeshSDFNotResident;

/// Instance flag bits (instance table entry .flags; mirror Slang).
constexpr uint32_t kLumenMeshSDFInstanceFlagNonUniformScale = 1u << 0; ///< Anisotropic scaling: SDF distances are approximate.

// -------------------------------------------------------------------------------------
// Atlas / instance identity enums
// -------------------------------------------------------------------------------------

/// Which 3D texture a page lives in.
enum class AtlasKind : uint8_t
{
    Fine = 0,   ///< R16Float; mip 0 of High-quality volumes (formatMip0 == R16Float).
    Coarse = 1, ///< R8Snorm; mips >= 1 and Low-quality mip 0.
};

/// Sampling miss reasons (CPU mirror of Slang enum LumenMeshSDFAtlasMissReason).
enum class AtlasMissReason : uint32_t
{
    None = 0,             ///< resident == true; reason not meaningful.
    NoInstance = 1,       ///< instanceID out of range / inactive / meshID invalid.
    InstanceNotResident = 2, ///< Page-table slot is kLumenMeshSDFNotResident (atlas full / evicted / budget).
    OutOfInstanceBounds = 3, ///< World position outside the instance's world AABB.
    InvalidMip = 4,       ///< mip >= volume mipCount (or volume has no mips).
    PageCorrupt = 5,      ///< Computed page slot exceeds atlas capacity (guards corruption).
};
constexpr uint32_t kLumenMeshSDFAtlasMissReasonCount = 6;

// -------------------------------------------------------------------------------------
// Host-facing mesh description (content identity feeds page dedup)
// -------------------------------------------------------------------------------------

/// CPU-side description of one ".msdf" volume (mesh). Fields mirror the parts of
/// LumenMeshSDFVolumeDescriptor the atlas needs, plus the S6-A2 content hash that
/// drives page sharing. resolution / mipCount / bbox / voxelSize / normalizationScale
/// are OUTPUT-space, exactly as in Agent G's .msdf contract.
struct LumenMeshSDFAtlasMeshDesc
{
    std::array<uint32_t, 3> resolution = {0, 0, 0}; ///< mip0 voxel counts (x fastest).
    uint32_t mipCount = 1;                          ///< 1..kLumenMeshSDFMaxMipCount.
    VolumeFormat formatMip0 = VolumeFormat::R16Float; ///< Quality tier of mip 0.
    MipPooling pooling = MipPooling::MinAbs;        ///< Pooling mode (dedup key input).
    uint64_t contentHash = 0;                       ///< S6-A2 meshContentHash (dedup key).
    float quantRange = 1.f;                         ///< R8Snorm scale (max |d| over mip0).
    float normalizationScale = 1.f;                 ///< world = output / scale.
    float voxelSize = 1.f;                          ///< Output-space voxel size (uniform).
    std::array<float, 3> bboxMin = {0.f, 0.f, 0.f}; ///< Output-space grid min.
    std::array<float, 3> bboxMax = {1.f, 1.f, 1.f}; ///< Output-space grid max.
    uint32_t signConvention = kLumenMeshSDFSignConventionPositiveOutside;
    uint32_t signReliable = 1;                      ///< .msdf signReliable byte.
};

// -------------------------------------------------------------------------------------
// Instance description (host input) and GPU table entry
// -------------------------------------------------------------------------------------

/// Host input: the instance's object-to-world affine, object = the mesh's local
/// space in which the .msdf volume is defined (output = object * normalizationScale).
/// Non-uniform scaling is legal (see NON-UNIFORM SCALE above).
struct LumenMeshSDFAtlasInstanceDesc
{
    /// Row-major 3x3 linear part: world = forwardLinear * pObject + forwardTranslation.
    /// forwardLinear[0] = row 0, i.e. world.x = dot(forwardLinear[0], p) + c.x.
    std::array<float, 9> forwardLinear = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    std::array<float, 3> forwardTranslation = {0.f, 0.f, 0.f};
};

/// GPU instance-table entry. BYTE-IDENTICAL to `LumenMeshSDFAtlasInstance` in
/// LumenMeshSDFAtlas.slang (84 bytes, Slang scalar layout == C layout).
/// The root pass memcpys the table into a StructuredBuffer.
///
/// Layout (frozen):
///     +0   invRows[0] (float4)   world -> object affine rows; pObj =
///                                invRows[0].xyz*w.x + invRows[1].xyz*w.y +
///                                invRows[2].xyz*w.z + (row .w components).
///     +16  invRows[1]
///     +32  invRows[2]
///     +48  boundsMin (float3)    instance world AABB (conservative under rotation).
///     +60  boundsMax (float3)
///     +72  meshID (uint)         index into the volumes buffer; 0xFFFFFFFF = dead slot.
///     +76  flags (uint)          kLumenMeshSDFInstanceFlag*.
///     +80  worldScalePerOutput   output->world distance factor (see header notes).
struct LumenMeshSDFAtlasInstance
{
    std::array<std::array<float, 4>, 3> invRows = {};
    std::array<float, 3> boundsMin = {0.f, 0.f, 0.f};
    std::array<float, 3> boundsMax = {0.f, 0.f, 0.f};
    uint32_t meshID = kLumenMeshSDFAtlasInvalidID;
    uint32_t flags = 0;
    float worldScalePerOutput = 1.f;
};
static_assert(sizeof(LumenMeshSDFAtlasInstance) == 84, "GPU instance table entry must be 84 bytes");
static_assert(offsetof(LumenMeshSDFAtlasInstance, invRows) == 0, "invRows offset frozen");
static_assert(offsetof(LumenMeshSDFAtlasInstance, boundsMin) == 48, "boundsMin offset frozen");
static_assert(offsetof(LumenMeshSDFAtlasInstance, boundsMax) == 60, "boundsMax offset frozen");
static_assert(offsetof(LumenMeshSDFAtlasInstance, meshID) == 72, "meshID offset frozen");
static_assert(offsetof(LumenMeshSDFAtlasInstance, flags) == 76, "flags offset frozen");
static_assert(offsetof(LumenMeshSDFAtlasInstance, worldScalePerOutput) == 80, "worldScalePerOutput offset frozen");

// -------------------------------------------------------------------------------------
// Sample result + stats
// -------------------------------------------------------------------------------------

/// CPU mirror of the Slang sampling contract (see LumenMeshSDFAtlas.slang).
struct LumenMeshSDFAtlasSampleResult
{
    bool resident = false;        ///< True when the sample read a resident page.
    float distanceOutput = 0.f;   ///< Math-convention distance, OUTPUT units.
    uint32_t pageSlot = kLumenMeshSDFAtlasInvalidPage; ///< Base page slot of the mip.
    AtlasMissReason reason = AtlasMissReason::None;
};

/// Snapshot of all counters (see getStats()).
struct LumenMeshSDFAtlasStats
{
    uint64_t frameIndex = 0;      ///< Frames elapsed since construction/reset.
    uint32_t pagesPerSide = 0;    ///< Page slots per side of each atlas.
    uint64_t capacityPagesPerAtlas = 0; ///< pagesPerSide^3 (per atlas texture).
    uint64_t residentPagesFine = 0;     ///< Resident pages in the fine atlas.
    uint64_t residentPagesCoarse = 0;   ///< Resident pages in the coarse atlas.
    uint64_t residentPages = 0;         ///< Sum across both atlases.
    uint64_t fineBytes = 0;             ///< GPU bytes of resident fine pages.
    uint64_t coarseBytes = 0;           ///< GPU bytes of resident coarse pages.
    uint64_t residentBytes = 0;         ///< Sum (getResidentBytes()).
    uint64_t memoryBudgetBytes = 0;     ///< Current byte budget; 0 = unlimited (capacity-bound).
    uint64_t pageBudget = 0;            ///< Current page budget; 0 = unlimited (capacity-bound).
    uint32_t meshCount = 0;             ///< Registered (deduplicated) meshes.
    uint32_t activeInstanceCount = 0;   ///< Registered instances (incl. non-resident).
    uint32_t residentInstanceCount = 0; ///< Instances with ALL mips resident.
    uint32_t nonResidentInstanceCount = 0; ///< active - resident.
    uint32_t groupCount = 0;            ///< Active page groups.
    uint32_t sharedGroupCount = 0;      ///< Groups referenced by >= 2 instances.
    uint64_t allocationCount = 0;       ///< Successful group allocations.
    uint64_t releaseCount = 0;          ///< Groups destroyed when their last instance detached.
    uint64_t groupEvictionCount = 0;    ///< Groups evicted by LRU / budget passes.
    uint64_t pagesEvictedCount = 0;     ///< Pages freed by evictions (page turnover).
    uint64_t allocationFailureCount = 0; ///< Mips that could not be placed.
    uint64_t overBudgetCount = 0;       ///< Budget pass could not fully enforce (no evictable group).
    uint64_t touchCount = 0;            ///< Successful touchInstance() calls.
    uint64_t sampleCount = 0;           ///< sample() calls (CPU mirror).
    uint64_t hitCount = 0;              ///< Resident samples.
    uint64_t missCount = 0;             ///< Non-resident samples.
    std::array<uint64_t, kLumenMeshSDFAtlasMissReasonCount> missByReason = {};
    uint32_t minResidencyFrames = 1;    ///< Min residency window in effect.
};

// -------------------------------------------------------------------------------------
// Pure layout helpers (canonical formulas; mirrored in the Slang file)
// -------------------------------------------------------------------------------------

/// Mip dims: mip[0] = resolution, mip[m] = max(1, ceil(mip[m-1]/2)) (S6-B1 rule).
inline std::array<uint32_t, 3> atlasMipDims(const std::array<uint32_t, 3>& resolution, uint32_t mip)
{
    std::array<uint32_t, 3> dims = resolution;
    for (uint32_t m = 0; m < mip; ++m)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            dims[axis] = std::max<uint32_t>(1, (dims[axis] + 1) >> 1);
        }
    }
    return dims;
}

/// Brick grid of a mip: ceil(dims / kLumenMeshSDFAtlasPageSize) per axis.
inline std::array<uint32_t, 3> atlasBrickDims(const std::array<uint32_t, 3>& mipDims)
{
    return {
        (mipDims[0] + kLumenMeshSDFAtlasPageSize - 1) / kLumenMeshSDFAtlasPageSize,
        (mipDims[1] + kLumenMeshSDFAtlasPageSize - 1) / kLumenMeshSDFAtlasPageSize,
        (mipDims[2] + kLumenMeshSDFAtlasPageSize - 1) / kLumenMeshSDFAtlasPageSize,
    };
}

/// Pages needed by one mip of a volume.
inline uint32_t atlasPagesForMip(const std::array<uint32_t, 3>& resolution, uint32_t mip)
{
    const std::array<uint32_t, 3> bricks = atlasBrickDims(atlasMipDims(resolution, mip));
    return bricks[0] * bricks[1] * bricks[2];
}

/// Atlas a mip's pages live in: mip0 of an R16Float volume -> Fine, else Coarse.
inline AtlasKind atlasKindForMip(VolumeFormat formatMip0, uint32_t mip)
{
    return (mip == 0 && formatMip0 == VolumeFormat::R16Float) ? AtlasKind::Fine : AtlasKind::Coarse;
}

/// Byte size of one resident page of the given atlas kind.
inline uint64_t atlasBytesPerPage(AtlasKind kind)
{
    return kind == AtlasKind::Fine ? kLumenMeshSDFAtlasBytesPerFinePage : kLumenMeshSDFAtlasBytesPerCoarsePage;
}

/// FNV-1a64 (same algorithm as Agent G's msdfFNV1a64; distinct name avoids ODR
/// collisions when a TU links LumenMeshSDF.cpp). Used for the mesh dedup key.
inline uint64_t atlasFNV1a64(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;
    constexpr uint64_t kPrime = 0x100000001b3ULL;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= kPrime;
    }
    return h;
}

/// Dedup key of a mesh description: contentHash + everything that changes the
/// encoded pages (resolution, mip0 format, pooling, mipCount). Same bytes -> same
/// pages -> pages can be shared by instances.
inline uint64_t atlasMeshIdentityKey(const LumenMeshSDFAtlasMeshDesc& desc)
{
    struct KeyBytes
    {
        uint64_t contentHash;
        std::array<uint32_t, 3> resolution;
        uint32_t formatMip0;
        uint32_t pooling;
        uint32_t mipCount;
    } key{desc.contentHash, desc.resolution, static_cast<uint32_t>(desc.formatMip0),
         static_cast<uint32_t>(desc.pooling), desc.mipCount};
    return atlasFNV1a64(&key, sizeof(key));
}

/// Validate a host mesh description; false on malformed input.
inline bool atlasValidateMeshDesc(const LumenMeshSDFAtlasMeshDesc& desc)
{
    if (desc.mipCount < 1 || desc.mipCount > kLumenMeshSDFMaxMipCount) return false;
    if (desc.resolution[0] < 1 || desc.resolution[1] < 1 || desc.resolution[2] < 1) return false;
    if (!(desc.voxelSize > 0.f) || !std::isfinite(desc.voxelSize)) return false;
    if (!(desc.normalizationScale > 0.f) || !std::isfinite(desc.normalizationScale)) return false;
    if (!(desc.quantRange > 0.f) || !std::isfinite(desc.quantRange)) return false;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (desc.bboxMax[axis] <= desc.bboxMin[axis]) return false;
        if (!std::isfinite(desc.bboxMin[axis]) || !std::isfinite(desc.bboxMax[axis])) return false;
    }
    return true;
}

/// Invert a 3x3 matrix given row-major (returns false when singular). Mirrors the
/// standard adjugate formula; used to derive the world->object affine rows.
inline bool atlasInvert3x3(const std::array<float, 9>& m, std::array<float, 9>& out)
{
    const float a = m[0], b = m[1], c = m[2];
    const float d = m[3], e = m[4], f = m[5];
    const float g = m[6], h = m[7], i = m[8];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float inv = 1.f / det;
    out = {
        (e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
        (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
        (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv,
    };
    return true;
}

// -------------------------------------------------------------------------------------
// The atlas (header-only class; all methods inline)
// -------------------------------------------------------------------------------------

/// Mesh SDF atlas host: layout, page allocation, residency, budgets, instance
/// table and a CPU mirror of the Slang sampling path. See the file header for the
/// frozen layout and residency semantics. Deterministic; NOT thread-safe.
class LumenMeshSDFAtlas
{
public:
    /// \param pagesPerSide Atlas geometry: pagesPerSide^3 page slots per atlas.
    ///        Clamped to [1, 1625] (capacity fits uint32). Default 8 (256^3 texels).
    /// \param memoryBudgetBytes Hard GPU-byte budget across both atlases; 0 = unlimited
    ///        (bounded by capacity only). Enforced after every group placement.
    /// \param pageBudget Hard resident-page budget; 0 = unlimited (capacity-bound).
    /// \param minResidencyFrames Min frames a group stays resident after last touch;
    ///        clamped to >= 1.
    explicit LumenMeshSDFAtlas(
        uint32_t pagesPerSide = kLumenMeshSDFAtlasDefaultPagesPerSide,
        uint64_t memoryBudgetBytes = 0,
        uint64_t pageBudget = 0,
        uint32_t minResidencyFrames = kLumenMeshSDFAtlasDefaultMinResidencyFrames)
        : mPagesPerSide(clampPagesPerSide(pagesPerSide))
        , mCapacityPerAtlas(static_cast<uint64_t>(mPagesPerSide) * mPagesPerSide * mPagesPerSide)
        , mMemoryBudgetBytes(memoryBudgetBytes)
        , mPageBudget(pageBudget)
        , mMinResidencyFrames(std::max<uint32_t>(1, minResidencyFrames))
        , mSlots({{std::vector<SlotState>(static_cast<size_t>(mCapacityPerAtlas)), std::vector<SlotState>(static_cast<size_t>(mCapacityPerAtlas))}})
        , mSnapshots({{std::vector<std::vector<float>>(static_cast<size_t>(mCapacityPerAtlas)), std::vector<std::vector<float>>(static_cast<size_t>(mCapacityPerAtlas))}})
    {
        rebuildUploadBuffers();
    }

    // -- Meshes ----------------------------------------------------------------

    /// Register a volume. Deduplicates by content identity: an identical mesh
    /// returns the existing meshID (its pages are shared). Returns
    /// kLumenMeshSDFAtlasInvalidID on malformed input or capacity overflow.
    uint32_t registerMesh(const LumenMeshSDFAtlasMeshDesc& desc)
    {
        if (!atlasValidateMeshDesc(desc)) return kLumenMeshSDFAtlasInvalidID;
        const uint64_t key = atlasMeshIdentityKey(desc);
        auto it = mMeshKeyToID.find(key);
        if (it != mMeshKeyToID.end()) return it->second; // content-identical mesh: share pages

        const uint32_t meshID = static_cast<uint32_t>(mMeshes.size());
        MeshRecord rec;
        rec.desc = desc;
        rec.volumeDescriptor = buildVolumeDescriptor(desc);
        mMeshes.push_back(rec);
        mMeshKeyToID[key] = meshID;
        markUploadDirty();
        return meshID;
    }

    /// Number of registered (deduplicated) meshes == volumes buffer size.
    uint32_t meshCount() const { return static_cast<uint32_t>(mMeshes.size()); }

    /// Base volume descriptor (G's 96-byte struct) for a mesh. atlasPage is
    /// kLumenMeshSDFNotResident and the AtlasResident flag is clear; the host
    /// applies per-instance residency with applyResidencyToDescriptor().
    /// Returns false when meshID is invalid.
    bool getVolumeDescriptor(uint32_t meshID, LumenMeshSDFVolumeDescriptor& out) const
    {
        if (meshID >= mMeshes.size()) return false;
        out = mMeshes[meshID].volumeDescriptor;
        return true;
    }

    /// Set descriptor.atlasPage + kLumenMeshSDFFlagAtlasResident from the instance's
    /// mip-0 residency (the S6-B1 -> S6-B2 rebind hook Agent G reserved).
    void applyResidencyToDescriptor(uint32_t instanceID, LumenMeshSDFVolumeDescriptor& desc) const
    {
        const uint32_t slot = pageSlot(instanceID, 0);
        desc.atlasPage = slot == kLumenMeshSDFAtlasInvalidPage ? kLumenMeshSDFNotResident : slot;
        if (desc.atlasPage != kLumenMeshSDFNotResident)
            desc.flags |= kLumenMeshSDFFlagAtlasResident;
        else
            desc.flags &= ~kLumenMeshSDFFlagAtlasResident;
    }

    // -- Instances -------------------------------------------------------------

    /// Register an instance of `meshID`. Places every mip's pages (deduplicating
    /// against other instances of the same mesh). If ANY mip cannot be placed
    /// (atlas full / budget) the instance is registered as NON-RESIDENT (all
    /// samples return InstanceNotResident) - stable degradation, never a crash.
    /// Returns the instanceID or kLumenMeshSDFAtlasInvalidID on bad input /
    /// capacity overflow. Check isResident() afterwards.
    uint32_t registerInstance(uint32_t meshID, const LumenMeshSDFAtlasInstanceDesc& desc)
    {
        if (meshID >= mMeshes.size()) return kLumenMeshSDFAtlasInvalidID;
        if (mInstances.size() >= kLumenMeshSDFAtlasMaxInstances) return kLumenMeshSDFAtlasInvalidID;

        InstanceRecord inst;
        if (!computeInstanceRecord(mMeshes[meshID].desc, desc, inst)) return kLumenMeshSDFAtlasInvalidID;
        inst.meshID = meshID;

        const uint32_t instanceID = static_cast<uint32_t>(mInstances.size());
        mInstances.push_back(inst);
        markUploadDirty();

        for (uint32_t mip = 0; mip < inst.mipCount; ++mip)
        {
            const uint32_t groupID = placeGroup(meshID, mip, instanceID);
            if (groupID == kLumenMeshSDFAtlasInvalidID)
            {
                // Stable degradation: drop everything placed so far, keep the
                // instance registered but fully non-resident.
                ++mAllocationFailureCount;
                for (uint32_t m = 0; m < mip; ++m)
                {
                    detachGroup(mInstances[instanceID].groupIDs[m], instanceID, /*destroyIfOrphaned=*/true);
                }
                mInstances[instanceID].pageSlots.fill(kLumenMeshSDFAtlasInvalidPage);
                mInstances[instanceID].groupIDs.fill(kLumenMeshSDFAtlasInvalidID);
                return instanceID;
            }
        }
        return instanceID;
    }

    /// Remove an instance: detach its groups (destroyed when orphaned; pages
    /// freed immediately - the host must ensure the GPU is no longer reading
    /// them, same contract as LumenSurfaceCache::releasePage).
    void removeInstance(uint32_t instanceID)
    {
        if (instanceID >= mInstances.size() || !mInstances[instanceID].active) return;
        InstanceRecord& inst = mInstances[instanceID];
        for (uint32_t mip = 0; mip < inst.mipCount; ++mip)
        {
            detachGroup(inst.groupIDs[mip], instanceID, /*destroyIfOrphaned=*/true);
        }
        inst.active = false;
        inst.pageSlots.fill(kLumenMeshSDFAtlasInvalidPage);
        inst.groupIDs.fill(kLumenMeshSDFAtlasInvalidID);
        markUploadDirty();
    }

    /// LRU touch + re-residency: touches every resident group of the instance and
    /// re-places mips that lost their pages to eviction (host must re-upload the
    /// page contents; getMipGeneration() detects staleness). Returns true when the
    /// instance is fully resident afterwards.
    bool touchInstance(uint32_t instanceID)
    {
        if (instanceID >= mInstances.size() || !mInstances[instanceID].active) return false;
        InstanceRecord& inst = mInstances[instanceID];
        for (uint32_t mip = 0; mip < inst.mipCount; ++mip)
        {
            if (inst.pageSlots[mip] == kLumenMeshSDFAtlasInvalidPage)
            {
                const uint32_t groupID = placeGroup(inst.meshID, mip, instanceID);
                if (groupID == kLumenMeshSDFAtlasInvalidID)
                {
                    ++mAllocationFailureCount;
                }
            }
        }
        // Touch all groups currently referenced by the instance.
        for (uint32_t mip = 0; mip < inst.mipCount; ++mip)
        {
            const uint32_t groupID = inst.groupIDs[mip];
            if (groupID != kLumenMeshSDFAtlasInvalidID && mGroups[groupID].active)
            {
                mGroups[groupID].lastTouchedFrame = mFrameIndex;
            }
        }
        ++mTouchCount;
        markUploadDirty();
        return isResident(instanceID);
    }

    // -- Residency queries -----------------------------------------------------

    /// Base page slot of the instance's mip brick grid, or kLumenMeshSDFAtlasInvalidPage.
    uint32_t pageSlot(uint32_t instanceID, uint32_t mip) const
    {
        if (instanceID >= mInstances.size() || !mInstances[instanceID].active) return kLumenMeshSDFAtlasInvalidPage;
        const InstanceRecord& inst = mInstances[instanceID];
        if (mip >= kLumenMeshSDFMaxMipCount) return kLumenMeshSDFAtlasInvalidPage;
        return inst.pageSlots[mip];
    }

    /// True when the instance's mip is resident.
    bool isResident(uint32_t instanceID, uint32_t mip) const
    {
        return pageSlot(instanceID, mip) != kLumenMeshSDFAtlasInvalidPage;
    }

    /// True when ALL mips of the instance are resident.
    bool isResident(uint32_t instanceID) const
    {
        if (instanceID >= mInstances.size() || !mInstances[instanceID].active) return false;
        const InstanceRecord& inst = mInstances[instanceID];
        for (uint32_t mip = 0; mip < inst.mipCount; ++mip)
        {
            if (inst.pageSlots[mip] == kLumenMeshSDFAtlasInvalidPage) return false;
        }
        return true;
    }

    /// Allocation epoch of the group backing (instanceID, mip); 0 when not
    /// resident. Changes after an eviction + re-placement: the host must re-upload
    /// the page contents before sampling (stale-page detection).
    uint32_t getMipGeneration(uint32_t instanceID, uint32_t mip) const
    {
        if (instanceID >= mInstances.size() || !mInstances[instanceID].active) return 0;
        if (mip >= kLumenMeshSDFMaxMipCount) return 0;
        const uint32_t groupID = mInstances[instanceID].groupIDs[mip];
        if (groupID == kLumenMeshSDFAtlasInvalidID || !mGroups[groupID].active) return 0;
        return mGroups[groupID].generation;
    }

    // -- CPU mirror of the Slang sampling contract -----------------------------

    /// CPU mirror of lumenMeshSDFAtlasSample (LumenMeshSDFAtlas.slang): same math,
    /// same miss reasons, counters updated (hit/miss). Distances are OUTPUT units.
    /// Requires uploadVolumeFloats() to have filled the pages; un-uploaded pages
    /// sample as 0 (documented placeholder). Not the GPU path - validation only.
    LumenMeshSDFAtlasSampleResult sample(uint32_t instanceID, const float worldPos[3], uint32_t mip)
    {
        LumenMeshSDFAtlasSampleResult result;
        ++mSampleCount;

        if (instanceID >= mInstances.size() || !mInstances[instanceID].active)
        {
            return finishMiss(result, AtlasMissReason::NoInstance);
        }
        const InstanceRecord& inst = mInstances[instanceID];
        if (inst.meshID >= mMeshes.size())
        {
            return finishMiss(result, AtlasMissReason::NoInstance);
        }
        if (worldPos[0] < inst.boundsMin[0] || worldPos[1] < inst.boundsMin[1] || worldPos[2] < inst.boundsMin[2] ||
            worldPos[0] > inst.boundsMax[0] || worldPos[1] > inst.boundsMax[1] || worldPos[2] > inst.boundsMax[2])
        {
            return finishMiss(result, AtlasMissReason::OutOfInstanceBounds);
        }
        const MeshRecord& mesh = mMeshes[inst.meshID];
        const LumenMeshSDFVolumeDescriptor& vol = mesh.volumeDescriptor;
        if (vol.mipCount == 0 || mip >= vol.mipCount)
        {
            return finishMiss(result, AtlasMissReason::InvalidMip);
        }
        if (inst.pageSlots[mip] == kLumenMeshSDFAtlasInvalidPage)
        {
            return finishMiss(result, AtlasMissReason::InstanceNotResident);
        }

        // Continuous mip-0 voxel coords (voxel-center convention, S6-B1):
        // pObj = A*world + t ; pOut = pObj * normalizationScale ;
        // u = (pOut - bboxMin) * invVoxelSize - 0.5.
        const float3_t pObj = worldToObject(inst, worldPos);
        const float pOut[3] = {pObj[0] * vol.normalizationScale, pObj[1] * vol.normalizationScale, pObj[2] * vol.normalizationScale};
        const float u0[3] = {
            (pOut[0] - vol.bboxMin[0]) * vol.invVoxelSize - 0.5f,
            (pOut[1] - vol.bboxMin[1]) * vol.invVoxelSize - 0.5f,
            (pOut[2] - vol.bboxMin[2]) * vol.invVoxelSize - 0.5f,
        };
        const float shift = static_cast<float>(uint64_t(1) << mip);
        const float uMip[3] = {u0[0] / shift, u0[1] / shift, u0[2] / shift};

        const std::array<uint32_t, 3> dims = atlasMipDims(mesh.desc.resolution, mip);
        const uint32_t maxCoord[3] = {dims[0] - 1, dims[1] - 1, dims[2] - 1};
        const float cell[3] = {
            clampCoord(uMip[0], maxCoord[0]),
            clampCoord(uMip[1], maxCoord[1]),
            clampCoord(uMip[2], maxCoord[2]),
        };
        const uint32_t base[3] = {std::min(static_cast<uint32_t>(cell[0]), maxCoord[0]),
                                  std::min(static_cast<uint32_t>(cell[1]), maxCoord[1]),
                                  std::min(static_cast<uint32_t>(cell[2]), maxCoord[2])};
        const uint32_t bx[3] = {base[0] < maxCoord[0] ? 1u : 0u, base[1] < maxCoord[1] ? 1u : 0u, base[2] < maxCoord[2] ? 1u : 0u};
        const float f[3] = {cell[0] - static_cast<float>(base[0]),
                            cell[1] - static_cast<float>(base[1]),
                            cell[2] - static_cast<float>(base[2])};

        const uint32_t cornerBaseSlot = inst.pageSlots[mip];
        const bool coarse = atlasKindForMip(mesh.desc.formatMip0, mip) == AtlasKind::Coarse;

        float corner[8];
        for (int c = 0; c < 8; ++c)
        {
            const uint32_t voxel[3] = {
                base[0] + (c & 1 ? bx[0] : 0u),
                base[1] + (c & 2 ? bx[1] : 0u),
                base[2] + (c & 4 ? bx[2] : 0u),
            };
            uint32_t slot = 0;
            uint32_t texel[3] = {0, 0, 0};
            if (!voxelToPageAndTexel(voxel, dims, cornerBaseSlot, slot, texel) ||
                slot >= mCapacityPerAtlas)
            {
                return finishMiss(result, AtlasMissReason::PageCorrupt);
            }
            const std::vector<float>& image = mSnapshots[static_cast<size_t>(coarse ? 1 : 0)][slot];
            float value = 0.f;
            if (!image.empty())
            {
                const size_t idx = (static_cast<size_t>(texel[2]) * kLumenMeshSDFAtlasPageSize + texel[1]) * kLumenMeshSDFAtlasPageSize + texel[0];
                value = image[idx];
                if (coarse) value *= vol.quantRange; // R8Snorm code/127 * R, identical to S6-B1
            }
            corner[c] = value;
        }

        const float c0 = lerp(lerp(corner[0], corner[1], f[0]), lerp(corner[2], corner[3], f[0]), f[1]);
        const float c1 = lerp(lerp(corner[4], corner[5], f[0]), lerp(corner[6], corner[7], f[0]), f[1]);
        float distance = lerp(c0, c1, f[2]);

        // Math convention: positive outside. signConvention == 0 means the stored
        // field is already math convention (never hardcoded - S6-B1 rule).
        if (vol.signConvention != kLumenMeshSDFSignConventionPositiveOutside)
        {
            distance = -distance;
        }

        result.resident = true;
        result.distanceOutput = distance;
        result.pageSlot = cornerBaseSlot;
        result.reason = AtlasMissReason::None;
        ++mHitCount;
        return result;
    }

    /// Continuous mip-0 voxel coords for a world position (CPU mirror of
    /// lumenMeshSDFAtlasWorldToVoxel). Exact under any affine transform.
    std::array<float, 3> worldToVoxel(uint32_t instanceID, const float worldPos[3]) const
    {
        std::array<float, 3> u = {0.f, 0.f, 0.f};
        if (instanceID >= mInstances.size() || !mInstances[instanceID].active) return u;
        if (mInstances[instanceID].meshID >= mMeshes.size()) return u;
        const InstanceRecord& inst = mInstances[instanceID];
        const LumenMeshSDFVolumeDescriptor& vol = mMeshes[inst.meshID].volumeDescriptor;
        const float3_t pObj = worldToObject(inst, worldPos);
        for (int axis = 0; axis < 3; ++axis)
        {
            const float pOut = pObj[axis] * vol.normalizationScale;
            u[axis] = (pOut - vol.bboxMin[axis]) * vol.invVoxelSize - 0.5f;
        }
        return u;
    }

    // -- CPU page contents (validation / tests only) ---------------------------

    /// Upload TEXTURE-STORED mip data (x-fastest, mip voxel count elements) into
    /// the CPU snapshot of every page of the mesh's mip. Stored values mirror what
    /// the GPU textures hold: fine pages = float distance; coarse pages = the
    /// R8SNORM code / 127, i.e. distance / quantRange clamped to [-1, 1] (the
    /// sample path rescales by quantRange, identical to S6-B1). Pages are shared,
    /// so one upload serves all instances of the mesh. The GPU path uploads Agent
    /// G's ENCODED bytes into the same page regions; this mirrors it for CPU tests.
    /// Returns false when the mesh/mip has no resident pages or data size mismatch.
    bool uploadVolumeFloats(uint32_t meshID, uint32_t mip, const std::vector<float>& data)
    {
        if (meshID >= mMeshes.size()) return false;
        const LumenMeshSDFAtlasMeshDesc& desc = mMeshes[meshID].desc;
        if (mip >= desc.mipCount) return false;
        const std::array<uint32_t, 3> dims = atlasMipDims(desc.resolution, mip);
        const size_t expected = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
        if (data.size() != expected) return false;

        const GroupKey key{meshID, mip};
        const auto it = mGroupMap.find(key);
        if (it == mGroupMap.end() || !mGroups[it->second].active) return false;

        const PageGroup& group = mGroups[it->second];
        const std::array<uint32_t, 3> bricks = atlasBrickDims(dims);
        for (uint32_t bz = 0; bz < bricks[2]; ++bz)
        {
            for (uint32_t by = 0; by < bricks[1]; ++by)
            {
                for (uint32_t bx = 0; bx < bricks[0]; ++bx)
                {
                    const uint32_t brickIdx = bz * (bricks[0] * bricks[1]) + by * bricks[0] + bx;
                    std::vector<float>& img = mSnapshots[static_cast<size_t>(group.kind)][group.basePage + brickIdx];
                    img.assign(kLumenMeshSDFAtlasPageSize * kLumenMeshSDFAtlasPageSize * kLumenMeshSDFAtlasPageSize, 0.f);
                    for (uint32_t z = 0; z < kLumenMeshSDFAtlasPageSize; ++z)
                    {
                        for (uint32_t y = 0; y < kLumenMeshSDFAtlasPageSize; ++y)
                        {
                            for (uint32_t x = 0; x < kLumenMeshSDFAtlasPageSize; ++x)
                            {
                                const uint32_t vx = bx * kLumenMeshSDFAtlasPageSize + x;
                                const uint32_t vy = by * kLumenMeshSDFAtlasPageSize + y;
                                const uint32_t vz = bz * kLumenMeshSDFAtlasPageSize + z;
                                if (vx < dims[0] && vy < dims[1] && vz < dims[2])
                                {
                                    const size_t src = (static_cast<size_t>(vz) * dims[1] + vy) * dims[0] + vx;
                                    const size_t dst = (static_cast<size_t>(z) * kLumenMeshSDFAtlasPageSize + y) * kLumenMeshSDFAtlasPageSize + x;
                                    img[dst] = data[src];
                                }
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

    // -- Frame / budget --------------------------------------------------------

    /// Advance the frame counter and flush pending-free bookkeeping (slots evicted
    /// this frame are counted as free from the next frame on - mirror of
    /// LumenSurfaceCache::endFrame semantics; reuse-before-flush is allowed).
    void endFrame() { ++mFrameIndex; }

    /// Current frame index.
    uint64_t getFrameIndex() const { return mFrameIndex; }

    /// Byte budget; 0 = unlimited (capacity-bound).
    uint64_t getMemoryBudgetBytes() const { return mMemoryBudgetBytes; }
    /// Page budget; 0 = unlimited (capacity-bound).
    uint64_t getPageBudget() const { return mPageBudget; }

    void setMemoryBudgetBytes(uint64_t bytes) { mMemoryBudgetBytes = bytes; }
    void setPageBudget(uint64_t pages) { mPageBudget = pages; }

    // -- Geometry / capacity ---------------------------------------------------

    /// Page slots per side of each atlas.
    uint32_t pagesPerSide() const { return mPagesPerSide; }

    /// pagesPerSide^3 page slots per atlas texture.
    uint64_t capacityPagesPerAtlas() const { return mCapacityPerAtlas; }

    /// GPU bytes of all resident pages.
    uint64_t getResidentBytes() const
    {
        uint64_t fine = 0, coarse = 0;
        countResident(fine, coarse);
        return fine * kLumenMeshSDFAtlasBytesPerFinePage + coarse * kLumenMeshSDFAtlasBytesPerCoarsePage;
    }

    /// True when resident pages and bytes are within both budgets (0 = unlimited).
    bool isWithinBudget() const
    {
        uint64_t fine = 0, coarse = 0;
        countResident(fine, coarse);
        const uint64_t pages = fine + coarse;
        const uint64_t bytes = fine * kLumenMeshSDFAtlasBytesPerFinePage + coarse * kLumenMeshSDFAtlasBytesPerCoarsePage;
        return (mMemoryBudgetBytes == 0 || bytes <= mMemoryBudgetBytes) &&
               (mPageBudget == 0 || pages <= mPageBudget);
    }

    // -- GPU-facing snapshots --------------------------------------------------

    /// Page table upload: size kLumenMeshSDFAtlasMaxInstances *
    /// kLumenMeshSDFMaxMipCount uint32s. Entry [instanceID * 12 + mip] = base page
    /// slot of the mip's brick grid or kLumenMeshSDFNotResident. Rebuilt lazily on
    /// access after any mutation, so the root pass can upload at any point.
    const std::vector<uint32_t>& getPageTableBuffer() const
    {
        rebuildUploadBuffersIfDirty();
        return mPageTableBuffer;
    }

    /// Instance table upload: LumenMeshSDFAtlasInstance per registered instance
    /// (dead slots carry meshID = 0xFFFFFFFF; the GPU returns NoInstance).
    const std::vector<LumenMeshSDFAtlasInstance>& getInstanceTable() const
    {
        rebuildUploadBuffersIfDirty();
        return mInstanceTable;
    }

    /// Active instance count (== mInstances.size(), incl. non-resident).
    uint32_t instanceCount() const { return static_cast<uint32_t>(mInstances.size()); }

    /// Volume descriptors upload: one 96-byte LumenMeshSDFVolumeDescriptor per mesh
    /// (atlasPage = NotResident in the base table; apply per-instance residency
    /// with applyResidencyToDescriptor() for the S6-B1 descriptor rebind hook).
    const std::vector<LumenMeshSDFVolumeDescriptor>& getVolumeDescriptors() const
    {
        rebuildUploadBuffersIfDirty();
        return mVolumeDescriptors;
    }

    /// Force-rebuild the upload snapshots now (getters also rebuild lazily).
    void refreshUploadBuffers() { rebuildUploadBuffers(); }

    // -- Stats -----------------------------------------------------------------

    /// Snapshot of all counters and derived state.
    LumenMeshSDFAtlasStats getStats() const;

    /// Map residency to LumenGIResourceStats (meshSdfBytes = resident bytes).
    LumenGIResourceStats toResourceStats() const;

    /// Reset to a freshly constructed state (keeps atlas size and budgets).
    void reset();

private:
    // -- Types -----------------------------------------------------------------

    struct float3_t : public std::array<float, 3>
    {
        using std::array<float, 3>::array;
    };

    struct MeshRecord
    {
        LumenMeshSDFAtlasMeshDesc desc;
        LumenMeshSDFVolumeDescriptor volumeDescriptor;
        bool active = true;
    };

    struct InstanceRecord
    {
        bool active = true;
        uint32_t meshID = kLumenMeshSDFAtlasInvalidID;
        uint32_t mipCount = 0;
        uint32_t flags = 0;
        float worldScalePerOutput = 1.f;
        std::array<std::array<float, 4>, 3> invRows = {};
        std::array<float, 3> boundsMin = {0.f, 0.f, 0.f};
        std::array<float, 3> boundsMax = {0.f, 0.f, 0.f};
        std::array<uint32_t, kLumenMeshSDFMaxMipCount> pageSlots = {};
        std::array<uint32_t, kLumenMeshSDFMaxMipCount> groupIDs = {};
        InstanceRecord() { pageSlots.fill(kLumenMeshSDFAtlasInvalidPage); groupIDs.fill(kLumenMeshSDFAtlasInvalidID); }
    };

    struct PageGroup
    {
        bool active = false;
        uint32_t meshID = kLumenMeshSDFAtlasInvalidID;
        uint32_t mip = 0;
        AtlasKind kind = AtlasKind::Fine;
        uint32_t basePage = kLumenMeshSDFAtlasInvalidPage;
        uint32_t pageCount = 0;
        uint32_t refCount = 0;
        uint64_t lastTouchedFrame = 0;
        uint32_t generation = 0; ///< basePage allocation epoch (stale-page detection).
        std::vector<uint32_t> instances; ///< referencing instance IDs.
    };

    struct GroupKey
    {
        uint32_t meshID;
        uint32_t mip;
        bool operator<(const GroupKey& o) const
        {
            if (meshID != o.meshID) return meshID < o.meshID;
            return mip < o.mip;
        }
    };

    struct SlotState
    {
        uint32_t generation = 0; ///< Allocation epochs of this slot.
        bool allocated = false;  ///< Owned by a group.
        bool pendingFree = false; ///< Evicted this frame; reusable; flushed at endFrame.
    };

    // -- Helpers ---------------------------------------------------------------

    static uint32_t clampPagesPerSide(uint32_t pagesPerSide)
    {
        // 1625^3 = 4292890625 < 2^32; largest cube fitting uint32 page IDs.
        constexpr uint32_t kMaxPagesPerSide = 1625;
        return std::clamp<uint32_t>(pagesPerSide, 1, kMaxPagesPerSide);
    }

    static float clampCoord(float v, uint32_t maxCoord)
    {
        return std::max(0.f, std::min(v, static_cast<float>(maxCoord)));
    }

    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    static LumenMeshSDFVolumeDescriptor buildVolumeDescriptor(const LumenMeshSDFAtlasMeshDesc& desc)
    {
        LumenMeshSDFVolumeDescriptor v{};
        v.resolution[0] = desc.resolution[0];
        v.resolution[1] = desc.resolution[1];
        v.resolution[2] = desc.resolution[2];
        v.formatMip0 = static_cast<uint32_t>(desc.formatMip0);
        v.formatCoarse = static_cast<uint32_t>(VolumeFormat::R8Snorm);
        v.mipCount = desc.mipCount;
        v.bboxMin[0] = desc.bboxMin[0];
        v.bboxMin[1] = desc.bboxMin[1];
        v.bboxMin[2] = desc.bboxMin[2];
        v.bboxMax[0] = desc.bboxMax[0];
        v.bboxMax[1] = desc.bboxMax[1];
        v.bboxMax[2] = desc.bboxMax[2];
        v.voxelSize = desc.voxelSize;
        v.invVoxelSize = 1.f / desc.voxelSize;
        v.normalizationScale = desc.normalizationScale;
        v.invNormalizationScale = 1.f / desc.normalizationScale;
        v.quantRange = desc.quantRange;
        v.signConvention = desc.signConvention;
        v.signReliable = desc.signReliable;
        v.atlasPage = kLumenMeshSDFNotResident;
        v.contentHashLo = static_cast<uint32_t>(desc.contentHash & 0xFFFFFFFFull);
        v.contentHashHi = static_cast<uint32_t>(desc.contentHash >> 32);
        v.flags = 0;
        if (desc.signReliable) v.flags |= kLumenMeshSDFFlagSignReliable;
        if (desc.formatMip0 == VolumeFormat::R16Float) v.flags |= kLumenMeshSDFFlagQualityHigh;
        if (desc.pooling == MipPooling::MinAbs) v.flags |= kLumenMeshSDFFlagPoolMinAbs;
        return v;
    }

    /// Derive the instance record from the forward affine: inverse rows, flags,
    /// world-scale factor and world bounds. Returns false on degenerate transforms.
    static bool computeInstanceRecord(
        const LumenMeshSDFAtlasMeshDesc& mesh,
        const LumenMeshSDFAtlasInstanceDesc& in,
        InstanceRecord& out)
    {
        // Forward 3x3 rows -> inverse rows (world -> object).
        std::array<float, 9> invLinear;
        if (!atlasInvert3x3(in.forwardLinear, invLinear)) return false;
        // t = -A * c  (c = forwardTranslation).
        const float cx = in.forwardTranslation[0];
        const float cy = in.forwardTranslation[1];
        const float cz = in.forwardTranslation[2];
        const float tx = -(invLinear[0] * cx + invLinear[1] * cy + invLinear[2] * cz);
        const float ty = -(invLinear[3] * cx + invLinear[4] * cy + invLinear[5] * cz);
        const float tz = -(invLinear[6] * cx + invLinear[7] * cy + invLinear[8] * cz);

        out.invRows[0] = {invLinear[0], invLinear[1], invLinear[2], tx};
        out.invRows[1] = {invLinear[3], invLinear[4], invLinear[5], ty};
        out.invRows[2] = {invLinear[6], invLinear[7], invLinear[8], tz};

        // Scale analysis of the FORWARD linear part (columns).
        const float col0Len = std::sqrt(in.forwardLinear[0] * in.forwardLinear[0] +
                                        in.forwardLinear[3] * in.forwardLinear[3] +
                                        in.forwardLinear[6] * in.forwardLinear[6]);
        const float col1Len = std::sqrt(in.forwardLinear[1] * in.forwardLinear[1] +
                                        in.forwardLinear[4] * in.forwardLinear[4] +
                                        in.forwardLinear[7] * in.forwardLinear[7]);
        const float col2Len = std::sqrt(in.forwardLinear[2] * in.forwardLinear[2] +
                                        in.forwardLinear[5] * in.forwardLinear[5] +
                                        in.forwardLinear[8] * in.forwardLinear[8]);
        if (col0Len < 1e-6f || col1Len < 1e-6f || col2Len < 1e-6f) return false;

        const float maxCol = std::max(col0Len, std::max(col1Len, col2Len));
        const float minCol = std::min(col0Len, std::min(col1Len, col2Len));
        const float dot01 = in.forwardLinear[0] * in.forwardLinear[1] + in.forwardLinear[3] * in.forwardLinear[4] + in.forwardLinear[6] * in.forwardLinear[7];
        const float dot02 = in.forwardLinear[0] * in.forwardLinear[2] + in.forwardLinear[3] * in.forwardLinear[5] + in.forwardLinear[6] * in.forwardLinear[8];
        const float dot12 = in.forwardLinear[1] * in.forwardLinear[2] + in.forwardLinear[4] * in.forwardLinear[5] + in.forwardLinear[7] * in.forwardLinear[8];
        const bool uniform = (maxCol / minCol <= 1.001f) &&
                             (std::fabs(dot01) <= 1e-3f * maxCol * maxCol) &&
                             (std::fabs(dot02) <= 1e-3f * maxCol * maxCol) &&
                             (std::fabs(dot12) <= 1e-3f * maxCol * maxCol);

        out.flags = uniform ? 0u : kLumenMeshSDFInstanceFlagNonUniformScale;
        // Output -> world distance factor: similarities are exact (k / s);
        // anisotropic instances get the conservative max-axis factor.
        const float scalePerObject = uniform ? (col0Len + col1Len + col2Len) / 3.f : maxCol;
        out.worldScalePerOutput = scalePerObject / mesh.normalizationScale;

        // World AABB: transform the 8 corners of the OUTPUT bbox (object space of
        // the volume is output / normalizationScale) by the forward affine.
        out.boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        out.boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        const float invS = 1.f / mesh.normalizationScale;
        for (int corner = 0; corner < 8; ++corner)
        {
            const float ox = (corner & 1) ? mesh.bboxMax[0] : mesh.bboxMin[0];
            const float oy = (corner & 2) ? mesh.bboxMax[1] : mesh.bboxMin[1];
            const float oz = (corner & 4) ? mesh.bboxMax[2] : mesh.bboxMin[2];
            const float px = ox * invS, py = oy * invS, pz = oz * invS;
            const float wx = in.forwardLinear[0] * px + in.forwardLinear[1] * py + in.forwardLinear[2] * pz + cx;
            const float wy = in.forwardLinear[3] * px + in.forwardLinear[4] * py + in.forwardLinear[5] * pz + cy;
            const float wz = in.forwardLinear[6] * px + in.forwardLinear[7] * py + in.forwardLinear[8] * pz + cz;
            out.boundsMin[0] = std::min(out.boundsMin[0], wx);
            out.boundsMin[1] = std::min(out.boundsMin[1], wy);
            out.boundsMin[2] = std::min(out.boundsMin[2], wz);
            out.boundsMax[0] = std::max(out.boundsMax[0], wx);
            out.boundsMax[1] = std::max(out.boundsMax[1], wy);
            out.boundsMax[2] = std::max(out.boundsMax[2], wz);
        }

        out.mipCount = mesh.mipCount;
        return true;
    }

    /// pObj = A*world + t from the stored inverse rows.
    static float3_t worldToObject(const InstanceRecord& inst, const float worldPos[3])
    {
        float3_t p;
        p[0] = inst.invRows[0][0] * worldPos[0] + inst.invRows[1][0] * worldPos[1] + inst.invRows[2][0] * worldPos[2] + inst.invRows[0][3];
        p[1] = inst.invRows[0][1] * worldPos[0] + inst.invRows[1][1] * worldPos[1] + inst.invRows[2][1] * worldPos[2] + inst.invRows[1][3];
        p[2] = inst.invRows[0][2] * worldPos[0] + inst.invRows[1][2] * worldPos[1] + inst.invRows[2][2] * worldPos[2] + inst.invRows[2][3];
        return p;
    }

    /// Map an integer voxel coordinate of a mip to (page slot, atlas texel).
    /// Canonical formula (mirrors the Slang side):
    ///   brick = voxel / pageSize ; local = voxel % pageSize ;
    ///   slot = baseSlot + (bz*bx*by + by*bx + bx) ; texel = slot*pageSize + local.
    /// False when the computed slot exceeds atlas capacity (PageCorrupt).
    static bool voxelToPageAndTexel(const uint32_t voxel[3], const std::array<uint32_t, 3>& dims,
                                    uint32_t baseSlot, uint32_t& outSlot, uint32_t outTexel[3])
    {
        const std::array<uint32_t, 3> bricks = atlasBrickDims(dims);
        const uint32_t brick[3] = {voxel[0] / kLumenMeshSDFAtlasPageSize,
                                   voxel[1] / kLumenMeshSDFAtlasPageSize,
                                   voxel[2] / kLumenMeshSDFAtlasPageSize};
        const uint32_t local[3] = {voxel[0] % kLumenMeshSDFAtlasPageSize,
                                   voxel[1] % kLumenMeshSDFAtlasPageSize,
                                   voxel[2] % kLumenMeshSDFAtlasPageSize};
        const uint32_t brickIdx = brick[2] * (bricks[0] * bricks[1]) + brick[1] * bricks[0] + brick[0];
        const uint64_t slot64 = static_cast<uint64_t>(baseSlot) + brickIdx;
        if (slot64 >= 0xFFFFFFFFull) return false;
        outSlot = static_cast<uint32_t>(slot64);
        outTexel[0] = brick[0] * kLumenMeshSDFAtlasPageSize + local[0];
        outTexel[1] = brick[1] * kLumenMeshSDFAtlasPageSize + local[1];
        outTexel[2] = brick[2] * kLumenMeshSDFAtlasPageSize + local[2];
        return true;
    }

    LumenMeshSDFAtlasSampleResult finishMiss(LumenMeshSDFAtlasSampleResult& result, AtlasMissReason reason)
    {
        result.reason = reason;
        ++mMissCount;
        ++mMissByReason[static_cast<size_t>(reason)];
        return result;
    }

    void countResident(uint64_t& fine, uint64_t& coarse) const
    {
        fine = 0;
        coarse = 0;
        for (const PageGroup& g : mGroups)
        {
            if (!g.active) continue;
            if (g.kind == AtlasKind::Fine) fine += g.pageCount;
            else coarse += g.pageCount;
        }
    }

    /// Find the LRU eviction candidate among groups outside the min-residency
    /// window: oldest lastTouchedFrame, ties by smallest basePage. Returns
    /// kLumenMeshSDFAtlasInvalidID when nothing is evictable.
    uint32_t findEvictionCandidate() const
    {
        uint32_t candidate = kLumenMeshSDFAtlasInvalidID;
        uint64_t oldestTouch = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < mGroups.size(); ++i)
        {
            const PageGroup& g = mGroups[i];
            if (!g.active) continue;
            if (mFrameIndex - g.lastTouchedFrame < mMinResidencyFrames) continue;
            if (g.lastTouchedFrame < oldestTouch)
            {
                oldestTouch = g.lastTouchedFrame;
                candidate = static_cast<uint32_t>(i);
            }
        }
        return candidate;
    }

    /// Evict a group: free its slots (pendingFree until endFrame), clear CPU
    /// snapshots, update every referencing instance's page-table entry to
    /// NotResident, destroy the group.
    void evictGroup(uint32_t groupID)
    {
        PageGroup& g = mGroups[groupID];
        if (!g.active) return;
        for (uint32_t instanceID : g.instances)
        {
            InstanceRecord& inst = mInstances[instanceID];
            if (inst.groupIDs[g.mip] == groupID)
            {
                inst.pageSlots[g.mip] = kLumenMeshSDFAtlasInvalidPage;
                inst.groupIDs[g.mip] = kLumenMeshSDFAtlasInvalidID;
            }
        }
        for (uint32_t p = 0; p < g.pageCount; ++p)
        {
            SlotState& slot = mSlots[static_cast<size_t>(g.kind)][g.basePage + p];
            slot.allocated = false;
            slot.pendingFree = true;
            mSnapshots[static_cast<size_t>(g.kind)][g.basePage + p].clear();
            ++mPagesEvictedCount;
        }
        mGroupMap.erase(GroupKey{g.meshID, g.mip});
        g.active = false;
        ++mGroupEvictionCount;
    }

    /// Detach an instance from a group (refcount--, destroy on orphan).
    void detachGroup(uint32_t groupID, uint32_t instanceID, bool destroyIfOrphaned)
    {
        if (groupID == kLumenMeshSDFAtlasInvalidID || groupID >= mGroups.size()) return;
        PageGroup& g = mGroups[groupID];
        if (!g.active) return;
        g.instances.erase(std::remove(g.instances.begin(), g.instances.end(), instanceID), g.instances.end());
        if (g.refCount > 0) --g.refCount;
        if (destroyIfOrphaned && g.refCount == 0 && g.instances.empty())
        {
            for (uint32_t p = 0; p < g.pageCount; ++p)
            {
                SlotState& slot = mSlots[static_cast<size_t>(g.kind)][g.basePage + p];
                slot.allocated = false;
                slot.pendingFree = false;
                mSnapshots[static_cast<size_t>(g.kind)][g.basePage + p].clear();
            }
            mGroupMap.erase(GroupKey{g.meshID, g.mip});
            g.active = false;
            ++mReleaseCount;
        }
    }

    /// First contiguous run of `count` free slots in the atlas, ascending scan
    /// (deterministic). Returns the base slot or invalid.
    uint32_t findRun(AtlasKind kind, uint32_t count) const
    {
        const auto& slots = mSlots[static_cast<size_t>(kind)];
        uint32_t runStart = kLumenMeshSDFAtlasInvalidPage;
        uint32_t runLen = 0;
        for (uint32_t s = 0; s < mCapacityPerAtlas; ++s)
        {
            if (!slots[s].allocated)
            {
                if (runLen == 0) runStart = s;
                ++runLen;
                if (runLen >= count) return runStart;
            }
            else
            {
                runLen = 0;
            }
        }
        return kLumenMeshSDFAtlasInvalidPage;
    }

    /// Allocate `count` contiguous slots (fine/coarse), evicting LRU groups until
    /// a run fits or nothing is evictable. Marks slots allocated and bumps
    /// generations. Returns the base slot or kLumenMeshSDFAtlasInvalidPage.
    uint32_t allocRun(AtlasKind kind, uint32_t count)
    {
        for (uint32_t attempt = 0; attempt < mGroups.size() + 1; ++attempt)
        {
            const uint32_t base = findRun(kind, count);
            if (base != kLumenMeshSDFAtlasInvalidPage)
            {
                auto& slots = mSlots[static_cast<size_t>(kind)];
                for (uint32_t p = 0; p < count; ++p)
                {
                    SlotState& slot = slots[base + p];
                    slot.allocated = true;
                    slot.pendingFree = false;
                    if (slot.generation != std::numeric_limits<uint32_t>::max()) ++slot.generation;
                }
                return base;
            }
            const uint32_t victim = findEvictionCandidate();
            if (victim == kLumenMeshSDFAtlasInvalidID) return kLumenMeshSDFAtlasInvalidPage;
            evictGroup(victim);
        }
        return kLumenMeshSDFAtlasInvalidPage;
    }

    /// Place (or deduplicate) the pages of (meshID, mip) for an instance.
    /// Touches the group and runs the budget pass. Returns the group ID or invalid.
    uint32_t placeGroup(uint32_t meshID, uint32_t mip, uint32_t instanceID)
    {
        const GroupKey key{meshID, mip};
        const auto it = mGroupMap.find(key);
        if (it != mGroupMap.end() && mGroups[it->second].active)
        {
            PageGroup& g = mGroups[it->second];
            ++g.refCount;
            g.instances.push_back(instanceID);
            g.lastTouchedFrame = mFrameIndex;
            mInstances[instanceID].pageSlots[mip] = g.basePage;
            mInstances[instanceID].groupIDs[mip] = it->second;
            return it->second;
        }

        const MeshRecord& mesh = mMeshes[meshID];
        const AtlasKind kind = atlasKindForMip(mesh.desc.formatMip0, mip);
        const uint32_t pageCount = atlasPagesForMip(mesh.desc.resolution, mip);
        const uint32_t base = allocRun(kind, pageCount);
        if (base == kLumenMeshSDFAtlasInvalidPage) return kLumenMeshSDFAtlasInvalidID;

        PageGroup g;
        g.active = true;
        g.meshID = meshID;
        g.mip = mip;
        g.kind = kind;
        g.basePage = base;
        g.pageCount = pageCount;
        g.refCount = 1;
        g.lastTouchedFrame = mFrameIndex;
        g.generation = mSlots[static_cast<size_t>(kind)][base].generation;
        g.instances.push_back(instanceID);

        const uint32_t groupID = static_cast<uint32_t>(mGroups.size());
        mGroups.push_back(std::move(g));
        mGroupMap[key] = groupID;
        mInstances[instanceID].pageSlots[mip] = base;
        mInstances[instanceID].groupIDs[mip] = groupID;
        ++mAllocationCount;

        enforceBudgets();
        return groupID;
    }

    /// Evict LRU groups until resident pages/bytes are within both budgets.
    /// When nothing is evictable the atlas stays temporarily over budget and
    /// overBudgetCount is incremented (never a failure).
    void enforceBudgets()
    {
        for (uint32_t pass = 0; pass < mGroups.size() + 1; ++pass)
        {
            uint64_t fine = 0, coarse = 0;
            countResident(fine, coarse);
            const uint64_t pages = fine + coarse;
            const uint64_t bytes = fine * kLumenMeshSDFAtlasBytesPerFinePage + coarse * kLumenMeshSDFAtlasBytesPerCoarsePage;
            const bool overBytes = mMemoryBudgetBytes != 0 && bytes > mMemoryBudgetBytes;
            const bool overPages = mPageBudget != 0 && pages > mPageBudget;
            if (!overBytes && !overPages) return;
            const uint32_t victim = findEvictionCandidate();
            if (victim == kLumenMeshSDFAtlasInvalidID)
            {
                ++mOverBudgetCount;
                return;
            }
            evictGroup(victim);
        }
    }

    /// Rebuild the upload snapshots (page table, instance table, volume table).
    void rebuildUploadBuffers()
    {
        mUploadDirty = false;
        // Page table: fixed stride kLumenMeshSDFMaxMipCount per instance.
        mPageTableBuffer.assign(static_cast<size_t>(kLumenMeshSDFAtlasMaxInstances) * kLumenMeshSDFMaxMipCount, kLumenMeshSDFNotResident);
        for (size_t i = 0; i < mInstances.size(); ++i)
        {
            const InstanceRecord& inst = mInstances[i];
            if (!inst.active) continue;
            for (uint32_t mip = 0; mip < inst.mipCount; ++mip)
            {
                mPageTableBuffer[i * kLumenMeshSDFMaxMipCount + mip] = inst.pageSlots[mip];
            }
        }

        // Instance table: one entry per registered instance; dead slots carry an
        // invalid meshID so the GPU returns NoInstance.
        mInstanceTable.resize(mInstances.size());
        for (size_t i = 0; i < mInstances.size(); ++i)
        {
            LumenMeshSDFAtlasInstance& out = mInstanceTable[i];
            const InstanceRecord& inst = mInstances[i];
            if (!inst.active)
            {
                out = LumenMeshSDFAtlasInstance{}; // meshID = 0xFFFFFFFF -> NoInstance
                continue;
            }
            out.invRows = inst.invRows;
            out.boundsMin = inst.boundsMin;
            out.boundsMax = inst.boundsMax;
            out.meshID = inst.meshID;
            out.flags = inst.flags;
            out.worldScalePerOutput = inst.worldScalePerOutput;
        }

        mVolumeDescriptors.clear();
        for (const MeshRecord& mesh : mMeshes)
        {
            mVolumeDescriptors.push_back(mesh.volumeDescriptor);
        }
    }

    /// Lazy rebuild of the upload snapshots when any state changed since the
    /// last access (getters are const; buffers are mutable).
    void rebuildUploadBuffersIfDirty() const
    {
        if (mUploadDirty)
        {
            const_cast<LumenMeshSDFAtlas*>(this)->rebuildUploadBuffers();
        }
    }

    /// Mark the upload snapshots stale after any mutation.
    void markUploadDirty() { mUploadDirty = true; }

    // -- State -----------------------------------------------------------------

    uint32_t mPagesPerSide = kLumenMeshSDFAtlasDefaultPagesPerSide;
    uint64_t mCapacityPerAtlas = 0; ///< pagesPerSide^3 slots per atlas.
    uint64_t mMemoryBudgetBytes = 0; ///< 0 = unlimited (capacity-bound).
    uint64_t mPageBudget = 0;        ///< 0 = unlimited (capacity-bound).
    uint32_t mMinResidencyFrames = 1;
    uint64_t mFrameIndex = 0;

    std::vector<MeshRecord> mMeshes;
    std::map<uint64_t, uint32_t> mMeshKeyToID;
    std::vector<InstanceRecord> mInstances;
    std::vector<PageGroup> mGroups;
    std::map<GroupKey, uint32_t> mGroupMap;
    std::array<std::vector<SlotState>, 2> mSlots;
    std::array<std::vector<std::vector<float>>, 2> mSnapshots; ///< [atlas][slot] lazy CPU images.

    // Upload snapshots (rebuilt lazily; see rebuildUploadBuffersIfDirty).
    mutable std::vector<uint32_t> mPageTableBuffer;
    mutable std::vector<LumenMeshSDFAtlasInstance> mInstanceTable;
    mutable std::vector<LumenMeshSDFVolumeDescriptor> mVolumeDescriptors;
    mutable bool mUploadDirty = true;

    // Stats.
    uint64_t mAllocationCount = 0;
    uint64_t mReleaseCount = 0;
    uint64_t mGroupEvictionCount = 0;
    uint64_t mPagesEvictedCount = 0;
    uint64_t mAllocationFailureCount = 0;
    uint64_t mOverBudgetCount = 0;
    uint64_t mTouchCount = 0;
    uint64_t mSampleCount = 0;
    uint64_t mHitCount = 0;
    uint64_t mMissCount = 0;
    std::array<uint64_t, kLumenMeshSDFAtlasMissReasonCount> mMissByReason = {};
};

inline LumenMeshSDFAtlasStats LumenMeshSDFAtlas::getStats() const
{
    LumenMeshSDFAtlasStats stats;
    stats.frameIndex = mFrameIndex;
    stats.pagesPerSide = mPagesPerSide;
    stats.capacityPagesPerAtlas = mCapacityPerAtlas;
    uint64_t fine = 0, coarse = 0;
    countResident(fine, coarse);
    stats.residentPagesFine = fine;
    stats.residentPagesCoarse = coarse;
    stats.residentPages = fine + coarse;
    stats.fineBytes = fine * kLumenMeshSDFAtlasBytesPerFinePage;
    stats.coarseBytes = coarse * kLumenMeshSDFAtlasBytesPerCoarsePage;
    stats.residentBytes = stats.fineBytes + stats.coarseBytes;
    stats.memoryBudgetBytes = mMemoryBudgetBytes;
    stats.pageBudget = mPageBudget;
    stats.meshCount = static_cast<uint32_t>(mMeshes.size());
    stats.groupCount = 0;
    stats.sharedGroupCount = 0;
    for (const PageGroup& g : mGroups)
    {
        if (!g.active) continue;
        ++stats.groupCount;
        if (g.refCount >= 2) ++stats.sharedGroupCount;
    }
    stats.activeInstanceCount = 0;
    stats.residentInstanceCount = 0;
    for (const InstanceRecord& inst : mInstances)
    {
        if (!inst.active) continue;
        ++stats.activeInstanceCount;
        bool allResident = true;
        for (uint32_t mip = 0; mip < inst.mipCount; ++mip)
        {
            if (inst.pageSlots[mip] == kLumenMeshSDFAtlasInvalidPage) { allResident = false; break; }
        }
        if (allResident) ++stats.residentInstanceCount;
    }
    stats.nonResidentInstanceCount = stats.activeInstanceCount - stats.residentInstanceCount;
    stats.allocationCount = mAllocationCount;
    stats.releaseCount = mReleaseCount;
    stats.groupEvictionCount = mGroupEvictionCount;
    stats.pagesEvictedCount = mPagesEvictedCount;
    stats.allocationFailureCount = mAllocationFailureCount;
    stats.overBudgetCount = mOverBudgetCount;
    stats.touchCount = mTouchCount;
    stats.sampleCount = mSampleCount;
    stats.hitCount = mHitCount;
    stats.missCount = mMissCount;
    stats.missByReason = mMissByReason;
    stats.minResidencyFrames = mMinResidencyFrames;
    return stats;
}

inline LumenGIResourceStats LumenMeshSDFAtlas::toResourceStats() const
{
    LumenGIResourceStats stats;
    stats.meshSdfBytes = getResidentBytes();
    return stats;
}

inline void LumenMeshSDFAtlas::reset()
{
    const uint32_t pagesPerSide = mPagesPerSide;
    const uint64_t memoryBudget = mMemoryBudgetBytes;
    const uint64_t pageBudget = mPageBudget;
    const uint32_t minResidency = mMinResidencyFrames;
    *this = LumenMeshSDFAtlas(pagesPerSide, memoryBudget, pageBudget, minResidency);
}

} // namespace MeshSDF
} // namespace LumenGI
