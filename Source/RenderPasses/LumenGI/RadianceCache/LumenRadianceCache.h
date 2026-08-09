// =====================================================================================
//  LumenGI - World-space Radiance Cache (RC) clipmap host (CPU-only, header-only)
//  -------------------------------------------------------------------------------------
//  A camera-centered multi-level "clipmap" of radiance probes: level m is a grid of
//  `resolution^3` probes per side covering a world extent `baseExtent * 2^m` (baseExtent
//  default 4 m, resolution default 8 probes per side). Near-field lighting is sampled
//  from the FINE (small-extent) levels, far-field from the COARSE levels.
//
//  This file is the pure-CPU contract (Wave S7-A1). It owns the probe *bookkeeping*:
//  the clipmap geometry, the probe slot pool (free-list + LRU), the per-frame refresh
//  schedule (score x per-frame budget) and the world -> {level, cell, weights, radiance}
//  query. The actual radiance values are produced by a later GPU tracing stage; the root
//  pass commits them through updateProbe() and reads them back through query()/getSlot().
//  The slot pool stores the probe payload on the CPU so query() interpolates without GPU
//  reads; the GPU owns a mirror atlas, addressed by the probe keys this file issues.
//
//  FROZEN DESIGN (single source of truth for the CPU contract)
//  -------------------------------------------------------------------------------------
//  * kRadianceCacheDynamicLevels = 1. Level 0 is DYNAMIC: it is centered on the camera
//    and scrolls when the camera moves (the center re-snaps to the nearest whole level-0
//    voxel). Levels >= kRadianceCacheDynamicLevels are STATIC: anchored at the world
//    origin, they never scroll (far-field content changes slowly).
//  * Voxel sizes are integer multiples: voxelSize_m = 2^m * voxelSize_0, and level 0's
//    center is snapped to an integer multiple of voxelSize_0. Every level's lattice
//    therefore passes through the world origin, so all lattices are mutually aligned
//    sub-lattices and probe faces align across levels (no cracks at LOD borders).
//  * Rounding rules (frozen, identical to LumenGlobalDistanceField):
//      - world -> cell index: floor(vc) (toward -infinity; a negative coordinate like
//        -1.3 voxels floors to -2, which is out of bounds).
//      - camera snap: round-half-toward-zero (|scroll| is nonzero only when the camera
//        move EXCEEDS half a voxel; a move of exactly half a voxel does not scroll).
//        Negative coordinates are handled via fabs() + sign restore.
//  * Scrolling RE-PARENTS level-0 slots: after a scroll of s level-0 voxels, cached
//    content that stays inside the footprint moves from cell c to cell c - s and keeps
//    its radiance; only the newly exposed slabs are re-populated by the refresh budget.
//  * Deterministic: pure integer/index arithmetic plus IEEE binary-float, no randomness,
//    no wall-clock dependence. Two instances driven with identical inputs produce
//    identical outputs.
//
//  INTEGRATION NOTES
//  -------------------------------------------------------------------------------------
//  * PURE C++17, standard library only. No Falcor include, no CMake target (the root
//    pass includes this header and converts Falcor::float3 at the boundary).
//  * The class lives in namespace Falcor; the minimal vector types are nested in the
//    class on purpose so they cannot collide with Falcor::float3 (Utils/Math/VectorTypes.h).
//  * Map estimateMemoryBytes() -> LumenGIResourceStats::radianceCacheBytes at the root.
//  * Syntax check: cl /Zs /std:c++17 /EHsc LumenRadianceCache.h
// =====================================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Falcor
{

// -------------------------------------------------------------------------------------
// Frozen constants
// -------------------------------------------------------------------------------------

///< Number of leading camera-following (scrollable) levels. Frozen: 1.
constexpr uint32_t kRadianceCacheDynamicLevels = 1;
///< Hard cap on the level count (levelCount is clamped to [kDynamicLevels, this]).
constexpr uint32_t kRadianceCacheMaxLevels = 16;
///< Default level count. Level 0 is dynamic; levels 1..5 are static.
constexpr uint32_t kRadianceCacheDefaultLevelCount = 6;
///< Default world extent of level 0 in meters. extent_m = base * 2^m.
constexpr float kRadianceCacheDefaultBaseExtentMeters = 4.0f;
///< Default probes per side (all levels share one resolution). 8^3 = 512 probes/level.
constexpr uint32_t kRadianceCacheDefaultResolution = 8;
///< Default number of probes refreshed per frame (the per-frame refresh budget).
constexpr uint32_t kRadianceCacheDefaultRefreshBudgetPerFrame = 64;
///< Default slot-pool capacity (probe cache slots, page allocation pool).
constexpr uint32_t kRadianceCacheDefaultMaxSlots = 4096;
///< Default LRU min residency: a probe stays untouchable for N frames after its update.
constexpr uint32_t kRadianceCacheDefaultMinResidencyFrames = 2;
///< Age clamp used by the refresh priority (frames). Old probes all tie at this age.
constexpr uint32_t kRadianceCacheMaxAgeFrames = 1024;
///< A probe is "fresh" when it was updated within this many frames.
constexpr uint32_t kRadianceCacheFreshnessWindowFrames = 120;
///< Confidence a probe gets on its FIRST successful update.
constexpr float kRadianceCacheFirstUpdateConfidence = 0.25f;
///< Confidence blend alpha for every subsequent update: c += (1-c) * alpha.
constexpr float kRadianceCacheConfidenceAlpha = 0.25f;
///< GPU-resident payload bytes of one probe slot: radiance float3 (12) + direction
///< encoding uint (4) + confidence float (4) + lastUpdateFrame uint64 (8) +
///< generation uint32 (4) = 32 bytes. CPU-side bookkeeping (level/cell/visibility) is
///< not part of the GPU memory estimate.
constexpr uint32_t kRadianceCacheBytesPerProbeSlot = 32;
///< Approximate CPU bytes of one clipmap cell (hash-map entry + metadata).
constexpr uint32_t kRadianceCacheBytesPerCellMeta = 16;
///< Slot ID of an invalid/unallocated slot. Valid slot IDs are 1..getMaxSlots().
constexpr uint32_t kInvalidProbeSlot = 0;
///< Sentinel for an unset direction encoding (0xFFFFFFFF is never produced by the
///< octahedral encoder, which maps to [0, 0xFFFF]^2).
constexpr uint32_t kRadianceCacheInvalidDirectionEncoding = 0xFFFFFFFFu;

// =====================================================================================
// LumenRadianceCache - camera-centered probe clipmap with slot pool + refresh schedule
// =====================================================================================
class LumenRadianceCache
{
public:
    // ----------------------------------------------------------------------------------
    // Minimal value types. Nested on purpose: the real Falcor build has Falcor::float3,
    // so a namespace-scope `float3` here would collide at the include boundary. The root
    // pass converts Falcor::float3 <-> LumenRadianceCache::float3 at the API boundary.
    // ----------------------------------------------------------------------------------

    ///< Minimal 3D float vector used by the public API.
    struct float3
    {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;

        constexpr float3() = default;
        constexpr float3(float px, float py, float pz) : x(px), y(py), z(pz) {}
    };

    ///< Integer 3D index (level-local probe cell coordinates). Used for cells, scrolls.
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

    ///< Per-level parameters. extent_m = baseExtent * 2^m; voxelSize_m = extent_m / resolution.
    struct LumenRadianceCacheLevel
    {
        uint32_t resolution = kRadianceCacheDefaultResolution; ///< Probes per side.
        float worldExtent = 0.f;   ///< World-space cube side length of this level.
        float voxelSize = 0.f;     ///< worldExtent / resolution (uniform, axis-independent).
    };

    ///< One probe cache slot (the page-allocation unit). The first five fields are the
    ///< GPU-resident payload (see kRadianceCacheBytesPerProbeSlot); the rest is CPU
    ///< bookkeeping. Callers read slots through getSlot() (const ref, never write).
    struct RadianceCacheProbeSlot
    {
        float radiance[3] = {0.f, 0.f, 0.f};                 ///< Interpolated radiance RGB.
        uint32_t directionEncoding = kRadianceCacheInvalidDirectionEncoding; ///< Octahedral dir.
        float confidence = 0.f;                              ///< 0..1 update confidence.
        uint64_t lastUpdateFrame = 0;                        ///< Frame of the last update.
        uint32_t generation = 0;                             ///< Allocation epochs (key part).
        uint32_t level = 0;                                  ///< Clipmap level this slot lives in.
        index3 cell;                                         ///< Level-local probe cell.
        float visibilityWeight = 1.f;                        ///< External visibility (0..1).
        bool allocated = false;                              ///< Owned by a clipmap cell.
    };

    ///< One refresh unit returned by tick(): trace radiance for this probe and commit it
    ///< through updateProbe(slot, ...). `key` is the current probe key of the slot (level +
    ///< cell + generation), the token the GPU uses to validate cached entries.
    struct RadianceCacheRefreshRequest
    {
        uint32_t slot = kInvalidProbeSlot; ///< Slot to trace into.
        uint32_t level = 0;                ///< Level of the probe.
        index3 cell;                       ///< Level-local cell of the probe.
        uint64_t key = 0;                  ///< makeProbeKey(level, cell, generation).
        bool isNewSlot = false;            ///< True when the slot was allocated by this tick.
    };

    ///< Result of query(worldPos): the containing level/cell, trilinear interpolation
    ///< weights, the eight surrounding corner cells/slots/keys, and the interpolated
    ///< radiance + confidence with a fresh/expired verdict.
    struct RadianceCacheQueryResult
    {
        uint32_t level = 0;           ///< Level that owns the queried point.
        index3 cell;                  ///< Containing cell (floor corner).
        float3 frac;                  ///< Fractional position inside the cell (in [0,1]^3).
        float radiance[3] = {0.f, 0.f, 0.f}; ///< Trilinearly interpolated radiance.
        float confidence = 0.f;       ///< Weight-sum-normalized confidence.
        bool valid = false;           ///< True when >= 1 contributing corner has a slot.
        bool fresh = false;           ///< True when every contributing corner is fresh.
        bool expired = true;          ///< !fresh; true when any contributor is stale/missing.
        uint32_t allocatedCornerCount = 0; ///< How many of the 8 corners are resident.
        index3 cornerCells[8];        ///< The 8 corner cells (order: x bit 2, y bit 1, z bit 0).
        uint32_t cornerSlots[8] = {kInvalidProbeSlot, kInvalidProbeSlot, kInvalidProbeSlot,
                                   kInvalidProbeSlot, kInvalidProbeSlot, kInvalidProbeSlot,
                                   kInvalidProbeSlot, kInvalidProbeSlot}; ///< Resident corner slots.
        uint64_t cornerKeys[8] = {0, 0, 0, 0, 0, 0, 0, 0}; ///< Probe keys of resident corners.
        bool cornerFresh[8] = {false, false, false, false, false, false, false, false};
    };

    ///< Snapshot of allocator/cache state (see getStats()).
    struct RadianceCacheStats
    {
        uint32_t levelCount = 0;              ///< Current levels (shrinks after drops).
        uint32_t resolution = 0;              ///< Probes per side (all levels).
        uint32_t maxSlots = 0;                ///< Slot-pool capacity.
        uint32_t allocatedSlotCount = 0;      ///< Slots owned by clipmap cells.
        uint32_t freeSlotCount = 0;           ///< Slots on the free-list.
        uint32_t emptyCellCount = 0;          ///< Clipmap cells that still lack a slot.
        uint32_t refreshBudgetPerFrame = 0;   ///< Refresh budget in effect.
        uint32_t lastRefreshCount = 0;        ///< Requests returned by the last tick().
        uint64_t frameIndex = 0;              ///< Frames elapsed since construction/reset.
        uint64_t memoryBudgetBytes = 0;       ///< Budget; 0 means unlimited.
        uint64_t estimateMemoryBytes = 0;     ///< estimateMemoryBytes() at snapshot time.
        uint64_t allocationCount = 0;         ///< Total slot (re)allocations.
        uint64_t evictionCount = 0;           ///< Total LRU evictions.
        uint64_t releaseCount = 0;            ///< Total explicit releaseProbe() calls.
        uint64_t updateCount = 0;             ///< Total updateProbe() commits.
        uint64_t dropCount = 0;               ///< Total dropped static levels (degradation).
        uint64_t queryCount = 0;              ///< Total query() calls.
        uint64_t refreshCount = 0;            ///< Cumulative refresh requests issued.
    };

    // ----------------------------------------------------------------------------------
    // Construction
    // ----------------------------------------------------------------------------------

    ///< Build the probe clipmap. Level m spans baseExtent * 2^m meters at `resolution`
    ///< probes per side; the camera starts at the world origin (already on the lattice).
    ///< `maxSlots` caps the slot pool (free-list/LRU pressure beyond it). Invalid inputs
    ///< are clamped (baseExtent > 0, resolution >= 1, kDynamicLevels <= levelCount <=
    ///< kMaxLevels, maxSlots >= 1, refreshBudget >= 1, minResidency >= 1).
    explicit LumenRadianceCache(
        float baseExtentMeters = kRadianceCacheDefaultBaseExtentMeters,
        uint32_t levelCount = kRadianceCacheDefaultLevelCount,
        uint32_t resolution = kRadianceCacheDefaultResolution,
        uint32_t maxSlots = kRadianceCacheDefaultMaxSlots,
        uint32_t refreshBudgetPerFrame = kRadianceCacheDefaultRefreshBudgetPerFrame,
        uint32_t minResidencyFrames = kRadianceCacheDefaultMinResidencyFrames,
        uint64_t memoryBudgetBytes = 0);

    // ----------------------------------------------------------------------------------
    // Level / parameter access
    // ----------------------------------------------------------------------------------

    uint32_t getLevelCount() const { return mLevelCount; }
    uint32_t getResolution() const { return mResolution; }
    float getBaseExtent() const { return mBaseExtent; }
    uint32_t getMaxSlots() const { return static_cast<uint32_t>(mSlots.size() - 1); }
    uint32_t getMinResidencyFrames() const { return mMinResidencyFrames; }
    uint32_t getRefreshBudgetPerFrame() const { return mRefreshBudgetPerFrame; }
    void setRefreshBudgetPerFrame(uint32_t budget) { mRefreshBudgetPerFrame = std::max(1u, budget); }

    ///< Level parameters. `level` is clamped into [0, mLevelCount-1] (deterministic no-op
    ///< for stale handles after dropFarthestStaticLevel()).
    const LumenRadianceCacheLevel& getLevel(uint32_t level) const
    {
        return mLevels[clampLevel(level)];
    }

    // ----------------------------------------------------------------------------------
    // Camera / scroll
    // ----------------------------------------------------------------------------------

    ///< Re-center the dynamic level on `worldPos`. The camera center is SNAPPED to the
    ///< nearest whole level-0 voxel (round-half-toward-zero), so movements within half a
    ///< voxel produce no scroll. On a real scroll the level-0 slots are RE-PARENTED
    ///< (cell -= scroll; content leaving the footprint is freed), so cached radiance
    ///< survives camera motion and only the newly exposed slabs need re-population.
    void setCamera(const float3& worldPos);

    ///< Integer scroll (in level-0 voxel units, per axis) produced by the last setCamera().
    ///< Nonzero on an axis only when that move exceeded half a voxel.
    index3 scrollFromCameraMove() const { return mScroll; }

    ///< Snapped camera center in world units (an integer multiple of voxelSize_0).
    const float3& getCameraCenter() const { return mCameraCenter; }

    // ----------------------------------------------------------------------------------
    // Level selection and world <-> cell mapping
    // ----------------------------------------------------------------------------------

    ///< Pick the level that should own `worldPos`:
    ///<  1. NEAR FIELD -> the dynamic levels: any point inside a dynamic level's footprint
    ///<     (finest first) is sampled from it. With kRadianceCacheDynamicLevels = 1 this
    ///<     is level 0 whenever maxAbs(p - cameraCenter) <= extent_0 / 2.
    ///<  2. FAR FIELD  -> the static levels: the FINEST static level whose origin-anchored
    ///<     footprint contains the point (maxAbs(p) <= extent_m / 2).
    ///<  3. FALLBACK   -> the coarsest remaining level (points beyond every footprint are
    ///<     clamped; worldToCell then reports inBounds == false).
    ///< A point exactly on a footprint boundary is considered inside (`<=`).
    uint32_t levelIndexForWorld(const float3& worldPos) const;

    ///< Convert a world position to a level-local cell index and the fractional position
    ///< inside it. gridMin_m = levelCenter_m - extent_m/2; the continuous cell coordinate
    ///< is vc = (worldPos - gridMin_m) / voxelSize_m and the containing cell is floor(vc).
    ///< Returns false when any component of the index falls outside [0, resolution).
    bool worldToCell(uint32_t level, const float3& worldPos, index3& outCell, float3& outFrac) const;

    ///< World-space center of a probe (inverse of worldToCell, cell centers):
    ///< world = gridMin_m + (cell + 0.5) * voxelSize_m. Well-defined for any cell; inside
    ///< the footprint only when the cell was in bounds.
    float3 probeWorldPosition(uint32_t level, const index3& cell) const;

    // ----------------------------------------------------------------------------------
    // Probe key contract (level + cell + generation -> uint64)
    // ----------------------------------------------------------------------------------

    ///< Pack the probe key: generation in bits [0,16), cell.x [16,26), cell.y [26,36),
    ///< cell.z [36,46), level [46,52); bits [52,64) are zero. Supports resolution <= 1024,
    ///< level <= 63 and generation <= 65535. The generation component lets the GPU detect
    ///< stale reads: reusing a slot for a different cell bumps the generation and the key
    ///< changes even though the slot index stays the same.
    static uint64_t makeProbeKey(uint32_t level, const index3& cell, uint32_t generation);

    ///< Cell lookup key WITHOUT the generation: bits cell.x [0,10), cell.y [10,20),
    ///< cell.z [20,30), level [30,36). Used only by the CPU's cell->slot map.
    static uint64_t makeCellKey(uint32_t level, const index3& cell);

    // ----------------------------------------------------------------------------------
    // Direction encoding (octahedral, packed into one uint)
    // ----------------------------------------------------------------------------------

    ///< Encode a unit direction with the octahedral mapping, quantized to 2x16 bits:
    ///<   v = dir / (|x| + |y| + |z|)
    ///<   if v.z < 0: v.xy = (1 - |v.yx|) * sign(v.xy)
    ///<   u = v.x*0.5+0.5, t = v.y*0.5+0.5  ->  uint32((t*65535)<<16 | (u*65535))
    ///< Returns kRadianceCacheInvalidDirectionEncoding for a degenerate (zero) input.
    static uint32_t encodeDirection(const float3& dir);

    ///< Inverse of encodeDirection: 16-bit fixed point -> [0,1]^2 -> octahedron -> unit
    ///< sphere. Deterministic; decode(encode(d)) reproduces d up to quantization.
    static float3 decodeDirection(uint32_t encoded);

    // ----------------------------------------------------------------------------------
    // Slot pool (free-list / LRU page allocation)
    // ----------------------------------------------------------------------------------

    ///< Find the slot currently owned by `cell` of `level`. Returns kInvalidProbeSlot if
    ///< the cell is not yet resident. Never allocates.
    uint32_t findProbe(uint32_t level, const index3& cell) const;

    ///< Make sure `cell` of `level` owns a slot. Returns the existing slot, or allocates
    ///< one from the free-list, or LRU-evicts an old slot when the pool is full. Returns
    ///< kInvalidProbeSlot when the pool is full and nothing is evictable. Bumping the
    ///< generation on reuse invalidates stale GPU keys. Allocation is lazy: the clipmap
    ///< populates progressively as the refresh schedule touches cells.
    uint32_t allocateProbe(uint32_t level, const index3& cell);

    ///< Explicitly release a slot back to the free-list. False (no-op) for invalid or
    ///< not-allocated slots.
    bool releaseProbe(uint32_t slot);

    ///< Commit a traced radiance result into a slot: stores the payload, stamps the
    ///< update frame, and raises the confidence:
    ///<   first update : confidence = kRadianceCacheFirstUpdateConfidence
    ///<   later updates: confidence = min(1, confidence + (1 - confidence) * alpha)
    bool updateProbe(uint32_t slot, const float radiance[3], uint32_t directionEncoding);

    ///< Override the refresh visibility weight of a slot (0..1, default 1). The root pass
    ///< feeds this from view-dependent signals (frustum/shadow); refreshScore() multiplies
    ///< it in. Clamped to [0,1].
    bool setProbeVisibilityWeight(uint32_t slot, float weight);

    ///< True when the slot is allocated and was updated within the freshness window.
    bool isProbeFresh(uint32_t slot) const;

    ///< Const read access to a slot. slot 0 / out-of-range returns a static invalid slot.
    const RadianceCacheProbeSlot& getSlot(uint32_t slot) const;

    ///< Current probe key of a slot (0 when not allocated). The GPU validates cached
    ///< entries against this token.
    uint64_t getProbeKey(uint32_t slot) const;

    // ----------------------------------------------------------------------------------
    // Refresh scheduling
    // ----------------------------------------------------------------------------------

    ///< Refresh priority (score) of a slot, per the frozen formula:
    ///<
    ///<     score = visibilityWeight * distanceWeight * age
    ///<
    ///<   * visibilityWeight: per-slot, externally supplied (default 1), in [0,1].
    ///<   * distanceWeight  = 1 / (1 + dist / radius), radius = level worldExtent / 2 and
    ///<     dist = |probeWorld - cameraCenter|. A probe at the camera scores 1, at the
    ///<     footprint edge 0.5, farther asymptotically lower (never 0, so far probes still
    ///<     eventually refresh).
    ///<   * age = frames since last update, clamped to [1, kRadianceCacheMaxAgeFrames];
    ///<     a never-updated probe (confidence == 0) is treated as maximally stale.
    ///< Ties are broken deterministically (level asc, cell asc, slot asc).
    float refreshScore(const RadianceCacheProbeSlot& slot) const;

    ///< Advance the frame counter and compute the refresh schedule for this frame:
    ///<   1. Candidates = all allocated slots (scored by refreshScore) plus all clipmap
    ///<      cells that still lack a slot (scored with visibility 1, distanceWeight and
    ///<      maximal age, so empty near-field cells are always picked before stale far
    ///<      probes).
    ///<   2. Sort by score desc (deterministic tie-break) and take the first
    ///<      mRefreshBudgetPerFrame.
    ///<   3. Empty-cell candidates are allocated now (free-list, else LRU eviction); a
    ///<      candidate whose slot was evicted earlier in the same pass is skipped.
    ///< Returns the per-frame refresh list; consume it before the next tick() call.
    const std::vector<RadianceCacheRefreshRequest>& tick();

    ///< The refresh list produced by the last tick().
    const std::vector<RadianceCacheRefreshRequest>& getLastRefreshRequests() const
    {
        return mRefreshRequests;
    }

    // ----------------------------------------------------------------------------------
    // Query
    // ----------------------------------------------------------------------------------

    ///< Interpolate the radiance cache at a world position: pick the owning level, the
    ///< containing cell, the 8 surrounding corner cells, their probe keys/slots and the
    ///< trilinear weights (standard product form, weights sum to 1 over all 8 corners),
    ///< then accumulate the interpolated radiance and confidence. Missing corners are
    ///< skipped and the weights are re-normalized over the resident corners only; `fresh`
    ///< is true when every contributing corner is fresh (see isProbeFresh), otherwise
    ///< `expired` is true. valid == false when no corner is resident (radiance is zeroed).
    RadianceCacheQueryResult query(const float3& worldPos) const;

    // ----------------------------------------------------------------------------------
    // Memory estimate and budget degradation
    // ----------------------------------------------------------------------------------

    ///< GPU-resident memory estimate: every allocated slot weighs `bytesPerProbeSlot`
    ///< (32 bytes) and every clipmap cell of the current levels weighs `bytesPerCellMeta`:
    ///<
    ///<     bytes = allocatedSlots * bytesPerProbeSlot + sum_m(resolution^3) * bytesPerCellMeta
    ///<
    ///< Degradation drops static levels -> their slots and cells disappear from the sum.
    uint64_t estimateMemoryBytes(
        uint64_t bytesPerProbeSlot = kRadianceCacheBytesPerProbeSlot,
        uint64_t bytesPerCellMeta = kRadianceCacheBytesPerCellMeta) const;

    ///< Degrade under budget pressure: drop the FARTHEST static level (the coarsest /
    ///< largest-extent remaining level, i.e. the last one), freeing all its slots. Returns
    ///< false when no static level remains (levelCount == kDynamicLevels). The dynamic
    ///< level is never dropped.
    bool dropFarthestStaticLevel();

    ///< Current memory budget; 0 means unlimited.
    uint64_t getMemoryBudgetBytes() const { return mMemoryBudgetBytes; }

    ///< Set a new budget (0 = unlimited). Enforcement happens on the next enforceBudget().
    void setMemoryBudgetBytes(uint64_t memoryBudgetBytes) { mMemoryBudgetBytes = memoryBudgetBytes; }

    ///< Convenience: set the budget and drop farthest static levels until the estimate
    ///< fits it. Returns whether the estimate is now within budget (false when even the
    ///< dynamic-only clipmap exceeds the budget; budget 0 always returns true).
    bool enforceBudget(uint64_t budgetBytes);

    ///< Enforce the currently stored budget (no-op success when it is 0).
    bool enforceBudget();

    // ----------------------------------------------------------------------------------
    // Frame / stats / reset
    // ----------------------------------------------------------------------------------

    ///< Advance one frame without scheduling refreshes (used when the root only queries).
    void advanceFrame() { ++mFrameIndex; }

    ///< Current frame index (incremented by tick()/advanceFrame()).
    uint64_t getFrameIndex() const { return mFrameIndex; }

    ///< Snapshot of allocator and cache state (see RadianceCacheStats).
    RadianceCacheStats getStats() const;

    ///< Reset to a freshly constructed state, keeping the current level count / geometry
    ///< (including any budget degradation) and slot capacity. Clears all slots, the
    ///< cell->slot map, the free-list (rebuilt ascending), generations, counters and the
    ///< frame index. Used on scene reload.
    void reset();

private:
    // ----------------------------------------------------------------------------------
    // Private helpers
    // ----------------------------------------------------------------------------------

    uint32_t clampLevel(uint32_t level) const { return std::min(level, mLevelCount - 1u); }

    ///< World-space center of a level's grid: the dynamic level follows the snapped
    ///< camera, static levels are anchored at the world origin.
    float3 levelCenter(uint32_t level) const
    {
        return (level < kRadianceCacheDynamicLevels) ? mCameraCenter : float3(0.f, 0.f, 0.f);
    }

    ///< True when maxAbs(p - center) <= extent/2 (footprint includes its boundary).
    static bool insideFootprint(const float3& center, const LumenRadianceCacheLevel& lvl, const float3& p);

    ///< Round-to-nearest integer voxel coordinate, half-integers rounded toward zero
    ///< (|scroll| is nonzero only when the move EXCEEDS half a voxel; -1.5 -> -1, -0.5 -> 0).
    static int32_t snapVoxel(float v);

    ///< distanceWeight of refreshScore: 1 / (1 + dist/radius).
    float distanceWeight(uint32_t level, const float3& probeWorld) const;

    ///< Allocate a slot for a cell (see allocateProbe; no map lookup shortcut).
    uint32_t allocateSlot(uint32_t level, const index3& cell);

    ///< Re-parent level-0 slots after a camera scroll: cell -= scroll, content leaving the
    ///< footprint is freed.
    void reparentLevel0(const index3& scroll);

    ///< Free a slot (map erase, mark free, push free-list). No counter side effects.
    void freeSlot(uint32_t slot);

    ///< LRU eviction candidate: the allocated slot with the smallest lastUpdateFrame among
    ///< those outside the min-residency window (never-updated slots are always evictable)
    ///< and not queued for refresh by the current tick(). Ties broken by smallest slot ID.
    uint32_t findEvictionCandidate() const;

    ///< Evict a slot (freeSlot + eviction counter).
    void evictSlot(uint32_t slot);

    ///< The 8 trilinear weights for a fractional position inside a cell.
    ///<   w[(i<<2)|(j<<1)|k] = (i? fx:1-fx) * (j? fy:1-fy) * (k? fz:1-fz)
    ///< weights sum to 1 over all 8 corners.
    static void trilinearWeights(const float3& frac, float out[8]);

    // ----------------------------------------------------------------------------------
    // State
    // ----------------------------------------------------------------------------------

    float mBaseExtent = 0.f;                       ///< Frozen constructor base extent (meters).
    uint32_t mResolution = kRadianceCacheDefaultResolution; ///< Probes per side (all levels).
    uint32_t mLevelCount = 0;                      ///< Current level count (shrinks on drops).
    std::vector<LumenRadianceCacheLevel> mLevels;
    int32_t mCenterVoxel[3] = {0, 0, 0};           ///< Snapped camera center, level-0 units.
    float3 mCameraCenter = float3(0.f, 0.f, 0.f);  ///< Snapped camera center, world units.
    index3 mScroll;                                ///< Scroll of the last setCamera().

    std::vector<RadianceCacheProbeSlot> mSlots;    ///< Slot table; index 0 unused (invalid).
    std::vector<uint32_t> mFreeList;               ///< LIFO stack of free slot IDs.
    std::unordered_map<uint64_t, uint32_t> mCellToSlot; ///< Cell key -> slot ID.
    std::vector<RadianceCacheRefreshRequest> mRefreshRequests; ///< Last tick() schedule.

    uint32_t mRefreshBudgetPerFrame = kRadianceCacheDefaultRefreshBudgetPerFrame;
    uint32_t mMinResidencyFrames = kRadianceCacheDefaultMinResidencyFrames;
    uint64_t mMemoryBudgetBytes = 0;               ///< Budget; 0 = unlimited.

    uint64_t mFrameIndex = 0;
    uint64_t mAllocationCount = 0;
    uint64_t mEvictionCount = 0;
    uint64_t mReleaseCount = 0;
    uint64_t mUpdateCount = 0;
    uint64_t mDropCount = 0;
    uint64_t mRefreshCount = 0;
    mutable uint64_t mQueryCount = 0;              ///< mutable: query() is const.
};

// =====================================================================================
// Implementation
// =====================================================================================

inline LumenRadianceCache::LumenRadianceCache(
    float baseExtentMeters,
    uint32_t levelCount,
    uint32_t resolution,
    uint32_t maxSlots,
    uint32_t refreshBudgetPerFrame,
    uint32_t minResidencyFrames,
    uint64_t memoryBudgetBytes)
    : mMemoryBudgetBytes(memoryBudgetBytes)
{
    const float base = baseExtentMeters > 0.f ? baseExtentMeters : kRadianceCacheDefaultBaseExtentMeters;
    const uint32_t res = std::max(1u, resolution);
    const uint32_t count = std::min(std::max(levelCount, kRadianceCacheDynamicLevels), kRadianceCacheMaxLevels);
    const uint32_t slots = std::max(1u, maxSlots);

    mBaseExtent = base;
    mResolution = res;
    mLevelCount = count;
    mRefreshBudgetPerFrame = std::max(1u, refreshBudgetPerFrame);
    mMinResidencyFrames = std::max(1u, minResidencyFrames);

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

    // Slot table index 0 is the invalid sentinel; valid slot IDs are 1..slots.
    mSlots.assign(static_cast<size_t>(slots) + 1, RadianceCacheProbeSlot{});
    // Build the free-list descending so pop_back() hands out ascending IDs (deterministic).
    for (uint32_t slotID = slots; slotID >= 1; --slotID)
    {
        mFreeList.push_back(slotID);
    }
}

inline void LumenRadianceCache::setCamera(const float3& worldPos)
{
    const float vs0 = mLevels[0].voxelSize;
    const int32_t nv[3] = {snapVoxel(worldPos.x / vs0), snapVoxel(worldPos.y / vs0), snapVoxel(worldPos.z / vs0)};
    const int32_t ov[3] = {mCenterVoxel[0], mCenterVoxel[1], mCenterVoxel[2]};

    const int32_t dx = nv[0] - ov[0];
    const int32_t dy = nv[1] - ov[1];
    const int32_t dz = nv[2] - ov[2];
    if (dx != 0 || dy != 0 || dz != 0)
    {
        reparentLevel0(index3(dx, dy, dz));
    }

    for (int a = 0; a < 3; ++a)
    {
        mCenterVoxel[a] = nv[a];
    }
    // Snapped world-space center = integer voxel coordinate * voxelSize_0.
    mCameraCenter = float3(static_cast<float>(mCenterVoxel[0]) * vs0,
                           static_cast<float>(mCenterVoxel[1]) * vs0,
                           static_cast<float>(mCenterVoxel[2]) * vs0);
    mScroll = index3(dx, dy, dz);
}

inline uint32_t LumenRadianceCache::levelIndexForWorld(const float3& worldPos) const
{
    for (uint32_t m = 0; m < kRadianceCacheDynamicLevels && m < mLevelCount; ++m)
    {
        if (insideFootprint(levelCenter(m), mLevels[m], worldPos))
        {
            return m;
        }
    }
    const float3 origin(0.f, 0.f, 0.f);
    for (uint32_t m = kRadianceCacheDynamicLevels; m < mLevelCount; ++m)
    {
        if (insideFootprint(origin, mLevels[m], worldPos))
        {
            return m;
        }
    }
    return mLevelCount - 1u;
}

inline bool LumenRadianceCache::worldToCell(uint32_t level, const float3& worldPos, index3& outCell, float3& outFrac) const
{
    const uint32_t m = clampLevel(level);
    const LumenRadianceCacheLevel& lvl = mLevels[m];
    const float3 center = levelCenter(m);
    const float half = lvl.worldExtent * 0.5f;
    const float vs = lvl.voxelSize;

    const float gx = worldPos.x - (center.x - half);
    const float gy = worldPos.y - (center.y - half);
    const float gz = worldPos.z - (center.z - half);
    const float vcx = gx / vs;
    const float vcy = gy / vs;
    const float vcz = gz / vs;

    const int32_t cx = static_cast<int32_t>(std::floor(vcx));
    const int32_t cy = static_cast<int32_t>(std::floor(vcy));
    const int32_t cz = static_cast<int32_t>(std::floor(vcz));
    const int32_t R = static_cast<int32_t>(lvl.resolution);

    outCell.x = cx;
    outCell.y = cy;
    outCell.z = cz;
    // frac in [0,1] (clamped against float rounding at exact voxel boundaries).
    outFrac.x = std::min(1.0f, std::max(0.0f, vcx - static_cast<float>(cx)));
    outFrac.y = std::min(1.0f, std::max(0.0f, vcy - static_cast<float>(cy)));
    outFrac.z = std::min(1.0f, std::max(0.0f, vcz - static_cast<float>(cz)));

    return cx >= 0 && cx < R && cy >= 0 && cy < R && cz >= 0 && cz < R;
}

inline LumenRadianceCache::float3 LumenRadianceCache::probeWorldPosition(uint32_t level, const index3& cell) const
{
    const uint32_t m = clampLevel(level);
    const LumenRadianceCacheLevel& lvl = mLevels[m];
    const float3 center = levelCenter(m);
    const float half = lvl.worldExtent * 0.5f;
    const float vs = lvl.voxelSize;
    return float3(center.x - half + (static_cast<float>(cell.x) + 0.5f) * vs,
                  center.y - half + (static_cast<float>(cell.y) + 0.5f) * vs,
                  center.z - half + (static_cast<float>(cell.z) + 0.5f) * vs);
}

inline uint64_t LumenRadianceCache::makeProbeKey(uint32_t level, const index3& cell, uint32_t generation)
{
    return (static_cast<uint64_t>(generation & 0xFFFFu)) |
           (static_cast<uint64_t>(cell.x & 0x3FF) << 16) |
           (static_cast<uint64_t>(cell.y & 0x3FF) << 26) |
           (static_cast<uint64_t>(cell.z & 0x3FF) << 36) |
           (static_cast<uint64_t>(level & 0x3Fu) << 46);
}

inline uint64_t LumenRadianceCache::makeCellKey(uint32_t level, const index3& cell)
{
    return (static_cast<uint64_t>(cell.x & 0x3FF)) |
           (static_cast<uint64_t>(cell.y & 0x3FF) << 10) |
           (static_cast<uint64_t>(cell.z & 0x3FF) << 20) |
           (static_cast<uint64_t>(level & 0x3Fu) << 30);
}

inline uint32_t LumenRadianceCache::encodeDirection(const float3& dir)
{
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len <= 0.f)
    {
        return kRadianceCacheInvalidDirectionEncoding;
    }
    const float inv = 1.f / len;
    const float nx = dir.x * inv;
    const float ny = dir.y * inv;
    const float nz = dir.z * inv;

    const float absSum = std::fabs(nx) + std::fabs(ny) + std::fabs(nz);
    float ox = nx / absSum;
    float oy = ny / absSum;
    if (nz < 0.f)
    {
        // Fold the lower hemisphere onto the octahedron (standard mapping).
        const float oxSign = ox >= 0.f ? 1.f : -1.f;
        const float oySign = oy >= 0.f ? 1.f : -1.f;
        const float nx2 = (1.f - std::fabs(oy)) * oxSign;
        const float ny2 = (1.f - std::fabs(ox)) * oySign;
        ox = nx2;
        oy = ny2;
    }
    const float u = ox * 0.5f + 0.5f;
    const float v = oy * 0.5f + 0.5f;
    uint32_t uq = static_cast<uint32_t>(std::min(65535.0f, std::max(0.0f, u * 65535.0f)));
    uint32_t vq = static_cast<uint32_t>(std::min(65535.0f, std::max(0.0f, v * 65535.0f)));
    // The octahedral fold maps the exact -Z direction to (1,1) -> 0xFFFFFFFF, which would
    // alias with kRadianceCacheInvalidDirectionEncoding. Clamp that single corner so the
    // sentinel is never produced by a valid direction.
    if (uq == 0xFFFFu && vq == 0xFFFFu)
    {
        vq = 0xFFFEu;
    }
    return (vq << 16) | uq;
}

