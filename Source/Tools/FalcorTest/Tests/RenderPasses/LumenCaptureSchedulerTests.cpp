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

/** CPU tests for the per-frame capture scheduler (Wave S2.2 Agent A).

    The scheduler is a template over a card scene and a page cache. Production instantiates
    it with LumenCardScene + LumenSurfaceCache (LumenCaptureSchedulerForScene); these tests
    use a lightweight MockCardScene that mimics the LumenCardScene contract (compacted card
    table, priority-descending dirty list) without requiring a Falcor Scene, plus the real
    LumenSurfaceCache. This keeps the scheduler fully CPU-testable.
*/

#include "Testing/UnitTest.h"
#include "../../../../RenderPasses/LumenGI/Capture/LumenCaptureScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Falcor
{
namespace
{
// Atlas sizes in texels: 64 -> 16 pages, 512 -> 32 pages, 1024 -> 64 pages
// (kLumenSurfaceCacheTileSize = 16).
constexpr uint32_t kTestAtlas16PagesTexels = 64;
constexpr uint32_t kTestAtlas32PagesTexels = 512;
constexpr uint32_t kTestAtlas64PagesTexels = 1024;
constexpr uint32_t kTestResidencyFrames = 5;

constexpr uint32_t kDirtyBounds = (uint32_t)LumenCardDirtyFlags::Bounds;
constexpr uint32_t kDirtyMaterial = (uint32_t)LumenCardDirtyFlags::Material;

/** Minimal card scene implementing the scheduler's duck-typed contract with the same
    semantics as LumenCardScene: one instance ordinal per record, six compacted cards per
    supported instance, dirty list sorted by priority descending (cardIndex tie-break),
    rebuild() compacts cards and drops unsupported instances.
*/
class MockCardScene
{
public:
    // ---- Scheduler contract (mirrors LumenCardScene) ----
    uint32_t getCardCount() const { return (uint32_t)mCards.size(); }
    const LumenCard& getCard(uint32_t cardIndex) const { return mCards[cardIndex]; }
    int32_t getInstanceIndex(uint32_t sceneInstanceID) const
    {
        return sceneInstanceID < mInstanceIndexBySceneID.size() ? mInstanceIndexBySceneID[sceneInstanceID] : -1;
    }
    const LumenCardInstance& getInstance(uint32_t instanceIndex) const { return mInstances[instanceIndex]; }
    const std::vector<uint32_t>& getDirtyCardIndices() const { return mDirtyCardIndices; }
    void clearDirty(uint32_t cardIndex)
    {
        if (cardIndex >= mCards.size())
        {
            return;
        }
        mCards[cardIndex].dirtyFlags = 0;
        rebuildDirtyList();
    }

    // ---- Test control ----
    uint32_t addInstance(uint32_t sceneInstanceID, float size)
    {
        LumenCardInstance instance;
        instance.sceneInstanceID = sceneInstanceID;
        instance.firstCardIndex = kLumenCardInvalidID;
        instance.unsupportedReasons = 0;
        mInstances.push_back(instance);
        mInstanceSizes.push_back(size);
        rebuild();
        rebuildDirtyList();
        return (uint32_t)mInstances.size() - 1;
    }

    void markUnsupported(uint32_t instanceOrdinal, uint32_t reasons)
    {
        mInstances[instanceOrdinal].unsupportedReasons = reasons;
        rebuild();
        rebuildDirtyList();
    }

    void setDirty(uint32_t cardIndex, uint32_t flags)
    {
        mCards[cardIndex].dirtyFlags = flags;
        rebuildDirtyList();
    }

    uint32_t dirtyCount() const { return (uint32_t)mDirtyCardIndices.size(); }
    bool isDirty(uint32_t cardIndex) const
    {
        return cardIndex < mCards.size() && mCards[cardIndex].dirtyFlags != 0;
    }

private:
    void rebuild()
    {
        mCards.clear();
        mInstanceIndexBySceneID.clear();
        for (size_t ordinal = 0; ordinal < mInstances.size(); ++ordinal)
        {
            LumenCardInstance& instance = mInstances[ordinal];
            if (instance.sceneInstanceID >= mInstanceIndexBySceneID.size())
            {
                mInstanceIndexBySceneID.resize((size_t)instance.sceneInstanceID + 1, -1);
            }
            mInstanceIndexBySceneID[instance.sceneInstanceID] = (int32_t)ordinal;
            if (instance.unsupportedReasons != 0)
            {
                continue;
            }
            instance.firstCardIndex = (uint32_t)mCards.size();
            const float size = mInstanceSizes[ordinal];
            for (uint32_t face = 0; face < kLumenCardFaceCount; ++face)
            {
                LumenCard card;
                card.meshID = 0;
                card.instanceID = instance.sceneInstanceID;
                card.faceIndex = face;
                card.dirtyFlags = kDirtyBounds | kDirtyMaterial;
                card.extent = Falcor::float4(size, size, size, 0.f);
                card.priority = size * size;
                mCards.push_back(card);
            }
        }
    }

    void rebuildDirtyList()
    {
        mDirtyCardIndices.clear();
        for (uint32_t i = 0; i < (uint32_t)mCards.size(); ++i)
        {
            if (mCards[i].dirtyFlags != 0)
            {
                mDirtyCardIndices.push_back(i);
            }
        }
        std::sort(
            mDirtyCardIndices.begin(),
            mDirtyCardIndices.end(),
            [this](uint32_t lhs, uint32_t rhs)
            {
                const float lhsPriority = mCards[lhs].priority;
                const float rhsPriority = mCards[rhs].priority;
                if (lhsPriority != rhsPriority)
                {
                    return lhsPriority > rhsPriority;
                }
                return lhs < rhs;
            });
    }

    std::vector<LumenCard> mCards;
    std::vector<LumenCardInstance> mInstances;
    std::vector<float> mInstanceSizes;
    std::vector<int32_t> mInstanceIndexBySceneID;
    std::vector<uint32_t> mDirtyCardIndices;
};

using Scheduler = LumenCaptureScheduler<MockCardScene, LumenSurfaceCache>;
} // namespace

CPU_TEST(LumenCaptureScheduler_PriorityOrderedCaptures)
{
    MockCardScene scene;
    scene.addInstance(0, 2.f);  //< cards 0..5, priority 4
    scene.addInstance(1, 10.f); //< cards 6..11, priority 100
    scene.addInstance(2, 5.f);  //< cards 12..17, priority 25
    LumenSurfaceCache cache(kTestAtlas32PagesTexels, 0, kTestResidencyFrames);
    Scheduler scheduler(&scene, &cache, 64);

    LumenCaptureFrame frame = scheduler.scheduleFrame(IScene::UpdateFlags::GeometryChanged);

    EXPECT_EQ(frame.commands.size(), 18u);
    EXPECT_EQ(frame.stats.requestedCards, 18u);
    EXPECT_EQ(frame.stats.captureCommands, 18u);
    EXPECT_EQ(frame.stats.newPageAllocations, 18u);
    EXPECT_EQ(frame.stats.recaptureWithPage, 0u);
    EXPECT_EQ(frame.stats.allocationFailures, 0u);
    EXPECT_EQ(frame.stats.budgetCappedCards, 0u);
    EXPECT_EQ(scheduler.getStats().structuralRebuildCount, 1u);

    // High priority first (cards of the size-10 instance), then size-5, then size-2;
    // ties broken by card index; pages handed out ascending from the free-list.
    const uint32_t expectedCards[18] = {6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 0, 1, 2, 3, 4, 5};
    for (uint32_t i = 0; i < 18; ++i)
    {
        EXPECT_EQ(frame.commands[i].cardIndex, expectedCards[i]);
        EXPECT_EQ(frame.commands[i].pageID, i + 1);
        EXPECT_EQ(frame.commands[i].generation, 1u);
        EXPECT_EQ(frame.commands[i].dirtyFlags, kDirtyBounds | kDirtyMaterial);
        EXPECT_EQ(cache.getGeneration(frame.commands[i].pageID), 1u);
    }

    scheduler.completeCaptures(frame.commands);
    EXPECT_EQ(scene.dirtyCount(), 0u);
    EXPECT_TRUE(scheduler.getCardState(6) == LumenCaptureCardState::Resident);
    EXPECT_EQ(cache.getAllocatedPageCount(), 18u);

    // A clean scene schedules nothing.
    LumenCaptureFrame idle = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(idle.commands.size(), 0u);
    EXPECT_EQ(idle.stats.requestedCards, 0u);
}

CPU_TEST(LumenCaptureScheduler_BudgetCapsPerFrame)
{
    MockCardScene scene;
    for (uint32_t i = 0; i < 10; ++i)
    {
        scene.addInstance(i, 10.f); //< 60 cards, equal priority -> cardIndex order
    }
    LumenSurfaceCache cache(kTestAtlas64PagesTexels, 0, kTestResidencyFrames);
    Scheduler scheduler(&scene, &cache, 4);

    EXPECT_EQ(kLumenCaptureDefaultMaxPagesPerFrame, 64u);

    for (uint32_t frameIndex = 1; frameIndex <= 15; ++frameIndex)
    {
        LumenCaptureFrame frame = scheduler.scheduleFrame(IScene::UpdateFlags::None);
        EXPECT_EQ(frame.commands.size(), 4u);
        for (uint32_t j = 0; j < 4; ++j)
        {
            const LumenCaptureCommand& cmd = frame.commands[j];
            EXPECT_EQ(cmd.cardIndex, (frameIndex - 1) * 4 + j);
            EXPECT_EQ(cmd.pageID, (frameIndex - 1) * 4 + j + 1);
        }
        EXPECT_EQ(frame.stats.captureCommands, 4u);
        EXPECT_EQ(frame.stats.newPageAllocations, 4u);
        EXPECT_EQ(frame.stats.recaptureWithPage, 0u);
        EXPECT_EQ(frame.stats.allocationFailures, 0u);
        EXPECT_EQ(frame.stats.budgetCappedCards, (15 - frameIndex) * 4);
        scheduler.completeCaptures(frame.commands);
    }

    const LumenCaptureSchedulerStats stats = scheduler.getStats();
    EXPECT_EQ(stats.frameIndex, 15u);
    EXPECT_EQ(stats.totalCaptureCommands, 60u);
    EXPECT_EQ(stats.totalAllocations, 60u);
    EXPECT_EQ(stats.totalRecaptures, 0u);
    EXPECT_EQ(stats.completedCaptures, 60u);
    EXPECT_EQ(stats.maxQueuedFrames, 14u);
    EXPECT_TRUE(std::abs(stats.averageQueuedFrames - 7.0) < 1e-9);
    EXPECT_EQ(stats.pendingQueueDepth, 0u);
    EXPECT_EQ(scheduler.getCardState(59) == LumenCaptureCardState::Resident, true);
    EXPECT_EQ(scene.dirtyCount(), 0u);
}

CPU_TEST(LumenCaptureScheduler_AtlasFullPendingRetryStarvation)
{
    MockCardScene scene;
    for (uint32_t i = 0; i < 20; ++i)
    {
        scene.addInstance(i, 10.f); //< 120 cards
    }
    LumenSurfaceCache cache(kTestAtlas16PagesTexels, 0, kTestResidencyFrames); //< 16 pages only
    Scheduler scheduler(&scene, &cache, 64);

    // Frame 1: the frame budget limits emitted commands (pages per frame), not allocation
    // attempts. All 120 cards are tried: 16 succeed (pages 1..16), the remaining 104 fail
    // because the atlas is full and nothing is evictable inside the residency window.
    LumenCaptureFrame frame1 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame1.commands.size(), 16u);
    for (uint32_t i = 0; i < 16; ++i)
    {
        EXPECT_EQ(frame1.commands[i].pageID, i + 1);
    }
    EXPECT_EQ(frame1.stats.newPageAllocations, 16u);
    EXPECT_EQ(frame1.stats.allocationFailures, 104u);
    EXPECT_EQ(frame1.stats.budgetCappedCards, 0u);
    EXPECT_EQ(frame1.stats.pendingCards, 104u);
    EXPECT_EQ(frame1.stats.starvationFrames, 104u);
    scheduler.completeCaptures(frame1.commands);

    // Frames 2..5: atlas full, nothing evictable inside the residency window.
    for (uint32_t frameIndex = 2; frameIndex <= 5; ++frameIndex)
    {
        LumenCaptureFrame frame = scheduler.scheduleFrame(IScene::UpdateFlags::None);
        EXPECT_EQ(frame.commands.size(), 0u);
        EXPECT_EQ(frame.stats.allocationFailures, 104u);
        EXPECT_EQ(frame.stats.pendingCards, 104u);
    }
    EXPECT_EQ(scheduler.getStats().totalAllocationFailures, 104u + 4 * 104u);

    // Frame 6: the residency window of pages 1..16 passed; the pending cards evict and
    // reuse them in ascending order (LRU tie-break), 16 commands, 88 cards stay pending.
    LumenCaptureFrame frame6 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame6.commands.size(), 16u);
    for (uint32_t i = 0; i < 16; ++i)
    {
        EXPECT_EQ(frame6.commands[i].cardIndex, 16u + i);
        EXPECT_EQ(frame6.commands[i].pageID, i + 1);
        EXPECT_EQ(cache.getGeneration(frame6.commands[i].pageID), 2u);
    }
    EXPECT_EQ(frame6.stats.allocationFailures, 88u);
    EXPECT_EQ(frame6.stats.pendingCards, 88u);
    scheduler.completeCaptures(frame6.commands);

    // Frame 7: the evicted pages belonged to the first cohort; their cards are detected
    // as lost (generation mismatch) and re-queued, but everything is inside the fresh
    // residency window again, so nothing is emitted this frame.
    LumenCaptureFrame frame7 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame7.commands.size(), 0u);
    EXPECT_EQ(frame7.stats.lostPages, 16u);
    EXPECT_EQ(frame7.stats.allocationFailures, 104u);

    // Frames 8..10: still protected.
    for (uint32_t frameIndex = 8; frameIndex <= 10; ++frameIndex)
    {
        LumenCaptureFrame frame = scheduler.scheduleFrame(IScene::UpdateFlags::None);
        EXPECT_EQ(frame.commands.size(), 0u);
        EXPECT_EQ(frame.stats.allocationFailures, 104u);
    }

    // Frame 11: the frame-6 pages (last touched at frame 6) pass the 5-frame residency
    // window. The cards whose pages are evicted during this frame (16..31) are only
    // detected next frame, so the worklist is the first cohort (0..15) plus the pending
    // cards: 16 commands, 88 failures.
    LumenCaptureFrame frame11 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame11.commands.size(), 16u);
    for (uint32_t i = 0; i < 16; ++i)
    {
        EXPECT_EQ(frame11.commands[i].cardIndex, i);
        EXPECT_EQ(frame11.commands[i].pageID, i + 1);
        EXPECT_EQ(cache.getGeneration(frame11.commands[i].pageID), 3u);
    }
    EXPECT_EQ(frame11.stats.allocationFailures, 88u);
    EXPECT_EQ(frame11.stats.pendingCards, 88u);
    scheduler.completeCaptures(frame11.commands);

    // Frame 12: the frame-11 pages are inside the fresh residency window again. The
    // frame-11 evictions orphan cards 16..31, which are detected as lost and re-queued.
    LumenCaptureFrame frame12 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame12.commands.size(), 0u);
    EXPECT_EQ(frame12.stats.lostPages, 16u);
    EXPECT_EQ(frame12.stats.allocationFailures, 104u);
    EXPECT_EQ(frame12.stats.pendingCards, 104u);

    const LumenCaptureSchedulerStats stats = scheduler.getStats();
    EXPECT_EQ(stats.totalAllocations, 48u);
    EXPECT_EQ(stats.totalRecaptures, 0u);
    EXPECT_EQ(stats.totalLostPages, 32u);
    EXPECT_EQ(stats.totalAllocationFailures, stats.totalStarvationFrames);
    EXPECT_EQ(stats.totalStarvationFrames, 104u + 4 * 104u + 88u + 104u + 3 * 104u + 88u + 104u);
    EXPECT_EQ(stats.maxPendingDepth, 104u);
    EXPECT_EQ(stats.pendingQueueDepth, 104u);
}

