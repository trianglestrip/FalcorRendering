#include "NaniteViewer.h"
#include "Scene/SceneBuilder.h"
#include "Utils/Settings/Settings.h"

#include <fstream>
#include <iostream>

namespace
{
struct Arguments
{
    std::filesystem::path fnanitePath = "data/nanite/cube.fnanite";
    std::filesystem::path screenshotPath;
    std::filesystem::path csvPath;
    bool headlessCapture = false;
};

struct AABBVertex
{
    float3 pos;
    float4 color;
};

float4 clusterColor(uint32_t clusterIndex, bool selected)
{
    if (selected) return float4(1.f, 1.f, 0.2f, 1.f);

    const float hue = float(clusterIndex * 0.6180339887f);
    const float r = std::abs(std::sin(hue * 6.2831853f));
    const float g = std::abs(std::sin((hue + 0.33f) * 6.2831853f));
    const float b = std::abs(std::sin((hue + 0.66f) * 6.2831853f));
    return float4(r, g, b, 0.85f);
}

void appendBoxLines(std::vector<AABBVertex>& vertices, const Nanite::Bounds& bounds, const float4& color)
{
    const float3 corners[8] = {
        float3(bounds.min.x, bounds.min.y, bounds.min.z),
        float3(bounds.max.x, bounds.min.y, bounds.min.z),
        float3(bounds.max.x, bounds.min.y, bounds.max.z),
        float3(bounds.min.x, bounds.min.y, bounds.max.z),
        float3(bounds.min.x, bounds.max.y, bounds.min.z),
        float3(bounds.max.x, bounds.max.y, bounds.min.z),
        float3(bounds.max.x, bounds.max.y, bounds.max.z),
        float3(bounds.min.x, bounds.max.y, bounds.max.z),
    };

    const uint32_t edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };

    for (const auto& edge : edges)
    {
        vertices.push_back({corners[edge[0]], color});
        vertices.push_back({corners[edge[1]], color});
    }
}

Arguments parseArguments(int argc, char** argv)
{
    Arguments args;
    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        auto requireValue = [&]() -> std::string
        {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + option);
            return argv[++i];
        };

        if (option == "--fnanite" || option == "-f")
        {
            args.fnanitePath = requireValue();
        }
        else if (option == "--screenshot")
        {
            args.screenshotPath = requireValue();
            args.headlessCapture = true;
        }
        else if (option == "--csv")
        {
            args.csvPath = requireValue();
            args.headlessCapture = true;
        }
        else if (option == "--help" || option == "-h")
        {
            std::cout << "NaniteViewer --fnanite <path.fnanite> [--screenshot out.png] [--csv perf.csv]\n";
            std::exit(0);
        }
    }
    return args;
}
} // namespace

FALCOR_EXPORT_D3D12_AGILITY_SDK

NaniteViewer::NaniteViewer(const SampleAppConfig& config) : SampleApp(config) {}

NaniteViewer::~NaniteViewer() {}

void NaniteViewer::onLoad(RenderContext* pRenderContext)
{
    prepareAABBDebugPass();
    if (!mAssetPath.empty())
    {
        loadAsset(mAssetPath);
        if (!mHeadlessCapture)
            setupRenderGraph(pRenderContext);
    }
}

void NaniteViewer::setupRenderGraph(RenderContext* pRenderContext)
{
    if (!mpScene || !mpNaniteAsset) return;

    mpRenderGraph = RenderGraph::create(getDevice(), "NaniteViewer");
    Properties passProps;
    passProps["lodErrorThreshold"] = mLodErrorThreshold;
    passProps["debugMode"] = std::string("Shaded");
    mpNaniteRasterPass = mpRenderGraph->createPass("NaniteRaster", "NaniteRaster", passProps);
    mpRenderGraph->markOutput("NaniteRaster.output");
    mpRenderGraph->setScene(mpScene);
    mpRenderGraph->onResize(getTargetFbo().get());
    if (!mpRenderGraph->compile(pRenderContext))
    {
        FALCOR_THROW("Failed to compile NaniteViewer render graph.");
    }
    mRenderGraphReady = true;
}