inline LumenRadianceCache::float3 LumenRadianceCache::decodeDirection(uint32_t encoded)
{
    const float u = (static_cast<float>(encoded & 0xFFFFu)) / 65535.0f * 2.0f - 1.0f;
    const float v = (static_cast<float>((encoded >> 16) & 0xFFFFu)) / 65535.0f * 2.0f - 1.0f;
    float nx = u;
    float ny = v;
    float nz = 1.f - std::fabs(u) - std::fabs(v);
    if (nz < 0.f)
    {
        // Folded point (|x|+|y|>1, lower hemisphere): unfold back to the octahedron and
        // KEEP the negative z (the unfolded point is on the z<0 octant).
        const float tx = nx;
        const float nxSign = tx >= 0.f ? 1.f : -1.f;
        const float nySign = ny >= 0.f ? 1.f : -1.f;
        nx = (1.f - std::fabs(ny)) * nxSign;
        ny = (1.f - std::fabs(tx)) * nySign;
    }
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len <= 0.f)
    {
        return float3(0.f, 0.f, 1.f);
    }
    const float inv = 1.f / len;
    return float3(nx * inv, ny * inv, nz * inv);
}

inline uint32_t LumenRadianceCache::findProbe(uint32_t level, const index3& cell) const
{
    const auto it = mCellToSlot.find(makeCellKey(clampLevel(level), cell));
    return it == mCellToSlot.end() ? kInvalidProbeSlot : it->second;
}

