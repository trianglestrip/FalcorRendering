#include "NaniteRaster.h"
#include "NaniteGBufferComposite.h"

#include "Core/API/IndirectCommands.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "Utils/Scripting/ScriptBindings.h"
#include "Utils/Timing/Profiler.h"

#include <chrono>
#include <limits>
#include <numeric>

namespace
{
const char kCullShader[] = "RenderPasses/NaniteRaster/NaniteCull.cs.slang";
const char kRasterShader[] = "RenderPasses/NaniteRaster/NaniteRaster.cs.slang";
const char kResolveShader[] = "RenderPasses/NaniteRaster/NaniteMaterialResolve.cs.slang";
const char kHzbShader[] = "RenderPasses/NaniteRaster/NaniteHZB.cs.slang";

constexpr uint32_t kCullOccluded = 5;
constexpr uint32_t kCullTested = 7;

const char kVisibility[] = "visibility";
const char kDepth[] = "depth";
const char kClusterID[] = "clusterID";
const char kBaseColor[] = "baseColor";
const char kNormal[] = "normal";
const char kPosition[] = "position";
const char kTexCoord[] = "texCoord";
const char kOcclusionHeatmap[] = "occlusionHeatmap";
const char kOutput[] = "output";

void registerBindings(pybind11::module& m)
{
    pybind11::class_<NaniteRaster, RenderPass, ref<NaniteRaster>> pass(m, "NaniteRaster");
    pass.def_property(
        "debugMode",
        [](const NaniteRaster& self) { return enumToString(self.getDebugMode()); },
        [](NaniteRaster& self, const std::string& value) { self.setDebugMode(stringToEnum<NaniteDebugMode>(value)); }
    );
}
} // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, NaniteRaster>();
    registry.registerClass<RenderPass, NaniteGBufferComposite>();
    Falcor::ScriptBindings::registerBinding(registerBindings);
}

NaniteRaster::NaniteRaster(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    if (auto mode = props.getOpt<NaniteDebugMode>("debugMode"))
        setDebugMode(*mode);
    if (auto threshold = props.getOpt<float>("lodErrorThreshold"))
        mLodErrorThreshold = *threshold;
    if (auto occ = props.getOpt<bool>("enableOcclusion"))
        mEnableOcclusion = *occ;
    preparePasses();
}

Properties NaniteRaster::getProperties() const
{
    Properties props;
    props["debugMode"] = enumToString(mDebugMode);
    props["lodErrorThreshold"] = mLodErrorThreshold;
    props["enableOcclusion"] = mEnableOcclusion;
    return props;
}

void NaniteRaster::preparePasses()
{
    mpCullPass = ComputePass::create(mpDevice, kCullShader, "main");
    mpBuildIndirectPass = ComputePass::create(mpDevice, kCullShader, "buildIndirectArgs");
    mpRasterPass = ComputePass::create(mpDevice, kRasterShader, "main");
    mpResolvePass = ComputePass::create(mpDevice, kResolveShader, "main");
    mpHzbMip0Pass = ComputePass::create(mpDevice, kHzbShader, "buildMip0");
    mpHzbMip1Pass = ComputePass::create(mpDevice, kHzbShader, "buildMip1");
    mpOcclusionPass = ComputePass::create(mpDevice, kHzbShader, "occlusionCull");
    mpHeatmapPass = ComputePass::create(mpDevice, kHzbShader, "heatmap");
    mPassesPrepared = true;
}

RenderPassReflection NaniteRaster::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addOutput(kVisibility, "Visibility buffer (cluster, triangle)").format(ResourceFormat::RG32Uint);
    reflector.addOutput(kDepth, "Depth buffer").format(ResourceFormat::R32Float);
    reflector.addOutput(kClusterID, "Cluster ID debug").format(ResourceFormat::R32Uint);
    reflector.addOutput(kBaseColor, "Resolved base color").format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kNormal, "Resolved normal").format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kPosition, "Resolved position").format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kTexCoord, "Resolved texture coordinates").format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kOcclusionHeatmap, "Occlusion heatmap").format(ResourceFormat::R32Float);
    reflector.addOutput(kOutput, "Final shaded output").format(ResourceFormat::RGBA32Float);
    return reflector;
}