CPU_TEST(LumenCaptureScheduler_MaterialDirtyKeepsPageResident)
{
    MockCardScene scene;
    scene.addInstance(0, 10.f);
    LumenSurfaceCache cache(kTestAtlas16PagesTexels, 0, kTestResidencyFrames);
    Scheduler scheduler(&scene, &cache, 64);

    LumenCaptureFrame frame1 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame1.commands.size(), 6u);
    scheduler.completeCaptures(frame1.commands);
    EXPECT_EQ(cache.getAllocatedPageCount(), 6u);

    // Material change re-captures card 0 onto its EXISTING page; nothing is released.
    scene.setDirty(0, kDirtyMaterial);
    LumenCaptureFrame frame2 = scheduler.scheduleFrame(IScene::UpdateFlags::MaterialsChanged);
    EXPECT_EQ(frame2.commands.size(), 1u);
    EXPECT_EQ(frame2.commands[0].cardIndex, 0u);
    EXPECT_EQ(frame2.commands[0].pageID, 1u);
    EXPECT_EQ(frame2.commands[0].generation, 1u);
    EXPECT_EQ(frame2.commands[0].dirtyFlags, kDirtyMaterial);
    EXPECT_EQ(frame2.stats.recaptureWithPage, 1u);
    EXPECT_EQ(frame2.stats.newPageAllocations, 0u);
    EXPECT_EQ(frame2.stats.touchCalls, 1u);
    EXPECT_EQ(cache.getAllocatedPageCount(), 6u);
    EXPECT_EQ(scheduler.getStats().totalReleases, 0u);

    scheduler.completeCaptures(frame2.commands);
    EXPECT_FALSE(scene.isDirty(0));
    EXPECT_TRUE(scheduler.getCardState(0) == LumenCaptureCardState::Resident);
    EXPECT_EQ(cache.getAllocatedPageCount(), 6u);
}