inline uint32_t LumenRadianceCache::allocateProbe(uint32_t level, const index3& cell)
{
    return allocateSlot(clampLevel(level), cell);
}

inline bool LumenRadianceCache::releaseProbe(uint32_t slot)
{
    if (slot == kInvalidProbeSlot || slot >= mSlots.size() || !mSlots[slot].allocated)
    {
        return false;
    }
    freeSlot(slot);
    ++mReleaseCount;
    return true;
}

inline bool LumenRadianceCache::updateProbe(uint32_t slot, const float radiance[3], uint32_t directionEncoding)
{
    if (slot == kInvalidProbeSlot || slot >= mSlots.size())
    {
        return false;
    }
    RadianceCacheProbeSlot& s = mSlots[slot];
    if (!s.allocated)
    {
        return false;
    }
    s.radiance[0] = radiance[0];
    s.radiance[1] = radiance[1];
    s.radiance[2] = radiance[2];
    s.directionEncoding = directionEncoding;
    s.lastUpdateFrame = mFrameIndex;
    // Confidence: first update jumps to kFirstUpdateConfidence, later updates blend up.
    s.confidence = s.confidence <= 0.f
        ? kRadianceCacheFirstUpdateConfidence
        : std::min(1.0f, s.confidence + (1.0f - s.confidence) * kRadianceCacheConfidenceAlpha);
    ++mUpdateCount;
    return true;
}