void NaniteViewer::onShutdown() {}

void NaniteViewer::prepareAABBDebugPass()
{
    mpAABBPass = RasterPass::create(
        getDevice(), "Samples/NaniteViewer/NaniteAABBDebug.3d.slang", "vsMain", "psMain");

    DepthStencilState::Desc dsDesc;
    dsDesc.setDepthEnabled(true).setDepthFunc(ComparisonFunc::Less);
    mpAABBPass->getState()->setDepthStencilState(DepthStencilState::create(dsDesc));
}

void NaniteViewer::loadAsset(const std::filesystem::path& path)
{
    mAssetPath = path;
    mpScene = SceneBuilder(getDevice(), path, Settings(), SceneBuilder::Flags::Default).getScene();
    if (!mpScene)
    {
        FALCOR_THROW("Failed to build scene for Nanite asset.");
    }

    mpNaniteAsset = mpScene->getNaniteAsset();
    if (!mpNaniteAsset)
    {
        FALCOR_THROW("Scene does not contain a Nanite asset.");
    }

    mpCamera = mpScene->getCamera();
    const float radius = std::max(mpScene->getSceneBounds().radius(), 0.5f);
    mpScene->setCameraSpeed(radius * 0.05f);
    mpScene->setCameraController(Scene::CameraControllerType::FirstPerson);
    mpScene->setCameraControlsEnabled(true);
    mpCamera->setDepthRange(std::max(0.01f, radius / 750.f), radius * 10.f);
    mpCamera->setAspectRatio(float(getTargetFbo()->getWidth()) / float(getTargetFbo()->getHeight()));

    if (mHeadlessCapture)
    {
        mShowClusterAABBs = true;
        mCaptureScreenshot = !mScreenshotPath.empty();
        mDumpCsv = !mCsvPath.empty();
    }

    rebuildClusterAABBGeometry();

    mRenderGraphReady = false;
    mpRenderGraph = nullptr;
    mpNaniteRasterPass = nullptr;
}

void NaniteViewer::rebuildClusterAABBGeometry()
{
    mpAABBVao = nullptr;
    mpAABBVertexBuffer = nullptr;
    mAABBLineVertexCount = 0;

    if (!mpNaniteAsset) return;

    const Nanite::Asset& asset = mpNaniteAsset->getCpuAsset();
    std::vector<AABBVertex> vertices;
    appendBoxLines(vertices, asset.bounds, float4(0.85f, 0.85f, 0.85f, 1.f));

    if (mShowClusterAABBs)
    {
        for (uint32_t i = 0; i < asset.clusters.size(); ++i)
        {
            appendBoxLines(vertices, asset.clusters[i].bounds, clusterColor(i, i == mSelectedCluster));
        }
    }
    else if (mSelectedCluster < asset.clusters.size())
    {
        appendBoxLines(vertices, asset.clusters[mSelectedCluster].bounds, clusterColor(mSelectedCluster, true));
    }

    mAABBLineVertexCount = static_cast<uint32_t>(vertices.size());
    if (vertices.empty()) return;

    mpAABBVertexBuffer = getDevice()->createBuffer(
        mAABBLineVertexCount * sizeof(AABBVertex),
        ResourceBindFlags::Vertex,
        MemoryType::Upload,
        vertices.data());

    ref<VertexBufferLayout> bufferLayout = VertexBufferLayout::create();
    bufferLayout->addElement("POSITION", offsetof(AABBVertex, pos), ResourceFormat::RGB32Float, 1, 0);
    bufferLayout->addElement("COLOR", offsetof(AABBVertex, color), ResourceFormat::RGBA32Float, 1, 1);
    ref<VertexLayout> layout = VertexLayout::create();
    layout->addBufferLayout(0, bufferLayout);
    mpAABBVao = Vao::create(Vao::Topology::LineList, layout, {mpAABBVertexBuffer});
}

