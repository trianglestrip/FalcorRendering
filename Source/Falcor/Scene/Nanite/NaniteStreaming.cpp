#include "NaniteStreaming.h"

#include <algorithm>

namespace Falcor
{

ref<NaniteStreamingManager> NaniteStreamingManager::create(ref<NaniteAsset> pAsset)
{
    return make_ref<NaniteStreamingManager>(pAsset);
}

NaniteStreamingManager::NaniteStreamingManager(ref<NaniteAsset> pAsset)
    : mpAsset(pAsset)
{
    const auto& pages = pAsset->getCpuAsset().pages;
    mResidentFlags.assign(pages.size(), 0u);

    if (!pages.empty())
    {
        mResidentFlags[0] = 1u;
        if (!mStreamingEnabled)
        {
            for (uint32_t i = 0; i < pages.size(); ++i)
            {
                if (pages[i].flags & Nanite::kPageFlagResident)
                {
                    mResidentFlags[i] = 1u;
                }
            }
        }

        mpPageRequestBuffer = mpAsset->getDevice()->createBuffer(
            pages.size() * sizeof(uint32_t),
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
            MemoryType::DeviceLocal);

        buildFallbackTable();
    }

    updateStats();
}

void NaniteStreamingManager::setVramBudgetBytes(uint64_t budgetBytes)
{
    mBudgetBytes = budgetBytes;
    updateStats();
}

void NaniteStreamingManager::buildFallbackTable()
{
    const auto& pages = mpAsset->getCpuAsset().pages;
    mFallbackPages.assign(pages.size(), 0u);
    for (uint32_t i = 1; i < pages.size(); ++i)
    {
        mFallbackPages[i] = 0;
    }

    if (!mFallbackPages.empty())
    {
        mpPageFallbackBuffer = mpAsset->getDevice()->createBuffer(
            mFallbackPages.size() * sizeof(uint32_t),
            ResourceBindFlags::ShaderResource,
            MemoryType::DeviceLocal,
            mFallbackPages.data());
    }
}

void NaniteStreamingManager::uploadInitialResidency(RenderContext* pRenderContext)
{
    syncGpuTables(pRenderContext);
}

void NaniteStreamingManager::beginFrame()
{
    mStats.framePageMissCount = 0;
}

void NaniteStreamingManager::collectGpuPageRequests(RenderContext* pRenderContext, const ref<Buffer>& pGpuRequestBuffer)
{
    if (!mStreamingEnabled || !pGpuRequestBuffer || mResidentFlags.empty()) return;

    const uint32_t pageCount = static_cast<uint32_t>(mResidentFlags.size());
    std::vector<uint32_t> requests(pageCount, 0);
    pGpuRequestBuffer->getBlob(requests.data(), 0, pageCount * sizeof(uint32_t));

    for (uint32_t pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        if (requests[pageIndex] == 0 || isPageResident(pageIndex)) continue;

        mPendingRequests.push_back(pageIndex);
        mStats.pageMissCount += requests[pageIndex];
        mStats.framePageMissCount += requests[pageIndex];
    }

    dedupePendingRequests();
    (void)pRenderContext;
}

void NaniteStreamingManager::dedupePendingRequests()
{
    std::sort(mPendingRequests.begin(), mPendingRequests.end());
    mPendingRequests.erase(std::unique(mPendingRequests.begin(), mPendingRequests.end()), mPendingRequests.end());
}

bool NaniteStreamingManager::evictForBudget(uint64_t requiredBytes, uint32_t excludePage)
{
    if (mBudgetBytes == 0) return true;

    updateStats();
    while (mStats.residentBytes + requiredBytes > mBudgetBytes)
    {
        bool evicted = false;
        for (int32_t i = static_cast<int32_t>(mResidentFlags.size()) - 1; i >= 1; --i)
        {
            const uint32_t pageIndex = static_cast<uint32_t>(i);
            if (pageIndex == excludePage || !mResidentFlags[pageIndex]) continue;

            mResidentFlags[pageIndex] = 0u;
            ++mStats.evictionCount;
            updateStats();
            evicted = true;
            break;
        }

        if (!evicted) return false;
    }

    return true;
}

void NaniteStreamingManager::uploadPageCpu(uint32_t pageIndex, RenderContext* pRenderContext)
{
    if (pageIndex >= mResidentFlags.size() || mResidentFlags[pageIndex]) return;

    const auto& pages = mpAsset->getCpuAsset().pages;
    const uint64_t pageBytes = pageIndex < pages.size() ? pages[pageIndex].byteSize : 0;
    if (!evictForBudget(pageBytes, pageIndex)) return;

    mpAsset->uploadPageCpuData(pRenderContext, pageIndex);
    mResidentFlags[pageIndex] = 1u;
    updateStats();
}

void NaniteStreamingManager::markPageResident(uint32_t pageIndex, RenderContext* pRenderContext)
{
    uploadPageCpu(pageIndex, pRenderContext);
}

void NaniteStreamingManager::requestPage(uint32_t pageIndex)
{
    if (pageIndex >= mResidentFlags.size()) return;
    if (mResidentFlags[pageIndex]) return;
    mPendingRequests.push_back(pageIndex);
    ++mStats.pageMissCount;
    ++mStats.framePageMissCount;
    dedupePendingRequests();
}

void NaniteStreamingManager::processRequests(RenderContext* pRenderContext)
{
    for (uint32_t pageIndex : mPendingRequests)
    {
        uploadPageCpu(pageIndex, pRenderContext);
    }
    mPendingRequests.clear();
    syncGpuTables(pRenderContext);
}

bool NaniteStreamingManager::isPageResident(uint32_t pageIndex) const
{
    return pageIndex < mResidentFlags.size() && mResidentFlags[pageIndex] != 0;
}

uint32_t NaniteStreamingManager::getFallbackPageIndex(uint32_t pageIndex) const
{
    if (mResidentFlags.empty()) return 0;
    if (pageIndex >= mResidentFlags.size()) return 0;
    if (isPageResident(pageIndex)) return pageIndex;

    uint32_t fallback = pageIndex < mFallbackPages.size() ? mFallbackPages[pageIndex] : 0;
    while (fallback < mResidentFlags.size())
    {
        if (isPageResident(fallback)) return fallback;

        if (fallback >= mFallbackPages.size()) break;
        const uint32_t next = mFallbackPages[fallback];
        if (next == fallback) break;
        fallback = next;
    }

    return isPageResident(0) ? 0 : pageIndex;
}

void NaniteStreamingManager::syncGpuTables(RenderContext* pRenderContext)
{
    if (mpAsset && mpAsset->getPageResidencyBuffer())
    {
        mpAsset->uploadPageResidency(pRenderContext, mResidentFlags);
    }

    if (mpPageFallbackBuffer && !mFallbackPages.empty())
    {
        mpPageFallbackBuffer->setBlob(mFallbackPages.data(), 0, mFallbackPages.size() * sizeof(uint32_t));
    }
}

void NaniteStreamingManager::updateStats()
{
    mStats.budgetBytes = mBudgetBytes;
    mStats.totalPages = static_cast<uint32_t>(mResidentFlags.size());
    mStats.residentPages = 0;
    mStats.residentBytes = 0;

    const auto& pages = mpAsset->getCpuAsset().pages;
    for (uint32_t i = 0; i < mResidentFlags.size(); ++i)
    {
        if (mResidentFlags[i])
        {
            ++mStats.residentPages;
            if (i < pages.size()) mStats.residentBytes += pages[i].byteSize;
        }
    }
}

} // namespace Falcor
