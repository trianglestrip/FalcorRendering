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
#include "../../../../RenderPasses/LumenGI/RadianceCache/LumenRadianceCache.h"

#include <cmath>
#include <cstdint>
#include <vector>

// CPU tests for the world-space Radiance Cache clipmap host (Wave S7-A1). The
// class is pure CPU (no GPU/Device dependency), so every contract below is
// testable here: frozen constants, clipmap geometry, probe key / direction
// encoders, the slot pool (free-list + LRU), refresh scheduling, trilinear
// queries, budget degradation and camera scrolling -- all fully deterministic.

namespace Falcor
{

CPU_TEST(LumenRadianceCache_Constants)
{
    EXPECT_EQ(kRadianceCacheDynamicLevels, 1u);
    EXPECT_EQ(kRadianceCacheMaxLevels, 16u);
    EXPECT_EQ(kRadianceCacheDefaultLevelCount, 6u);
    EXPECT_EQ(kRadianceCacheDefaultBaseExtentMeters, 4.0f);
    EXPECT_EQ(kRadianceCacheDefaultResolution, 8u);
    EXPECT_EQ(kRadianceCacheMaxResolution, 1024u);
    EXPECT_EQ(kRadianceCacheDefaultRefreshBudgetPerFrame, 64u);
    EXPECT_EQ(kRadianceCacheDefaultMaxSlots, 4096u);
    EXPECT_EQ(kRadianceCacheDefaultMinResidencyFrames, 2u);
    EXPECT_EQ(kRadianceCacheBytesPerProbeSlot, 32u);
    EXPECT_EQ(kRadianceCacheBytesPerCellMeta, 16u);
    EXPECT_EQ(kInvalidProbeSlot, 0u);
    EXPECT_EQ(kRadianceCacheInvalidDirectionEncoding, 0xFFFFFFFFu);
    EXPECT_EQ(kRadianceCacheFirstUpdateConfidence, 0.25f);
    EXPECT_GE(kRadianceCacheFreshnessWindowFrames, 1u);
    EXPECT_GE(kRadianceCacheMaxAgeFrames, kRadianceCacheFreshnessWindowFrames);
}

CPU_TEST(LumenRadianceCache_ConstructionDefaults)
{
    LumenRadianceCache cache;
    EXPECT_EQ(cache.getLevelCount(), 6u);
    EXPECT_EQ(cache.getResolution(), 8u);
    EXPECT_EQ(cache.getBaseExtent(), 4.0f);
    EXPECT_EQ(cache.getMaxSlots(), 4096u);
    EXPECT_EQ(cache.getRefreshBudgetPerFrame(), 64u);
    EXPECT_EQ(cache.getMinResidencyFrames(), 2u);
    EXPECT_EQ(cache.getMemoryBudgetBytes(), 0ull);
    EXPECT_EQ(cache.getFrameIndex(), 0ull);

    const LumenRadianceCache::RadianceCacheStats stats = cache.getStats();
    EXPECT_EQ(stats.levelCount, 6u);
    EXPECT_EQ(stats.resolution, 8u);
    EXPECT_EQ(stats.maxSlots, 4096u);
    EXPECT_EQ(stats.allocatedSlotCount, 0u);
    EXPECT_EQ(stats.freeSlotCount, 4096u);
    EXPECT_EQ(stats.emptyCellCount, 6u * 8u * 8u * 8u);
    EXPECT_EQ(stats.allocationCount, 0ull);
    EXPECT_EQ(stats.evictionCount, 0ull);

    // Camera starts snapped at the world origin.
    EXPECT_EQ(cache.getCameraCenter().x, 0.0f);
    EXPECT_EQ(cache.getCameraCenter().y, 0.0f);
    EXPECT_EQ(cache.getCameraCenter().z, 0.0f);
}

CPU_TEST(LumenRadianceCache_ConstructionClampsInvalidInputs)
{
    // All-zero inputs are clamped to the minimum viable values.
    LumenRadianceCache clamped(0.f, 0, 0, 0, 0, 0);
    EXPECT_EQ(clamped.getBaseExtent(), 4.0f); // <= 0 falls back to the default.
    EXPECT_EQ(clamped.getLevelCount(), 1u);   // clamped up to kRadianceCacheDynamicLevels.
    EXPECT_EQ(clamped.getResolution(), 1u);
    EXPECT_EQ(clamped.getMaxSlots(), 1u);
    EXPECT_EQ(clamped.getRefreshBudgetPerFrame(), 1u);
    EXPECT_EQ(clamped.getMinResidencyFrames(), 1u);

    // Oversized level counts clamp to the hard cap.
    LumenRadianceCache maxed(4.f, 100, 8, 8, 8, 8);
    EXPECT_EQ(maxed.getLevelCount(), 16u);
    EXPECT_EQ(maxed.getMaxSlots(), 8u);

    LumenRadianceCache keySafe(4.f, 1, 4096, 1, 1, 1);
    EXPECT_EQ(keySafe.getResolution(), kRadianceCacheMaxResolution);
}

CPU_TEST(LumenRadianceCache_LevelGeometry)
{
    LumenRadianceCache cache(4.f, 6, 8, 4096, 8, 2, 0);
    // extent_m = baseExtent * 2^m; voxelSize = extent / resolution. All exact in float.
    for (uint32_t m = 0; m < 6; ++m)
    {
        const LumenRadianceCache::LumenRadianceCacheLevel& lvl = cache.getLevel(m);
        const float expectedExtent = 4.0f * static_cast<float>(1u << m);
        const float expectedVoxel = expectedExtent / 8.0f;
        EXPECT_EQ(lvl.resolution, 8u);
        EXPECT_EQ(lvl.worldExtent, expectedExtent);
        EXPECT_EQ(lvl.voxelSize, expectedVoxel);
    }
    // Stale-level handles clamp deterministically to the coarsest remaining level.
    EXPECT_EQ(cache.getLevel(99).worldExtent, 4.0f * static_cast<float>(1u << 5));
}

CPU_TEST(LumenRadianceCache_ProbeKeyPacking)
{
    // makeProbeKey: generation [0,16), cell.x [16,26), cell.y [26,36), cell.z [36,46), level [46,52).
    const uint64_t key = LumenRadianceCache::makeProbeKey(1, LumenRadianceCache::index3(2, 3, 4), 5);
    EXPECT_EQ(key & 0xFFFFull, 5ull);
    EXPECT_EQ((key >> 16) & 0x3FFull, 2ull);
    EXPECT_EQ((key >> 26) & 0x3FFull, 3ull);
    EXPECT_EQ((key >> 36) & 0x3FFull, 4ull);
    EXPECT_EQ((key >> 46) & 0x3Full, 1ull);
    // A different generation or cell changes the key (stale-read detection).
    EXPECT_NE(LumenRadianceCache::makeProbeKey(1, LumenRadianceCache::index3(2, 3, 4), 6), key);
    EXPECT_NE(LumenRadianceCache::makeProbeKey(1, LumenRadianceCache::index3(5, 3, 4), 5), key);

    // makeCellKey: cell.x [0,10), cell.y [10,20), cell.z [20,30), level [30,36).
    const uint64_t ck = LumenRadianceCache::makeCellKey(3, LumenRadianceCache::index3(1, 2, 3));
    EXPECT_EQ(ck & 0x3FFull, 1ull);
    EXPECT_EQ((ck >> 10) & 0x3FFull, 2ull);
    EXPECT_EQ((ck >> 20) & 0x3FFull, 3ull);
    EXPECT_EQ((ck >> 30) & 0x3Full, 3ull);
    EXPECT_NE(ck, key);
}

CPU_TEST(LumenRadianceCache_DirectionEncoding)
{
    // A degenerate input encodes to the invalid sentinel.
    EXPECT_EQ(
        LumenRadianceCache::encodeDirection(LumenRadianceCache::float3(0.f, 0.f, 0.f)),
        kRadianceCacheInvalidDirectionEncoding);

    // decode(encode(+Z)) is (nearly) +Z.
    const LumenRadianceCache::float3 up =
        LumenRadianceCache::decodeDirection(LumenRadianceCache::encodeDirection(LumenRadianceCache::float3(0.f, 0.f, 1.f)));
    EXPECT_GT(up.z, 0.99f);

    // decode(encode(d)) is unit length and stays near d for a spread of directions.
    const LumenRadianceCache::float3 dirs[] = {
        LumenRadianceCache::float3(1.f, 0.f, 0.f),
        LumenRadianceCache::float3(0.f, 1.f, 0.f),
        LumenRadianceCache::float3(0.f, 0.f, -1.f),
        LumenRadianceCache::float3(1.f, 1.f, 1.f),
        LumenRadianceCache::float3(-1.f, 2.f, 0.5f),
    };
    for (const LumenRadianceCache::float3& d : dirs)
    {
        const uint32_t enc = LumenRadianceCache::encodeDirection(d);
        EXPECT_NE(enc, kRadianceCacheInvalidDirectionEncoding);
        const LumenRadianceCache::float3 dec = LumenRadianceCache::decodeDirection(enc);
        const float len = std::sqrt(dec.x * dec.x + dec.y * dec.y + dec.z * dec.z);
        EXPECT_TRUE(std::fabs(len - 1.0f) < 1e-3f);
        const float dLen = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const float dot = (dec.x * d.x + dec.y * d.y + dec.z * d.z) / (len * dLen);
        EXPECT_GT(dot, 0.99f);
    }
}

CPU_TEST(LumenRadianceCache_WorldToCellRoundTrip)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    const LumenRadianceCache::index3 cell(3, 3, 3);
    const LumenRadianceCache::float3 world = cache.probeWorldPosition(0, cell);
    // Level-0 grid min is -2 (extent 4 centered at origin), voxel 0.5.
    EXPECT_EQ(world.x, -0.25f);
    EXPECT_EQ(world.y, -0.25f);
    EXPECT_EQ(world.z, -0.25f);

