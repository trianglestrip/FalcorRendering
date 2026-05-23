/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#include "PBRTOfflineRenderer.h"

FALCOR_EXPORT_D3D12_AGILITY_SDK
static const float4 kClear = float4(0.1f, 0.1f, 0.12f, 1.f);

PBRTOfflineRenderer::PBRTOfflineRenderer(const SampleAppConfig& c) : SampleApp(c), mExecutor(std::thread::hardware_concurrency()) { buildTaskGraph(); }
PBRTOfflineRenderer::~PBRTOfflineRenderer() { mExecutor.wait_for_all(); }

void PBRTOfflineRenderer::onLoad(RenderContext* pCtx)
{
    if (!mScenePath.empty()) loadScene(pCtx);
    else logInfo("No scene. Drag .pbrt file or use --scene <path>");
}

void PBRTOfflineRenderer::onShutdown() { mExecutor.wait_for_all(); mpRasterPass = nullptr; mpScene = nullptr; }

void PBRTOfflineRenderer::loadScene(RenderContext* pCtx)
{
    logInfo("Loading: {}", mScenePath.string());
    try
    {
        mpScene = Scene::create(getDevice(), mScenePath);
        if (!mpScene) { logError("Failed to load scene."); return; }

        auto pCam = mpScene->getCamera();
        float r = mpScene->getSceneBounds().radius();
        mpScene->setCameraSpeed(r * 0.25f);
        pCam->setDepthRange(std::max(0.1f, r / 750.f), r * 10.f);
        const auto& wd = getConfig().windowDesc;
        pCam->setAspectRatio(float(wd.width) / float(wd.height));

        mSceneLoaded = true; mFrameCount = 0; mStartTime = getGlobalClock().getTime();

        // Enable emissive lights and build light collection (needed for PBRT area lights)
        mpScene->getRenderSettings().useEmissiveLights = true;
        mpScene->getLightCollection(pCtx);

        logInfo("Scene OK. Geometry: {}, Lights: a={}, e={}, env={}",
            mpScene->getGeometryCount(), mpScene->useAnalyticLights(),
            mpScene->useEmissiveLights(), mpScene->useEnvLight());

        // Create raster pass (like HelloDXR)
        ProgramDesc d;
        d.addShaderModules(mpScene->getShaderModules());
        d.addShaderLibrary("Samples/PBRTOfflineRenderer/PBRTOfflineRenderer.3d.slang").vsEntry("vsMain").psEntry("psMain");
        d.addTypeConformances(mpScene->getTypeConformances());
        mpRasterPass = RasterPass::create(getDevice(), d, mpScene->getSceneDefines());
    }
    catch (const std::exception& e)
    {
        logError("Exception: {}", e.what());
    }
}

void PBRTOfflineRenderer::onFrameRender(RenderContext* pCtx, const ref<Fbo>& pFbo)
{
    pCtx->clearFbo(pFbo.get(), kClear, 1.f, 0, FboAttachmentType::All);
    if (!mSceneLoaded || !mpScene || !mpRasterPass) return;

    mpScene->update(pCtx, getGlobalClock().getTime());

    mpRasterPass->getState()->setFbo(pFbo);
    mpScene->rasterize(pCtx, mpRasterPass->getState().get(), mpRasterPass->getVars().get());

    mFrameCount++;
    if (mFrameCount == 1)
    {
        logInfo("First frame rendered.");
        if (!mOutputPath.empty()) saveOutput(pCtx);
    }
}

void PBRTOfflineRenderer::saveOutput(RenderContext* pCtx)
{
    if (mOutputPath.empty()) return;
    logInfo("Saving: {}", mOutputPath.string());

    // Async save via Taskflow (non-blocking)
    {
        std::lock_guard<std::mutex> lock(mSaveMutex);
        mSavePath = mOutputPath;
        // Capture the current color texture from the target FBO
        mSaveTexture = getTargetFbo()->getColorTexture(0);
        mSavePending = true;
    }

    // Run the taskflow to save asynchronously
    mExecutor.run(mTaskflow).wait();

    logInfo("Saved OK.");
}

void PBRTOfflineRenderer::buildTaskGraph()
{
    // Build a task graph for async operations:
    // 1. Save texture to file on a worker thread
    mTaskflow.emplace([this]() {
        std::lock_guard<std::mutex> lock(mSaveMutex);
        if (mSavePending && mSaveTexture)
        {
            try
            {
                mSaveTexture->captureToFile(0, 0, mSavePath, Bitmap::FileFormat::PngFile, Bitmap::ExportFlags::None, false);
            }
            catch (const std::exception&) {}
            mSavePending = false;
        }
    });
}

void PBRTOfflineRenderer::onGuiRender(Gui* pGui)
{
    Gui::Window w(pGui, "PBRT Renderer", {300, 200});
    if (mpScene) { w.text(fmt::format("Scene: {}", mScenePath.filename().string())); w.text(fmt::format("Geometry: {}", mpScene->getGeometryCount())); }
    else w.text("No scene. Drag .pbrt file or use --scene <path>");
    w.text(fmt::format("Frame: {}", mFrameCount));
    if (w.button("Save Screenshot")) { if (mOutputPath.empty()) { mOutputPath = mScenePath; mOutputPath.replace_extension(".exr"); } saveOutput(getRenderContext()); }
    renderGlobalUI(pGui);
}

bool PBRTOfflineRenderer::onKeyEvent(const KeyboardEvent& e)
{
    if (e.type == KeyboardEvent::Type::KeyPressed && e.key == Input::Key::S && e.hasModifier(Input::Modifier::Ctrl))
    {
        if (mOutputPath.empty()) { mOutputPath = mScenePath; mOutputPath.replace_extension(".exr"); }
        saveOutput(getRenderContext()); return true;
    }
    return mpScene && mpScene->onKeyEvent(e);
}

bool PBRTOfflineRenderer::onMouseEvent(const MouseEvent& e) { return mpScene && mpScene->onMouseEvent(e); }

void PBRTOfflineRenderer::onDroppedFile(const std::filesystem::path& p)
{
    auto ext = p.extension().string();
    if (ext == ".pbrt" || ext == ".pyscene") { mScenePath = p; loadScene(getRenderContext()); }
}

static void parseArgs(int argc, char** argv, PBRTOfflineRenderer& app)
{
    for (int i = 1; i < argc; i++) { std::string a = argv[i]; if (a == "--scene" && i + 1 < argc) app.setScenePath(argv[++i]); else if (a == "--output" && i + 1 < argc) app.setOutputPath(argv[++i]); }
}

int runMain(int argc, char** argv)
{
    SampleAppConfig c;
    c.windowDesc.title = "PBRT Renderer - Falcor"; c.windowDesc.resizableWindow = true; c.windowDesc.width = 1280; c.windowDesc.height = 720;
    PBRTOfflineRenderer app(c); parseArgs(argc, argv, app); return app.run();
}
int main(int argc, char** argv) { return catchAndReportAllExceptions([&]() { return runMain(argc, argv); }); }