CPU_TEST(LumenCaptureScheduler_ResetReleasesAllPages)
{
    MockCardScene scene;
    scene.addInstance(0, 10.f);
    scene.addInstance(1, 10.f);
    LumenSurfaceCache cache(kTestAtlas16PagesTexels, 0, kTestResidencyFrames);
    Scheduler scheduler(&scene, &cache, 64);

    LumenCaptureFrame frame1 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame1.commands.size(), 12u);
    EXPECT_EQ(cache.getAllocatedPageCount(), 12u);

    // Scene reload: every scheduler-held page is released and the state machine resets.
    scheduler.reset();
    EXPECT_EQ(cache.getAllocatedPageCount(), 0u);
    EXPECT_EQ(cache.getFreeCount(), 16u);
    EXPECT_EQ(scheduler.getStats().frameIndex, 0u);
    EXPECT_EQ(scheduler.getStats().totalCaptureCommands, 0u);
    EXPECT_TRUE(scheduler.getCardState(0) == LumenCaptureCardState::NotTracked);

    // The mock scene is still dirty, so the next frame re-captures all cards; the freed
    // pages come back from the LIFO free-list (descending page IDs).
    LumenCaptureFrame frame2 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame2.commands.size(), 12u);
    std::vector<uint8_t> occupied(17, 0);
    for (const LumenCaptureCommand& cmd : frame2.commands)
    {
        EXPECT_NE(cmd.pageID, kInvalidPageID);
        EXPECT_LE(cmd.pageID, 16u);
        EXPECT_EQ(occupied[cmd.pageID], 0);
        occupied[cmd.pageID] = 1;
    }
    for (uint32_t pageID = 1; pageID <= 12; ++pageID)
    {
        EXPECT_EQ(occupied[pageID], 1);
    }
    EXPECT_EQ(cache.getAllocatedPageCount(), 12u);
}