void NaniteViewer::onResize(uint32_t width, uint32_t height)
{
    if (mpCamera) mpCamera->setAspectRatio(float(width) / float(height));
    if (mpRenderGraph)
    {
        mpRenderGraph->onResize(getTargetFbo().get());
    }
}

void NaniteViewer::onFrameRender(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo)
{
    if (!mpNaniteAsset) return;

    const float4 clearColor(0.12f, 0.14f, 0.18f, 1.f);
    pRenderContext->clearFbo(pTargetFbo.get(), clearColor, 1.f, 0, FboAttachmentType::All);

    mpScene->update(pRenderContext, getGlobalClock().getTime());

    if (mRenderGraphReady && mpRenderGraph)
    {
        mpRenderGraph->execute(pRenderContext);

        ref<Texture> pNaniteOutput = mpRenderGraph->getOutput("NaniteRaster.output")->asTexture();
        if (pNaniteOutput)
        {
            pRenderContext->blit(pNaniteOutput->getSRV(), pTargetFbo->getRenderTargetView(0));
        }
    }

    mLastFrameTimeMs = getFrameRate().getLastFrameTime();
    mVisibleClusters = static_cast<uint32_t>(mpNaniteAsset->getCpuAsset().clusters.size());

    if (mShowClusterAABBs)
        drawClusterAABBs(pRenderContext, pTargetFbo);
    if (mDumpCsv) dumpPerformanceCsv();
    maybeCaptureScreenshot(pRenderContext, pTargetFbo);
}

void NaniteViewer::drawClusterAABBs(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo)
{
    if (!mpAABBPass || !mpAABBVao || mAABBLineVertexCount == 0 || !mpCamera) return;

    mpAABBPass->getRootVar()["gCB"]["viewProj"] = mpCamera->getViewProjMatrix();
    mpAABBPass->getState()->setFbo(pTargetFbo);
    mpAABBPass->getState()->setVao(mpAABBVao);
    mpAABBPass->draw(pRenderContext, mAABBLineVertexCount, 0);
}

void NaniteViewer::maybeCaptureScreenshot(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo)
{
    if (!mCaptureScreenshot) return;

    pRenderContext->submit(true);
    if (!mScreenshotPath.parent_path().empty())
    {
        std::filesystem::create_directories(mScreenshotPath.parent_path());
    }
    pTargetFbo->getColorTexture(0)->captureToFile(
        0, 0, mScreenshotPath, Bitmap::FileFormat::PngFile, Bitmap::ExportFlags::None, false);
    mCaptureScreenshot = false;
    logInfo("NaniteViewer screenshot saved to {}", mScreenshotPath.string());

    if (mHeadlessCapture)
    {
        shutdown(0);
    }
}

void NaniteViewer::dumpPerformanceCsv()
{
    if (!mCsvPath.parent_path().empty()) std::filesystem::create_directories(mCsvPath.parent_path());

    const bool writeHeader = !std::filesystem::exists(mCsvPath);
    std::ofstream csv(mCsvPath, std::ios::app);
    if (writeHeader) csv << "frame,frame_time_ms,visible_clusters\n";
    csv << getFrameRate().getFrameCount() << "," << mLastFrameTimeMs << "," << mVisibleClusters << "\n";
    mDumpCsv = false;

    if (mHeadlessCapture && !mCaptureScreenshot)
    {
        shutdown(0);
    }
}

