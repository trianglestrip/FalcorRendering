#pragma once
#include "Falcor.h"
#include "Core/Pass/RasterPass.h"
#include "Core/SampleApp.h"
#include "RenderGraph/RenderGraph.h"
#include "Scene/Nanite/NaniteAsset.h"

using namespace Falcor;

class NaniteViewer : public SampleApp
{
public:
    NaniteViewer(const SampleAppConfig& config);
    ~NaniteViewer() override;

    void onLoad(RenderContext* pRenderContext) override;
    void onShutdown() override;
    void onResize(uint32_t width, uint32_t height) override;
    void onFrameRender(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo) override;
    void onGuiRender(Gui* pGui) override;
    bool onKeyEvent(const KeyboardEvent& keyEvent) override;
    bool onMouseEvent(const MouseEvent& mouseEvent) override;

    void setAssetPath(const std::filesystem::path& path) { mAssetPaths = {path}; }
    void setAssetPaths(const std::vector<std::filesystem::path>& paths) { mAssetPaths = paths; }
    void setScreenshotPath(const std::filesystem::path& path) { mScreenshotPath = path; }
    void setCsvPath(const std::filesystem::path& path) { mCsvPath = path; }
    void setHeadlessCapture(bool enable) { mHeadlessCapture = enable; }

protected:
    void loadAsset(const std::filesystem::path& path);
    void loadAssets(const std::vector<std::filesystem::path>& paths);
    void setupRenderGraph(RenderContext* pRenderContext);

private:
    void prepareAABBDebugPass();
    void rebuildClusterAABBGeometry();
    void drawClusterAABBs(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo);
    void maybeCaptureScreenshot(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo);
    void dumpPerformanceCsv();

    ref<Scene> mpScene;
    ref<NaniteAsset> mpNaniteAsset;
    ref<Camera> mpCamera;
    ref<RenderGraph> mpRenderGraph;
    ref<RenderPass> mpNaniteRasterPass;
    ref<RasterPass> mpAABBPass;
    ref<Vao> mpAABBVao;
    ref<Buffer> mpAABBVertexBuffer;
    uint32_t mAABBLineVertexCount = 0;

    std::filesystem::path mAssetPath;
    std::vector<std::filesystem::path> mAssetPaths;
    std::filesystem::path mScreenshotPath = "data/nanite/screenshots/viewer_capture.png";
    std::filesystem::path mCsvPath = "data/nanite/nanite_perf.csv";
    bool mShowClusterAABBs = false;
    bool mCaptureScreenshot = false;
    bool mDumpCsv = false;
    bool mHeadlessCapture = false;
    bool mRenderGraphReady = false;
    uint32_t mSelectedCluster = 0;
    uint32_t mVisibleClusters = 0;
    float mLastFrameTimeMs = 0.f;
    float mLodErrorThreshold = 2.f;
    uint32_t mDebugMode = 0;
};