CPU_TEST(LumenCaptureScheduler_UnsupportedInstanceReleasesPages)
{
    MockCardScene scene;
    scene.addInstance(0, 10.f); //< cards 0..5
    scene.addInstance(1, 10.f); //< cards 6..11
    LumenSurfaceCache cache(kTestAtlas16PagesTexels, 0, kTestResidencyFrames);
    Scheduler scheduler(&scene, &cache, 64);

    LumenCaptureFrame frame1 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame1.commands.size(), 12u);
    scheduler.completeCaptures(frame1.commands);
    EXPECT_EQ(cache.getAllocatedPageCount(), 12u);

    // The instance becomes unsupported: its cards vanish from the card table (compacted
    // rebuild), the scheduler releases their six pages and keeps the surviving cards.
    scene.markUnsupported(1, (uint32_t)LumenCardUnsupportedReason::NotTriangleMesh);
    EXPECT_EQ(scene.getCardCount(), 6u);

    LumenCaptureFrame frame2 = scheduler.scheduleFrame(IScene::UpdateFlags::MeshesChanged);
    EXPECT_EQ(frame2.stats.releasedPages, 6u);
    EXPECT_EQ(frame2.stats.newPageAllocations, 0u);
    EXPECT_EQ(frame2.commands.size(), 6u); //< rebuild marked the survivors dirty again
    for (uint32_t i = 0; i < 6; ++i)
    {
        EXPECT_EQ(frame2.commands[i].cardIndex, i);
        EXPECT_EQ(frame2.commands[i].pageID, i + 1); //< survivor pages kept resident
    }
    EXPECT_EQ(frame2.stats.recaptureWithPage, 6u);
    EXPECT_EQ(cache.getAllocatedPageCount(), 6u);
    EXPECT_EQ(scheduler.getStats().totalReleases, 6u);
    EXPECT_TRUE(scheduler.getCardState(0) == LumenCaptureCardState::Capturing);
    EXPECT_TRUE(scheduler.getCardState(6) == LumenCaptureCardState::NotTracked);

    scheduler.completeCaptures(frame2.commands);
    EXPECT_TRUE(scheduler.getCardState(0) == LumenCaptureCardState::Resident);
    EXPECT_EQ(cache.getAllocatedPageCount(), 6u);
}