inline bool LumenRadianceCache::setProbeVisibilityWeight(uint32_t slot, float weight)
{
    if (slot == kInvalidProbeSlot || slot >= mSlots.size() || !mSlots[slot].allocated)
    {
        return false;
    }
    mSlots[slot].visibilityWeight = std::min(1.0f, std::max(0.0f, weight));
    return true;
}

inline bool LumenRadianceCache::isProbeFresh(uint32_t slot) const
{
    if (slot == kInvalidProbeSlot || slot >= mSlots.size())
    {
        return false;
    }
    const RadianceCacheProbeSlot& s = mSlots[slot];
    // A slot is never-updated when its confidence is still 0 (lastUpdateFrame may be 0 on
    // the very first frame, so it is not a reliable "never updated" marker).
    if (!s.allocated || s.confidence <= 0.f)
    {
        return false;
    }
    return (mFrameIndex - s.lastUpdateFrame) <= kRadianceCacheFreshnessWindowFrames;
}

inline const LumenRadianceCache::RadianceCacheProbeSlot& LumenRadianceCache::getSlot(uint32_t slot) const
{
    static const RadianceCacheProbeSlot kInvalid;
    if (slot == kInvalidProbeSlot || slot >= mSlots.size())
    {
        return kInvalid;
    }
    return mSlots[slot];
}