    LumenRadianceCache::index3 back;
    LumenRadianceCache::float3 frac;
    EXPECT_TRUE(cache.worldToCell(0, world, back, frac));
    EXPECT_TRUE(back == cell);
    EXPECT_TRUE(std::fabs(frac.x - 0.5f) < 1e-6f);
    EXPECT_TRUE(std::fabs(frac.y - 0.5f) < 1e-6f);
    EXPECT_TRUE(std::fabs(frac.z - 0.5f) < 1e-6f);

    // Points outside the footprint report out-of-bounds.
    LumenRadianceCache::index3 oob;
    LumenRadianceCache::float3 oobFrac;
    EXPECT_FALSE(cache.worldToCell(0, LumenRadianceCache::float3(100.f, 0.f, 0.f), oob, oobFrac));
}

CPU_TEST(LumenRadianceCache_LevelSelection)
{
    LumenRadianceCache cache(4.f, 6, 8, 4096, 8, 1, 0);
    // Camera starts at the origin: the origin belongs to the dynamic level 0.
    EXPECT_EQ(cache.levelIndexForWorld(LumenRadianceCache::float3(0.f, 0.f, 0.f)), 0u);
    // Level half-extents are 2,4,8,...: pick the finest containing level.
    EXPECT_EQ(cache.levelIndexForWorld(LumenRadianceCache::float3(3.f, 0.f, 0.f)), 1u);
    EXPECT_EQ(cache.levelIndexForWorld(LumenRadianceCache::float3(6.f, 0.f, 0.f)), 2u);
    // Beyond every static footprint the coarsest level is the fallback.
    EXPECT_EQ(cache.levelIndexForWorld(LumenRadianceCache::float3(200.f, 0.f, 0.f)), 5u);
}