CPU_TEST(LumenCaptureScheduler_InFlightTimeoutReemits)
{
    MockCardScene scene;
    scene.addInstance(0, 10.f);
    LumenSurfaceCache cache(kTestAtlas16PagesTexels, 0, kTestResidencyFrames);
    Scheduler scheduler(&scene, &cache, 64);

    LumenCaptureFrame frame1 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame1.commands.size(), 6u);
    EXPECT_EQ(frame1.stats.inFlightCards, 0u);

    // Without completion the commands stay in flight for kLumenCaptureDefaultInFlightTimeoutFrames
    // frames; the cards remain dirty and are skipped, not re-emitted.
    for (uint32_t frameIndex = 2; frameIndex <= 9; ++frameIndex)
    {
        LumenCaptureFrame frame = scheduler.scheduleFrame(IScene::UpdateFlags::None);
        EXPECT_EQ(frame.commands.size(), 0u);
        EXPECT_EQ(frame.stats.inFlightCards, 6u);
        EXPECT_EQ(frame.stats.requestedCards, 6u);
    }

    // Past the timeout the commands are re-emitted onto the same pages.
    LumenCaptureFrame frame10 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame10.commands.size(), 6u);
    EXPECT_EQ(frame10.stats.inFlightCards, 0u);
    EXPECT_EQ(frame10.stats.recaptureWithPage, 6u);
    for (uint32_t i = 0; i < 6; ++i)
    {
        EXPECT_EQ(frame10.commands[i].pageID, i + 1);
        EXPECT_EQ(frame10.commands[i].generation, 1u);
    }
}

