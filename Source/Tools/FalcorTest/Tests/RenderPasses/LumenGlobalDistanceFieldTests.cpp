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
 # PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/
#include "Testing/UnitTest.h"
#include "../../../../RenderPasses/LumenGI/MeshSDF/LumenGlobalDistanceField.h"

#include <cmath>
#include <cstdint>

namespace Falcor
{
namespace
{

namespace gdf = LumenGI::GlobalDistanceField;

// -----------------------------------------------------------------------------
// Test harness parameters (all binary-exact in float, so every asserted index is
// exact): base = 100 m, 4 levels, 128 voxels/side.
//   extent_m:    100, 200, 400, 800
//   voxelSize_m: 100/128 = 0.78125, 1.5625, 3.125, 6.25
// -----------------------------------------------------------------------------
constexpr float kBaseExtent = 100.f;
constexpr uint32_t kLevelCount = 4;
constexpr uint32_t kResolution = 128;
constexpr uint64_t kVoxelsPerLevel = 128ull * 128ull * 128ull; // 2,097,152
constexpr float kLevel0Voxel = 100.f / 128.f;

// -----------------------------------------------------------------------------
// Small assert helpers (EXPECT_* reference `ctx`, so they are passed explicitly).
// -----------------------------------------------------------------------------

void expectNear(CPUUnitTestContext& ctx, float a, float b, float eps = 1e-4f)
{
    EXPECT_LE(std::fabs(a - b), eps);
}

void expectIndex(CPUUnitTestContext& ctx, const gdf::index3& actual, int32_t x, int32_t y, int32_t z)
{
    EXPECT_EQ(actual.x, x);
    EXPECT_EQ(actual.y, y);
    EXPECT_EQ(actual.z, z);
}

bool vecNear(const gdf::float3& a, const float3& b, float eps = 1e-4f)
{
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

/// True when `r` is a slab of thickness [lo, hi] on `axis`, spanning the full level on
/// the other two axes (inclusive ranges).
bool isSlab(const gdf::VoxelRange& r, int axis, bool added, int32_t lo, int32_t hi, int32_t R)
{
    if (r.axis != axis || r.added != added)
        return false;
    const int32_t cLo = axis == 0 ? r.min.x : (axis == 1 ? r.min.y : r.min.z);
    const int32_t cHi = axis == 0 ? r.max.x : (axis == 1 ? r.max.y : r.max.z);
    if (cLo != lo || cHi != hi)
        return false;
    for (int a2 = 0; a2 < 3; ++a2)
    {
        if (a2 == axis)
            continue;
        const int32_t mn = a2 == 0 ? r.min.x : (a2 == 1 ? r.min.y : r.min.z);
        const int32_t mx = a2 == 0 ? r.max.x : (a2 == 1 ? r.max.y : r.max.z);
        if (mn != 0 || mx != R - 1)
            return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Level geometry: extents double per level, voxel sizes scale, resolution shared,
// frozen struct default, memory estimate = sum(resolution^3 * bytesPerVoxel).
// -----------------------------------------------------------------------------
CPU_TEST(LumenGDF_LevelGeometryExtentsAndMemory)
{
    gdf::LumenGlobalDistanceField gdf(kBaseExtent, kLevelCount, kResolution);

    EXPECT_EQ(gdf.getLevelCount(), kLevelCount);
    EXPECT_EQ(gdf.getResolution(), kResolution);
    EXPECT_EQ(gdf.getBaseExtent(), kBaseExtent);

    // Frozen struct default.
    const gdf::LumenGDFLevel def = gdf::LumenGDFLevel{};
    EXPECT_EQ(def.resolution, 128u);

    // extent_m == base * 2^m and voxelSize_m == extent_m / resolution.
    float expectedExtent = kBaseExtent;
    for (uint32_t m = 0; m < kLevelCount; ++m)
    {
        const gdf::LumenGDFLevel& lvl = gdf.getLevel(m);
        EXPECT_EQ(lvl.resolution, kResolution);
        expectNear(ctx, lvl.worldExtent, expectedExtent, 1e-3f);
        expectNear(ctx, lvl.voxelSize, expectedExtent / kResolution, 1e-4f);
        expectedExtent *= 2.f;
    }
    expectNear(ctx, gdf.getLevel(0).voxelSize, kLevel0Voxel, 1e-5f);
    expectNear(ctx, gdf.getLevel(3).voxelSize, 800.f / 128.f, 1e-4f);

    // Memory estimate = levelCount * resolution^3 * bytesPerVoxel.
    EXPECT_EQ(gdf.estimateMemoryBytes(2.f), kVoxelsPerLevel * kLevelCount * 2ull);
    EXPECT_EQ(gdf.estimateMemoryBytes(1.f), kVoxelsPerLevel * kLevelCount);
    // Independent of camera state.
    gdf.setCamera(gdf::float3(123.4f, -56.7f, 89.0f));
    EXPECT_EQ(gdf.estimateMemoryBytes(2.f), kVoxelsPerLevel * kLevelCount * 2ull);
}

// -----------------------------------------------------------------------------
// Camera snapping: round-half-toward-zero, negative coordinates, half-voxel
// threshold, and the "1.5-voxel move -> scroll +/-1 or +/-2" rule.
// -----------------------------------------------------------------------------
CPU_TEST(LumenGDF_CameraSnapAndNegativeCoordinates)
{
    gdf::LumenGlobalDistanceField gdf(kBaseExtent, kLevelCount, kResolution);
    const float vs = kLevel0Voxel; // 0.78125

    gdf.setCamera(gdf::float3(0.f, 0.f, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(0, 0, 0));

    // Moves below/at half a voxel produce no scroll (nonzero only when EXCEEDING half).
    gdf.setCamera(gdf::float3(0.4f * vs, 0.f, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(0, 0, 0));
    gdf.setCamera(gdf::float3(0.5f * vs, 0.f, 0.f)); // exactly half -> no scroll
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(0, 0, 0));

    // 0.6 voxel -> +1 (exceeds half).
    gdf.setCamera(gdf::float3(0.6f * vs, 0.f, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(1, 0, 0));
    // Snapped center is an integer multiple of the level-0 voxel size.
    expectNear(ctx, gdf.getCameraCenter().x, 1.f * vs, 1e-5f);

    // Moving back to the origin scrolls -1.
    gdf.setCamera(gdf::float3(0.f, 0.f, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(-1, 0, 0));

    // Negative coordinates: -2.5 voxels snaps to -2 (half toward zero), center = -2*vs.
    gdf.setCamera(gdf::float3(-2.5f * vs, 0.f, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(-2, 0, 0));
    expectNear(ctx, gdf.getCameraCenter().x, -2.f * vs, 1e-5f);

    // A move of -1.5 voxels from the origin snaps to -1.
    gdf::LumenGlobalDistanceField neg(kBaseExtent, kLevelCount, kResolution);
    neg.setCamera(gdf::float3(0.f, 0.f, 0.f));
    neg.setCamera(gdf::float3(-1.5f * vs, 0.f, 0.f));
    EXPECT_TRUE(neg.scrollFromCameraMove() == gdf::index3(-1, 0, 0));

    // 1.5-voxel rule from an INTEGER phase -> +/-1 (round-half-toward-zero).
    gdf::LumenGlobalDistanceField phaseInt(kBaseExtent, kLevelCount, kResolution);
    phaseInt.setCamera(gdf::float3(0.f, 0.f, 0.f));
    phaseInt.setCamera(gdf::float3(1.5f * vs, 0.f, 0.f));
    const int32_t scrollInt = phaseInt.scrollFromCameraMove().x;
    EXPECT_TRUE(scrollInt == 1 || scrollInt == 2);
    EXPECT_EQ(scrollInt, 1);

    // 1.5-voxel rule from a HALF phase -> +/-2 (the extra half carries into the next voxel).
    gdf::LumenGlobalDistanceField phaseHalf(kBaseExtent, kLevelCount, kResolution);
    phaseHalf.setCamera(gdf::float3(0.5f * vs, 0.f, 0.f)); // snaps to 0, no scroll
    EXPECT_TRUE(phaseHalf.scrollFromCameraMove() == gdf::index3(0, 0, 0));
    phaseHalf.setCamera(gdf::float3(2.f * vs, 0.f, 0.f)); // 2.0 - 0.5 phase -> +2
    const int32_t scrollHalf = phaseHalf.scrollFromCameraMove().x;
    EXPECT_TRUE(scrollHalf == 1 || scrollHalf == 2);
    EXPECT_EQ(scrollHalf, 2);
}

// -----------------------------------------------------------------------------
// worldToIndex / indexToWorld: camera-centered mapping, floor for negatives,
// clamping/bounds, exact round-trips, and cross-level lattice alignment.
// -----------------------------------------------------------------------------
CPU_TEST(LumenGDF_WorldToIndexMapping)
{
    gdf::LumenGlobalDistanceField gdf(kBaseExtent, kLevelCount, kResolution);
    const float vs = kLevel0Voxel;
    // Camera at the origin -> level-0 gridMin = -64 * vs = -50.
    gdf.setCamera(gdf::float3(0.f, 0.f, 0.f));

    {
        gdf::index3 idx;
        bool inBounds = false;
        gdf.worldToIndex(0, gdf::float3(0.f, 0.f, 0.f), idx, inBounds);
        expectIndex(ctx, idx, 64, 64, 64);
        EXPECT_TRUE(inBounds);
    }
    {
        // Negative world coordinate that is still inside the grid maps via floor.
        gdf::index3 idx;
        bool inBounds = false;
        gdf.worldToIndex(0, gdf::float3(-25.f, 0.f, 0.f), idx, inBounds);
        expectIndex(ctx, idx, 32, 64, 64);
        EXPECT_TRUE(inBounds);
    }
    {
        // Just below gridMin floors to -1 -> out of bounds.
        gdf::index3 idx;
        bool inBounds = true;
        gdf.worldToIndex(0, gdf::float3(-50.f - vs, 0.f, 0.f), idx, inBounds);
        EXPECT_EQ(idx.x, -1);
        EXPECT_FALSE(inBounds);
    }
    {
        // Exactly at gridMin + R*vs -> index R -> out of bounds (upper clamp).
        gdf::index3 idx;
        bool inBounds = true;
        gdf.worldToIndex(0, gdf::float3(50.f, 0.f, 0.f), idx, inBounds);
        EXPECT_EQ(idx.x, static_cast<int32_t>(kResolution));
        EXPECT_FALSE(inBounds);
    }

    // Round-trip: index -> world center -> same index, in bounds.
    const int32_t samples[] = {0, 10, 64, 100, 127};
    for (int32_t i : samples)
    {
        const gdf::float3 w = gdf.indexToWorld(0, gdf::index3(i, i, i));
        gdf::index3 idx;
        bool inBounds = false;
        gdf.worldToIndex(0, w, idx, inBounds);
        expectIndex(ctx, idx, i, i, i);
        EXPECT_TRUE(inBounds);
    }

    // Cross-level lattice alignment: a static-level voxel CORNER (gridMin_m + i*vsm)
    // is also a level-0 voxel corner. For level m and index i the level-0 index of that
    // corner is 2^m * (i - 64) + 64. Level 1, i=48 -> corner at -25 -> level-0 index 32.
    {
        gdf::index3 idx;
        bool inBounds = false;
        gdf.worldToIndex(0, gdf::float3(-25.f, -25.f, -25.f), idx, inBounds);
        expectIndex(ctx, idx, 32, 32, 32);
        EXPECT_TRUE(inBounds);
    }
    // Level 2, i=72 -> corner at +25 -> level-0 index 96.
    {
        gdf::index3 idx;
        bool inBounds = false;
        gdf.worldToIndex(0, gdf::float3(25.f, 25.f, 25.f), idx, inBounds);
        expectIndex(ctx, idx, 96, 96, 96);
        EXPECT_TRUE(inBounds);
    }
    // Level 3, i=48 -> corner at -100, which is outside the level-0 footprint (-64).
    {
        gdf::index3 idx;
        bool inBounds = true;
        gdf.worldToIndex(0, gdf::float3(-100.f, 0.f, 0.f), idx, inBounds);
        EXPECT_EQ(idx.x, -64);
        EXPECT_FALSE(inBounds);
    }

    // Static levels are origin-anchored: the same lattice-point logic holds on them.
    {
        gdf::index3 idx;
        bool inBounds = false;
        gdf.worldToIndex(1, gdf::float3(0.f, 0.f, 0.f), idx, inBounds); // origin is a corner
        EXPECT_EQ(idx.x, 64);
        EXPECT_TRUE(inBounds);
        EXPECT_EQ(idx.y, 64);
        EXPECT_EQ(idx.z, 64);
    }
}

// -----------------------------------------------------------------------------
// Scroll bookkeeping: scroll magnitudes, dirty = newly added slab per scrolled
// axis, removed = complement, static levels never dirty, isVoxelResident semantics.
// -----------------------------------------------------------------------------
CPU_TEST(LumenGDF_ScrollDirtyAndResident)
{
    gdf::LumenGlobalDistanceField gdf(kBaseExtent, kLevelCount, kResolution);
    const float vs = kLevel0Voxel;
    const int32_t R = static_cast<int32_t>(kResolution);

    gdf.setCamera(gdf::float3(0.f, 0.f, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(0, 0, 0));
    EXPECT_TRUE(gdf.dirtyRegions(0).empty());
    EXPECT_TRUE(gdf.removedRegions(0).empty());

    // +2 voxels on X: fresh slab at the high end [126,127].
    gdf.setCamera(gdf::float3(2.f * vs, 0.f, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(2, 0, 0));
    {
        const auto dirty = gdf.dirtyRegions(0);
        const auto removed = gdf.removedRegions(0);
        EXPECT_EQ(dirty.size(), 1u);
        EXPECT_EQ(removed.size(), 1u);
        EXPECT_TRUE(isSlab(dirty[0], 0, true, R - 2, R - 1, R));
        EXPECT_TRUE(isSlab(removed[0], 0, false, 0, 1, R));
    }

    // isVoxelResident == "inside the dirty (pending-update) zone".
    EXPECT_TRUE(gdf.isVoxelResident(0, gdf::index3(R - 2, 0, 0)));
    EXPECT_TRUE(gdf.isVoxelResident(0, gdf::index3(R - 1, R - 1, R - 1)));
    EXPECT_FALSE(gdf.isVoxelResident(0, gdf::index3(R - 3, 0, 0)));
    EXPECT_FALSE(gdf.isVoxelResident(0, gdf::index3(0, 0, 0)));       // removed, not dirty
    EXPECT_FALSE(gdf.isVoxelResident(0, gdf::index3(R, 0, 0)));       // out of bounds
    EXPECT_FALSE(gdf.isVoxelResident(1, gdf::index3(0, 0, 0)));       // static, never pending

    // Diagonal move: one slab per scrolled axis (+2 X, +3 Y). Reset first so the
    // scroll is measured from the origin (fresh camera, no accumulated offset).
    gdf.setCamera(gdf::float3(0.f, 0.f, 0.f));
    gdf.setCamera(gdf::float3(2.f * vs, 3.f * vs, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(2, 3, 0));
    {
        const auto dirty = gdf.dirtyRegions(0);
        EXPECT_EQ(dirty.size(), 2u);
        EXPECT_TRUE(isSlab(dirty[0], 0, true, R - 2, R - 1, R));
        EXPECT_TRUE(isSlab(dirty[1], 1, true, R - 3, R - 1, R));
        EXPECT_TRUE(gdf.isVoxelResident(0, gdf::index3(R - 2, R - 3, 0)));
        // X=R-3 is outside the X slab [R-2,R-1] AND Y=R-4 is outside the Y slab
        // [R-3,R-1], so this voxel is not pending (OR semantics across axes).
        EXPECT_FALSE(gdf.isVoxelResident(0, gdf::index3(R - 3, R - 4, 0)));
    }

    // Negative scroll (-2 X, -3 Y): fresh slabs enter at the low end.
    gdf.setCamera(gdf::float3(0.f, 0.f, 0.f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(-2, -3, 0));
    {
        const auto dirty = gdf.dirtyRegions(0);
        EXPECT_EQ(dirty.size(), 2u);
        EXPECT_TRUE(isSlab(dirty[0], 0, true, 0, 1, R));
        EXPECT_TRUE(isSlab(dirty[1], 1, true, 0, 2, R));
        const auto removed = gdf.removedRegions(0);
        EXPECT_EQ(removed.size(), 2u);
        EXPECT_TRUE(isSlab(removed[0], 0, false, R - 2, R - 1, R));
        EXPECT_TRUE(isSlab(removed[1], 1, false, R - 3, R - 1, R));
    }

    // Static levels never produce dirty/removed regions regardless of camera motion.
    for (uint32_t m = gdf::kDynamicLevels; m < kLevelCount; ++m)
    {
        EXPECT_TRUE(gdf.dirtyRegions(m).empty());
        EXPECT_TRUE(gdf.removedRegions(m).empty());
        EXPECT_FALSE(gdf.isVoxelResident(m, gdf::index3(64, 64, 64)));
    }
}

// -----------------------------------------------------------------------------
// Level selection: near field -> dynamic level 0, far field -> finest static
// level that contains the point, fallback -> coarsest, boundary included.
// -----------------------------------------------------------------------------
CPU_TEST(LumenGDF_LevelSelection)
{
    gdf::LumenGlobalDistanceField gdf(kBaseExtent, kLevelCount, kResolution);
    gdf.setCamera(gdf::float3(0.f, 0.f, 0.f));

    // Level 0 footprint: maxAbs <= 50 (extent/2 = base/2). Boundary included.
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(0.f, 0.f, 0.f)), 0u);
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(50.f, 0.f, 0.f)), 0u);
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(50.5f, 0.f, 0.f)), 1u);

    // Static far field: finest level whose footprint (maxAbs <= 100/200/400) contains it.
    // Half extents are 50/100/200/400, so 200 is level 2's boundary and 300/400 are level 3.
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(100.f, 0.f, 0.f)), 1u);  // boundary of level 1
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(101.f, 0.f, 0.f)), 2u);
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(200.f, 0.f, 0.f)), 2u);  // boundary of level 2
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(201.f, 0.f, 0.f)), 3u);
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(300.f, 0.f, 0.f)), 3u);
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(400.f, 0.f, 0.f)), 3u);  // boundary of level 3
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(1000.f, 0.f, 0.f)), 3u); // coarsest fallback

    // Near field follows the camera: after moving to x=100, the dynamic level owns
    // points within 50 m of (100, 0, 0).
    gdf.setCamera(gdf::float3(100.f, 0.f, 0.f));
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(80.f, 0.f, 0.f)), 0u);  // 20 m from camera
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(180.f, 0.f, 0.f)), 2u); // 80 m from camera
}