CPU_TEST(LumenRadianceCache_SlotAllocationAscendingAndIdempotent)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    const uint32_t s1 = cache.allocateProbe(0, LumenRadianceCache::index3(0, 0, 0));
    const uint32_t s2 = cache.allocateProbe(0, LumenRadianceCache::index3(1, 0, 0));
    EXPECT_EQ(s1, 1u); //< Free-list pop_back hands out ascending IDs.
    EXPECT_EQ(s2, 2u);

    // Allocating the same cell again is idempotent (no second slot, no counter bump).
    EXPECT_EQ(cache.allocateProbe(0, LumenRadianceCache::index3(0, 0, 0)), s1);
    EXPECT_EQ(cache.getStats().allocationCount, 2ull);
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(0, 0, 0)), s1);
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(1, 0, 0)), s2);
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(7, 7, 7)), kInvalidProbeSlot);
}

CPU_TEST(LumenRadianceCache_ReleaseAndReuseBumpsGeneration)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    const uint32_t a = cache.allocateProbe(0, LumenRadianceCache::index3(1, 1, 1));
    const uint32_t b = cache.allocateProbe(0, LumenRadianceCache::index3(2, 1, 1));
    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, 2u);
    EXPECT_EQ(cache.getSlot(a).generation, 1u);

    EXPECT_TRUE(cache.releaseProbe(a));
    EXPECT_FALSE(cache.releaseProbe(a)); //< Double release is a no-op.
    EXPECT_FALSE(cache.releaseProbe(kInvalidProbeSlot));
    EXPECT_FALSE(cache.releaseProbe(9999));
    EXPECT_EQ(cache.getStats().releaseCount, 1ull);

    // The LIFO free-list hands the released slot back out with a bumped generation.
    const uint32_t c = cache.allocateProbe(0, LumenRadianceCache::index3(3, 1, 1));
    EXPECT_EQ(c, a);
    EXPECT_EQ(cache.getSlot(c).generation, 2u);
    EXPECT_EQ(cache.getProbeKey(c), LumenRadianceCache::makeProbeKey(0, LumenRadianceCache::index3(3, 1, 1), 2));
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(1, 1, 1)), kInvalidProbeSlot);
}