CPU_TEST(LumenCaptureScheduler_DirtyWhileInFlightNotCleared)
{
    MockCardScene scene;
    scene.addInstance(0, 10.f);
    LumenSurfaceCache cache(kTestAtlas16PagesTexels, 0, kTestResidencyFrames);
    Scheduler scheduler(&scene, &cache, 64);

    LumenCaptureFrame frame1 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame1.commands.size(), 6u);
    scheduler.completeCaptures(frame1.commands);

    // Material dirt is captured on the resident page; while the command is in flight a
    // Bounds change arrives. Completing the command must NOT clear the newer dirt.
    scene.setDirty(0, kDirtyMaterial);
    LumenCaptureFrame frame2 = scheduler.scheduleFrame(IScene::UpdateFlags::MaterialsChanged);
    EXPECT_EQ(frame2.commands.size(), 1u);
    EXPECT_EQ(frame2.commands[0].dirtyFlags, kDirtyMaterial);

    scene.setDirty(0, kDirtyMaterial | kDirtyBounds);
    scheduler.completeCaptures(frame2.commands);
    EXPECT_TRUE(scene.isDirty(0));
    EXPECT_TRUE(scheduler.getCardState(0) == LumenCaptureCardState::Resident);
    EXPECT_EQ(cache.getAllocatedPageCount(), 6u);

    // The still-dirty card is re-captured next frame, and completion clears it.
    LumenCaptureFrame frame3 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame3.commands.size(), 1u);
    EXPECT_EQ(frame3.commands[0].cardIndex, 0u);
    EXPECT_EQ(frame3.commands[0].pageID, 1u);
    EXPECT_EQ(frame3.commands[0].dirtyFlags, kDirtyMaterial | kDirtyBounds);
    EXPECT_EQ(frame3.stats.recaptureWithPage, 1u);
    scheduler.completeCaptures(frame3.commands);
    EXPECT_FALSE(scene.isDirty(0));
}

