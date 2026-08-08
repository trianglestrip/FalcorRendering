// =====================================================================================
//  LumenGI - Global Distance Field (GDF) clipmap host (CPU-only, header-only)
//  -------------------------------------------------------------------------------------
//  A camera-centered multi-level "clipmap" of signed distance: level m is a cube of
//  `resolution` voxels per side covering a world extent `baseExtent * 2^m`. Near-field
//  geometry is sampled from the FINE (small-extent) levels, far-field from the COARSE
//  levels. This file is the pure-CPU contract; the voxel *payload* (the actual SDF
//  values) is owned by a later bake/atlas stage.
//
//  FROZEN DESIGN (single source of truth for the CPU contract)
//  -------------------------------------------------------------------------------------
//  * kDynamicLevels = 1. Level 0 is DYNAMIC: it is centered on the camera and scrolls
//    when the camera moves (the center re-snaps to the nearest whole level-0 voxel,
//    exposing new voxel slabs that must be (re)built). Levels >= kDynamicLevels are
//    STATIC: anchored at the world origin, they never scroll and are assumed baked once
//    (far-field content changes slowly).
//  * All levels share one voxel lattice PHASE: level 0's center is snapped to an integer
//    multiple of voxelSize_0, so its lattice always passes through the world origin; and
//    voxelSize_m == 2^m * voxelSize_0, so every static level's lattice is a sub-lattice
//    of level 0's. Voxel faces therefore align across levels (no cracks at LOD borders).
//  * Rounding rules (frozen):
//      - world -> voxel index: floor(vc) (toward -infinity; a negative coordinate like
//        -1.3 voxels floors to -2, which is out of bounds).
//      - camera snap: round-half-toward-zero (|scroll| is nonzero only when the camera
//        move EXCEEDS half a voxel; a move of exactly half a voxel does not scroll).
//        Negative coordinates are handled via fabs() + sign restore: -1.5 snaps to -1,
//        -0.5 snaps to 0, +1.5 snaps to +1, +2.5 snaps to +2.
//  * All math is integer/index arithmetic plus IEEE binary-float; deterministic, no
//    randomness, no wall-clock dependence. Two instances driven with identical inputs
//    produce identical outputs.
//
//  INTEGRATION NOTES
//  -------------------------------------------------------------------------------------
//  * PURE C++17, standard library only. No Falcor include, no CMake target (the root
//    pass includes this header and converts Falcor::float3 at the boundary).
//  * Syntax check: cl /Zs /std:c++17 /EHsc LumenGlobalDistanceField.h
// =====================================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace LumenGI
{
namespace GlobalDistanceField
{

// -------------------------------------------------------------------------------------
// Frozen constants
// -------------------------------------------------------------------------------------

///< Number of leading camera-following (scrollable) levels. Frozen: 1.
constexpr uint32_t kDynamicLevels = 1;
///< Hard cap on the number of levels (levelCount is clamped to [kDynamicLevels, this]).
constexpr uint32_t kMaxGDFLevels = 16;

// -------------------------------------------------------------------------------------
// Minimal value types (standard library only; the root pass converts Falcor::float3
// to/from LumenGI::GlobalDistanceField::float3 at the API boundary).
// -------------------------------------------------------------------------------------

/// Minimal 3D float vector used by the public API.
struct float3
{
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    constexpr float3() = default;
    constexpr float3(float px, float py, float pz) : x(px), y(py), z(pz) {}
};

/// Integer 3D index (voxel coordinates, level-local). Used for indices and scrolls.
struct index3
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    constexpr index3() = default;
    constexpr index3(int32_t px, int32_t py, int32_t pz) : x(px), y(py), z(pz) {}
    bool operator==(const index3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const index3& o) const { return !(*this == o); }
};

/// Per-level parameters. extent_m = baseExtent * 2^m; voxelSize_m = extent_m / resolution.
struct LumenGDFLevel
{
    uint32_t resolution = 128; ///< Voxels per side (frozen default; all levels share it).
    float worldExtent = 0.f;   ///< World-space cube side length of this level.
    float voxelSize = 0.f;     ///< worldExtent / resolution (uniform, axis-independent).
};

/// One scroll slab: a voxel region that is thin on `axis` and spans the full level on the
/// other two axes. min/max are inclusive level-local voxel indices.
struct VoxelRange
{
    int32_t axis = -1; ///< 0 = X, 1 = Y, 2 = Z; the axis the slab spans. -1 = invalid.
    index3 min;        ///< Inclusive lower corner (thin on `axis`).
    index3 max;        ///< Inclusive upper corner (thin on `axis`).
    bool added = true; ///< true = newly covered (needs update); false = removed/stale.
};

// =====================================================================================
// LumenGlobalDistanceField - camera-centered multi-level clipmap
// =====================================================================================
class LumenGlobalDistanceField
{
public:
    ///< Build a clipmap of `levelCount` levels. Level m spans baseExtent * 2^m meters
    ///< at `resolution` voxels per side. The camera starts at the world origin (already
    ///< on the voxel lattice), scroll = (0,0,0). Invalid inputs are clamped
    ///< (baseExtent>0, resolution>=1, kDynamicLevels <= levelCount <= kMaxGDFLevels).
    LumenGlobalDistanceField(float baseExtentMeters, uint32_t levelCount, uint32_t resolution)
    {
        const float base = baseExtentMeters > 0.f ? baseExtentMeters : 1.f;
        const uint32_t res = resolution >= 1u ? resolution : 1u;
        const uint32_t count = std::min(std::max(levelCount, kDynamicLevels), kMaxGDFLevels);

        mBaseExtent = base;
        mResolution = res;
        mLevelCount = count;
        mLevels.resize(count);
        float extent = base;
        for (uint32_t m = 0; m < count; ++m)
        {
            // extent_m = base * 2^m (running *2 keeps the value exact in binary floats).
            mLevels[m].resolution = res;
            mLevels[m].worldExtent = extent;
            mLevels[m].voxelSize = extent / static_cast<float>(res);
            extent *= 2.0f;
        }

        mCenterVoxel[0] = 0;
        mCenterVoxel[1] = 0;
        mCenterVoxel[2] = 0;
        mCameraCenter = float3(0.f, 0.f, 0.f);
        mScroll = index3();
    }

    // ----------------------------------------------------------------------------------
    // Level / parameter access
    // ----------------------------------------------------------------------------------

    uint32_t getLevelCount() const { return mLevelCount; }
    uint32_t getResolution() const { return mResolution; }
    float getBaseExtent() const { return mBaseExtent; }
    const float3& getCameraCenter() const { return mCameraCenter; }

    ///< Level parameters. `level` is clamped into [0, mLevelCount-1] (deterministic no-op
    ///< for stale handles after dropFarthestStaticLevel()).
    const LumenGDFLevel& getLevel(uint32_t level) const { return mLevels[clampLevel(level)]; }

    // ----------------------------------------------------------------------------------
    // Camera / scroll
    // ----------------------------------------------------------------------------------

    ///< Re-center the dynamic levels on `worldPos`. The camera center is SNAPPED to the
    ///< nearest whole level-0 voxel (round-half-toward-zero, see file header), so small
    ///< movements within half a voxel produce no scroll. The integer per-axis scroll
    ///< (in level-0 voxel units) produced by THIS call is stored and reported by
    ///< scrollFromCameraMove(). The first call is treated as a move from the origin
    ///< anchor and follows the same rule.
    void setCamera(const float3& worldPos)
    {
        const float vs0 = getLevel(0).voxelSize;
        const int32_t nv[3] = {snapVoxel(worldPos.x / vs0), snapVoxel(worldPos.y / vs0), snapVoxel(worldPos.z / vs0)};
        const int32_t ov[3] = {mCenterVoxel[0], mCenterVoxel[1], mCenterVoxel[2]};

        mScroll.x = nv[0] - ov[0];
        mScroll.y = nv[1] - ov[1];
        mScroll.z = nv[2] - ov[2];
        for (int a = 0; a < 3; ++a)
            mCenterVoxel[a] = nv[a];
        // Snapped world-space center = integer voxel coordinate * voxelSize_0.
        mCameraCenter = float3(static_cast<float>(mCenterVoxel[0]) * vs0,
                               static_cast<float>(mCenterVoxel[1]) * vs0,
                               static_cast<float>(mCenterVoxel[2]) * vs0);
    }

    ///< Integer scroll (in level-0 voxel units, per axis) produced by the last setCamera().
    ///< Nonzero on an axis only when that move exceeded half a voxel. Static levels never
    ///< scroll, so this is the dynamic level's scroll.
    index3 scrollFromCameraMove() const { return mScroll; }

    // ----------------------------------------------------------------------------------
    // Level selection and world <-> voxel mapping
    // ----------------------------------------------------------------------------------

    ///< Pick the level that should own `worldPos`:
    ///<  1. NEAR FIELD -> the dynamic levels: any point inside a dynamic level's footprint
    ///<     (finest first) is sampled from it. With kDynamicLevels = 1 this is level 0
    ///<     whenever maxAbs(p - cameraCenter) <= extent_0 / 2.
    ///<  2. FAR FIELD  -> the static levels: the FINEST static level whose origin-anchored
    ///<     footprint contains the point (maxAbs(p) <= extent_m / 2).
    ///<  3. FALLBACK   -> the coarsest remaining level (points beyond every footprint are
    ///<     clamped; worldToIndex then reports inBounds == false).
    ///< A point exactly on a footprint boundary is considered inside (`<=`).
    uint32_t levelIndexForWorld(const float3& worldPos) const
    {
        for (uint32_t m = 0; m < kDynamicLevels && m < mLevelCount; ++m)
        {
            if (insideFootprint(levelCenter(m), mLevels[m], worldPos))
                return m;
        }
        const float3 origin(0.f, 0.f, 0.f);
        for (uint32_t m = kDynamicLevels; m < mLevelCount; ++m)
        {
            if (insideFootprint(origin, mLevels[m], worldPos))
                return m;
        }
        return mLevelCount - 1u;
    }

    ///< Convert a world position to a level-local voxel index. gridMin_m = levelCenter_m -
    ///< extent_m/2 (the world coordinate of voxel 0's low corner); the continuous voxel
    ///< coordinate is vc = (worldPos - gridMin_m) / voxelSize_m and the containing voxel is
    ///< floor(vc) (floor, not truncation, so negative coordinates land on the correct
    ///< voxel: floor(-0.3) = -1, which is out of bounds). `inBounds` is false when any
    ///< component of the index falls outside [0, resolution).
    void worldToIndex(uint32_t level, const float3& worldPos, index3& outIndex, bool& outInBounds) const
    {
        const uint32_t m = clampLevel(level);
        const LumenGDFLevel& lvl = mLevels[m];
        const float3 center = levelCenter(m);
        const float half = lvl.worldExtent * 0.5f;
        const float vs = lvl.voxelSize;
        const int32_t vcx = static_cast<int32_t>(std::floor((worldPos.x - (center.x - half)) / vs));
        const int32_t vcy = static_cast<int32_t>(std::floor((worldPos.y - (center.y - half)) / vs));
        const int32_t vcz = static_cast<int32_t>(std::floor((worldPos.z - (center.z - half)) / vs));
        const int32_t R = static_cast<int32_t>(lvl.resolution);

        outIndex.x = vcx;
        outIndex.y = vcy;
        outIndex.z = vcz;
        outInBounds = vcx >= 0 && vcx < R && vcy >= 0 && vcy < R && vcz >= 0 && vcz < R;
    }

    ///< Inverse of worldToIndex: the world-space center of the voxel at `index`:
    ///< world = gridMin_m + (index + 0.5) * voxelSize_m. Always well-defined; the result
    ///< is inside the footprint only when `index` was in bounds.
    float3 indexToWorld(uint32_t level, const index3& index) const
    {
        const uint32_t m = clampLevel(level);
        const LumenGDFLevel& lvl = mLevels[m];
        const float3 center = levelCenter(m);
        const float half = lvl.worldExtent * 0.5f;
        const float vs = lvl.voxelSize;
        return float3(center.x - half + (static_cast<float>(index.x) + 0.5f) * vs,
                      center.y - half + (static_cast<float>(index.y) + 0.5f) * vs,
                      center.z - half + (static_cast<float>(index.z) + 0.5f) * vs);
    }

    // ----------------------------------------------------------------------------------
    // Scroll bookkeeping
    // ----------------------------------------------------------------------------------

    ///< Voxel ranges that must be (re)built after the last camera move: one newly-covered
    ///< slab per scrolled axis. Only the dynamic levels scroll; static levels always
    ///< return an empty list. After a scroll of s level-0 voxels on axis a (thickness
    ///< thick = min(|s|, resolution)):
    ///<   s > 0 : the grid moved +a, fresh voxels entered at the high end  -> [R-thick, R-1]
    ///<   s < 0 : fresh voxels entered at the low end                     -> [0, thick-1]
    ///< The other two axes span the full level. Ranges are inclusive.
    std::vector<VoxelRange> dirtyRegions(uint32_t level) const
    {
        std::vector<VoxelRange> out;
        if (level >= kDynamicLevels)
            return out;
        const int32_t R = static_cast<int32_t>(mResolution);
        for (int axis = 0; axis < 3; ++axis)
        {
            int32_t lo = 0, hi = R - 1;
            if (!addedSlabRange(axisComp(mScroll, axis), R, lo, hi))
                continue;
            out.push_back(makeSlab(axis, lo, hi, R, true));
        }
        return out;
    }

    ///< Complement of dirtyRegions: the slabs whose data became stale and may be recycled:
    ///<   s > 0 : removed at the low end  -> [0, thick-1]
    ///<   s < 0 : removed at the high end -> [R-thick, R-1]
    std::vector<VoxelRange> removedRegions(uint32_t level) const
    {
        std::vector<VoxelRange> out;
        if (level >= kDynamicLevels)
            return out;
        const int32_t R = static_cast<int32_t>(mResolution);
        for (int axis = 0; axis < 3; ++axis)
        {
            int32_t lo = 0, hi = R - 1;
            if (!removedSlabRange(axisComp(mScroll, axis), R, lo, hi))
                continue;
            out.push_back(makeSlab(axis, lo, hi, R, false));
        }
        return out;
    }

    ///< Per spec: "resident" is interpreted as "currently inside the dirty (pending-update)
    ///< region" - returns true for voxels that do NOT yet hold valid data and must be built
    ///< this frame. Only the dynamic level can be pending; static levels are always
    ///< resident (false). Out-of-bounds indices are never pending.
    bool isVoxelResident(uint32_t level, const index3& index) const
    {
        if (level >= kDynamicLevels)
            return false;
        const int32_t R = static_cast<int32_t>(mResolution);
        if (index.x < 0 || index.x >= R || index.y < 0 || index.y >= R || index.z < 0 || index.z >= R)
            return false;
        for (int axis = 0; axis < 3; ++axis)
        {
            int32_t lo = 0, hi = R - 1;
            if (addedSlabRange(axisComp(mScroll, axis), R, lo, hi) && axisComp(index, axis) >= lo && axisComp(index, axis) <= hi)
                return true;
        }
        return false;
    }

    // ----------------------------------------------------------------------------------
    // Memory budget
    // ----------------------------------------------------------------------------------

    ///< Pure memory estimate: sum over the current levels of resolution^3 voxels each
    ///< weighing `bytesPerVoxel`: bytes = levelCount * resolution^3 * bytesPerVoxel.
    uint64_t estimateMemoryBytes(float bytesPerVoxel) const
    {
        const uint64_t voxelsPerLevel = static_cast<uint64_t>(mResolution) * mResolution * mResolution;
        return static_cast<uint64_t>(static_cast<double>(voxelsPerLevel) * mLevelCount * bytesPerVoxel);
    }

    ///< Degrade under budget pressure: drop the FARTHEST static level (the coarsest /
    ///< largest-extent remaining level, i.e. the last one). Returns false when no static
    ///< level remains (levelCount == kDynamicLevels). The caller decides when to call it
    ///< (e.g. when estimateMemoryBytes() exceeds the configured budget); enforceBudget()
    ///< wraps it.
    bool dropFarthestStaticLevel()
    {
        if (mLevelCount <= kDynamicLevels)
            return false;
        --mLevelCount;
        mLevels.pop_back();
        return true;
    }

    ///< Convenience: drop farthest static levels until estimateMemoryBytes(bytesPerVoxel)
    ///< fits `budgetBytes`. Returns whether the estimate is now within budget (false when
    ///< even the dynamic-only clipmap exceeds the budget).
    bool enforceBudget(float bytesPerVoxel, uint64_t budgetBytes)
    {
        while (estimateMemoryBytes(bytesPerVoxel) > budgetBytes && dropFarthestStaticLevel())
        {
        }
        return estimateMemoryBytes(bytesPerVoxel) <= budgetBytes;
    }

private:
    // ----------------------------------------------------------------------------------
    // Private helpers
    // ----------------------------------------------------------------------------------

    uint32_t clampLevel(uint32_t level) const { return std::min(level, mLevelCount - 1u); }

    ///< World-space center of a level's grid: dynamic levels follow the snapped camera,
    ///< static levels are anchored at the world origin.
    float3 levelCenter(uint32_t level) const
    {
        return (level < kDynamicLevels) ? mCameraCenter : float3(0.f, 0.f, 0.f);
    }

    ///< True when maxAbs(p - center) <= extent/2 (footprint includes its boundary).
    static bool insideFootprint(const float3& center, const LumenGDFLevel& lvl, const float3& p)
    {
        const float half = lvl.worldExtent * 0.5f;
        return std::fabs(p.x - center.x) <= half && std::fabs(p.y - center.y) <= half && std::fabs(p.z - center.z) <= half;
    }

    ///< Round-to-nearest integer voxel coordinate, half-integers rounded toward zero.
    ///< Rule: |scroll| is nonzero only when the camera move EXCEEDS half a voxel, so a
    ///< coordinate exactly on the .5 boundary snaps to the already-committed center. Negative
    ///< coordinates are handled through fabs() + sign restore (-1.5 -> -1, -0.5 -> 0).
    static int32_t snapVoxel(float v)
    {
        const float a = std::fabs(v);
        int32_t n = static_cast<int32_t>(std::floor(a + 0.5f));
        // Exact .5 boundary (2a == 2n-1 in float): round toward zero.
        if (2.0f * a == 2.0f * static_cast<float>(n) - 1.0f)
            --n;
        return v < 0.f ? -n : n;
    }

    static int32_t axisComp(const index3& i, int axis) { return axis == 0 ? i.x : (axis == 1 ? i.y : i.z); }
    static void setAxis(index3& i, int axis, int32_t v)
    {
        if (axis == 0)
            i.x = v;
        else if (axis == 1)
            i.y = v;
        else
            i.z = v;
    }

    ///< Fills lo/hi (inclusive) with the newly-covered slab on the scrolled axis. Returns
    ///< false when there is no scroll on that axis. Thickness is clamped to the level size.
    static bool addedSlabRange(int32_t s, int32_t R, int32_t& lo, int32_t& hi)
    {
        if (s == 0)
            return false;
        const int32_t thick = static_cast<int32_t>(std::min<uint64_t>(static_cast<uint64_t>(std::abs(static_cast<int64_t>(s))), static_cast<uint64_t>(R)));
        if (s > 0)
        {
            lo = R - thick;
            hi = R - 1;
        }
        else
        {
            lo = 0;
            hi = thick - 1;
        }
        return true;
    }

    ///< Fills lo/hi (inclusive) with the stale/removed slab on the scrolled axis.
    static bool removedSlabRange(int32_t s, int32_t R, int32_t& lo, int32_t& hi)
    {
        if (s == 0)
            return false;
        const int32_t thick = static_cast<int32_t>(std::min<uint64_t>(static_cast<uint64_t>(std::abs(static_cast<int64_t>(s))), static_cast<uint64_t>(R)));
        if (s > 0)
        {
            lo = 0;
            hi = thick - 1;
        }
        else
        {
            lo = R - thick;
            hi = R - 1;
        }
        return true;
    }

    static VoxelRange makeSlab(int axis, int32_t lo, int32_t hi, int32_t R, bool added)
    {
        VoxelRange r;
        r.axis = axis;
        r.added = added;
        setAxis(r.min, axis, lo);
        setAxis(r.max, axis, hi);
        for (int a2 = 0; a2 < 3; ++a2)
        {
            if (a2 != axis)
            {
                setAxis(r.min, a2, 0);
                setAxis(r.max, a2, R - 1);
            }
        }
        return r;
    }

    // ----------------------------------------------------------------------------------
    // State
    // ----------------------------------------------------------------------------------

    float mBaseExtent = 0.f;    ///< Frozen constructor base extent (meters).
    uint32_t mResolution = 128; ///< Voxels per side (all levels share it).
    uint32_t mLevelCount = 0;   ///< Current level count (decremented by budget drops).
    std::vector<LumenGDFLevel> mLevels;
    int32_t mCenterVoxel[3] = {0, 0, 0}; ///< Snapped camera center in level-0 voxel units.
    float3 mCameraCenter = float3(0.f, 0.f, 0.f); ///< Snapped camera center in world units.
    index3 mScroll;                            ///< Scroll of the last setCamera() (level-0 units).
};

} // namespace GlobalDistanceField
} // namespace LumenGI