inline uint64_t LumenRadianceCache::getProbeKey(uint32_t slot) const
{
    if (slot == kInvalidProbeSlot || slot >= mSlots.size() || !mSlots[slot].allocated)
    {
        return 0;
    }
    const RadianceCacheProbeSlot& s = mSlots[slot];
    return makeProbeKey(s.level, s.cell, s.generation);
}

inline float LumenRadianceCache::refreshScore(const RadianceCacheProbeSlot& s) const
{
    const float dw = distanceWeight(s.level, probeWorldPosition(s.level, s.cell));
    const uint64_t rawAge = s.confidence <= 0.f ? kRadianceCacheMaxAgeFrames
                                                : std::min<uint64_t>(
                                                      std::max<uint64_t>(1u, mFrameIndex - s.lastUpdateFrame),
                                                      kRadianceCacheMaxAgeFrames);
    return s.visibilityWeight * dw * static_cast<float>(rawAge);
}

inline const std::vector<LumenRadianceCache::RadianceCacheRefreshRequest>& LumenRadianceCache::tick()
{
    ++mFrameIndex;
    mRefreshRequests.clear();

    struct Candidate
    {
        uint32_t slot = kInvalidProbeSlot;
        uint32_t level = 0;
        index3 cell;
        float score = 0.f;
        bool allocated = false;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<size_t>(mLevelCount) * mResolution * mResolution * mResolution);

    // 1a. Allocated slots, scored by the frozen refresh formula.
    for (size_t i = 1; i < mSlots.size(); ++i)
    {
        const RadianceCacheProbeSlot& s = mSlots[i];
        if (!s.allocated)
        {
            continue;
        }
        Candidate c;
        c.slot = static_cast<uint32_t>(i);
        c.level = s.level;
        c.cell = s.cell;
        c.score = refreshScore(s);
        c.allocated = true;
        candidates.push_back(c);
    }

    // 1b. Empty clipmap cells (never-updated by definition): visibility 1, max age, so a
    //     near empty cell always outranks a stale far probe at the same distance.
    for (uint32_t m = 0; m < mLevelCount; ++m)
    {
        const int32_t R = static_cast<int32_t>(mResolution);
        for (int32_t z = 0; z < R; ++z)
        {
            for (int32_t y = 0; y < R; ++y)
            {
                for (int32_t x = 0; x < R; ++x)
                {
                    const index3 cell(x, y, z);
                    if (mCellToSlot.find(makeCellKey(m, cell)) != mCellToSlot.end())
                    {
                        continue;
                    }
                    Candidate c;
                    c.slot = kInvalidProbeSlot;
                    c.level = m;
                    c.cell = cell;
                    c.score = distanceWeight(m, probeWorldPosition(m, cell)) * static_cast<float>(kRadianceCacheMaxAgeFrames);
                    c.allocated = false;
                    candidates.push_back(c);
                }
            }
        }
    }

    // 2. Deterministic sort: score desc, then level/cell/slot asc.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
              {
                  if (a.score != b.score) return a.score > b.score;
                  if (a.level != b.level) return a.level < b.level;
                  if (a.cell.x != b.cell.x) return a.cell.x < b.cell.x;
                  if (a.cell.y != b.cell.y) return a.cell.y < b.cell.y;
                  if (a.cell.z != b.cell.z) return a.cell.z < b.cell.z;
                  return a.slot < b.slot;
              });

    // 3. Take the first mRefreshBudgetPerFrame candidates; allocate slots for empty cells.
    const size_t n = std::min(candidates.size(), static_cast<size_t>(mRefreshBudgetPerFrame));
    for (size_t i = 0; i < n; ++i)
    {
        Candidate& c = candidates[i];
        uint32_t slot = c.slot;
        bool isNew = false;
        if (!c.allocated)
        {
            slot = allocateSlot(c.level, c.cell);
            if (slot == kInvalidProbeSlot)
            {
                // Pool full and nothing evictable; skip this cell this frame.
                continue;
            }
            isNew = true;
        }
        // Guard: an allocated candidate may have been LRU-evicted by a later allocation
        // in this same pass (the pool only evicts candidates not already queued below, but
        // an earlier allocation could still have freed a candidate further down the list).
        if (!mSlots[slot].allocated)
        {
            continue;
        }
        RadianceCacheRefreshRequest req;
        req.slot = slot;
        req.level = c.level;
        req.cell = c.cell;
        req.key = makeProbeKey(c.level, c.cell, mSlots[slot].generation);
        req.isNewSlot = isNew;
        mRefreshRequests.push_back(req);
    }
    mRefreshCount += mRefreshRequests.size();
    return mRefreshRequests;
}