// -----------------------------------------------------------------------------
// Budget: estimate formula, enforceBudget drops the farthest static level, floor at
// the dynamic-only clipmap, and level selection moves to the new coarsest level.
// -----------------------------------------------------------------------------
CPU_TEST(LumenGDF_BudgetDropFarthestStatic)
{
    gdf::LumenGlobalDistanceField gdf(kBaseExtent, kLevelCount, kResolution);
    EXPECT_EQ(gdf.estimateMemoryBytes(2.f), kVoxelsPerLevel * 4ull * 2ull);

    // Budget at the full size: no drop.
    EXPECT_TRUE(gdf.enforceBudget(2.f, kVoxelsPerLevel * 4ull * 2ull));
    EXPECT_EQ(gdf.getLevelCount(), 4u);

    // Budget for 3 levels: the farthest (level 3) is dropped.
    EXPECT_TRUE(gdf.enforceBudget(2.f, kVoxelsPerLevel * 3ull * 2ull));
    EXPECT_EQ(gdf.getLevelCount(), 3u);
    EXPECT_EQ(gdf.estimateMemoryBytes(2.f), kVoxelsPerLevel * 3ull * 2ull);
    // Level selection now clamps at the new coarsest level (level 2).
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(500.f, 0.f, 0.f)), 2u);

    // Explicit drop: farthest static level (level 2) goes next.
    EXPECT_TRUE(gdf.dropFarthestStaticLevel());
    EXPECT_EQ(gdf.getLevelCount(), 2u);
    EXPECT_EQ(gdf.estimateMemoryBytes(2.f), kVoxelsPerLevel * 2ull * 2ull);
    EXPECT_EQ(gdf.levelIndexForWorld(gdf::float3(500.f, 0.f, 0.f)), 1u);

    EXPECT_TRUE(gdf.dropFarthestStaticLevel());
    EXPECT_EQ(gdf.getLevelCount(), 1u);
    EXPECT_EQ(gdf.estimateMemoryBytes(2.f), kVoxelsPerLevel * 1ull * 2ull);

    // Cannot drop below the dynamic level; the clipmap degrades gracefully.
    EXPECT_FALSE(gdf.dropFarthestStaticLevel());
    EXPECT_EQ(gdf.getLevelCount(), 1u);

    // A budget below the dynamic-only estimate cannot be met.
    EXPECT_FALSE(gdf.enforceBudget(2.f, 1ull));
    EXPECT_EQ(gdf.getLevelCount(), 1u);
    EXPECT_EQ(gdf.estimateMemoryBytes(2.f), kVoxelsPerLevel * 2ull);
}