CPU_TEST(LumenRadianceCache_UpdateProbeConfidenceAndPayload)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    cache.advanceFrame(); //< Frame >= 1 so lastUpdateFrame is nonzero (freshness).
    const uint32_t slot = cache.allocateProbe(0, LumenRadianceCache::index3(2, 2, 2));
    const float rad[3] = {1.f, 2.f, 3.f};
    EXPECT_TRUE(cache.updateProbe(slot, rad, 42u));
    EXPECT_EQ(cache.getSlot(slot).confidence, kRadianceCacheFirstUpdateConfidence);
    EXPECT_EQ(cache.getSlot(slot).lastUpdateFrame, 1ull);
    EXPECT_EQ(cache.getSlot(slot).radiance[0], 1.f);
    EXPECT_EQ(cache.getSlot(slot).radiance[1], 2.f);
    EXPECT_EQ(cache.getSlot(slot).radiance[2], 3.f);
    EXPECT_EQ(cache.getSlot(slot).directionEncoding, 42u);
    EXPECT_EQ(cache.getStats().updateCount, 1ull);

    // Later updates blend the confidence up monotonically toward 1.
    float previous = cache.getSlot(slot).confidence;
    for (int i = 0; i < 20; ++i)
    {
        EXPECT_TRUE(cache.updateProbe(slot, rad, 42u));
        const float current = cache.getSlot(slot).confidence;
        EXPECT_GT(current, previous);
        previous = current;
    }
    EXPECT_LE(cache.getSlot(slot).confidence, 1.0f);
    EXPECT_GT(cache.getSlot(slot).confidence, 0.99f);

    // Updates on invalid or released slots are rejected.
    EXPECT_FALSE(cache.updateProbe(kInvalidProbeSlot, rad, 42u));
    EXPECT_FALSE(cache.updateProbe(slot + 1000, rad, 42u));
    const uint32_t other = cache.allocateProbe(0, LumenRadianceCache::index3(3, 3, 3));
    EXPECT_TRUE(cache.releaseProbe(other));
    EXPECT_FALSE(cache.updateProbe(other, rad, 42u));
}

CPU_TEST(LumenRadianceCache_LruEvictionWhenPoolFull)
{
    // 1 level, 8^3 cells but only 4 slots: the pool must LRU-evict to grow.
    LumenRadianceCache cache(4.f, 1, 8, 4, 8, 1, 0);
    const uint32_t s1 = cache.allocateProbe(0, LumenRadianceCache::index3(0, 0, 0));
    const uint32_t s2 = cache.allocateProbe(0, LumenRadianceCache::index3(1, 0, 0));
    const uint32_t s3 = cache.allocateProbe(0, LumenRadianceCache::index3(2, 0, 0));
    const uint32_t s4 = cache.allocateProbe(0, LumenRadianceCache::index3(3, 0, 0));
    EXPECT_EQ(s1, 1u);
    EXPECT_EQ(s2, 2u);
    EXPECT_EQ(s3, 3u);
    EXPECT_EQ(s4, 4u);

    // Pool full: allocating a fifth cell evicts the oldest (slot 1) and reuses it.
    const uint32_t s5 = cache.allocateProbe(0, LumenRadianceCache::index3(4, 0, 0));
    EXPECT_EQ(s5, 1u);
    EXPECT_EQ(cache.getSlot(1).generation, 2u);
    EXPECT_EQ(cache.getStats().evictionCount, 1ull);
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(0, 0, 0)), kInvalidProbeSlot); //< Old cell released.
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(4, 0, 0)), 1u);
    EXPECT_EQ(cache.getProbeKey(1), LumenRadianceCache::makeProbeKey(0, LumenRadianceCache::index3(4, 0, 0), 2));
}