inline LumenRadianceCache::RadianceCacheQueryResult LumenRadianceCache::query(const float3& worldPos) const
{
    RadianceCacheQueryResult result;
    result.level = levelIndexForWorld(worldPos);

    index3 cell;
    float3 frac;
    if (!worldToCell(result.level, worldPos, cell, frac))
    {
        // Point beyond every footprint (fallback level, out of bounds): invalid.
        return result;
    }
    result.cell = cell;
    result.frac = frac;

    const int32_t R = static_cast<int32_t>(mResolution);
    float w[8];
    trilinearWeights(frac, w);

    float wx = 0.f, wy = 0.f, wz = 0.f;
    float weightSum = 0.f;
    float confSum = 0.f;
    bool anyExpired = false;

    for (int i = 0; i < 8; ++i)
    {
        const index3 corner(cell.x + ((i >> 2) & 1), cell.y + ((i >> 1) & 1), cell.z + (i & 1));
        result.cornerCells[i] = corner;
        if (corner.x < 0 || corner.x >= R || corner.y < 0 || corner.y >= R || corner.z < 0 || corner.z >= R)
        {
            continue; // Corner outside the clipmap footprint: not a contributor.
        }
        const auto it = mCellToSlot.find(makeCellKey(result.level, corner));
        if (it == mCellToSlot.end())
        {
            continue; // Corner cell not resident yet.
        }
        const uint32_t slot = it->second;
        const RadianceCacheProbeSlot& s = mSlots[slot];
        if (!s.allocated)
        {
            continue; // Defensive: the map must always point at an allocated slot.
        }
        result.cornerSlots[i] = slot;
        result.cornerKeys[i] = makeProbeKey(result.level, corner, s.generation);
        result.cornerFresh[i] = isProbeFresh(slot);
        if (!result.cornerFresh[i])
        {
            anyExpired = true;
        }
        ++result.allocatedCornerCount;
        const float wi = w[i];
        wx += wi * s.radiance[0];
        wy += wi * s.radiance[1];
        wz += wi * s.radiance[2];
        weightSum += wi;
        confSum += wi * s.confidence;
    }

    if (result.allocatedCornerCount == 0 || weightSum <= 0.f)
    {
        return result; // No resident corner: valid=false, expired=true, radiance zeroed.
    }

    // Re-normalize over the resident corners only (missing corners contribute nothing).
    const float inv = 1.f / weightSum;
    result.radiance[0] = wx * inv;
    result.radiance[1] = wy * inv;
    result.radiance[2] = wz * inv;
    result.confidence = confSum * inv;
    result.valid = true;
    result.fresh = !anyExpired;
    result.expired = anyExpired;
    ++mQueryCount;
    return result;
}

