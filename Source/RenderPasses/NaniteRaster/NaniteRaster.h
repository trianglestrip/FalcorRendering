#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Scene/Nanite/NaniteAsset.h"

using namespace Falcor;

enum class NaniteDebugMode
{
    Shaded = 0,
    ClusterID = 1,
    Depth = 2,
    CullReason = 3,
    LOD = 4,
    OcclusionHeatmap = 5,
};

FALCOR_ENUM_INFO(
    NaniteDebugMode,
    {
        { NaniteDebugMode::Shaded, "Shaded" },
        { NaniteDebugMode::ClusterID, "ClusterID" },
        { NaniteDebugMode::Depth, "Depth" },
        { NaniteDebugMode::CullReason, "CullReason" },
        { NaniteDebugMode::LOD, "LOD" },
        { NaniteDebugMode::OcclusionHeatmap, "OcclusionHeatmap" },
    }
);
FALCOR_ENUM_REGISTER(NaniteDebugMode);

class NaniteRaster : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(NaniteRaster, "NaniteRaster", "Nanite software rasterization pass.");

    static ref<NaniteRaster> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<NaniteRaster>(pDevice, props);
    }

    NaniteRaster(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual     void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;

    void setDebugMode(NaniteDebugMode mode) { mDebugMode = mode; }
    NaniteDebugMode getDebugMode() const { return mDebugMode; }
    void setLodErrorThreshold(float threshold) { mLodErrorThreshold = threshold; }
    void setEnableOcclusion(bool enable) { mEnableOcclusion = enable; }

    struct FrameStats
    {
        uint32_t visibleClusters = 0;
        uint32_t testedClusters = 0;
        uint32_t occludedClusters = 0;
        float frameTimeMs = 0.f;
    };
    FrameStats getFrameStats() const { return mFrameStats; }

private:
    void preparePasses();
    void bindCamera(ShaderVar& var, const float4x4& viewProj, const float3& cameraPos, uint2 screenSize);
    void clearOutputs(RenderContext* pRenderContext, const RenderData& renderData);
    void fillAllVisibleClusters();
    void processStreaming(RenderContext* pRenderContext);
    ref<Buffer> getPageRequestBuffer() const;

    ref<Device> mpDevice;
    ref<NaniteAsset> mpNaniteAsset;
    ref<Scene> mpScene;

    ref<ComputePass> mpCullPass;
    ref<ComputePass> mpBuildIndirectPass;
    ref<ComputePass> mpRasterPass;
    ref<ComputePass> mpResolvePass;
    ref<ComputePass> mpHzbMip0Pass;
    ref<ComputePass> mpHzbMip1Pass;
    ref<ComputePass> mpOcclusionPass;
    ref<ComputePass> mpHeatmapPass;

    ref<Buffer> mpVisibleClusters;
    ref<Buffer> mpVisibleClusterCount;
    ref<Buffer> mpFilteredClusters;
    ref<Buffer> mpFilteredClusterCount;
    ref<Buffer> mpCullStats;
    ref<Buffer> mpPageRequests;
    ref<Buffer> mpDefaultPageFallback;
    ref<Buffer> mpRootNodes;
    ref<Buffer> mpClusterDebugReason;
    ref<Buffer> mpRasterIndirectArgs;

    std::vector<uint32_t> mRootNodeIndices;

    ref<Texture> mpHZBMip0;
    ref<Texture> mpHZBMip1;
    ref<Texture> mpPrevHZBMip0;
    ref<Texture> mpPrevHZBMip1;

    NaniteDebugMode mDebugMode = NaniteDebugMode::Shaded;
    float mLodErrorThreshold = 2.f;
    bool mEnableOcclusion = false;
    bool mEnableStreaming = true;
    bool mFullResidentRaster = false;
    bool mUseGpuCulling = false;
    uint32_t mMaxVisibleClusters = 65536;
    FrameStats mFrameStats{};
    bool mPassesPrepared = false;
    bool mHasPrevHZB = false;
};