CPU_TEST(LumenRadianceCache_TickSchedulesBudgetNewSlots)
{
    LumenRadianceCache cache(4.f, 2, 8, 1024, 32, 1, 0);
    const std::vector<LumenRadianceCache::RadianceCacheRefreshRequest>& reqs = cache.tick();
    EXPECT_EQ(reqs.size(), 32u); //< Budget honoured exactly on a cold cache.
    EXPECT_EQ(cache.getStats().lastRefreshCount, 32u);
    EXPECT_EQ(cache.getStats().refreshCount, 32ull);
    for (const LumenRadianceCache::RadianceCacheRefreshRequest& r : reqs)
    {
        EXPECT_NE(r.slot, kInvalidProbeSlot);
        EXPECT_TRUE(r.isNewSlot); //< Empty cells are allocated on the first tick.
        EXPECT_NE(r.key, 0ull);
    }
    // Refresh requests accumulate across frames.
    cache.tick();
    EXPECT_EQ(cache.getStats().refreshCount, 64ull);
}

CPU_TEST(LumenRadianceCache_TickReservesEveryLevelWhenBudgetAllows)
{
    // A global distance score must not starve a cold clipmap level. With a
    // budget >= levelCount the first tick reserves one cold cell per level,
    // then fills the remaining requests by the normal deterministic ordering.
    LumenRadianceCache cache(4.f, 6, 8, 4096, 64, 1, 0);
    const auto& requests = cache.tick();
    EXPECT_EQ(requests.size(), 64u);
    std::vector<bool> seen(6u, false);
    for (const auto& request : requests)
    {
        if (request.level < seen.size())
            seen[request.level] = true;
    }
    for (bool levelSeen : seen)
        EXPECT_TRUE(levelSeen);
}

CPU_TEST(LumenRadianceCache_TickPopulatesColdStaticLevels)
{
    // GPU confidence is fed back asynchronously.  A cold allocated slot must
    // not starve empty cells in the static clipmap while that feedback is
    // pending; after enough bounded ticks, level 1 must receive ownership.
    LumenRadianceCache cache(4.f, 2, 8, 1024, 32, 1, 0);
    for (uint32_t frame = 0; frame < 20u; ++frame)
        cache.tick();

    bool hasStaticProbe = false;
    for (uint32_t slot = 1u; slot <= cache.getMaxSlots(); ++slot)
    {
        if (cache.getSlot(slot).allocated && cache.getSlot(slot).level == 1u)
        {
            hasStaticProbe = true;
            break;
        }
    }
    EXPECT_TRUE(hasStaticProbe);
}

CPU_TEST(LumenRadianceCache_TickDeterministicAcrossInstances)
{
    auto collectKeys = []()
    {
        LumenRadianceCache cache(4.f, 3, 8, 4096, 48, 1, 0);
        cache.setCamera(LumenRadianceCache::float3(1.3f, -2.7f, 0.5f));
        std::vector<uint64_t> keys;
        for (int frame = 0; frame < 3; ++frame)
        {
            const std::vector<LumenRadianceCache::RadianceCacheRefreshRequest>& reqs = cache.tick();
            for (const LumenRadianceCache::RadianceCacheRefreshRequest& r : reqs)
            {
                keys.push_back(r.key);
            }
        }
        return keys;
    };
    // Identical inputs must produce identical schedules (no randomness, no clock).
    const std::vector<uint64_t> first = collectKeys();
    const std::vector<uint64_t> second = collectKeys();
    EXPECT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i)
    {
        EXPECT_EQ(first[i], second[i]);
    }
}

