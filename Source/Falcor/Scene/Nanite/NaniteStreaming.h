#pragma once

#include "NaniteAsset.h"

namespace Falcor
{

struct NaniteStreamingStats
{
    uint64_t residentBytes = 0;
    uint64_t budgetBytes = 0;
    uint32_t residentPages = 0;
    uint32_t totalPages = 0;
    uint32_t pageMissCount = 0;
    uint32_t framePageMissCount = 0;
    uint32_t evictionCount = 0;
};

class NaniteStreamingManager : public Object
{
    FALCOR_OBJECT(NaniteStreamingManager)
public:
    static ref<NaniteStreamingManager> create(ref<NaniteAsset> pAsset);

    NaniteStreamingManager(ref<NaniteAsset> pAsset);

    void setStreamingEnabled(bool enabled) { mStreamingEnabled = enabled; }
    bool isStreamingEnabled() const { return mStreamingEnabled; }

    void setVramBudgetBytes(uint64_t budgetBytes);
    uint64_t getVramBudgetBytes() const { return mBudgetBytes; }

    void uploadInitialResidency(RenderContext* pRenderContext);
    void beginFrame();
    void collectGpuPageRequests(RenderContext* pRenderContext, const ref<Buffer>& pGpuRequestBuffer);
    void processRequests(RenderContext* pRenderContext);

    void markPageResident(uint32_t pageIndex, RenderContext* pRenderContext);
    void requestPage(uint32_t pageIndex);
    bool isPageResident(uint32_t pageIndex) const;
    uint32_t getFallbackPageIndex(uint32_t pageIndex) const;

    NaniteStreamingStats getStats() const { return mStats; }
    const ref<Buffer>& getPageRequestBuffer() const { return mpPageRequestBuffer; }
    const ref<Buffer>& getPageFallbackBuffer() const { return mpPageFallbackBuffer; }
    const std::vector<uint32_t>& getResidentFlags() const { return mResidentFlags; }

private:
    void buildFallbackTable();
    void updateStats();
    void syncGpuTables(RenderContext* pRenderContext);
    bool evictForBudget(uint64_t requiredBytes, uint32_t excludePage);
    void uploadPageCpu(uint32_t pageIndex, RenderContext* pRenderContext);
    void dedupePendingRequests();

    ref<NaniteAsset> mpAsset;
    ref<Buffer> mpPageRequestBuffer;
    ref<Buffer> mpPageFallbackBuffer;
    std::vector<uint32_t> mResidentFlags;
    std::vector<uint32_t> mFallbackPages;
    std::vector<uint32_t> mPendingRequests;
    uint64_t mBudgetBytes = 256ull * 1024 * 1024;
    bool mStreamingEnabled = true;
    NaniteStreamingStats mStats{};
};

} // namespace Falcor
