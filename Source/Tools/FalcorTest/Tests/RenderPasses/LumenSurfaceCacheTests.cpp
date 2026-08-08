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
#include "../../../../RenderPasses/LumenGI/LumenGIStats.h"
#include "../../../../RenderPasses/LumenGI/SurfaceCache/LumenSurfaceCache.h"

#include <cstdint>
#include <vector>

namespace Falcor
{
namespace
{
// Small atlas: 64 texels per side -> 4x4 tiles = 16 pages (kLumenSurfaceCacheTileSize = 16).
constexpr uint32_t kSmallAtlasSizeTexels = 64;
constexpr uint32_t kSmallAtlasTileCountPerSide = 4;
constexpr uint32_t kSmallAtlasPageCount = kSmallAtlasTileCountPerSide * kSmallAtlasTileCountPerSide;
constexpr uint32_t kTestResidencyFrames = 5;
constexpr uint64_t kTestBudgetBytes = 10ull * kLumenSurfaceCacheBytesPerPage;
} // namespace

CPU_TEST(LumenSurfaceCache_SequentialAllocationFillsAtlas)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, 0, kTestResidencyFrames);
    EXPECT_EQ(cache.getPageCount(), kSmallAtlasPageCount);
    EXPECT_EQ(cache.getFreeCount(), kSmallAtlasPageCount);
    EXPECT_EQ(cache.getAllocatedPageCount(), 0);
    EXPECT_EQ(cache.getResidentBytes(), 0);

    std::vector<uint8_t> occupied(kSmallAtlasPageCount, 0);
    for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
    {
        const uint32_t pageID = cache.allocatePage();
        EXPECT_NE(pageID, kInvalidPageID);
        EXPECT_EQ(pageID, i + 1); //< Ascending order while the free-list is untouched.
        EXPECT_TRUE(cache.isPageAllocated(pageID));
        EXPECT_EQ(cache.getGeneration(pageID), 1);

        // The stored coordinate matches the layout formula and the texel rect fits the atlas.
        const LumenSurfaceCacheCoord coord = cache.getPageAtlasCoord(pageID);
        EXPECT_EQ(coord.atlasX, (pageID - 1) % kSmallAtlasTileCountPerSide);
        EXPECT_EQ(coord.atlasY, (pageID - 1) / kSmallAtlasTileCountPerSide);
        EXPECT_LE((coord.atlasX + 1) * kLumenSurfaceCacheTileSize, kSmallAtlasSizeTexels);
        EXPECT_LE((coord.atlasY + 1) * kLumenSurfaceCacheTileSize, kSmallAtlasSizeTexels);

        // No two allocated pages may overlap.
        const size_t slot = static_cast<size_t>(coord.atlasY) * kSmallAtlasTileCountPerSide + coord.atlasX;
        EXPECT_EQ(occupied[slot], 0);
        occupied[slot] = 1;
    }
    for (uint8_t isOccupied : occupied)
    {
        EXPECT_EQ(isOccupied, 1);
    }

    EXPECT_EQ(cache.getFreeCount(), 0);
    EXPECT_EQ(cache.getAllocatedPageCount(), kSmallAtlasPageCount);
    const uint64_t expectedBytes = static_cast<uint64_t>(kSmallAtlasPageCount) * kLumenSurfaceCacheBytesPerPage;
    EXPECT_EQ(cache.getResidentBytes(), expectedBytes);

    // Resource stats split matches the per-texel sizes of the three atlases.
    const LumenGIResourceStats resources = cache.toResourceStats();
    const uint64_t texelCount =
        static_cast<uint64_t>(kSmallAtlasPageCount) * kLumenSurfaceCacheTileSize * kLumenSurfaceCacheTileSize;
    EXPECT_EQ(resources.surfaceCacheMaterialBytes, texelCount * kLumenSurfaceCacheMaterialBytesPerTexel);
    EXPECT_EQ(resources.surfaceCacheRadianceBytes, texelCount * kLumenSurfaceCacheRadianceBytesPerTexel);
    EXPECT_EQ(resources.surfaceCacheMetadataBytes, texelCount * kLumenSurfaceCacheMetadataBytesPerTexel);
    EXPECT_EQ(resources.getTotalBytes(), expectedBytes);
}