void NaniteRaster::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    if (!mpNaniteAsset) return;

    mpVisibleClusters = mpDevice->createBuffer(
        mMaxVisibleClusters * sizeof(uint32_t),
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
        MemoryType::DeviceLocal);

    mpVisibleClusterCount = mpDevice->createBuffer(
        sizeof(uint32_t),
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
        MemoryType::DeviceLocal);

    mpFilteredClusters = mpDevice->createBuffer(
        mMaxVisibleClusters * sizeof(uint32_t),
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
        MemoryType::DeviceLocal);

    mpFilteredClusterCount = mpDevice->createBuffer(
        sizeof(uint32_t),
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
        MemoryType::DeviceLocal);

    mpCullStats = mpDevice->createBuffer(
        8 * sizeof(uint32_t),
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
        MemoryType::DeviceLocal);

    const uint32_t pageCount = std::max(1u, static_cast<uint32_t>(mpNaniteAsset->getCpuAsset().pages.size()));
    if (!mpScene || !mpScene->getNanitePageRequestBuffer())
    {
        mpPageRequests = mpDevice->createBuffer(
            pageCount * sizeof(uint32_t),
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
            MemoryType::DeviceLocal);
    }

    std::vector<uint32_t> defaultFallback(pageCount, 0u);
    mpDefaultPageFallback = mpDevice->createBuffer(
        pageCount * sizeof(uint32_t),
        ResourceBindFlags::ShaderResource,
        MemoryType::DeviceLocal,
        defaultFallback.data());

    const uint32_t clusterCount = static_cast<uint32_t>(mpNaniteAsset->getCpuAsset().clusters.size());
    if (clusterCount > 0)
    {
        mpClusterDebugReason = mpDevice->createBuffer(
            clusterCount * sizeof(uint32_t),
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
            MemoryType::DeviceLocal);
    }

    mRootNodeIndices.clear();
    for (size_t i = 0; i < mpNaniteAsset->getCpuAsset().hierarchyNodes.size(); ++i)
    {
        if (mpNaniteAsset->getCpuAsset().hierarchyNodes[i].clusterGroupIndex == std::numeric_limits<uint32_t>::max())
            mRootNodeIndices.push_back(static_cast<uint32_t>(i));
    }
    if (!mRootNodeIndices.empty())
    {
        mpRootNodes = mpDevice->createBuffer(
            mRootNodeIndices.size() * sizeof(uint32_t),
            ResourceBindFlags::ShaderResource,
            MemoryType::DeviceLocal,
            mRootNodeIndices.data());
    }

    static const DispatchArguments kZeroDispatchArgs = {0, 1, 1};
    mpRasterIndirectArgs = mpDevice->createBuffer(
        sizeof(DispatchArguments),
        ResourceBindFlags::IndirectArg | ResourceBindFlags::UnorderedAccess,
        MemoryType::DeviceLocal,
        &kZeroDispatchArgs);

    mUseGpuCulling = !mRootNodeIndices.empty() && mpNaniteAsset->getHierarchyBuffer() != nullptr;

    if (mpScene && pRenderContext)
        mpScene->uploadNaniteAsset(pRenderContext);

    const uint2 res = compileData.defaultTexDims;
    mpHZBMip0 = mpDevice->createTexture2D(
        res.x, res.y, ResourceFormat::R32Float, 1, 1, nullptr,
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mpHZBMip1 = mpDevice->createTexture2D(
        std::max(1u, res.x / 2), std::max(1u, res.y / 2), ResourceFormat::R32Float, 1, 1, nullptr,
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mpPrevHZBMip0 = mpDevice->createTexture2D(
        res.x, res.y, ResourceFormat::R32Float, 1, 1, nullptr,
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mpPrevHZBMip1 = mpDevice->createTexture2D(
        std::max(1u, res.x / 2), std::max(1u, res.y / 2), ResourceFormat::R32Float, 1, 1, nullptr,
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mHasPrevHZB = false;
    if (pRenderContext)
    {
        pRenderContext->clearTexture(mpPrevHZBMip0.get(), float4(1.f, 0.f, 0.f, 0.f));
        pRenderContext->clearTexture(mpPrevHZBMip1.get(), float4(1.f, 0.f, 0.f, 0.f));
    }
}

void NaniteRaster::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    mpNaniteAsset = mpScene ? mpScene->getNaniteAsset() : nullptr;
}

ref<Buffer> NaniteRaster::getPageRequestBuffer() const
{
    if (mpScene)
    {
        const ref<Buffer>& pBuffer = mpScene->getNanitePageRequestBuffer();
        if (pBuffer) return pBuffer;
    }
    return mpPageRequests;
}

void NaniteRaster::processStreaming(RenderContext* pRenderContext)
{
    if (!mpScene || !mEnableStreaming) return;

    if (auto pPageRequests = getPageRequestBuffer())
    {
        mpScene->collectNanitePageRequests(pRenderContext, pPageRequests);
        mpScene->processNaniteStreamingRequests(pRenderContext);
    }
}

void NaniteRaster::bindCamera(ShaderVar& var, const float4x4& viewProj, const float3& cameraPos, uint2 screenSize)
{
    var["gCamera"]["viewProj"] = viewProj;
    var["gCamera"]["invViewProj"] = inverse(viewProj);
    var["gCamera"]["cameraPos"] = cameraPos;
    var["gCamera"]["lodErrorThreshold"] = mLodErrorThreshold;
    var["gCamera"]["screenSize"] = float2(screenSize);
    var["gCamera"]["nearPlane"] = 0.01f;
    var["gCamera"]["farPlane"] = 1000.f;
    var["gCamera"]["clusterCount"] = static_cast<uint32_t>(mpNaniteAsset->getCpuAsset().clusters.size());
    var["gCamera"]["hierarchyNodeCount"] = static_cast<uint32_t>(mpNaniteAsset->getCpuAsset().hierarchyNodes.size());
    var["gCamera"]["debugMode"] = static_cast<uint32_t>(mDebugMode);
    var["gCamera"]["enableHZB"] = mEnableOcclusion ? 1u : 0u;
    var["gCamera"]["enableOcclusion"] = mEnableOcclusion ? 1u : 0u;
    var["gCamera"]["rootNodeCount"] = static_cast<uint32_t>(mRootNodeIndices.size());
}

void NaniteRaster::fillAllVisibleClusters()
{
    const uint32_t clusterCount = static_cast<uint32_t>(mpNaniteAsset->getCpuAsset().clusters.size());
    if (clusterCount == 0 || !mpVisibleClusters || !mpVisibleClusterCount) return;

    const uint32_t count = std::min(clusterCount, mMaxVisibleClusters);
    std::vector<uint32_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0u);
    mpVisibleClusters->setBlob(indices.data(), 0, count * sizeof(uint32_t));
    mpVisibleClusterCount->setBlob(&count, 0, sizeof(uint32_t));
    mFrameStats.visibleClusters = count;
}

void NaniteRaster::clearOutputs(RenderContext* pRenderContext, const RenderData& renderData)
{
    pRenderContext->clearUAV(mpVisibleClusterCount->getUAV().get(), uint4(0));
    pRenderContext->clearUAV(mpFilteredClusterCount->getUAV().get(), uint4(0));
    pRenderContext->clearUAV(mpCullStats->getUAV().get(), uint4(0));

    if (mpClusterDebugReason)
        pRenderContext->clearUAV(mpClusterDebugReason->getUAV().get(), uint4(0xFFFFFFFF, 0, 0, 0));

    if (auto pPageRequests = getPageRequestBuffer())
    {
        const uint32_t pageCount = static_cast<uint32_t>(mpNaniteAsset->getCpuAsset().pages.size());
        std::vector<uint32_t> zeros(pageCount, 0);
        pPageRequests->setBlob(zeros.data(), 0, zeros.size() * sizeof(uint32_t));
    }

    pRenderContext->clearTexture(renderData.getTexture(kDepth).get(), float4(1.f, 0.f, 0.f, 0.f));
    pRenderContext->clearUAV(renderData.getTexture(kVisibility)->getUAV().get(), uint4(0xFFFFFFFF, 0xFFFFFFFF, 0, 0));
    pRenderContext->clearUAV(renderData.getTexture(kClusterID)->getUAV().get(), uint4(0xFFFFFFFF, 0, 0, 0));
}

void NaniteRaster::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpNaniteAsset || !mPassesPrepared || !mpVisibleClusters) return;

    FALCOR_PROFILE(pRenderContext, "NaniteRaster::execute");
    const auto startTime = std::chrono::high_resolution_clock::now();

    const uint2 screenSize = renderData.getDefaultTextureDims();
    float4x4 viewProj = float4x4::identity();
    float3 cameraPos = float3(0.f, 0.f, 5.f);

    if (mpScene)
    {
        viewProj = mpScene->getCamera()->getViewProjMatrix();
        cameraPos = mpScene->getCamera()->getPosition();
    }

    clearOutputs(pRenderContext, renderData);

    if (mpScene && mEnableStreaming)
        mpScene->beginNaniteStreamingFrame();

    const bool useHierarchyCull = !mFullResidentRaster && mUseGpuCulling;

    if (!useHierarchyCull)
    {
        fillAllVisibleClusters();
    }
    else
    {
        auto var = mpCullPass->getRootVar();
        var["gHierarchy"] = mpNaniteAsset->getHierarchyBuffer();
        var["gClusters"] = mpNaniteAsset->getClusterBuffer();
        var["gPageResidency"] = mpNaniteAsset->getPageResidencyBuffer();
        var["gRootNodes"] = mpRootNodes;
        var["gVisibleClusters"] = mpVisibleClusters;
        var["gVisibleClusterCount"] = mpVisibleClusterCount;
        var["gCullStats"] = mpCullStats;
        var["gPageRequests"] = getPageRequestBuffer();
        var["gClusterDebugReason"] = mpClusterDebugReason;
        if (mpScene)
        {
            const ref<Buffer>& pFallback = mpScene->getNanitePageFallbackBuffer();
            if (pFallback) var["gPageFallback"] = pFallback;
        }
        else
            var["gPageFallback"] = mpDefaultPageFallback;
        bindCamera(var, viewProj, cameraPos, screenSize);
        const uint32_t rootCount = std::max(1u, static_cast<uint32_t>(mRootNodeIndices.size()));
        mpCullPass->execute(pRenderContext, rootCount, 1, 1);
        processStreaming(pRenderContext);
    }

    ref<Buffer> pRasterClusters = mpVisibleClusters;
    ref<Buffer> pRasterClusterCount = mpVisibleClusterCount;

    if (mEnableOcclusion && mHasPrevHZB)
    {
        pRenderContext->clearUAV(mpFilteredClusterCount->getUAV().get(), uint4(0));

        auto occVar = mpOcclusionPass->getRootVar();
        occVar["gClusters"] = mpNaniteAsset->getClusterBuffer();
        occVar["gVisibleClusters"] = mpVisibleClusters;
        occVar["gVisibleClusterCount"] = mpVisibleClusterCount;
        occVar["gFilteredClusters"] = mpFilteredClusters;
        occVar["gFilteredClusterCount"] = mpFilteredClusterCount;
        occVar["gCullStats"] = mpCullStats;
        occVar["gHZBMip0Occ"] = mpPrevHZBMip0;
        occVar["gHZBMip1Occ"] = mpPrevHZBMip1;
        occVar["gViewProj"] = viewProj;
        occVar["gCameraPos"] = cameraPos;
        occVar["gScreenSizeOcc"] = float2(screenSize);
        occVar["gHZBMip1Size"] = float2(std::max(1u, screenSize.x / 2), std::max(1u, screenSize.y / 2));
        mpOcclusionPass->execute(pRenderContext, mMaxVisibleClusters, 1, 1);

        pRasterClusters = mpFilteredClusters;
        pRasterClusterCount = mpFilteredClusterCount;
    }

    {
        auto var = mpRasterPass->getRootVar();
        var["gClusters"] = mpNaniteAsset->getClusterBuffer();
        var["gVertices"] = mpNaniteAsset->getVertexBuffer();
        var["gIndices"] = mpNaniteAsset->getIndexBuffer();
        var["gVisibleClusters"] = pRasterClusters;
        var["gVisibleClusterCount"] = pRasterClusterCount;
        var["gVisibility"] = renderData.getTexture(kVisibility);
        var["gDepth"] = renderData.getTexture(kDepth);
        var["gClusterID"] = renderData.getTexture(kClusterID);
        bindCamera(var, viewProj, cameraPos, screenSize);

        if (mpBuildIndirectPass && mpRasterIndirectArgs)
        {
            auto indirectVar = mpBuildIndirectPass->getRootVar();
            indirectVar["gVisibleClusterCount"] = pRasterClusterCount;
            indirectVar["gRasterDispatchArgs"] = mpRasterIndirectArgs;
            mpBuildIndirectPass->execute(pRenderContext, 1, 1, 1);
            mpRasterPass->executeIndirect(pRenderContext, mpRasterIndirectArgs.get());
        }
        else
        {
            mpRasterPass->execute(pRenderContext, mMaxVisibleClusters, 1, 1);
        }
    }

    if (mEnableOcclusion)
    {
        auto var0 = mpHzbMip0Pass->getRootVar();
        var0["gDepth"] = renderData.getTexture(kDepth);
        var0["gHZBMip0"] = mpHZBMip0;
        var0["gScreenSize"] = float2(screenSize);
        mpHzbMip0Pass->execute(pRenderContext, screenSize.x, screenSize.y, 1);

        auto var1 = mpHzbMip1Pass->getRootVar();
        var1["gHZBMip0"] = mpHZBMip0;
        var1["gHZBMip1"] = mpHZBMip1;
        var1["gScreenSize"] = float2(screenSize);
        mpHzbMip1Pass->execute(pRenderContext, std::max(1u, screenSize.x / 2), std::max(1u, screenSize.y / 2), 1);

        pRenderContext->copyResource(mpPrevHZBMip0.get(), mpHZBMip0.get());
        pRenderContext->copyResource(mpPrevHZBMip1.get(), mpHZBMip1.get());
        mHasPrevHZB = true;

        if (mDebugMode == NaniteDebugMode::OcclusionHeatmap)
        {
            auto heatVar = mpHeatmapPass->getRootVar();
            heatVar["gDepth"] = renderData.getTexture(kDepth);
            heatVar["gHZBMip0"] = mpHZBMip0;
            heatVar["gOcclusionHeatmap"] = renderData.getTexture(kOcclusionHeatmap);
            heatVar["gScreenSize"] = float2(screenSize);
            mpHeatmapPass->execute(pRenderContext, screenSize.x, screenSize.y, 1);
        }
    }

    {
        auto var = mpResolvePass->getRootVar();
        var["gClusters"] = mpNaniteAsset->getClusterBuffer();
        var["gVertices"] = mpNaniteAsset->getVertexBuffer();
        var["gIndices"] = mpNaniteAsset->getIndexBuffer();
        var["gVisibility"] = renderData.getTexture(kVisibility);
        var["gDepth"] = renderData.getTexture(kDepth);
        var["gPosition"] = renderData.getTexture(kPosition);
        var["gNormal"] = renderData.getTexture(kNormal);
        var["gTexCoord"] = renderData.getTexture(kTexCoord);
        var["gBaseColor"] = renderData.getTexture(kBaseColor);
        if (mpClusterDebugReason)
            var["gClusterDebugReason"] = mpClusterDebugReason;
        bindCamera(var, viewProj, cameraPos, screenSize);
        mpResolvePass->execute(pRenderContext, screenSize.x, screenSize.y, 1);
    }

    ref<Texture> pOutput = renderData.getTexture(kOutput);
    ref<Texture> pSource = renderData.getTexture(kBaseColor);
    if (mDebugMode == NaniteDebugMode::OcclusionHeatmap && mEnableOcclusion)
    {
        pSource = renderData.getTexture(kOcclusionHeatmap);
    }
    pRenderContext->blit(pSource->getSRV(), pOutput->getRTV());

    const auto endTime = std::chrono::high_resolution_clock::now();
    mFrameStats.frameTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    if (pRasterClusterCount)
    {
        uint32_t visible = 0;
        pRasterClusterCount->getBlob(&visible, 0, sizeof(uint32_t));
        mFrameStats.visibleClusters = visible;
    }

    if (mpCullStats)
    {
        uint32_t stats[8] = {};
        mpCullStats->getBlob(stats, 0, sizeof(stats));
        mFrameStats.testedClusters = stats[kCullTested];
        mFrameStats.occludedClusters = stats[kCullOccluded];
        if (mFrameStats.testedClusters == 0 && mpVisibleClusterCount)
        {
            uint32_t preOcclusionVisible = 0;
            mpVisibleClusterCount->getBlob(&preOcclusionVisible, 0, sizeof(uint32_t));
            mFrameStats.testedClusters = preOcclusionVisible;
        }
    }
}