CPU_TEST(LumenCaptureScheduler_StaleCommandsRejected)
{
    MockCardScene scene;
    scene.addInstance(0, 10.f);
    LumenSurfaceCache cache(kTestAtlas16PagesTexels, 0, kTestResidencyFrames);
    Scheduler scheduler(&scene, &cache, 64);

    LumenCaptureFrame frame1 = scheduler.scheduleFrame(IScene::UpdateFlags::None);
    EXPECT_EQ(frame1.commands.size(), 6u);

    // Forged completions: wrong page, wrong generation. Both must be ignored.
    LumenCaptureCommand stalePage;
    stalePage.cardIndex = 0;
    stalePage.pageID = 7;
    stalePage.generation = 1;
    LumenCaptureCommand staleGeneration;
    staleGeneration.cardIndex = 0;
    staleGeneration.pageID = 1;
    staleGeneration.generation = 999;
    scheduler.completeCaptures({stalePage, staleGeneration});

    EXPECT_TRUE(scheduler.getCardState(0) == LumenCaptureCardState::Capturing);
    EXPECT_TRUE(scene.isDirty(0));
    EXPECT_EQ(scheduler.getStats().completedCaptures, 0u);
    EXPECT_EQ(scheduler.getStats().maxQueuedFrames, 0u);
    EXPECT_EQ(scheduler.getStats().averageQueuedFrames, 0.0);

    // The real commands complete normally.
    scheduler.completeCaptures(frame1.commands);
    EXPECT_TRUE(scheduler.getCardState(0) == LumenCaptureCardState::Resident);
    EXPECT_FALSE(scene.isDirty(0));
    EXPECT_EQ(scheduler.getStats().completedCaptures, 6u);
}