CPU_TEST(LumenSurfaceCache_FullAtlasAllocationFailsInsideMinResidency)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, 0, kTestResidencyFrames);
    for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
    {
        EXPECT_NE(cache.allocatePage(), kInvalidPageID);
    }

    // Atlas is full and every page was allocated in the current frame: nothing is evictable.
    EXPECT_EQ(cache.allocatePage(), kInvalidPageID);
    for (uint32_t frame = 1; frame < kTestResidencyFrames; ++frame)
    {
        cache.endFrame();
        EXPECT_EQ(cache.allocatePage(), kInvalidPageID);
    }

    // At frame kTestResidencyFrames the oldest page (page 1) is evicted and reused.
    cache.endFrame();
    const uint32_t pageID = cache.allocatePage();
    EXPECT_EQ(pageID, 1);
    EXPECT_TRUE(cache.isPageAllocated(pageID));
    EXPECT_EQ(cache.getGeneration(pageID), 2);
    const LumenSurfaceCacheStats stats = cache.getStats();
    EXPECT_EQ(stats.evictionCount, 1);
    EXPECT_EQ(stats.allocationCount, kSmallAtlasPageCount + 1);
    EXPECT_EQ(cache.getFreeCount(), 0);
}

CPU_TEST(LumenSurfaceCache_LruEvictionRespectsMinResidency)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, 0, kTestResidencyFrames);
    for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
    {
        EXPECT_NE(cache.allocatePage(), kInvalidPageID);
    }

    // Frame 5: pages 1..8 are touched this frame, pages 9..16 are the oldest cohort.
    for (uint32_t frame = 0; frame < kTestResidencyFrames; ++frame)
    {
        cache.endFrame();
    }
    for (uint32_t pageID = 1; pageID <= 8; ++pageID)
    {
        EXPECT_TRUE(cache.touchPage(pageID));
    }
    EXPECT_EQ(cache.getPageState(1) == LumenSurfaceCachePageState::Touched, true);
    EXPECT_EQ(cache.getPageState(12) == LumenSurfaceCachePageState::Allocated, true);

    // Allocation evicts pages 9, 10, 11 (oldest, ascending tie-break) and reuses them.
    EXPECT_EQ(cache.allocatePage(), 9);
    EXPECT_EQ(cache.allocatePage(), 10);
    EXPECT_EQ(cache.allocatePage(), 11);
    EXPECT_EQ(cache.getGeneration(9), 2);
    for (uint32_t pageID = 1; pageID <= 8; ++pageID)
    {
        EXPECT_TRUE(cache.isPageAllocated(pageID));
    }
    EXPECT_EQ(cache.getStats().evictionCount, 3);

    // Frame 10: pages 12..16 (last touched at frame 0) are still the oldest.
    for (uint32_t frame = 0; frame < kTestResidencyFrames; ++frame)
    {
        cache.endFrame();
    }
    EXPECT_EQ(cache.allocatePage(), 12);

    // Frame 15: the untouched cohort is exhausted before the touched pages become evictable.
    for (uint32_t frame = 0; frame < kTestResidencyFrames; ++frame)
    {
        cache.endFrame();
    }
    EXPECT_EQ(cache.allocatePage(), 13);
    EXPECT_EQ(cache.allocatePage(), 14);
    for (uint32_t pageID = 1; pageID <= 12; ++pageID)
    {
        EXPECT_TRUE(cache.isPageAllocated(pageID));
    }
    EXPECT_EQ(cache.getStats().evictionCount, 6);

    // Frame 20: the remaining pages touched at frame 5 are now the oldest; page 1 goes first.
    for (uint32_t frame = 0; frame < kTestResidencyFrames; ++frame)
    {
        cache.endFrame();
    }
    EXPECT_EQ(cache.allocatePage(), 15);
    EXPECT_EQ(cache.allocatePage(), 16);
    EXPECT_EQ(cache.allocatePage(), 1);
    EXPECT_EQ(cache.getGeneration(1), 2);
    for (uint32_t pageID = 2; pageID <= 8; ++pageID)
    {
        EXPECT_TRUE(cache.isPageAllocated(pageID));
    }
    EXPECT_EQ(cache.getStats().evictionCount, 9);
}