CPU_TEST(LumenRadianceCache_QueryInterpolatesResidentCorners)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    cache.advanceFrame(); //< Frame >= 1 so updates are "fresh".
    for (int i = 0; i < 8; ++i)
    {
        const LumenRadianceCache::index3 corner(
            3 + ((i >> 2) & 1),
            3 + ((i >> 1) & 1),
            3 + (i & 1));
        const uint32_t slot = cache.allocateProbe(0, corner);
        EXPECT_NE(slot, kInvalidProbeSlot);
        const float rad[3] = {static_cast<float>(i), static_cast<float>(i + 1), static_cast<float>(i + 2)};
        EXPECT_TRUE(cache.updateProbe(slot, rad, LumenRadianceCache::encodeDirection(LumenRadianceCache::float3(0.f, 0.f, 1.f))));
    }

    // Query the center of cell (3,3,3): all 8 corners resident, equal 1/8 weights.
    const LumenRadianceCache::RadianceCacheQueryResult r =
        cache.query(LumenRadianceCache::float3(-0.25f, -0.25f, -0.25f));
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.level, 0u);
    EXPECT_TRUE(r.cell == LumenRadianceCache::index3(3, 3, 3));
    EXPECT_EQ(r.allocatedCornerCount, 8u);
    EXPECT_TRUE(r.fresh);
    EXPECT_FALSE(r.expired);
    // Averaged corner payloads: (0..7)/8 = 3.5, (1..8)/8 = 4.5, (2..9)/8 = 5.5.
    EXPECT_TRUE(std::fabs(r.radiance[0] - 3.5f) < 1e-4f);
    EXPECT_TRUE(std::fabs(r.radiance[1] - 4.5f) < 1e-4f);
    EXPECT_TRUE(std::fabs(r.radiance[2] - 5.5f) < 1e-4f);
    EXPECT_TRUE(std::fabs(r.confidence - 0.25f) < 1e-4f);
    EXPECT_EQ(cache.getStats().queryCount, 1ull);
}

CPU_TEST(LumenRadianceCache_QueryEmptyCacheInvalid)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    const LumenRadianceCache::RadianceCacheQueryResult r = cache.query(LumenRadianceCache::float3(0.f, 0.f, 0.f));
    EXPECT_FALSE(r.valid);
    EXPECT_TRUE(r.expired);
    EXPECT_FALSE(r.fresh);
    EXPECT_EQ(r.allocatedCornerCount, 0u);
    EXPECT_EQ(r.radiance[0], 0.f);
}

CPU_TEST(LumenRadianceCache_QueryMarksLastUsedAndTouchValidatesSlot)
{
    LumenRadianceCache cache(4.f, 1, 8, 8, 8, 1, 0);
    cache.advanceFrame();
    const uint32_t slot = cache.allocateProbe(0, LumenRadianceCache::index3(3, 3, 3));
    const float rad[3] = {1.f, 1.f, 1.f};
    EXPECT_TRUE(cache.updateProbe(slot, rad, 0u));
    EXPECT_EQ(cache.getSlot(slot).lastUsedFrame, 1ull);

    cache.advanceFrame();
    EXPECT_TRUE(cache.touchProbe(slot));
    EXPECT_EQ(cache.getSlot(slot).lastUsedFrame, 2ull);
    EXPECT_FALSE(cache.touchProbe(kInvalidProbeSlot));
    EXPECT_FALSE(cache.touchProbe(9999u));

    // Query consumption also updates the usage timestamp, which is the signal used by
    // the UE-style LRU eviction policy rather than radiance update age alone.
    cache.advanceFrame();
    cache.query(LumenRadianceCache::float3(-0.25f, -0.25f, -0.25f));
    EXPECT_EQ(cache.getSlot(slot).lastUsedFrame, 3ull);
}

CPU_TEST(LumenRadianceCache_QueryOutsideFootprintInvalid)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    const LumenRadianceCache::RadianceCacheQueryResult r = cache.query(LumenRadianceCache::float3(100.f, 0.f, 0.f));
    EXPECT_FALSE(r.valid); //< Points beyond every footprint are invalid.
}

CPU_TEST(LumenRadianceCache_ProbeFreshnessWindow)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    cache.advanceFrame(); //< Frame 1 so lastUpdateFrame is nonzero.
    const uint32_t slot = cache.allocateProbe(0, LumenRadianceCache::index3(3, 3, 3));
    const float rad[3] = {1.f, 1.f, 1.f};
    EXPECT_FALSE(cache.isProbeFresh(slot)); //< Allocated but never updated.
    EXPECT_TRUE(cache.updateProbe(slot, rad, 0u));
    EXPECT_TRUE(cache.isProbeFresh(slot));

    // Once the freshness window elapses the probe is stale.
    for (uint32_t frame = 0; frame < kRadianceCacheFreshnessWindowFrames + 1; ++frame)
    {
        cache.advanceFrame();
    }
    EXPECT_FALSE(cache.isProbeFresh(slot));
}