// -----------------------------------------------------------------------------
// Dynamic/static isolation: camera motion scrolls level 0 but never touches the
// static levels' mapping or residency.
// -----------------------------------------------------------------------------
CPU_TEST(LumenGDF_StaticLevelsIndependentOfCamera)
{
    gdf::LumenGlobalDistanceField gdf(kBaseExtent, kLevelCount, kResolution);
    gdf.setCamera(gdf::float3(0.f, 0.f, 0.f));

    const gdf::float3 p(100.f, 100.f, 100.f);
    gdf::index3 staticBefore, staticAfter;
    bool inBefore = false, inAfter = false;
    gdf.worldToIndex(2, p, staticBefore, inBefore);
    expectIndex(ctx, staticBefore, 96, 96, 96); // vc = (100+200)/3.125 = 96
    EXPECT_TRUE(inBefore);

    // Move the camera to a fractional position (scrolled level 0 only).
    gdf.setCamera(gdf::float3(7.5f, -3.5f, 1.25f));
    EXPECT_TRUE(gdf.scrollFromCameraMove() == gdf::index3(10, -4, 2));

    // Static level mapping is byte-identical: anchored at the origin, never scrolls.
    gdf.worldToIndex(2, p, staticAfter, inAfter);
    EXPECT_TRUE(staticBefore == staticAfter);
    EXPECT_EQ(inAfter, inBefore);
    EXPECT_TRUE(gdf.dirtyRegions(2).empty());
    EXPECT_TRUE(gdf.removedRegions(2).empty());
    EXPECT_FALSE(gdf.isVoxelResident(2, gdf::index3(96, 96, 96)));

    // The dynamic level DID change: before the move origin maps to {64,64,64} (camera
    // centered at the origin, gridMin = -50), afterwards to {54,68,62}.
    gdf::LumenGlobalDistanceField dyn(kBaseExtent, kLevelCount, kResolution);
    dyn.setCamera(gdf::float3(0.f, 0.f, 0.f));
    gdf::index3 dynBefore, dynAfter;
    bool dIn = false;
    dyn.worldToIndex(0, gdf::float3(0.f, 0.f, 0.f), dynBefore, dIn);
    expectIndex(ctx, dynBefore, 64, 64, 64);
    dyn.setCamera(gdf::float3(7.5f, -3.5f, 1.25f));
    dyn.worldToIndex(0, gdf::float3(0.f, 0.f, 0.f), dynAfter, dIn);
    expectIndex(ctx, dynAfter, 54, 68, 62);
    EXPECT_FALSE(dynAfter == dynBefore);
}