CPU_TEST(LumenSurfaceCache_AllocateReleaseChurnNoLeak)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, 0, kTestResidencyFrames);
    constexpr uint32_t kRounds = 100;
    for (uint32_t round = 0; round < kRounds; ++round)
    {
        std::vector<uint32_t> pages(kSmallAtlasPageCount, kInvalidPageID);
        for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
        {
            pages[i] = cache.allocatePage();
            EXPECT_NE(pages[i], kInvalidPageID);
        }
        for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
        {
            for (uint32_t j = i + 1; j < kSmallAtlasPageCount; ++j)
            {
                EXPECT_NE(pages[i], pages[j]); //< The free-list never yields duplicates.
            }
        }
        for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
        {
            EXPECT_TRUE(cache.releasePage(pages[i]));
        }
        EXPECT_FALSE(cache.releasePage(pages[0])); //< Double release is a no-op.

        // The coordinate is a property of the page ID, so released pages keep it.
        const LumenSurfaceCacheCoord coord = cache.getPageAtlasCoord(pages[0]);
        EXPECT_EQ(coord.atlasX, (pages[0] - 1) % kSmallAtlasTileCountPerSide);
        EXPECT_EQ(coord.atlasY, (pages[0] - 1) / kSmallAtlasTileCountPerSide);
    }

    EXPECT_EQ(cache.getFreeCount(), kSmallAtlasPageCount);
    EXPECT_EQ(cache.getAllocatedPageCount(), 0);
    EXPECT_EQ(cache.getResidentBytes(), 0);
    const LumenSurfaceCacheStats stats = cache.getStats();
    EXPECT_EQ(stats.allocationCount, kRounds * kSmallAtlasPageCount);
    EXPECT_EQ(stats.releaseCount, kRounds * kSmallAtlasPageCount);
    EXPECT_EQ(stats.evictionCount, 0);

    // Reallocating the whole atlas is still duplicate-free and generations keep counting.
    std::vector<uint32_t> pages(kSmallAtlasPageCount, kInvalidPageID);
    for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
    {
        pages[i] = cache.allocatePage();
    }
    for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
    {
        for (uint32_t j = i + 1; j < kSmallAtlasPageCount; ++j)
        {
            EXPECT_NE(pages[i], pages[j]);
        }
        EXPECT_EQ(cache.getGeneration(pages[i]), kRounds + 1);
    }
}

CPU_TEST(LumenSurfaceCache_GenerationIncrementsOnReallocation)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, 0, kTestResidencyFrames);
    const uint32_t first = cache.allocatePage();
    EXPECT_EQ(first, 1);
    EXPECT_EQ(cache.getGeneration(first), 1);
    EXPECT_TRUE(cache.touchPage(first));
    EXPECT_EQ(cache.getGeneration(first), 1); //< Touches do not bump the generation.
    EXPECT_TRUE(cache.releasePage(first));
    EXPECT_EQ(cache.getGeneration(first), 1); //< Release does not bump the generation.

    // A released page is reallocated from the free-list with a bumped generation.
    const uint32_t second = cache.allocatePage();
    EXPECT_EQ(second, first);
    EXPECT_EQ(cache.getGeneration(second), 2);
    EXPECT_TRUE(cache.releasePage(second));
    EXPECT_EQ(cache.allocatePage(), first);
    EXPECT_EQ(cache.getGeneration(first), 3);
}