CPU_TEST(LumenRadianceCache_MemoryEstimateAndBudgetDegradation)
{
    LumenRadianceCache cache(4.f, 6, 8, 4096, 8, 1, 0);
    const uint64_t cells6 = 6ull * 8u * 8u * 8u;
    EXPECT_EQ(cache.estimateMemoryBytes(), cells6 * kRadianceCacheBytesPerCellMeta);
    for (int i = 0; i < 10; ++i)
    {
        cache.allocateProbe(0, LumenRadianceCache::index3(i, 0, 0));
    }
    EXPECT_EQ(cache.estimateMemoryBytes(), cells6 * kRadianceCacheBytesPerCellMeta + 10ull * kRadianceCacheBytesPerProbeSlot);

    // Dropping the farthest static level removes its cells; level-0 slots survive.
    EXPECT_TRUE(cache.dropFarthestStaticLevel());
    EXPECT_EQ(cache.getLevelCount(), 5u);
    EXPECT_EQ(cache.getStats().dropCount, 1ull);
    EXPECT_EQ(cache.estimateMemoryBytes(), 5ull * 8u * 8u * 8u * kRadianceCacheBytesPerCellMeta + 10ull * kRadianceCacheBytesPerProbeSlot);

    // The dynamic level is never dropped.
    LumenRadianceCache single(4.f, 1, 8, 8, 8, 1, 0);
    EXPECT_FALSE(single.dropFarthestStaticLevel());

    // Budget enforcement drops levels until the static-cell cost fits.
    LumenRadianceCache degraded(4.f, 6, 8, 4096, 8, 1, 0);
    EXPECT_TRUE(degraded.enforceBudget(30000ull)); //< 3 levels * 512 * 16 = 24576 <= 30000.
    EXPECT_EQ(degraded.getLevelCount(), 3u);
    EXPECT_LE(degraded.estimateMemoryBytes(), 30000ull);

    // A zero budget means unlimited: no degradation.
    LumenRadianceCache unlimited(4.f, 6, 8, 4096, 8, 1, 0);
    EXPECT_TRUE(unlimited.enforceBudget(0ull));
    EXPECT_EQ(unlimited.getLevelCount(), 6u);

    // A budget below even the dynamic-only cost cannot be met.
    LumenRadianceCache tiny(4.f, 6, 8, 4096, 8, 1, 0);
    EXPECT_FALSE(tiny.enforceBudget(100ull));
    EXPECT_EQ(tiny.getLevelCount(), 1u);
}

CPU_TEST(LumenRadianceCache_CameraScrollReparentsLevel0)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    const uint32_t slot = cache.allocateProbe(0, LumenRadianceCache::index3(6, 2, 2));
    const float rad[3] = {0.5f, 0.25f, 0.125f};
    EXPECT_TRUE(cache.updateProbe(slot, rad, 0u));

    // Move +4 level-0 voxels (world +2 m): cached content shifts by -4 in index space.
    cache.setCamera(LumenRadianceCache::float3(2.f, 0.f, 0.f));
    const LumenRadianceCache::index3 scroll = cache.scrollFromCameraMove();
    EXPECT_EQ(scroll.x, 4);
    EXPECT_EQ(scroll.y, 0);
    EXPECT_EQ(scroll.z, 0);
    EXPECT_EQ(cache.getCameraCenter().x, 2.0f);
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(2, 2, 2)), slot); //< Re-parented, radiance kept.
    EXPECT_TRUE(cache.getSlot(slot).cell == LumenRadianceCache::index3(2, 2, 2));
    EXPECT_EQ(cache.getProbeKey(slot), LumenRadianceCache::makeProbeKey(0, LumenRadianceCache::index3(2, 2, 2), 1));

    // Content scrolled out of the footprint is freed.
    LumenRadianceCache cache2(4.f, 1, 8, 64, 8, 1, 0);
    const uint32_t dropSlot = cache2.allocateProbe(0, LumenRadianceCache::index3(1, 1, 1));
    cache2.setCamera(LumenRadianceCache::float3(4.f, 0.f, 0.f)); //< +8 voxels.
    EXPECT_EQ(cache2.findProbe(0, LumenRadianceCache::index3(1, 1, 1)), kInvalidProbeSlot);
    EXPECT_EQ(cache2.getSlot(dropSlot).allocated, false);

    // A move of exactly half a voxel produces no scroll (round-half-toward-zero).
    LumenRadianceCache cache3(4.f, 1, 8, 64, 8, 1, 0);
    cache3.setCamera(LumenRadianceCache::float3(0.25f, 0.f, 0.f)); //< 0.25 / 0.5 = 0.5 voxels.
    const LumenRadianceCache::index3 noScroll = cache3.scrollFromCameraMove();
    EXPECT_EQ(noScroll.x, 0);
    EXPECT_EQ(noScroll.y, 0);
    EXPECT_EQ(noScroll.z, 0);
    // A 0.6 m move is > half a voxel and snaps to one full voxel.
    cache3.setCamera(LumenRadianceCache::float3(0.6f, 0.f, 0.f));
    EXPECT_EQ(cache3.scrollFromCameraMove().x, 1);
    EXPECT_EQ(cache3.getCameraCenter().x, 0.5f);
}

