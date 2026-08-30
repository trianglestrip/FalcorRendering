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
#pragma once

#include "../LumenGIStats.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace Falcor
{
/**
 * @brief Surface Cache tile-atlas page allocator (pure CPU, no GPU or Device dependency).
 *
 * Owned by Wave S2.1 Agent B. The GPU atlases (material/radiance/metadata) are a fixed grid
 * of square tiles ("pages"); this class tracks which pages are resident, hands out page IDs
 * from a free-list, updates LRU bookkeeping, evicts the least-recently-used pages when the
 * atlas is full or the memory budget is exceeded, and publishes stats compatible with
 * LumenGIResourceStats. The actual GPU textures are owned by the root integration pass;
 * this allocator never touches them.
 *
 * Thread safety: NOT thread-safe. Call all methods from a single thread (the render loop).
 *
 * Memory model: the page table is an array of per-page entries indexed by page ID
 * (O(1) lookup). The free-list is a LIFO stack. LRU eviction scans the page table linearly
 * (O(page count) per eviction); this is acceptable for a CPU-side allocator at the planned
 * atlas sizes and keeps behavior fully deterministic.
 */

///< Texels per page side. Every atlas tile is kLumenSurfaceCacheTileSize x kLumenSurfaceCacheTileSize texels.
constexpr uint32_t kLumenSurfaceCacheTileSize = 16;

///< Default atlas size in texels per side (square). The default yields 256x256 = 65536 pages.
constexpr uint32_t kLumenSurfaceCacheDefaultAtlasSize = 4096;

///< Default atlas size in pages per side: kLumenSurfaceCacheDefaultAtlasSize / kLumenSurfaceCacheTileSize.
constexpr uint32_t kLumenSurfaceCacheDefaultPagesPerSide = kLumenSurfaceCacheDefaultAtlasSize / kLumenSurfaceCacheTileSize;

///< Default memory budget for the three Surface Cache atlases (512 MiB, roadmap target).
constexpr uint64_t kLumenSurfaceCacheDefaultMemoryBudgetBytes = 512ull * 1024ull * 1024ull;

///< Minimum number of frames a page stays resident after its last touch before it becomes evictable.
constexpr uint32_t kLumenMinResidencyFrames = 60;

///< Byte size per texel of the material atlas (RGBA8: base color + opacity). Shared with Agent C.
constexpr uint32_t kLumenSurfaceCacheMaterialBytesPerTexel = 4;

///< Byte size per texel of the radiance atlas (RGBA16F: direct + indirect radiance).
constexpr uint32_t kLumenSurfaceCacheRadianceBytesPerTexel = 8;

///< Byte size per texel of the metadata atlas (depth, validity and normal octahedral: 8).
constexpr uint32_t kLumenSurfaceCacheMetadataBytesPerTexel = 8;

///< Bytes of GPU memory one resident page occupies across all three atlases.
constexpr uint64_t kLumenSurfaceCacheBytesPerPage =
    static_cast<uint64_t>(kLumenSurfaceCacheTileSize) * kLumenSurfaceCacheTileSize *
    (kLumenSurfaceCacheMaterialBytesPerTexel + kLumenSurfaceCacheRadianceBytesPerTexel + kLumenSurfaceCacheMetadataBytesPerTexel);

///< Page ID of an invalid/unallocated page. Valid page IDs are 1..getPageCount().
constexpr uint32_t kInvalidPageID = 0;

/**
 * @brief Atlas layout and page ID contract (shared with Agent A card->page mapping and
 * Agent C capture writes; root freezes it before GPU integration).
 *
 * The atlas is a square grid of kLumenSurfaceCacheTileSize x kLumenSurfaceCacheTileSize
 * tiles. For a grid of N x N tiles the page ID of tile (tileX, tileY) is:
 *
 *     pageID = tileY * N + tileX + 1, with 0 <= tileX, tileY < N
 *
 * and the inverse (this is what getPageAtlasCoord returns):
 *
 *     tileX = (pageID - 1) % N
 *     tileY = (pageID - 1) / N
 *
 * The texel offset of a page inside the atlas is:
 *
 *     texelX = tileX * kLumenSurfaceCacheTileSize
 *     texelY = tileY * kLumenSurfaceCacheTileSize
 *
 * The allocator guarantees this mapping for the current atlas size (page IDs are never
 * renumbered while the atlas size is unchanged). resize() re-derives coordinates from the
 * formula for the new size, so any cached GPU contents are logically stale after a resize;
 * the caller must re-capture. The coordinate is a property of the page ID, not of the page
 * state: getPageAtlasCoord returns the formula coordinate for any valid page ID, including
 * free pages.
 */

///< Page lifecycle state machine (shared contract).
///
///     allocatePage()   +------------+  touchPage()   +---------+
///   +----------------->| Allocated  |--------------->| Touched |
///   |                  +------------+                +---------+
///   |                        |  eviction (LRU)             |  eviction (LRU)
///   |                        v                             v
///   |                  +----------------+   endFrame()  +-------+
///   |                  | EvictedPending |------------->| Free  |
///   |                  +----------------+              +---+---+
///   |                     releasePage()                    |
///   +------------------------------------------------------+
///
/// - Free: page is in the free-list and can be handed out. Free pages do not count towards
///   the resident stats.
/// - Allocated: page is owned by a caller and may be evicted once it is outside its minimum
///   residency window.
/// - Touched: allocated and touched in the current frame. Touched pages are never evicted.
/// - EvictedPending: evicted this frame by the allocator. Still occupies an atlas tile until
///   the GPU has finished reading it; the allocator returns it to the free-list at the next
///   endFrame(). EvictedPending pages count as neither allocated nor free. The root pass must
///   drop its card->page references for EvictedPending/Free pages (query getPageState after
///   endFrame() each frame).
///
/// releasePage() and eviction both lead to Free: release makes the page immediately reusable,
/// eviction defers reuse to the next endFrame() so in-flight GPU reads finish first.
/// allocatePage() has two shortcuts that skip the Free state: when the free-list is empty it
/// evicts a page and immediately reallocates it (evict-and-reuse), and when pages are already
/// EvictedPending it reuses one of them before evicting anything else.
enum class LumenSurfaceCachePageState
{
    Free,
    Allocated,
    Touched,
    EvictedPending,
};

///< Sentinel for an out-of-range page ID in LumenSurfaceCacheCoord.
constexpr uint32_t kInvalidSurfaceCacheCoord = std::numeric_limits<uint32_t>::max();

///< Atlas tile coordinates returned by getPageAtlasCoord.
struct LumenSurfaceCacheCoord
{
    uint32_t atlasX = kInvalidSurfaceCacheCoord; ///< Tile column in the atlas grid.
    uint32_t atlasY = kInvalidSurfaceCacheCoord; ///< Tile row in the atlas grid.
};

///< Snapshot of allocator state and counters (see getStats()).
struct LumenSurfaceCacheStats
{
    uint32_t pageCount = 0;            ///< Total pages in the atlas (capacity).
    uint32_t allocatedPageCount = 0;   ///< Pages in Allocated or Touched state.
    uint32_t freePageCount = 0;        ///< Pages in the free-list (Free state).
    uint32_t evictedPendingCount = 0;  ///< Pages in EvictedPending state.
    uint64_t residentBytes = 0;        ///< GPU bytes of allocated pages across all three atlases.
    uint64_t memoryBudgetBytes = 0;    ///< Current budget; 0 means unlimited.
    uint64_t frameIndex = 0;           ///< Frames elapsed since construction/reset.
    uint64_t allocationCount = 0;      ///< Total allocatePage() successes.
    uint64_t releaseCount = 0;         ///< Total explicit releasePage() calls that succeeded.
    uint64_t evictionCount = 0;        ///< Total pages evicted by LRU/budget passes.
    uint64_t invalidationCount = 0;    ///< Total pages dropped by resize() shrinking.
    uint64_t touchCount = 0;           ///< Total successful touchPage() calls.
    uint32_t minResidencyFrames = 0;   ///< Minimum residency frames in effect.
    uint32_t lastAllocatedPageID = kInvalidPageID; ///< Most recent allocation/reuse page.
    uint32_t lastAllocatedGeneration = 0;          ///< Generation assigned by that allocation.
    uint64_t lastAllocatedFrame = 0;               ///< Allocator frame of that allocation.
    uint32_t lastEvictedPageID = kInvalidPageID;   ///< Most recent LRU/budget victim.
    uint32_t lastEvictedGeneration = 0;             ///< Victim generation before eviction.
    uint64_t lastEvictedFrame = 0;                  ///< Allocator frame of that eviction.
    uint32_t lastTouchedPageID = kInvalidPageID;    ///< Most recent successful touch.
    uint64_t lastTouchedFrame = 0;                  ///< Allocator frame of that touch.
};

/**
 * @brief Fixed tile-atlas page allocator for the Surface Cache.
 *
 * Allocation strategy: single LIFO free-list of page IDs. While the free-list is untouched
 * by releases, allocation hands out page IDs in ascending order (1, 2, 3, ...) so the
 * pageID -> tile mapping stays predictable. Releasing a page pushes it on top of the stack,
 * so freed pages are reused before never-used ones. Deterministic by construction; no
 * randomness anywhere.
 *
 * Eviction strategy: when the free-list is empty, allocatePage() evicts the page with the
 * smallest lastTouchedFrame among evictable pages (ties broken by smallest page ID) and
 * immediately reuses it. A page is evictable only when it is allocated and
 * frameIndex - lastTouchedFrame >= minResidencyFrames, i.e. it was not touched within the
 * last minResidencyFrames frames. If no page is evictable, allocation fails.
 *
 * Memory budget: if memoryBudgetBytes != 0, every successful allocation is followed by a
 * budget pass that evicts evictable pages (same LRU order) until residentBytes is within
 * budget. If all pages are inside their residency window the pass stops and the cache may
 * temporarily exceed the budget; the next allocation after the pages age out restores it.
 * A budget of 0 means unlimited (matches LumenGIStats::hasMemoryBudget semantics).
 */
class LumenSurfaceCache
{
public:
    /**
     * @brief Create an allocator over a square atlas.
     * @param atlasSizeTexels Atlas side length in texels. Rounded DOWN to the largest
     * multiple of kLumenSurfaceCacheTileSize that is at least one tile.
     * @param memoryBudgetBytes Combined GPU budget of the three atlases; 0 means unlimited.
     * @param minResidencyFrames Minimum residency window; clamped to at least 1.
     */
    explicit LumenSurfaceCache(
        uint32_t atlasSizeTexels = kLumenSurfaceCacheDefaultAtlasSize,
        uint64_t memoryBudgetBytes = kLumenSurfaceCacheDefaultMemoryBudgetBytes,
        uint32_t minResidencyFrames = kLumenMinResidencyFrames);

    /**
     * @brief Allocate a page.
     * @return A valid page ID (1..getPageCount()) on success, kInvalidPageID if the atlas is
     * full and no evictable page exists. On success the page state is Touched, its generation
     * is incremented, and the allocation may have caused budget-pass evictions.
     */
    uint32_t allocatePage();

    /**
     * @brief Explicitly release a page, returning it to the free-list immediately.
     * @return False (no-op) if the page ID is invalid or the page is not Allocated/Touched.
     */
    bool releasePage(uint32_t pageID);

    /**
     * @brief Mark a page as used in the current frame (LRU update).
     * @return False (no-op) if the page ID is invalid or the page is not Allocated/Touched.
     */
    bool touchPage(uint32_t pageID);

    /**
     * @brief Tile coordinate of a page per the layout contract.
     * @return The formula coordinate for any valid page ID regardless of state;
     * kInvalidSurfaceCacheCoord in both fields for pageID 0 or out of range.
     */
    LumenSurfaceCacheCoord getPageAtlasCoord(uint32_t pageID) const;

    ///< Lifecycle state of a page (see the state machine diagram above).
    LumenSurfaceCachePageState getPageState(uint32_t pageID) const;

    ///< True if the page is Allocated or Touched.
    bool isPageAllocated(uint32_t pageID) const;

    ///< Number of allocation epochs a page went through. Incremented on every (re)allocation,
    ///< saturates at UINT32_MAX, preserved across resize(), cleared by reset().
    uint32_t getGeneration(uint32_t pageID) const;

    ///< Total pages in the atlas (capacity, 0 when no page ID is valid).
    uint32_t getPageCount() const;

    ///< Pages in Allocated or Touched state.
    uint32_t getAllocatedPageCount() const;

    ///< Pages currently in the free-list (Free state; excludes EvictedPending).
    uint32_t getFreeCount() const;

    ///< GPU bytes of all allocated pages (getAllocatedPageCount() * kLumenSurfaceCacheBytesPerPage).
    uint64_t getResidentBytes() const;

    ///< Current memory budget; 0 means unlimited.
    uint64_t getMemoryBudgetBytes() const;

    ///< Set a new budget (0 = unlimited). Enforcement happens on the next allocatePage().
    void setMemoryBudgetBytes(uint64_t memoryBudgetBytes);

    ///< Advance the frame counter. Also flushes EvictedPending pages to the free-list.
    void endFrame();

    ///< Current frame index (incremented by endFrame()).
    uint64_t getFrameIndex() const;

    /**
     * @brief Resize the atlas to atlasTileCount x atlasTileCount pages (full remap).
     *
     * Page IDs are preserved for pages that fit in the new grid and their coordinates are
     * re-derived from the layout formula for the new size; pages whose ID exceeds the new
     * capacity are invalidated (state Free, dropped from the table). All GPU contents are
     * logically stale after a resize and must be re-captured by the caller. Generations are
     * preserved. The free-list is rebuilt in ascending order.
     * @return False if atlasTileCount is 0 or the resulting page count overflows uint32_t.
     */
    bool resize(uint32_t atlasTileCount);

    /**
     * @brief Reset the allocator to a freshly constructed state.
     *
     * Keeps the current atlas size but clears every page, the free-list (rebuilt ascending),
     * generations, counters and the frame index. Used on scene reload.
     */
    void reset();

    ///< Snapshot of all counters and derived state (see LumenSurfaceCacheStats).
    LumenSurfaceCacheStats getStats() const;

    /**
     * @brief Map the current residency to LumenGIResourceStats.
     *
     * surfaceCacheMaterialBytes/RadianceBytes/MetadataBytes are filled from the allocated
     * page count times the per-texel sizes of each atlas. All other fields stay zero. The
     * result is directly usable with LumenGIStats::isWithinMemoryBudget.
     */
    LumenGIResourceStats toResourceStats() const;

private:
    struct Page
    {
        uint32_t generation = 0;      ///< Allocation epochs (see getGeneration).
        uint64_t lastTouchedFrame = 0; ///< Frame of the last allocation/touch.
        bool allocated = false;       ///< Owned by a caller (Allocated or Touched).
    };

    uint32_t normalizeTileCount(uint64_t tileCountPerSide) const;

    ///< Smallest evictable page ID, or kInvalidPageID if none is evictable.
    uint32_t findEvictionCandidate() const;

    ///< Evict a page: mark EvictedPending and add it to the pending list.
    void evictPage(uint32_t pageID);

    ///< Evict evictable pages until residentBytes <= budget, or no evictable page remains.
    void enforceMemoryBudget();

    ///< Coordinate of a page ID for a given grid size (layout formula, no validation).
    static LumenSurfaceCacheCoord computeCoord(uint32_t pageID, uint32_t tileCountPerSide);

    std::vector<Page> mPages;                  ///< Page table; index 0 unused, pageID = index.
    std::vector<uint32_t> mFreeList;           ///< LIFO stack of free page IDs (ascending order build).
    std::vector<uint32_t> mEvictedPending;     ///< Page IDs evicted this frame, flushed by endFrame().
    uint32_t mTileCountPerSide = 0;            ///< Current grid size in pages per side.
    uint64_t mMemoryBudgetBytes = 0;           ///< Budget; 0 = unlimited.
    uint32_t mMinResidencyFrames = 1;          ///< Minimum residency window.
    uint64_t mFrameIndex = 0;                  ///< Frames elapsed.
    uint64_t mAllocationCount = 0;             ///< Successful allocations.
    uint64_t mReleaseCount = 0;                ///< Successful releases.
    uint64_t mEvictionCount = 0;               ///< Evicted pages (LRU/budget passes).
    uint64_t mInvalidationCount = 0;           ///< Pages dropped by resize() shrinking.
    uint64_t mTouchCount = 0;                  ///< Successful touches.
    uint32_t mLastAllocatedPageID = kInvalidPageID;
    uint32_t mLastAllocatedGeneration = 0;
    uint64_t mLastAllocatedFrame = 0;
    uint32_t mLastEvictedPageID = kInvalidPageID;
    uint32_t mLastEvictedGeneration = 0;
    uint64_t mLastEvictedFrame = 0;
    uint32_t mLastTouchedPageID = kInvalidPageID;
    uint64_t mLastTouchedFrame = 0;
};

inline LumenSurfaceCache::LumenSurfaceCache(
    uint32_t atlasSizeTexels,
    uint64_t memoryBudgetBytes,
    uint32_t minResidencyFrames)
    : mMemoryBudgetBytes(memoryBudgetBytes)
    , mMinResidencyFrames(std::max<uint32_t>(1, minResidencyFrames))
{
    // Round down to the largest multiple of the tile size (at least one tile).
    const uint32_t tileCountPerSide = normalizeTileCount(atlasSizeTexels / kLumenSurfaceCacheTileSize);
    mTileCountPerSide = tileCountPerSide;
    const uint32_t total = tileCountPerSide * tileCountPerSide;
    mPages.assign(static_cast<size_t>(total) + 1, Page{});
    // Build the free-list in descending order so pop_back() hands out ascending IDs.
    for (uint32_t pageID = total; pageID >= 1; --pageID)
    {
        mFreeList.push_back(pageID);
    }
}

inline uint32_t LumenSurfaceCache::allocatePage()
{
    uint32_t pageID = kInvalidPageID;
    if (!mFreeList.empty())
    {
        pageID = mFreeList.back();
        mFreeList.pop_back();
    }
    else if (!mEvictedPending.empty())
    {
        // Reuse a page evicted earlier this frame before evicting another one.
        pageID = mEvictedPending.back();
        mEvictedPending.pop_back();
    }
    else
    {
        // Atlas full: evict the least-recently-used evictable page and reuse it.
        const uint32_t victim = findEvictionCandidate();
        if (victim == kInvalidPageID)
        {
            return kInvalidPageID;
        }
        evictPage(victim);
        pageID = mEvictedPending.back();
        mEvictedPending.pop_back();
    }

    Page& page = mPages[pageID];
    if (page.generation != std::numeric_limits<uint32_t>::max())
    {
        ++page.generation;
    }
    page.lastTouchedFrame = mFrameIndex;
    page.allocated = true;
    ++mAllocationCount;
    mLastAllocatedPageID = pageID;
    mLastAllocatedGeneration = page.generation;
    mLastAllocatedFrame = mFrameIndex;

    enforceMemoryBudget();
    return pageID;
}

inline bool LumenSurfaceCache::releasePage(uint32_t pageID)
{
    if (pageID == kInvalidPageID || pageID >= mPages.size())
    {
        return false;
    }
    Page& page = mPages[pageID];
    if (!page.allocated)
    {
        return false;
    }
    page.allocated = false;
    mFreeList.push_back(pageID);
    ++mReleaseCount;
    return true;
}

inline bool LumenSurfaceCache::touchPage(uint32_t pageID)
{
    if (pageID == kInvalidPageID || pageID >= mPages.size())
    {
        return false;
    }
    Page& page = mPages[pageID];
    if (!page.allocated)
    {
        return false;
    }
    page.lastTouchedFrame = mFrameIndex;
    ++mTouchCount;
    mLastTouchedPageID = pageID;
    mLastTouchedFrame = mFrameIndex;
    return true;
}

inline LumenSurfaceCacheCoord LumenSurfaceCache::getPageAtlasCoord(uint32_t pageID) const
{
    if (pageID == kInvalidPageID || pageID >= mPages.size())
    {
        return LumenSurfaceCacheCoord{};
    }
    return computeCoord(pageID, mTileCountPerSide);
}

inline LumenSurfaceCachePageState LumenSurfaceCache::getPageState(uint32_t pageID) const
{
    if (pageID == kInvalidPageID || pageID >= mPages.size())
    {
        return LumenSurfaceCachePageState::Free;
    }
    for (uint32_t pending : mEvictedPending)
    {
        if (pending == pageID)
        {
            return LumenSurfaceCachePageState::EvictedPending;
        }
    }
    const Page& page = mPages[pageID];
    if (!page.allocated)
    {
        return LumenSurfaceCachePageState::Free;
    }
    return page.lastTouchedFrame == mFrameIndex ? LumenSurfaceCachePageState::Touched
                                                : LumenSurfaceCachePageState::Allocated;
}

inline bool LumenSurfaceCache::isPageAllocated(uint32_t pageID) const
{
    if (pageID == kInvalidPageID || pageID >= mPages.size())
    {
        return false;
    }
    return mPages[pageID].allocated;
}

inline uint32_t LumenSurfaceCache::getGeneration(uint32_t pageID) const
{
    if (pageID == kInvalidPageID || pageID >= mPages.size())
    {
        return 0;
    }
    return mPages[pageID].generation;
}

inline uint32_t LumenSurfaceCache::getPageCount() const
{
    return static_cast<uint32_t>(mPages.size() - 1);
}

inline uint32_t LumenSurfaceCache::getAllocatedPageCount() const
{
    return getPageCount() - getFreeCount() - static_cast<uint32_t>(mEvictedPending.size());
}

inline uint32_t LumenSurfaceCache::getFreeCount() const
{
    return static_cast<uint32_t>(mFreeList.size());
}

inline uint64_t LumenSurfaceCache::getResidentBytes() const
{
    return static_cast<uint64_t>(getAllocatedPageCount()) * kLumenSurfaceCacheBytesPerPage;
}

inline uint64_t LumenSurfaceCache::getMemoryBudgetBytes() const
{
    return mMemoryBudgetBytes;
}

inline void LumenSurfaceCache::setMemoryBudgetBytes(uint64_t memoryBudgetBytes)
{
    mMemoryBudgetBytes = memoryBudgetBytes;
}

inline void LumenSurfaceCache::endFrame()
{
    ++mFrameIndex;
    for (uint32_t pageID : mEvictedPending)
    {
        mFreeList.push_back(pageID);
    }
    mEvictedPending.clear();
}

inline uint64_t LumenSurfaceCache::getFrameIndex() const
{
    return mFrameIndex;
}

inline bool LumenSurfaceCache::resize(uint32_t atlasTileCount)
{
    if (atlasTileCount == 0)
    {
        return false;
    }
    const uint64_t newTotal64 = static_cast<uint64_t>(atlasTileCount) * atlasTileCount;
    if (newTotal64 > std::numeric_limits<uint32_t>::max() - 1)
    {
        return false;
    }
    const uint32_t newTotal = static_cast<uint32_t>(newTotal64);
    const uint32_t oldTotal = getPageCount();

    // Pages beyond the new capacity are dropped (evicted by resize, not by LRU).
    for (uint32_t pageID = newTotal + 1; pageID <= oldTotal; ++pageID)
    {
        Page& page = mPages[pageID];
        if (page.allocated)
        {
            page.allocated = false;
            ++mInvalidationCount;
        }
    }

    // Pending evictions that fit survive and are flushed to the free-list; the rest are dropped.
    for (uint32_t pageID : mEvictedPending)
    {
        if (pageID <= newTotal)
        {
            mFreeList.push_back(pageID);
        }
        else
        {
            ++mInvalidationCount;
        }
    }
    mEvictedPending.clear();

    // Keep only free-list entries that still fit; add the newly created pages ascending.
    mFreeList.erase(
        std::remove_if(
            mFreeList.begin(),
            mFreeList.end(),
            [newTotal](uint32_t pageID) { return pageID > newTotal; }),
        mFreeList.end());
    for (uint32_t pageID = newTotal; pageID > oldTotal; --pageID)
    {
        mFreeList.push_back(pageID);
    }

    mPages.resize(static_cast<size_t>(newTotal) + 1);
    mTileCountPerSide = atlasTileCount;
    return true;
}

inline void LumenSurfaceCache::reset()
{
    const uint32_t total = getPageCount();
    mPages.assign(static_cast<size_t>(total) + 1, Page{});
    mFreeList.clear();
    for (uint32_t pageID = total; pageID >= 1; --pageID)
    {
        mFreeList.push_back(pageID);
    }
    mEvictedPending.clear();
    mFrameIndex = 0;
    mAllocationCount = 0;
    mReleaseCount = 0;
    mEvictionCount = 0;
    mInvalidationCount = 0;
    mTouchCount = 0;
    mLastAllocatedPageID = kInvalidPageID;
    mLastAllocatedGeneration = 0;
    mLastAllocatedFrame = 0;
    mLastEvictedPageID = kInvalidPageID;
    mLastEvictedGeneration = 0;
    mLastEvictedFrame = 0;
    mLastTouchedPageID = kInvalidPageID;
    mLastTouchedFrame = 0;
}

inline LumenSurfaceCacheStats LumenSurfaceCache::getStats() const
{
    LumenSurfaceCacheStats stats;
    stats.pageCount = getPageCount();
    stats.allocatedPageCount = getAllocatedPageCount();
    stats.freePageCount = getFreeCount();
    stats.evictedPendingCount = static_cast<uint32_t>(mEvictedPending.size());
    stats.residentBytes = getResidentBytes();
    stats.memoryBudgetBytes = mMemoryBudgetBytes;
    stats.frameIndex = mFrameIndex;
    stats.allocationCount = mAllocationCount;
    stats.releaseCount = mReleaseCount;
    stats.evictionCount = mEvictionCount;
    stats.invalidationCount = mInvalidationCount;
    stats.touchCount = mTouchCount;
    stats.minResidencyFrames = mMinResidencyFrames;
    stats.lastAllocatedPageID = mLastAllocatedPageID;
    stats.lastAllocatedGeneration = mLastAllocatedGeneration;
    stats.lastAllocatedFrame = mLastAllocatedFrame;
    stats.lastEvictedPageID = mLastEvictedPageID;
    stats.lastEvictedGeneration = mLastEvictedGeneration;
    stats.lastEvictedFrame = mLastEvictedFrame;
    stats.lastTouchedPageID = mLastTouchedPageID;
    stats.lastTouchedFrame = mLastTouchedFrame;
    return stats;
}

inline LumenGIResourceStats LumenSurfaceCache::toResourceStats() const
{
    LumenGIResourceStats stats;
    const uint64_t texelCount =
        static_cast<uint64_t>(getAllocatedPageCount()) * kLumenSurfaceCacheTileSize * kLumenSurfaceCacheTileSize;
    stats.surfaceCacheMaterialBytes = texelCount * kLumenSurfaceCacheMaterialBytesPerTexel;
    stats.surfaceCacheRadianceBytes = texelCount * kLumenSurfaceCacheRadianceBytesPerTexel;
    stats.surfaceCacheMetadataBytes = texelCount * kLumenSurfaceCacheMetadataBytesPerTexel;
    return stats;
}

inline uint32_t LumenSurfaceCache::normalizeTileCount(uint64_t tileCountPerSide) const
{
    // 65535 * 65535 = 4294836225, the largest square that fits in 32-bit page IDs.
    constexpr uint64_t kMaxTileCount = 65535;
    tileCountPerSide = std::max<uint64_t>(1, tileCountPerSide);
    tileCountPerSide = std::min(tileCountPerSide, kMaxTileCount);
    return static_cast<uint32_t>(tileCountPerSide);
}

inline uint32_t LumenSurfaceCache::findEvictionCandidate() const
{
    uint32_t candidate = kInvalidPageID;
    uint64_t oldestTouch = 0;
    for (size_t i = 1; i < mPages.size(); ++i)
    {
        const Page& page = mPages[i];
        if (!page.allocated)
        {
            continue;
        }
        // Min residency: pages touched within the last mMinResidencyFrames frames stay resident.
        if (mFrameIndex - page.lastTouchedFrame < mMinResidencyFrames)
        {
            continue;
        }
        if (candidate == kInvalidPageID || page.lastTouchedFrame < oldestTouch)
        {
            candidate = static_cast<uint32_t>(i);
            oldestTouch = page.lastTouchedFrame;
        }
    }
    return candidate;
}

inline void LumenSurfaceCache::evictPage(uint32_t pageID)
{
    mLastEvictedPageID = pageID;
    mLastEvictedGeneration = mPages[pageID].generation;
    mLastEvictedFrame = mFrameIndex;
    mPages[pageID].allocated = false;
    mEvictedPending.push_back(pageID);
    ++mEvictionCount;
}

inline void LumenSurfaceCache::enforceMemoryBudget()
{
    if (mMemoryBudgetBytes == 0)
    {
        return;
    }
    while (getResidentBytes() > mMemoryBudgetBytes)
    {
        const uint32_t victim = findEvictionCandidate();
        if (victim == kInvalidPageID)
        {
            // Everything left is inside its residency window; stay temporarily over budget.
            return;
        }
        evictPage(victim);
    }
}

inline LumenSurfaceCacheCoord LumenSurfaceCache::computeCoord(uint32_t pageID, uint32_t tileCountPerSide)
{
    LumenSurfaceCacheCoord coord;
    coord.atlasX = (pageID - 1) % tileCountPerSide;
    coord.atlasY = (pageID - 1) / tileCountPerSide;
    return coord;
}
} // namespace Falcor