CPU_TEST(LumenSurfaceCache_ResizeRemapsAndInvalidates)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, 0, kTestResidencyFrames);
    for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
    {
        EXPECT_NE(cache.allocatePage(), kInvalidPageID);
    }

    // Grow 4x4 -> 8x8: all 16 pages survive with remapped coordinates, 48 pages are added.
    EXPECT_TRUE(cache.resize(8));
    EXPECT_EQ(cache.getPageCount(), 64);
    EXPECT_EQ(cache.getFreeCount(), 48);
    EXPECT_EQ(cache.getAllocatedPageCount(), 16);
    EXPECT_EQ(cache.getResidentBytes(), static_cast<uint64_t>(16) * kLumenSurfaceCacheBytesPerPage);
    std::vector<uint8_t> occupied(64, 0);
    for (uint32_t pageID = 1; pageID <= 16; ++pageID)
    {
        EXPECT_TRUE(cache.isPageAllocated(pageID));
        EXPECT_EQ(cache.getGeneration(pageID), 1); //< Generations survive resize.
        const LumenSurfaceCacheCoord coord = cache.getPageAtlasCoord(pageID);
        EXPECT_EQ(coord.atlasX, (pageID - 1) % 8);
        EXPECT_EQ(coord.atlasY, (pageID - 1) / 8);
        const size_t slot = static_cast<size_t>(coord.atlasY) * 8 + coord.atlasX;
        EXPECT_EQ(occupied[slot], 0);
        occupied[slot] = 1;
    }

    // Shrink 8x8 -> 2x2: only pages 1..4 survive, the other 12 are invalidated.
    EXPECT_TRUE(cache.resize(2));
    EXPECT_EQ(cache.getPageCount(), 4);
    EXPECT_EQ(cache.getFreeCount(), 0);
    EXPECT_EQ(cache.getAllocatedPageCount(), 4);
    EXPECT_EQ(cache.getResidentBytes(), static_cast<uint64_t>(4) * kLumenSurfaceCacheBytesPerPage);
    EXPECT_EQ(cache.getStats().invalidationCount, 12);
    for (uint32_t pageID = 1; pageID <= 4; ++pageID)
    {
        EXPECT_TRUE(cache.isPageAllocated(pageID));
        const LumenSurfaceCacheCoord coord = cache.getPageAtlasCoord(pageID);
        EXPECT_EQ(coord.atlasX, (pageID - 1) % 2);
        EXPECT_EQ(coord.atlasY, (pageID - 1) / 2);
    }
    for (uint32_t pageID = 5; pageID <= 16; ++pageID)
    {
        EXPECT_FALSE(cache.isPageAllocated(pageID)); //< Old handles are dead.
        EXPECT_FALSE(cache.releasePage(pageID));
        EXPECT_FALSE(cache.touchPage(pageID));
        const LumenSurfaceCacheCoord coord = cache.getPageAtlasCoord(pageID);
        EXPECT_EQ(coord.atlasX, kInvalidSurfaceCacheCoord);
        EXPECT_EQ(coord.atlasY, kInvalidSurfaceCacheCoord);
    }

    // Grow 2x2 -> 4x4: surviving pages keep their IDs and generations.
    EXPECT_TRUE(cache.resize(4));
    EXPECT_EQ(cache.getPageCount(), 16);
    EXPECT_EQ(cache.getFreeCount(), 12);
    EXPECT_EQ(cache.getAllocatedPageCount(), 4);
    EXPECT_EQ(cache.getGeneration(1), 1);

    // Invalid sizes are rejected.
    EXPECT_FALSE(cache.resize(0));
}

CPU_TEST(LumenSurfaceCache_ResetClearsState)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, kTestBudgetBytes, kTestResidencyFrames);
    for (uint32_t i = 0; i < 8; ++i)
    {
        const uint32_t pageID = cache.allocatePage();
        EXPECT_NE(pageID, kInvalidPageID);
        EXPECT_TRUE(cache.touchPage(pageID));
    }
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        cache.endFrame();
    }
    EXPECT_TRUE(cache.resize(8));

    cache.reset();
    EXPECT_EQ(cache.getPageCount(), 64); //< Reset keeps the current atlas size.
    EXPECT_EQ(cache.getFreeCount(), 64);
    EXPECT_EQ(cache.getAllocatedPageCount(), 0);
    EXPECT_EQ(cache.getResidentBytes(), 0);
    EXPECT_EQ(cache.getFrameIndex(), 0);
    const LumenSurfaceCacheStats stats = cache.getStats();
    EXPECT_EQ(stats.allocationCount, 0);
    EXPECT_EQ(stats.releaseCount, 0);
    EXPECT_EQ(stats.evictionCount, 0);
    EXPECT_EQ(stats.invalidationCount, 0);
    EXPECT_EQ(stats.touchCount, 0);
    EXPECT_FALSE(cache.isPageAllocated(1));

    // Generations restart from zero: the first allocation is generation 1 again.
    const uint32_t pageID = cache.allocatePage();
    EXPECT_EQ(pageID, 1);
    EXPECT_EQ(cache.getGeneration(pageID), 1);
    EXPECT_EQ(cache.getPageState(pageID) == LumenSurfaceCachePageState::Touched, true);
}