CPU_TEST(LumenRadianceCache_ProbeVisibilityWeight)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    const uint32_t slot = cache.allocateProbe(0, LumenRadianceCache::index3(4, 4, 4));
    EXPECT_TRUE(cache.setProbeVisibilityWeight(slot, 0.5f));
    EXPECT_EQ(cache.getSlot(slot).visibilityWeight, 0.5f);
    EXPECT_TRUE(cache.setProbeVisibilityWeight(slot, 2.f)); //< Clamped to 1.
    EXPECT_EQ(cache.getSlot(slot).visibilityWeight, 1.0f);
    EXPECT_TRUE(cache.setProbeVisibilityWeight(slot, -1.f)); //< Clamped to 0.
    EXPECT_EQ(cache.getSlot(slot).visibilityWeight, 0.0f);
    EXPECT_FALSE(cache.setProbeVisibilityWeight(kInvalidProbeSlot, 1.f));
    EXPECT_FALSE(cache.setProbeVisibilityWeight(slot + 1000, 1.f));
}

CPU_TEST(LumenRadianceCache_InvalidSlotAccessIsSafe)
{
    LumenRadianceCache cache(4.f, 1, 8, 64, 8, 1, 0);
    EXPECT_EQ(cache.getProbeKey(kInvalidProbeSlot), 0ull);
    EXPECT_EQ(cache.getProbeKey(9999), 0ull);
    EXPECT_EQ(cache.getSlot(kInvalidProbeSlot).allocated, false);
    EXPECT_EQ(cache.getSlot(9999).allocated, false);
    EXPECT_EQ(cache.getSlot(kInvalidProbeSlot).generation, 0u);
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(1, 2, 3)), kInvalidProbeSlot);
}

CPU_TEST(LumenRadianceCache_ResetRestoresFreshState)
{
    LumenRadianceCache cache(4.f, 2, 8, 128, 8, 1, 0);
    const uint32_t slot = cache.allocateProbe(0, LumenRadianceCache::index3(2, 2, 2));
    const float rad[3] = {1.f, 1.f, 1.f};
    cache.updateProbe(slot, rad, 0u);
    cache.tick();
    cache.query(LumenRadianceCache::float3(0.f, 0.f, 0.f));
    EXPECT_EQ(cache.getFrameIndex(), 1ull);

    cache.reset();
    // Geometry and slot capacity survive; state and counters are cleared.
    EXPECT_EQ(cache.getLevelCount(), 2u);
    EXPECT_EQ(cache.getMaxSlots(), 128u);
    EXPECT_EQ(cache.getFrameIndex(), 0ull);
    EXPECT_EQ(cache.getStats().allocatedSlotCount, 0u);
    EXPECT_EQ(cache.getStats().freeSlotCount, 128u);
    EXPECT_EQ(cache.getStats().allocationCount, 0ull);
    EXPECT_EQ(cache.getStats().updateCount, 0ull);
    EXPECT_EQ(cache.getStats().refreshCount, 0ull);
    EXPECT_EQ(cache.getStats().queryCount, 0ull);
    EXPECT_EQ(cache.findProbe(0, LumenRadianceCache::index3(2, 2, 2)), kInvalidProbeSlot);

    // The first allocation after reset restarts from slot 1, generation 1.
    const uint32_t first = cache.allocateProbe(0, LumenRadianceCache::index3(2, 2, 2));
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(cache.getSlot(first).generation, 1u);
}

} // namespace Falcor