inline uint64_t LumenRadianceCache::estimateMemoryBytes(uint64_t bytesPerProbeSlot, uint64_t bytesPerCellMeta) const
{
    uint64_t cells = 0;
    for (uint32_t m = 0; m < mLevelCount; ++m)
    {
        cells += static_cast<uint64_t>(mResolution) * mResolution * mResolution;
    }
    uint64_t allocatedSlots = 0;
    for (const RadianceCacheProbeSlot& s : mSlots)
    {
        if (s.allocated)
        {
            ++allocatedSlots;
        }
    }
    return allocatedSlots * bytesPerProbeSlot + cells * bytesPerCellMeta;
}

inline bool LumenRadianceCache::dropFarthestStaticLevel()
{
    if (mLevelCount <= kRadianceCacheDynamicLevels)
    {
        return false;
    }
    const uint32_t dropLevel = mLevelCount - 1u; // Coarsest (largest-extent) static level.
    --mLevelCount;
    for (size_t i = 1; i < mSlots.size(); ++i)
    {
        if (mSlots[i].allocated && mSlots[i].level == dropLevel)
        {
            freeSlot(static_cast<uint32_t>(i));
        }
    }
    mLevels.pop_back();
    ++mDropCount;
    return true;
}

inline bool LumenRadianceCache::enforceBudget(uint64_t budgetBytes)
{
    mMemoryBudgetBytes = budgetBytes;
    return enforceBudget();
}

inline bool LumenRadianceCache::enforceBudget()
{
    if (mMemoryBudgetBytes == 0)
    {
        return true;
    }
    while (estimateMemoryBytes() > mMemoryBudgetBytes && dropFarthestStaticLevel())
    {
    }
    return estimateMemoryBytes() <= mMemoryBudgetBytes;
}

inline LumenRadianceCache::RadianceCacheStats LumenRadianceCache::getStats() const
{
    RadianceCacheStats stats;
    stats.levelCount = mLevelCount;
    stats.resolution = mResolution;
    stats.maxSlots = getMaxSlots();
    stats.refreshBudgetPerFrame = mRefreshBudgetPerFrame;
    stats.frameIndex = mFrameIndex;
    stats.memoryBudgetBytes = mMemoryBudgetBytes;
    stats.estimateMemoryBytes = estimateMemoryBytes();
    stats.allocationCount = mAllocationCount;
    stats.evictionCount = mEvictionCount;
    stats.releaseCount = mReleaseCount;
    stats.updateCount = mUpdateCount;
    stats.dropCount = mDropCount;
    stats.queryCount = mQueryCount;
    stats.refreshCount = mRefreshCount;
    stats.lastRefreshCount = static_cast<uint32_t>(mRefreshRequests.size());

    uint32_t allocated = 0;
    for (const RadianceCacheProbeSlot& s : mSlots)
    {
        if (s.allocated)
        {
            ++allocated;
        }
    }
    stats.allocatedSlotCount = allocated;
    stats.freeSlotCount = static_cast<uint32_t>(mFreeList.size());

    uint64_t cells = 0;
    for (uint32_t m = 0; m < mLevelCount; ++m)
    {
        cells += static_cast<uint64_t>(mResolution) * mResolution * mResolution;
    }
    const uint64_t allocatedCells = static_cast<uint64_t>(allocated);
    stats.emptyCellCount = cells > allocatedCells ? static_cast<uint32_t>(cells - allocatedCells) : 0u;
    return stats;
}