void NaniteRaster::renderUI(Gui::Widgets& widget)
{
    widget.dropdown("Debug mode", mDebugMode);
    widget.var("LOD error threshold", mLodErrorThreshold, 0.1f, 20.f);
    widget.checkbox("Enable occlusion", mEnableOcclusion);
    widget.checkbox("Enable streaming", mEnableStreaming);
    widget.text("Visible clusters: " + std::to_string(mFrameStats.visibleClusters));
    widget.text("Tested clusters: " + std::to_string(mFrameStats.testedClusters));
    widget.text("Occluded clusters: " + std::to_string(mFrameStats.occludedClusters));
    widget.text("Frame time (ms): " + std::to_string(mFrameStats.frameTimeMs));

    if (mpScene)
    {
        const NaniteStreamingStats stats = mpScene->getNaniteStreamingStats();
        widget.text("Resident pages: " + std::to_string(stats.residentPages) + " / " + std::to_string(stats.totalPages));
        widget.text("Resident bytes: " + std::to_string(stats.residentBytes));
        widget.text("VRAM budget (bytes): " + std::to_string(stats.budgetBytes));
        widget.text("Page misses (total): " + std::to_string(stats.pageMissCount));
        widget.text("Page misses (frame): " + std::to_string(stats.framePageMissCount));
        widget.text("Evictions: " + std::to_string(stats.evictionCount));

        uint64_t budgetMb = stats.budgetBytes / (1024 * 1024);
        if (widget.var("VRAM budget (MB)", budgetMb, 1ull, 4096ull))
        {
            mpScene->setNaniteVramBudgetBytes(budgetMb * 1024 * 1024);
        }
    }
}