// -----------------------------------------------------------------------------
// Determinism: two identically-constructed instances driven along the same camera
// path produce identical scrolls, regions, indices, and budgets at every step.
// -----------------------------------------------------------------------------
CPU_TEST(LumenGDF_Determinism)
{
    gdf::LumenGlobalDistanceField a(kBaseExtent, kLevelCount, kResolution);
    gdf::LumenGlobalDistanceField b(kBaseExtent, kLevelCount, kResolution);

    auto fingerprint = [](gdf::LumenGlobalDistanceField& g, const gdf::float3& p) -> uint64_t
    {
        gdf::index3 idx;
        bool inBounds = false;
        g.worldToIndex(0, p, idx, inBounds);
        const gdf::index3 sc = g.scrollFromCameraMove();
        uint64_t h = (uint64_t)(uint32_t)idx.x;
        h = h * 131u + (uint64_t)(uint32_t)idx.y;
        h = h * 131u + (uint64_t)(uint32_t)idx.z;
        h = h * 131u + (uint64_t)(uint32_t)(inBounds ? 1 : 0);
        h = h * 131u + (uint64_t)(uint32_t)sc.x;
        h = h * 131u + (uint64_t)(uint32_t)sc.y;
        h = h * 131u + (uint64_t)(uint32_t)sc.z;
        h = h * 131u + (uint64_t)g.dirtyRegions(0).size();
        h = h * 131u + g.estimateMemoryBytes(2.f);
        return h;
    };

    const gdf::float3 path[] = {
        gdf::float3(0.f, 0.f, 0.f),
        gdf::float3(0.4f * kLevel0Voxel, 0.f, 0.f),
        gdf::float3(1.5f * kLevel0Voxel, -3.f * kLevel0Voxel, 2.f * kLevel0Voxel),
        gdf::float3(7.5f, -3.5f, 1.25f),
        gdf::float3(-2.5f * kLevel0Voxel, 0.f, 1.f),
    };
    const gdf::float3 probes[] = {
        gdf::float3(0.f, 0.f, 0.f),
        gdf::float3(25.f, -25.f, 25.f),
        gdf::float3(300.f, 0.f, 0.f),
    };

    for (const auto& cam : path)
    {
        a.setCamera(cam);
        b.setCamera(cam);
        for (const auto& probe : probes)
        {
            EXPECT_EQ(fingerprint(a, probe), fingerprint(b, probe));
        }
        EXPECT_TRUE(a.scrollFromCameraMove() == b.scrollFromCameraMove());
    }

    // Budget degradation is deterministic too.
    EXPECT_TRUE(a.enforceBudget(2.f, kVoxelsPerLevel * 2ull * 2ull));
    EXPECT_TRUE(b.enforceBudget(2.f, kVoxelsPerLevel * 2ull * 2ull));
    EXPECT_EQ(a.getLevelCount(), b.getLevelCount());
    EXPECT_EQ(a.estimateMemoryBytes(2.f), b.estimateMemoryBytes(2.f));
    EXPECT_EQ(a.levelIndexForWorld(gdf::float3(500.f, 0.f, 0.f)), b.levelIndexForWorld(gdf::float3(500.f, 0.f, 0.f)));
}

} // namespace
} // namespace Falcor