CPU_TEST(LumenCaptureScheduler_DeterministicSequences)
{
    struct RunResult
    {
        void record(const LumenCaptureFrame& frame)
        {
            std::vector<uint32_t> cards;
            std::vector<uint32_t> pages;
            for (const LumenCaptureCommand& cmd : frame.commands)
            {
                cards.push_back(cmd.cardIndex);
                pages.push_back(cmd.pageID);
            }
            commandCards.push_back(cards);
            commandPages.push_back(pages);
        }

        std::vector<std::vector<uint32_t>> commandCards;
        std::vector<std::vector<uint32_t>> commandPages;
        LumenCaptureSchedulerStats stats;
    };

    const auto runScenario = []()
    {
        RunResult result;
        MockCardScene scene;
        for (uint32_t i = 0; i < 10; ++i)
        {
            scene.addInstance(i, 5.f + static_cast<float>(i % 3));
        }
        LumenSurfaceCache cache(kTestAtlas64PagesTexels, 0, kTestResidencyFrames);
        Scheduler scheduler(&scene, &cache, 4);

        for (uint32_t frame = 0; frame < 15; ++frame)
        {
            LumenCaptureFrame frameOut = scheduler.scheduleFrame(IScene::UpdateFlags::None);
            result.record(frameOut);
            scheduler.completeCaptures(frameOut.commands);
        }
        scene.setDirty(3, kDirtyMaterial);
        {
            LumenCaptureFrame frameOut = scheduler.scheduleFrame(IScene::UpdateFlags::MaterialsChanged);
            result.record(frameOut);
            scheduler.completeCaptures(frameOut.commands);
        }
        for (uint32_t frame = 0; frame < 5; ++frame)
        {
            LumenCaptureFrame frameOut = scheduler.scheduleFrame(IScene::UpdateFlags::None);
            result.record(frameOut);
        }
        for (uint32_t frame = 0; frame < 2; ++frame)
        {
            LumenCaptureFrame frameOut = scheduler.scheduleFrame(IScene::UpdateFlags::None);
            result.record(frameOut);
            scheduler.completeCaptures(frameOut.commands);
        }
        result.stats = scheduler.getStats();
        return result;
    };

    const RunResult run1 = runScenario();
    const RunResult run2 = runScenario();

    EXPECT_EQ(run1.commandCards.size(), run2.commandCards.size());
    EXPECT_EQ(run1.commandPages.size(), run2.commandPages.size());
    for (size_t i = 0; i < run1.commandCards.size(); ++i)
    {
        EXPECT_EQ(run1.commandCards[i].size(), run2.commandCards[i].size());
        EXPECT_EQ(run1.commandPages[i].size(), run2.commandPages[i].size());
        for (size_t j = 0; j < run1.commandCards[i].size(); ++j)
        {
            EXPECT_EQ(run1.commandCards[i][j], run2.commandCards[i][j]);
            EXPECT_EQ(run1.commandPages[i][j], run2.commandPages[i][j]);
        }
    }
    EXPECT_EQ(run1.stats.frameIndex, run2.stats.frameIndex);
    EXPECT_EQ(run1.stats.lastUpdateFlags, run2.stats.lastUpdateFlags);
    EXPECT_EQ(run1.stats.maxPagesPerFrame, run2.stats.maxPagesPerFrame);
    EXPECT_EQ(run1.stats.inFlightTimeoutFrames, run2.stats.inFlightTimeoutFrames);
    EXPECT_EQ(run1.stats.totalCaptureCommands, run2.stats.totalCaptureCommands);
    EXPECT_EQ(run1.stats.totalAllocations, run2.stats.totalAllocations);
    EXPECT_EQ(run1.stats.totalRecaptures, run2.stats.totalRecaptures);
    EXPECT_EQ(run1.stats.totalAllocationFailures, run2.stats.totalAllocationFailures);
    EXPECT_EQ(run1.stats.totalStarvationFrames, run2.stats.totalStarvationFrames);
    EXPECT_EQ(run1.stats.totalReleases, run2.stats.totalReleases);
    EXPECT_EQ(run1.stats.totalLostPages, run2.stats.totalLostPages);
    EXPECT_EQ(run1.stats.totalTouches, run2.stats.totalTouches);
    EXPECT_EQ(run1.stats.completedCaptures, run2.stats.completedCaptures);
    EXPECT_EQ(run1.stats.averageQueuedFrames, run2.stats.averageQueuedFrames);
    EXPECT_EQ(run1.stats.maxQueuedFrames, run2.stats.maxQueuedFrames);
    EXPECT_EQ(run1.stats.pendingQueueDepth, run2.stats.pendingQueueDepth);
    EXPECT_EQ(run1.stats.maxPendingDepth, run2.stats.maxPendingDepth);
    EXPECT_EQ(run1.stats.structuralRebuildCount, run2.stats.structuralRebuildCount);
}

} // namespace Falcor