void NaniteViewer::onGuiRender(Gui* pGui)
{
    Gui::Window w(pGui, "Nanite Viewer", {420, 520}, {10, 10});
    renderGlobalUI(pGui);

    if (!mpNaniteAsset)
    {
        w.text("No asset loaded.");
        return;
    }

    const Nanite::Asset& asset = mpNaniteAsset->getCpuAsset();
    const NaniteMemoryStats mem = mpNaniteAsset->getMemoryStats();

    w.text("Asset: " + mAssetPath.filename().string());
    w.text("Version: " + std::to_string(asset.version));
    w.text("Clusters: " + std::to_string(asset.clusters.size()));
    w.text("Visible clusters: " + std::to_string(mVisibleClusters));
    w.text("GPU bytes: " + std::to_string(mem.totalGpuBytes));

    w.var("Debug mode", mDebugMode, 0u, 5u);
    w.var("LOD error threshold", mLodErrorThreshold, 0.1f, 20.f);

    const Nanite::Bounds& bb = asset.bounds;
    w.text("Bounds min: (" + std::to_string(bb.min.x) + ", " + std::to_string(bb.min.y) + ", " + std::to_string(bb.min.z) + ")");
    w.text("Bounds max: (" + std::to_string(bb.max.x) + ", " + std::to_string(bb.max.y) + ", " + std::to_string(bb.max.z) + ")");

    const bool prevShowAABBs = mShowClusterAABBs;
    w.checkbox("Show cluster AABB overlay", mShowClusterAABBs);
    if (w.button("Capture screenshot")) mCaptureScreenshot = true;
    if (w.button("Dump performance CSV")) mDumpCsv = true;

    if (!asset.clusters.empty())
    {
        const uint32_t prevSelectedCluster = mSelectedCluster;
        w.slider("Selected cluster", mSelectedCluster, 0u, static_cast<uint32_t>(asset.clusters.size() - 1));
        const Nanite::Cluster& cluster = asset.clusters[mSelectedCluster];
        w.text("Cluster tris: " + std::to_string(cluster.triangleCount));
        w.text("Cluster verts: " + std::to_string(cluster.vertexCount));
        w.text("Material: " + std::to_string(cluster.materialIndex));
        w.text("LOD error: " + std::to_string(cluster.geometricError));
        w.text("AABB min: (" + std::to_string(cluster.bounds.min.x) + ", " + std::to_string(cluster.bounds.min.y) + ", " +
               std::to_string(cluster.bounds.min.z) + ")");
        w.text("AABB max: (" + std::to_string(cluster.bounds.max.x) + ", " + std::to_string(cluster.bounds.max.y) + ", " +
               std::to_string(cluster.bounds.max.z) + ")");

        if (prevShowAABBs != mShowClusterAABBs || prevSelectedCluster != mSelectedCluster)
        {
            rebuildClusterAABBGeometry();
        }
    }

    w.text("FPS: " + getFrameRate().getMsg());
}

bool NaniteViewer::onKeyEvent(const KeyboardEvent& keyEvent)
{
    if (mpScene && mpScene->onKeyEvent(keyEvent)) return true;
    return false;
}

bool NaniteViewer::onMouseEvent(const MouseEvent& mouseEvent)
{
    if (mpScene && mpScene->onMouseEvent(mouseEvent)) return true;
    return false;
}

int runMain(int argc, char** argv)
{
    Arguments args = parseArguments(argc, argv);

    SampleAppConfig config;
    config.windowDesc.title = "Nanite Viewer";
    config.windowDesc.resizableWindow = true;
    config.headless = args.headlessCapture;
    if (args.headlessCapture)
    {
        config.windowDesc.width = 512;
        config.windowDesc.height = 512;
        config.showUI = false;
    }

    NaniteViewer app(config);
    app.setAssetPath(args.fnanitePath);
    if (!args.screenshotPath.empty()) app.setScreenshotPath(args.screenshotPath);
    if (!args.csvPath.empty()) app.setCsvPath(args.csvPath);
    app.setHeadlessCapture(args.headlessCapture);
    return app.run();
}

int main(int argc, char** argv)
{
    return catchAndReportAllExceptions([&]() { return runMain(argc, argv); });
}