inline void LumenRadianceCache::reset()
{
    const size_t slotCount = mSlots.size();
    mSlots.assign(slotCount, RadianceCacheProbeSlot{});
    mCellToSlot.clear();
    mFreeList.clear();
    for (uint32_t slotID = static_cast<uint32_t>(slotCount - 1); slotID >= 1; --slotID)
    {
        mFreeList.push_back(slotID);
    }
    mRefreshRequests.clear();
    mCenterVoxel[0] = mCenterVoxel[1] = mCenterVoxel[2] = 0;
    mCameraCenter = float3(0.f, 0.f, 0.f);
    mScroll = index3();
    mFrameIndex = 0;
    mAllocationCount = 0;
    mEvictionCount = 0;
    mReleaseCount = 0;
    mUpdateCount = 0;
    mDropCount = 0;
    mRefreshCount = 0;
    mQueryCount = 0;
}

// -------------------------------------------------------------------------------------
// Private helpers
// -------------------------------------------------------------------------------------

inline bool LumenRadianceCache::insideFootprint(const float3& center, const LumenRadianceCacheLevel& lvl, const float3& p)
{
    const float half = lvl.worldExtent * 0.5f;
    return std::fabs(p.x - center.x) <= half && std::fabs(p.y - center.y) <= half && std::fabs(p.z - center.z) <= half;
}

inline int32_t LumenRadianceCache::snapVoxel(float v)
{
    const float a = std::fabs(v);
    int32_t n = static_cast<int32_t>(std::floor(a + 0.5f));
    // Exact .5 boundary (2a == 2n-1 in float): round toward zero.
    if (2.0f * a == 2.0f * static_cast<float>(n) - 1.0f)
    {
        --n;
    }
    return v < 0.f ? -n : n;
}

inline float LumenRadianceCache::distanceWeight(uint32_t level, const float3& probeWorld) const
{
    const LumenRadianceCacheLevel& lvl = mLevels[clampLevel(level)];
    const float radius = lvl.worldExtent * 0.5f;
    const float dx = probeWorld.x - mCameraCenter.x;
    const float dy = probeWorld.y - mCameraCenter.y;
    const float dz = probeWorld.z - mCameraCenter.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    // 1/(1 + dist/radius): camera-probe -> 1, footprint edge -> 0.5, far -> asymptotic 0
    // (never exactly 0, so far probes still eventually refresh).
    return 1.0f / (1.0f + dist / radius);
}

inline uint32_t LumenRadianceCache::allocateSlot(uint32_t level, const index3& cell)
{
    const uint64_t ck = makeCellKey(level, cell);
    const auto it = mCellToSlot.find(ck);
    if (it != mCellToSlot.end())
    {
        return it->second;
    }

    uint32_t slot = kInvalidProbeSlot;
    if (!mFreeList.empty())
    {
        slot = mFreeList.back();
        mFreeList.pop_back();
    }
    else
    {
        const uint32_t victim = findEvictionCandidate();
        if (victim == kInvalidProbeSlot)
        {
            return kInvalidProbeSlot;
        }
        evictSlot(victim);
        slot = victim;
    }

    RadianceCacheProbeSlot& s = mSlots[slot];
    // Generation saturates at the 16-bit key range so keys never wrap/alias.
    if (s.generation < 0xFFFFu)
    {
        ++s.generation;
    }
    s.radiance[0] = s.radiance[1] = s.radiance[2] = 0.f;
    s.directionEncoding = kRadianceCacheInvalidDirectionEncoding;
    s.confidence = 0.f;
    s.lastUpdateFrame = 0;
    s.level = level;
    s.cell = cell;
    s.visibilityWeight = 1.f;
    s.allocated = true;
    mCellToSlot[ck] = slot;
    ++mAllocationCount;
    return slot;
}

inline void LumenRadianceCache::reparentLevel0(const index3& scroll)
{
    const int32_t R = static_cast<int32_t>(mResolution);
    // cell_new = cell_old - scroll (the grid moved +scroll in index space, so content that
    // stays inside the footprint shifts by -scroll). The mapping is bijective within the
    // footprint, so re-keyed cells never collide.
    for (size_t i = 1; i < mSlots.size(); ++i)
    {
        RadianceCacheProbeSlot& s = mSlots[i];
        if (!s.allocated || s.level != 0)
        {
            continue;
        }
        const int32_t nx = s.cell.x - scroll.x;
        const int32_t ny = s.cell.y - scroll.y;
        const int32_t nz = s.cell.z - scroll.z;
        if (nx >= 0 && nx < R && ny >= 0 && ny < R && nz >= 0 && nz < R)
        {
            mCellToSlot.erase(makeCellKey(0, s.cell));
            s.cell = index3(nx, ny, nz);
            mCellToSlot[makeCellKey(0, s.cell)] = static_cast<uint32_t>(i);
        }
        else
        {
            // Content scrolled out of the footprint: drop it, the refresh schedule
            // re-populates the newly exposed slabs.
            freeSlot(static_cast<uint32_t>(i));
        }
    }
}

inline void LumenRadianceCache::freeSlot(uint32_t slot)
{
    RadianceCacheProbeSlot& s = mSlots[slot];
    mCellToSlot.erase(makeCellKey(s.level, s.cell));
    s.allocated = false;
    mFreeList.push_back(slot);
}

inline uint32_t LumenRadianceCache::findEvictionCandidate() const
{
    uint32_t best = kInvalidProbeSlot;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (size_t i = 1; i < mSlots.size(); ++i)
    {
        const RadianceCacheProbeSlot& s = mSlots[i];
        if (!s.allocated)
        {
            continue;
        }
        // Min residency: probes updated within the last mMinResidencyFrames stay put
        // (the GPU may still be reading them). Never-updated slots (confidence 0) are
        // always evictable.
        if (s.confidence > 0.f && mFrameIndex - s.lastUpdateFrame < mMinResidencyFrames)
        {
            continue;
        }
        // Never evict a slot already queued for refresh by the current tick().
        bool pendingRefresh = false;
        for (const RadianceCacheRefreshRequest& r : mRefreshRequests)
        {
            if (r.slot == static_cast<uint32_t>(i))
            {
                pendingRefresh = true;
                break;
            }
        }
        if (pendingRefresh)
        {
            continue;
        }
        // Strict '<' keeps the smallest slot ID as the tie-break (deterministic).
        if (s.lastUpdateFrame < oldest)
        {
            oldest = s.lastUpdateFrame;
            best = static_cast<uint32_t>(i);
        }
    }
    return best;
}

inline void LumenRadianceCache::evictSlot(uint32_t slot)
{
    freeSlot(slot);
    ++mEvictionCount;
}

inline void LumenRadianceCache::trilinearWeights(const float3& frac, float out[8])
{
    // w[(i<<2)|(j<<1)|k] = (i ? fx : 1-fx) * (j ? fy : 1-fy) * (k ? fz : 1-fz)
    const float fx = frac.x, fy = frac.y, fz = frac.z;
    const float x0 = 1.f - fx, x1 = fx;
    const float y0 = 1.f - fy, y1 = fy;
    const float z0 = 1.f - fz, z1 = fz;
    out[0] = x0 * y0 * z0; // 000
    out[1] = x0 * y0 * z1; // 001
    out[2] = x0 * y1 * z0; // 010
    out[3] = x0 * y1 * z1; // 011
    out[4] = x1 * y0 * z0; // 100
    out[5] = x1 * y0 * z1; // 101
    out[6] = x1 * y1 * z0; // 110
    out[7] = x1 * y1 * z1; // 111
}

} // namespace Falcor