CPU_TEST(LumenSurfaceCache_BudgetBoundary)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, kTestBudgetBytes, kTestResidencyFrames);
    LumenGIStats stats;
    stats.memoryBudgetBytes = kTestBudgetBytes;
    const auto syncBudget = [&cache, &stats]() { stats.resources = cache.toResourceStats(); };

    // 10 pages fill the budget exactly.
    for (uint32_t i = 0; i < 10; ++i)
    {
        EXPECT_NE(cache.allocatePage(), kInvalidPageID);
        syncBudget();
        EXPECT_TRUE(stats.isWithinMemoryBudget());
        EXPECT_EQ(stats.getBudgetExcessBytes(), 0);
    }
    EXPECT_EQ(cache.getResidentBytes(), kTestBudgetBytes);

    // An 11th allocation exceeds the budget, but no page is evictable yet (min residency),
    // so the cache stays temporarily over budget instead of evicting protected pages.
    EXPECT_NE(cache.allocatePage(), kInvalidPageID);
    syncBudget();
    EXPECT_FALSE(stats.isWithinMemoryBudget());
    EXPECT_EQ(stats.getBudgetExcessBytes(), kLumenSurfaceCacheBytesPerPage);
    EXPECT_EQ(cache.getStats().evictionCount, 0);

    // Once the residency window passes, the next allocation evicts the two LRU pages (1 and
    // 2) and returns to the budget within the same call.
    for (uint32_t frame = 0; frame < kTestResidencyFrames; ++frame)
    {
        cache.endFrame();
    }
    EXPECT_NE(cache.allocatePage(), kInvalidPageID);
    syncBudget();
    EXPECT_TRUE(stats.isWithinMemoryBudget());
    EXPECT_EQ(cache.getResidentBytes(), kTestBudgetBytes);
    EXPECT_EQ(cache.getStats().evictionCount, 2);
    EXPECT_FALSE(cache.isPageAllocated(1));
    EXPECT_FALSE(cache.isPageAllocated(2));
    EXPECT_TRUE(cache.isPageAllocated(3));

    // Evicted pages are EvictedPending until endFrame, then Free and reusable.
    EXPECT_EQ(cache.getPageState(1) == LumenSurfaceCachePageState::EvictedPending, true);
    EXPECT_EQ(cache.getFreeCount(), 4);
    EXPECT_EQ(cache.getAllocatedPageCount(), 10);
    cache.endFrame();
    EXPECT_EQ(cache.getPageState(1) == LumenSurfaceCachePageState::Free, true);
    EXPECT_EQ(cache.getFreeCount(), 6);
}

CPU_TEST(LumenSurfaceCache_ZeroBudgetMeansUnlimited)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, 0, kTestResidencyFrames);
    EXPECT_EQ(cache.getMemoryBudgetBytes(), 0);
    for (uint32_t i = 0; i < kSmallAtlasPageCount; ++i)
    {
        EXPECT_NE(cache.allocatePage(), kInvalidPageID);
    }
    EXPECT_EQ(cache.getStats().evictionCount, 0);
    EXPECT_EQ(cache.getResidentBytes(), static_cast<uint64_t>(kSmallAtlasPageCount) * kLumenSurfaceCacheBytesPerPage);

    // Matches LumenGIStats semantics: a zero budget means none is configured, so the frame
    // is always considered within budget.
    LumenGIStats stats;
    stats.memoryBudgetBytes = 0;
    stats.resources = cache.toResourceStats();
    EXPECT_FALSE(stats.hasMemoryBudget());
    EXPECT_TRUE(stats.isWithinMemoryBudget());
}

CPU_TEST(LumenSurfaceCache_InvalidHandlesAreNoOps)
{
    LumenSurfaceCache cache(kSmallAtlasSizeTexels, 0, kTestResidencyFrames);
    EXPECT_EQ(cache.getPageCount(), kSmallAtlasPageCount);

    const LumenSurfaceCacheCoord outOfRange = cache.getPageAtlasCoord(kInvalidPageID);
    EXPECT_EQ(outOfRange.atlasX, kInvalidSurfaceCacheCoord);
    EXPECT_EQ(outOfRange.atlasY, kInvalidSurfaceCacheCoord);
    EXPECT_EQ(cache.getPageState(kInvalidPageID) == LumenSurfaceCachePageState::Free, true);
    EXPECT_FALSE(cache.isPageAllocated(kInvalidPageID));
    EXPECT_FALSE(cache.releasePage(kInvalidPageID));
    EXPECT_FALSE(cache.touchPage(kInvalidPageID));
    EXPECT_EQ(cache.getGeneration(kInvalidPageID), 0);
    EXPECT_FALSE(cache.releasePage(kSmallAtlasPageCount + 1)); //< Beyond this atlas capacity.
    EXPECT_EQ(cache.getPageAtlasCoord(kSmallAtlasPageCount + 1).atlasX, kInvalidSurfaceCacheCoord);

    // A released page is no longer touchable and reports Free.
    const uint32_t pageID = cache.allocatePage();
    EXPECT_TRUE(cache.releasePage(pageID));
    EXPECT_FALSE(cache.touchPage(pageID));
    EXPECT_FALSE(cache.isPageAllocated(pageID));
    EXPECT_EQ(cache.getPageState(pageID) == LumenSurfaceCachePageState::Free, true);
    EXPECT_EQ(cache.getStats().touchCount, 0);
}

} // namespace Falcor
