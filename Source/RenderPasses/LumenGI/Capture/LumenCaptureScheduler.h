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

#include "../Cards/LumenCardScene.h"
#include "../SurfaceCache/LumenSurfaceCache.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Falcor
{
/**
 * @brief Per-frame Surface Cache capture scheduler (pure CPU, no GPU/Device dependency).
 *
 * Owned by Wave S2.2 Agent A. This class orchestrates the "dirty card -> page allocation ->
 * per-frame capture budget -> touch/release -> invalidation propagation" pipeline on the CPU.
 * It creates no GPU resources and mutates no existing component: it reads dirty state from a
 * card scene, hands out pages through a page cache, and emits capture commands that the root
 * pass feeds to the capture pass (Agent B's LumenCardCapture shader). The scheduler never
 * touches the atlas textures.
 *
 * Duck-typed inputs (the scheduler is a template; the production instantiation
 * LumenCaptureSchedulerForScene binds the real components directly):
 *
 *   TCardScene (LumenCardScene satisfies all requirements):
 *     - uint32_t getCardCount() const
 *     - const LumenCard& getCard(uint32_t cardIndex) const        (priority, instanceID, dirtyFlags)
 *     - int32_t getInstanceIndex(uint32_t sceneInstanceID) const
 *     - const LumenCardInstance& getInstance(uint32_t ordinal) const  (unsupportedReasons)
 *     - const std::vector<uint32_t>& getDirtyCardIndices() const  (priority descending)
 *     - void clearDirty(uint32_t cardIndex)
 *
 *   TPageCache (LumenSurfaceCache satisfies all requirements):
 *     - uint32_t allocatePage()
 *     - bool releasePage(uint32_t pageID)
 *     - bool touchPage(uint32_t pageID)
 *     - bool isPageAllocated(uint32_t pageID) const
 *     - uint32_t getGeneration(uint32_t pageID) const
 *     - void endFrame()
 *
 * Per-frame contract with the root pass:
 *   1. root calls cardScene->update(pRenderContext, flags), then scheduleFrame(flags);
 *   2. root runs the capture pass over frame.commands (cardIndex/pageID pairs);
 *   3. root calls completeCaptures(frame.commands) once the capture pass has executed;
 *   4. next frame starts over. The scheduler owns the page cache's frame tick: endFrame()
 *      is called inside scheduleFrame(), the root must NOT call it separately.
 *
 * Invalidation propagation (aligned with the S2-A2 contract):
 * - The card scene maps IScene::UpdateFlags to per-card dirty flags. The scheduler consumes
 *   the dirty list as the authoritative capture work source; UpdateFlags are accepted for
 *   API symmetry and diagnostics (lastUpdateFlags, structuralRebuildCount) only.
 * - Material/emissive/displacement/bounds changes re-capture the affected cards onto their
 *   EXISTING pages: pages are never released for invalidation, the capture pass overwrites
 *   them in place.
 * - Geometry/Meshes rebuilds are reconciled every frame: cards that disappeared or whose
 *   instance became unsupported have their pages explicitly released; surviving card indices
 *   keep their pages and are re-captured through the dirty list.
 * - Pages evicted by the page cache (atlas pressure/resize) are detected by a generation
 *   check and the affected cards are re-queued internally (needsCapture) for re-allocation
 *   and re-capture. The scheduler can re-queue cards without a public "mark dirty" API on
 *   the card scene.
 * - Scene reload/reset: the root calls reset() when it swaps the scene. reset() releases
 *   every page held by the scheduler and clears the whole state machine. If the root also
 *   resets the page cache, both orders of (scheduler.reset(), cache.reset()) are safe.
 *
 * Determinism: no randomness, no hash tables; all containers are vectors, all orders derive
 * from the scene's priority order with cardIndex tie-breaks, and all page IDs come from the
 * deterministic free-list/LRU of the page cache. Two runs of the same input sequence produce
 * identical command streams and statistics.
 *
 * Thread safety: NOT thread-safe. Call all methods from a single thread (the render loop).
 */

///< Default per-frame capture budget in pages. The page count is the time budget proxy: the
///< capture pass cost is proportional to the number of pages written, so capping pages caps
///< the per-frame GPU time spike (S2 gate: no unacceptable P99 spikes).
static constexpr uint32_t kLumenCaptureDefaultMaxPagesPerFrame = 64;

///< A capture command that stays uncompleted for more than this many frames is re-emitted.
///< Guards against stalls if the root drops a completion (e.g. capture pass skipped).
static constexpr uint32_t kLumenCaptureDefaultInFlightTimeoutFrames = 8;

// Prevent a high-priority dirty card from monopolizing a tiny capture budget forever.
// Once a card has waited this many scheduling passes it is promoted ahead of the normal
// geometric priority. This mirrors UE's starvation protection in the surface-cache request
// queue and is required for deterministic page pressure/eviction tests.
static constexpr uint32_t kLumenCaptureStarvationPromotionFrames = 8;

///< Per-card scheduling state machine maintained by the scheduler.
///
///<     (dirty in scene / needsCapture)              allocatePage() succeeds
///<   --------------------------------------> +-----------------------+  completeCaptures()
///<   (budget exhausted: card stays dirty,     |     Capturing        | +------------------+
///<    state stays NotTracked, re-queued       +-----------------------+ |                  v
///<    through the dirty list next frame)            ^     |             |              +---------+
///<   allocatePage() fails (atlas full)              |     | in-flight  |              | Resident|
///<   +----------------------------------------------+     | timeout    |              +---------+
///<   |                                                    |             |                  |
///<   v                                                    +------------>|                  |
///<   +------------------------+  retry next frame         re-emit       |   page evicted   |
///<   |    PendingAllocation   |-----------------------------------------+   / unsupported /
///<   +------------------------+   (allocation success)                  |   removed     v
///<   |                                                       NotTracked <-- release/lost
enum class LumenCaptureCardState : uint32_t
{
    NotTracked,        ///< No page assigned and nothing pending.
    PendingAllocation, ///< Page allocation failed (atlas full); retried every frame.
    Capturing,         ///< Page assigned; a capture command is in flight.
    Resident,          ///< Page assigned and the last capture completed.
};

///< One capture command handed to the root pass, which forwards it to the capture shader.
///< The root feeds every command of a frame back to completeCaptures() after the GPU work.
struct LumenCaptureCommand
{
    uint32_t cardIndex = kLumenCardInvalidID; ///< Card to capture (LumenCardScene index).
    uint32_t pageID = kInvalidPageID;         ///< Atlas page to write into.
    uint32_t generation = 0;                  ///< Page generation at emission (stale-command guard).
    uint32_t dirtyFlags = 0;                  ///< LumenCardDirtyFlags snapshot at emission.
};

///< Per-frame scheduler statistics (see scheduleFrame()).
struct LumenCaptureFrameStats
{
    uint32_t requestedCards = 0;     ///< Worklist size (dirty ∪ needsCapture ∪ pending ∪ timed-out in-flight).
    uint32_t captureCommands = 0;    ///< Commands emitted this frame (== commands.size()).
    uint32_t newPageAllocations = 0; ///< Commands that freshly allocated a page.
    uint32_t recaptureWithPage = 0;  ///< Commands reusing a resident page.
    uint32_t allocationFailures = 0; ///< allocatePage() failures this frame (atlas full).
    uint32_t budgetCappedCards = 0;  ///< Ready cards deferred by the per-frame budget.
    uint32_t inFlightCards = 0;      ///< Cards already captured, awaiting GPU completion.
    uint32_t pendingCards = 0;       ///< Cards in PendingAllocation at end of frame.
    uint32_t starvationFrames = 0;   ///< == pendingCards: card-frames spent waiting for a page.
    uint32_t releasedPages = 0;      ///< Pages released because their card vanished/unsupported.
    uint32_t lostPages = 0;          ///< Pages evicted/reallocated out from under a card.
    uint32_t touchCalls = 0;         ///< Explicit touchPage() calls (re-captures; allocations are implicitly touched).
    uint32_t lastCommandCardID = kLumenCardInvalidID; ///< Last emitted card ID, or invalid when no command was emitted.
    uint32_t lastCommandPageID = kInvalidPageID;      ///< Page of the last emitted command.
    uint32_t lastCommandGeneration = 0;               ///< Generation of the last emitted command.
};

///< Per-frame output of scheduleFrame(). Commands are sorted by capture priority
///< (descending, cardIndex tie-break) and limited to the frame budget.
struct LumenCaptureFrame
{
    std::vector<LumenCaptureCommand> commands;
    LumenCaptureFrameStats stats;
};

///< Cumulative scheduler statistics (see getStats()).
struct LumenCaptureSchedulerStats
{
    uint64_t frameIndex = 0;              ///< Frames scheduled since construction/reset.
    uint32_t lastUpdateFlags = 0;         ///< IScene::UpdateFlags of the last scheduleFrame().
    uint32_t maxPagesPerFrame = 0;        ///< Budget in effect.
    uint32_t inFlightTimeoutFrames = 0;   ///< Timeout in effect.
    uint64_t totalCaptureCommands = 0;    ///< Commands emitted.
    uint64_t totalAllocations = 0;        ///< Fresh page allocations.
    uint64_t totalRecaptures = 0;         ///< Re-captures onto resident pages.
    uint64_t totalAllocationFailures = 0; ///< Failed allocatePage() calls.
    uint64_t totalStarvationFrames = 0;   ///< Cumulative card-frames waiting for a page.
    uint64_t totalReleases = 0;           ///< Pages released (unsupported/removed cards).
    uint64_t totalLostPages = 0;          ///< Pages evicted out from under a card.
    uint64_t totalTouches = 0;            ///< Explicit touchPage() calls.
    uint64_t totalRequestDeduplications = 0; ///< Dirty/pending sources collapsed into one work item.
    uint64_t completedCaptures = 0;       ///< Commands completed through completeCaptures().
    double averageQueuedFrames = 0.0;     ///< Total queued frames / completed captures.
    uint32_t maxQueuedFrames = 0;         ///< Longest card queue wait (dirty -> completed).
    uint32_t pendingQueueDepth = 0;       ///< Cards in PendingAllocation right now.
    uint32_t maxPendingDepth = 0;         ///< Deepest pending queue ever reached.
    uint64_t structuralRebuildCount = 0;  ///< scheduleFrame() calls carrying Geometry/MeshesChanged.
    uint32_t lastCommandCardID = kLumenCardInvalidID; ///< Last emitted card ID.
    uint32_t lastCommandPageID = kInvalidPageID;      ///< Page of the last emitted command.
    uint32_t lastCommandGeneration = 0;               ///< Generation of the last emitted command.
};

/**
 * @brief Per-frame capture scheduler (pure CPU, deterministic, header-only).
 *
 * See the file-level documentation for the per-frame contract, the invalidation rules and
 * the duck-typed input requirements.
 */
template <typename TCardScene, typename TPageCache>
class LumenCaptureScheduler
{
public:
    /**
     * @param pCardScene Card scene providing dirty cards and card metadata. Must be non-null.
     * @param pPageCache Page allocator handing out atlas pages. Must be non-null.
     * @param maxPagesPerFrame Per-frame capture budget; clamped to at least 1.
     * @param inFlightTimeoutFrames Completion timeout; clamped to at least 1.
     */
    explicit LumenCaptureScheduler(
        TCardScene* pCardScene,
        TPageCache* pPageCache,
        uint32_t maxPagesPerFrame = kLumenCaptureDefaultMaxPagesPerFrame,
        uint32_t inFlightTimeoutFrames = kLumenCaptureDefaultInFlightTimeoutFrames)
        : mpCardScene(pCardScene)
        , mpPageCache(pPageCache)
        , mMaxPagesPerFrame(std::max<uint32_t>(1, maxPagesPerFrame))
        , mInFlightTimeoutFrames(std::max<uint32_t>(1, inFlightTimeoutFrames))
    {
    }

    /**
     * @brief Run one scheduling pass. Call AFTER the card scene was updated with the same
     * UpdateFlags (the root calls cardScene->update(pRenderContext, flags) first).
     *
     * Steps: reconcile the per-card page table against the current card scene and page cache
     * (release pages of vanished/unsupported cards, detect evicted pages), merge the scene
     * dirty list with internally re-queued cards, sort by priority, allocate pages within
     * the frame budget, touch the pages of the emitted commands, accumulate statistics and
     * advance the page cache frame (endFrame()). The returned commands must be handed to the
     * capture pass and fed back through completeCaptures().
     *
     * @return The capture commands for this frame plus per-frame statistics.
     */
    LumenCaptureFrame scheduleFrame(Falcor::IScene::UpdateFlags updateFlags);

    /**
     * @brief Mark emitted commands as executed by the capture pass.
     *
     * Clears the card dirty flags of completed commands (only when no newer dirt arrived
     * while the command was in flight) and settles the queue-time statistics. Stale or
     * forged commands (wrong card, wrong page, wrong generation, already completed) are
     * ignored, so double completion and out-of-order completion are safe.
     */
    void completeCaptures(const std::vector<LumenCaptureCommand>& commands);

    /**
     * @brief Check whether a specific emitted command was accepted as Resident.
     *
     * This is intentionally separate from completeCaptures(). The root pass uses it to
     * advance card-specific request telemetry only after the scheduler's page and generation
     * validation succeeds; command emission alone is not capture completion.
     */
    bool isCaptureComplete(const LumenCaptureCommand& command) const;

    /**
     * @brief Scene reload/reset: release every page held by the scheduler and clear the
     * whole state machine (records, pending queue, statistics, frame index).
     *
     * Does NOT reset the page cache, which the root owns; both call orders are safe
     * (after cache.reset() the release calls are no-ops, before it they return the pages
     * to the cache free-list).
     */
    void reset();

    ///< Scheduling state of a card (see LumenCaptureCardState).
    LumenCaptureCardState getCardState(uint32_t cardIndex) const
    {
        if (cardIndex >= mRecords.size())
        {
            return LumenCaptureCardState::NotTracked;
        }
        return mRecords[cardIndex].state;
    }

    ///< Cumulative statistics.
    LumenCaptureSchedulerStats getStats() const;

    ///< Statistics of the last scheduleFrame() call.
    const LumenCaptureFrameStats& getLastFrameStats() const { return mLastFrameStats; }

    ///< Current capture budget in pages per frame.
    uint32_t getMaxPagesPerFrame() const { return mMaxPagesPerFrame; }

    ///< Set a new capture budget; clamped to at least 1.
    void setMaxPagesPerFrame(uint32_t value) { mMaxPagesPerFrame = std::max<uint32_t>(1, value); }

    ///< Frames scheduled since construction/reset.
    uint64_t getFrameIndex() const { return mFrameIndex; }

    ///< Cards currently waiting for a page (PendingAllocation).
    uint32_t getPendingCount() const { return countPending(); }

    /**
     * @brief Queue a host-validated demand request for a card.
     *
     * GPU feedback is consumed by the root before scheduleFrame(). The request is merged into
     * the same per-card worklist as dirty/evicted cards, so page allocation, priority ordering,
     * budget enforcement and generation-safe completion remain centralized here.
     *
     * @return true when this call changed the card's queued state; false for an invalid,
     * unsupported or already queued card.
     */
    bool enqueueFeedbackRequest(uint32_t cardIndex)
    {
        if (cardIndex >= mNeedsCapture.size() || !isCardSupported(cardIndex))
            return false;
        if (mNeedsCapture[cardIndex])
        {
            ++mTotalRequestDeduplications;
            return false;
        }
        mNeedsCapture[cardIndex] = 1;
        return true;
    }

    /**
     * @brief Query whether a feedback request would be a new scheduler work item.
     *
     * This is intentionally read-only.  Surface Cache telemetry uses it to
     * distinguish a new miss request from a repeated GPU miss for a card that is
     * already queued by dirty/in-flight work; repeated misses must not create
     * false N->N+1 publication events.
     */
    bool canAcceptFeedbackRequest(uint32_t cardIndex) const
    {
        return cardIndex < mNeedsCapture.size() && isCardSupported(cardIndex) && !mNeedsCapture[cardIndex];
    }

private:
    struct CardRecord
    {
        uint32_t pageID = kInvalidPageID;
        uint32_t generation = 0; ///< Page generation at assignment; 0 means no page.
        LumenCaptureCardState state = LumenCaptureCardState::NotTracked;
    };

    struct WorkItem
    {
        uint32_t cardIndex = kLumenCardInvalidID;
        float priority = 0.f;
        uint32_t queuedFrames = 0;
    };

    ///< Release pages of vanished/unsupported cards and detect evicted pages. Also sizes the
    ///< per-card bookkeeping to the current card count.
    void reconcile(LumenCaptureFrameStats& stats);

    ///< True when the card still exists and its instance is card-ized (unsupportedReasons == 0).
    bool isCardSupported(uint32_t cardIndex) const;

    ///< True when an in-flight capture command stayed uncompleted past the timeout.
    bool isInFlightTimedOut(uint32_t cardIndex) const
    {
        return mFrameIndex > mInFlightFrame[cardIndex] + mInFlightTimeoutFrames;
    }

    uint32_t countPending() const;

    TCardScene* mpCardScene = nullptr;
    TPageCache* mpPageCache = nullptr;
    uint32_t mMaxPagesPerFrame = kLumenCaptureDefaultMaxPagesPerFrame;
    uint32_t mInFlightTimeoutFrames = kLumenCaptureDefaultInFlightTimeoutFrames;
    uint64_t mFrameIndex = 0;
    uint32_t mLastUpdateFlags = 0;

    std::vector<CardRecord> mRecords;        ///< Per-card page mapping, indexed by cardIndex.
    std::vector<uint8_t> mNeedsCapture;      ///< Cards whose page was evicted; internal re-queue.
    std::vector<uint32_t> mQueuedFrames;     ///< Frames a card waited dirty -> completed.
    std::vector<uint64_t> mInFlightFrame;    ///< Frame a card's current command was emitted.
    std::vector<uint8_t> mInWorklist;        ///< Worklist dedupe scratch.
    std::vector<uint8_t> mEmitted;           ///< Cards with a command emitted this frame.
    std::vector<WorkItem> mWorklist;         ///< Sorted worklist scratch.

    uint64_t mTotalCaptureCommands = 0;
    uint64_t mTotalAllocations = 0;
    uint64_t mTotalRecaptures = 0;
    uint64_t mTotalAllocationFailures = 0;
    uint64_t mTotalStarvationFrames = 0;
    uint64_t mTotalReleases = 0;
    uint64_t mTotalLostPages = 0;
    uint64_t mTotalTouches = 0;
    uint64_t mTotalRequestDeduplications = 0;
    uint64_t mTotalQueuedFrames = 0;
    uint64_t mCompletedCaptureCount = 0;
    uint32_t mMaxQueuedFrames = 0;
    uint32_t mMaxPendingDepth = 0;
    uint64_t mStructuralRebuildCount = 0;
    uint32_t mLastCommandCardID = kLumenCardInvalidID;
    uint32_t mLastCommandPageID = kInvalidPageID;
    uint32_t mLastCommandGeneration = 0;
    LumenCaptureFrameStats mLastFrameStats;
};

/** Production instantiation over the real components (LumenCardScene + LumenSurfaceCache). */
using LumenCaptureSchedulerForScene = LumenCaptureScheduler<LumenCardScene, LumenSurfaceCache>;

template <typename TCardScene, typename TPageCache>
LumenCaptureFrame LumenCaptureScheduler<TCardScene, TPageCache>::scheduleFrame(Falcor::IScene::UpdateFlags updateFlags)
{
    mLastUpdateFlags = static_cast<uint32_t>(updateFlags);
    if (is_set(updateFlags, Falcor::IScene::UpdateFlags::GeometryChanged) ||
        is_set(updateFlags, Falcor::IScene::UpdateFlags::MeshesChanged))
    {
        ++mStructuralRebuildCount;
    }

    LumenCaptureFrame frame;
    LumenCaptureFrameStats& stats = frame.stats;

    reconcile(stats);

    const uint32_t cardCount = mpCardScene->getCardCount();
    if (mInWorklist.size() != cardCount)
    {
        mInWorklist.assign(cardCount, 0);
        mEmitted.assign(cardCount, 0);
    }
    std::fill(mInWorklist.begin(), mInWorklist.end(), 0);
    std::fill(mEmitted.begin(), mEmitted.end(), 0);

    // Merge the capture work sources: scene dirty cards, internally re-queued cards whose
    // page was evicted, pending allocation retries, and timed-out in-flight commands.
    mWorklist.clear();
    const auto addWorkItem = [this, cardCount](uint32_t cardIndex)
    {
        if (cardIndex >= cardCount)
        {
            return;
        }
        if (mInWorklist[cardIndex])
        {
            ++mTotalRequestDeduplications;
            return;
        }
        mInWorklist[cardIndex] = 1;
        mWorklist.push_back(WorkItem{
            cardIndex,
            mpCardScene->getCard(cardIndex).priority,
            cardIndex < mQueuedFrames.size() ? mQueuedFrames[cardIndex] : 0u,
        });
    };
    for (uint32_t cardIndex : mpCardScene->getDirtyCardIndices())
    {
        addWorkItem(cardIndex);
    }
    for (uint32_t i = 0; i < cardCount; ++i)
    {
        if (mNeedsCapture[i])
        {
            addWorkItem(i);
        }
        if (mRecords[i].state == LumenCaptureCardState::PendingAllocation)
        {
            addWorkItem(i);
        }
        if (mRecords[i].state == LumenCaptureCardState::Capturing && isInFlightTimedOut(i))
        {
            addWorkItem(i);
        }
    }

    // Priority descending, cardIndex tie-break: mirrors the card scene sort so the merged
    // worklist is a total order and the scheduler is deterministic.
    std::sort(
        mWorklist.begin(),
        mWorklist.end(),
        [](const WorkItem& lhs, const WorkItem& rhs)
        {
            const bool lhsPromoted = lhs.queuedFrames >= kLumenCaptureStarvationPromotionFrames;
            const bool rhsPromoted = rhs.queuedFrames >= kLumenCaptureStarvationPromotionFrames;
            if (lhsPromoted != rhsPromoted)
            {
                return lhsPromoted;
            }
            if (lhs.priority != rhs.priority)
            {
                return lhs.priority > rhs.priority;
            }
            return lhs.cardIndex < rhs.cardIndex;
        });

    stats.requestedCards = static_cast<uint32_t>(mWorklist.size());

    uint32_t budget = mMaxPagesPerFrame;
    frame.commands.reserve(std::min<size_t>(mWorklist.size(), budget));
    for (const WorkItem& item : mWorklist)
    {
        CardRecord& record = mRecords[item.cardIndex];

        // Already captured and still within the completion timeout: wait for the GPU.
        if (record.state == LumenCaptureCardState::Capturing && !isInFlightTimedOut(item.cardIndex))
        {
            ++stats.inFlightCards;
            continue;
        }

        if (budget == 0)
        {
            ++stats.budgetCappedCards;
            continue;
        }

        LumenCaptureCommand cmd;
        cmd.cardIndex = item.cardIndex;
        cmd.dirtyFlags = mpCardScene->getCard(item.cardIndex).dirtyFlags;

        // A page counts as valid only while the page cache still owns it to this card
        // (same generation). Evictions and reallocations are detected here, not just in
        // reconcile(), because an allocation earlier in this very loop can evict a page.
        const bool hasValidPage = record.pageID != kInvalidPageID &&
            mpPageCache->isPageAllocated(record.pageID) &&
            mpPageCache->getGeneration(record.pageID) == record.generation;

        if (hasValidPage)
        {
            cmd.pageID = record.pageID;
            cmd.generation = record.generation;
            ++stats.recaptureWithPage;
            if (mpPageCache->touchPage(record.pageID))
            {
                ++stats.touchCalls;
            }
            else
            {
                // Defensive: page died between reconcile and emission.
                record = CardRecord{};
                mNeedsCapture[item.cardIndex] = 1;
                ++stats.lostPages;
                continue;
            }
        }
        else
        {
            if (record.pageID != kInvalidPageID)
            {
                // Page was evicted or reallocated under this card; its capture is void.
                record = CardRecord{};
                mNeedsCapture[item.cardIndex] = 1;
                ++stats.lostPages;
            }
            const uint32_t pageID = mpPageCache->allocatePage();
            if (pageID == kInvalidPageID)
            {
                // Atlas full and nothing evictable: retry next frame, count starvation.
                record.state = LumenCaptureCardState::PendingAllocation;
                ++stats.allocationFailures;
                continue; //< Allocation failures do not consume budget.
            }
            cmd.pageID = pageID;
            cmd.generation = mpPageCache->getGeneration(pageID);
            record.pageID = pageID;
            record.generation = cmd.generation;
            ++stats.newPageAllocations;
        }

        record.state = LumenCaptureCardState::Capturing;
        mInFlightFrame[item.cardIndex] = mFrameIndex;
        mNeedsCapture[item.cardIndex] = 0;
        mEmitted[item.cardIndex] = 1;
        frame.commands.push_back(cmd);
        stats.lastCommandCardID = cmd.cardIndex;
        stats.lastCommandPageID = cmd.pageID;
        stats.lastCommandGeneration = cmd.generation;
        mLastCommandCardID = cmd.cardIndex;
        mLastCommandPageID = cmd.pageID;
        mLastCommandGeneration = cmd.generation;
        --budget;
    }

    // Cards that stayed ready but were not emitted this frame accumulate queue time.
    for (const WorkItem& item : mWorklist)
    {
        if (mEmitted[item.cardIndex])
        {
            continue;
        }
        const CardRecord& record = mRecords[item.cardIndex];
        if (record.state == LumenCaptureCardState::Capturing && !isInFlightTimedOut(item.cardIndex))
        {
            continue;
        }
        if (mQueuedFrames[item.cardIndex] != std::numeric_limits<uint32_t>::max())
        {
            ++mQueuedFrames[item.cardIndex];
        }
    }

    // End-of-frame pending depth: cards waiting for a page right now.
    uint32_t pendingCount = 0;
    for (uint32_t i = 0; i < cardCount; ++i)
    {
        if (mRecords[i].state == LumenCaptureCardState::PendingAllocation)
        {
            ++pendingCount;
        }
    }
    stats.pendingCards = pendingCount;
    stats.starvationFrames = pendingCount;
    mTotalStarvationFrames += pendingCount;
    mMaxPendingDepth = std::max(mMaxPendingDepth, pendingCount);

    stats.captureCommands = static_cast<uint32_t>(frame.commands.size());
    mTotalCaptureCommands += stats.captureCommands;
    mTotalAllocations += stats.newPageAllocations;
    mTotalRecaptures += stats.recaptureWithPage;
    mTotalAllocationFailures += stats.allocationFailures;
    mTotalReleases += stats.releasedPages;
    mTotalLostPages += stats.lostPages;
    mTotalTouches += stats.touchCalls;
    mLastFrameStats = stats;

    mpPageCache->endFrame();
    ++mFrameIndex;
    return frame;
}

template <typename TCardScene, typename TPageCache>
void LumenCaptureScheduler<TCardScene, TPageCache>::completeCaptures(const std::vector<LumenCaptureCommand>& commands)
{
    for (const LumenCaptureCommand& cmd : commands)
    {
        if (cmd.cardIndex >= mRecords.size())
        {
            continue;
        }
        CardRecord& record = mRecords[cmd.cardIndex];
        if (record.state != LumenCaptureCardState::Capturing)
        {
            continue;
        }
        if (record.pageID != cmd.pageID || record.generation != cmd.generation)
        {
            continue;
        }
        // The authoritative check: the page must still be allocated to this card at the
        // emitted generation (the record alone is not enough if reconcile has not run).
        if (!mpPageCache->isPageAllocated(cmd.pageID) ||
            mpPageCache->getGeneration(cmd.pageID) != cmd.generation)
        {
            continue;
        }

        const uint32_t waited = mQueuedFrames[cmd.cardIndex];
        mQueuedFrames[cmd.cardIndex] = 0;
        mTotalQueuedFrames += waited;
        mMaxQueuedFrames = std::max(mMaxQueuedFrames, waited);
        ++mCompletedCaptureCount;

        record.state = LumenCaptureCardState::Resident;
        mNeedsCapture[cmd.cardIndex] = 0;

        // Clear the scene dirty flags only when no newer dirt arrived while in flight;
        // otherwise the card stays dirty and is re-captured next frame.
        if (cmd.cardIndex < mpCardScene->getCardCount() &&
            mpCardScene->getCard(cmd.cardIndex).dirtyFlags == cmd.dirtyFlags)
        {
            mpCardScene->clearDirty(cmd.cardIndex);
        }
    }
}

template <typename TCardScene, typename TPageCache>
bool LumenCaptureScheduler<TCardScene, TPageCache>::isCaptureComplete(const LumenCaptureCommand& command) const
{
    if (command.cardIndex >= mRecords.size())
        return false;
    const CardRecord& record = mRecords[command.cardIndex];
    return record.state == LumenCaptureCardState::Resident && record.pageID == command.pageID &&
        record.generation == command.generation && mpPageCache->isPageAllocated(command.pageID) &&
        mpPageCache->getGeneration(command.pageID) == command.generation;
}

template <typename TCardScene, typename TPageCache>
void LumenCaptureScheduler<TCardScene, TPageCache>::reset()
{
    for (CardRecord& record : mRecords)
    {
        if (record.pageID != kInvalidPageID)
        {
            mpPageCache->releasePage(record.pageID);
        }
    }
    mRecords.clear();
    mNeedsCapture.clear();
    mQueuedFrames.clear();
    mInFlightFrame.clear();
    mWorklist.clear();
    mInWorklist.clear();
    mEmitted.clear();
    mFrameIndex = 0;
    mLastUpdateFlags = 0;
    mTotalCaptureCommands = 0;
    mTotalAllocations = 0;
    mTotalRecaptures = 0;
    mTotalAllocationFailures = 0;
    mTotalStarvationFrames = 0;
    mTotalReleases = 0;
    mTotalLostPages = 0;
    mTotalTouches = 0;
    mTotalRequestDeduplications = 0;
    mTotalQueuedFrames = 0;
    mCompletedCaptureCount = 0;
    mMaxQueuedFrames = 0;
    mMaxPendingDepth = 0;
    mStructuralRebuildCount = 0;
    mLastCommandCardID = kLumenCardInvalidID;
    mLastCommandPageID = kInvalidPageID;
    mLastCommandGeneration = 0;
    mLastFrameStats = LumenCaptureFrameStats{};
}

template <typename TCardScene, typename TPageCache>
void LumenCaptureScheduler<TCardScene, TPageCache>::reconcile(LumenCaptureFrameStats& stats)
{
    const uint32_t cardCount = mpCardScene->getCardCount();
    const uint32_t oldCount = static_cast<uint32_t>(mRecords.size());

    // Cards that no longer exist (card table shrank): release their pages.
    for (uint32_t i = cardCount; i < oldCount; ++i)
    {
        if (mRecords[i].pageID != kInvalidPageID && mpPageCache->releasePage(mRecords[i].pageID))
        {
            ++stats.releasedPages;
        }
    }

    mRecords.resize(cardCount);
    mNeedsCapture.resize(cardCount);
    mQueuedFrames.resize(cardCount);
    mInFlightFrame.resize(cardCount);

    for (uint32_t i = 0; i < cardCount; ++i)
    {
        CardRecord& record = mRecords[i];
        if (record.state == LumenCaptureCardState::NotTracked)
        {
            continue;
        }

        // Page validation: still allocated and owned by this card (generation unchanged).
        if (record.pageID != kInvalidPageID)
        {
            const bool pageAlive = mpPageCache->isPageAllocated(record.pageID) &&
                mpPageCache->getGeneration(record.pageID) == record.generation;
            if (!pageAlive)
            {
                // Evicted by LRU/budget, invalidated by resize, or reallocated elsewhere.
                record = CardRecord{};
                mNeedsCapture[i] = 1;
                ++stats.lostPages;
                continue;
            }
        }

        // Support validation: the card must still be backed by a card-ized instance.
        if (!isCardSupported(i))
        {
            if (record.pageID != kInvalidPageID && mpPageCache->releasePage(record.pageID))
            {
                ++stats.releasedPages;
            }
            record = CardRecord{};
            mNeedsCapture[i] = 0;
            mQueuedFrames[i] = 0;
        }
    }
}

template <typename TCardScene, typename TPageCache>
bool LumenCaptureScheduler<TCardScene, TPageCache>::isCardSupported(uint32_t cardIndex) const
{
    if (cardIndex >= mpCardScene->getCardCount())
    {
        return false;
    }
    const LumenCard& card = mpCardScene->getCard(cardIndex);
    const int32_t instanceIndex = mpCardScene->getInstanceIndex(card.instanceID);
    if (instanceIndex < 0)
    {
        return false;
    }
    const LumenCardInstance& instance = mpCardScene->getInstance(static_cast<uint32_t>(instanceIndex));
    return instance.unsupportedReasons == 0;
}

template <typename TCardScene, typename TPageCache>
uint32_t LumenCaptureScheduler<TCardScene, TPageCache>::countPending() const
{
    uint32_t count = 0;
    for (const CardRecord& record : mRecords)
    {
        if (record.state == LumenCaptureCardState::PendingAllocation)
        {
            ++count;
        }
    }
    return count;
}

template <typename TCardScene, typename TPageCache>
LumenCaptureSchedulerStats LumenCaptureScheduler<TCardScene, TPageCache>::getStats() const
{
    LumenCaptureSchedulerStats stats;
    stats.frameIndex = mFrameIndex;
    stats.lastUpdateFlags = mLastUpdateFlags;
    stats.maxPagesPerFrame = mMaxPagesPerFrame;
    stats.inFlightTimeoutFrames = mInFlightTimeoutFrames;
    stats.totalCaptureCommands = mTotalCaptureCommands;
    stats.totalAllocations = mTotalAllocations;
    stats.totalRecaptures = mTotalRecaptures;
    stats.totalAllocationFailures = mTotalAllocationFailures;
    stats.totalStarvationFrames = mTotalStarvationFrames;
    stats.totalReleases = mTotalReleases;
    stats.totalLostPages = mTotalLostPages;
    stats.totalTouches = mTotalTouches;
    stats.totalRequestDeduplications = mTotalRequestDeduplications;
    stats.completedCaptures = mCompletedCaptureCount;
    stats.averageQueuedFrames = mCompletedCaptureCount != 0
        ? static_cast<double>(mTotalQueuedFrames) / static_cast<double>(mCompletedCaptureCount)
        : 0.0;
    stats.maxQueuedFrames = mMaxQueuedFrames;
    stats.pendingQueueDepth = countPending();
    stats.maxPendingDepth = mMaxPendingDepth;
    stats.structuralRebuildCount = mStructuralRebuildCount;
    stats.lastCommandCardID = mLastCommandCardID;
    stats.lastCommandPageID = mLastCommandPageID;
    stats.lastCommandGeneration = mLastCommandGeneration;
    return stats;
}
} // namespace Falcor
