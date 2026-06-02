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
        d.addCompilerArguments({"-Wno-30081"});
        d.addShaderModules(mpScene->getShaderModules());
        d.addShaderLibrary("Samples/PBRTOfflineRenderer/PBRTOfflineRenderer.3d.slang").vsEntry("vsMain").psEntry("psMain");
        d.addTypeConformances(mpScene->getTypeConformances());
        mpRasterPass = RasterPass::create(getDevice(), d, mpScene->getSceneDefines());

        // Create shadow map resources
        mShadowMapSize = mFilamentSettings.shadowMapSize;
        mpShadowMapDepth = getDevice()->createTexture2D(
            mShadowMapSize, mShadowMapSize, ResourceFormat::D32Float, 1, 1, nullptr,
            ResourceBindFlags::DepthStencil | ResourceBindFlags::ShaderResource);
        mpShadowFbo = Fbo::create(getDevice(), {}, mpShadowMapDepth);

        // Create shadow depth raster pass (depth-only from light POV)
        ProgramDesc sd;
        sd.addCompilerArguments({"-Wno-30081"});
        sd.addShaderModules(mpScene->getShaderModules());
        sd.addShaderLibrary("Samples/PBRTOfflineRenderer/ShadowDepth.3d.slang").vsEntry("vsMain").psEntry("psMain");
        sd.addTypeConformances(mpScene->getTypeConformances());
        mpShadowRasterPass = RasterPass::create(getDevice(), sd, mpScene->getSceneDefines());

        Properties props;
        mpFilamentPostProcess = FilamentPostProcess::create(getDevice(), props);
    }
    catch (const std::exception& e)
    {
        logError("Exception: {}", e.what());
    }
}

void PBRTOfflineRenderer::renderShadowMap(RenderContext* pCtx)
{
    if (!mpShadowFbo || !mpShadowRasterPass || !mpScene) return;

    pCtx->clearFbo(mpShadowFbo.get(), float4(0,0,0,0), 1.f, 0, FboAttachmentType::Depth);

    float3 lightDir = normalize(mFilamentSettings.sunDirection);
    float sceneRadius = mpScene->getSceneBounds().radius();
    float3 sceneCenter = mpScene->getSceneBounds().center();
    float3 lightPos = sceneCenter - lightDir * sceneRadius * 2.f;
    float3 lightTarget = sceneCenter;

    float3 f = normalize(lightTarget - lightPos);
    float3 s = normalize(cross(f, float3(0,1,0)));
    float3 u = cross(s, f);
    float4x4 lightView;
    lightView[0] = float4(s.x, u.x, -f.x, 0);
    lightView[1] = float4(s.y, u.y, -f.y, 0);
    lightView[2] = float4(s.z, u.z, -f.z, 0);
    lightView[3] = float4(-dot(s,lightPos), -dot(u,lightPos), dot(f,lightPos), 1);

    float orthoSize = sceneRadius * 1.5f;
    float nearP = 0.1f, farP = sceneRadius * 5.f;
    float rcpRange = 1.f / (farP - nearP);
    float4x4 lightProj;
    lightProj[0] = float4(1.f/orthoSize, 0, 0, 0);
    lightProj[1] = float4(0, 1.f/orthoSize, 0, 0);
    lightProj[2] = float4(0, 0, rcpRange, 0);
    lightProj[3] = float4(0, 0, -nearP*rcpRange, 1);

    float4x4 lightViewProj = mul(lightProj, lightView);
    mFilamentSettings.shadowLightViewProj = lightViewProj;

    auto pCam = mpScene->getCamera();
    auto origView = pCam->getViewMatrix();
    auto origProj = pCam->getProjMatrix();
    pCam->setViewMatrix(lightView);
    pCam->setProjectionMatrix(lightProj);

    mpShadowRasterPass->getState()->setFbo(mpShadowFbo);
    mpScene->rasterize(pCtx, mpShadowRasterPass->getState().get(), mpShadowRasterPass->getVars().get());

    pCam->setViewMatrix(origView);
    pCam->setProjectionMatrix(origProj);
}

void PBRTOfflineRenderer::onFrameRender(RenderContext* pCtx, const ref<Fbo>& pFbo)
{
    pCtx->clearFbo(pFbo.get(), kClear, 1.f, 0, FboAttachmentType::All);
    if (!mSceneLoaded || !mpScene || !mpRasterPass) return;

    // --- Stage 0: Render shadow map ---
    if (mFilamentSettings.enableShadows && mpShadowRasterPass)
        renderShadowMap(pCtx);

    // Ensure intermediate texture is correct size
    auto pTarget = pFbo->getColorTexture(0);
    if (!mpIntermediateTexture || mpIntermediateTexture->getWidth() != pTarget->getWidth() || mpIntermediateTexture->getHeight() != pTarget->getHeight())
    {
        mpIntermediateTexture = getDevice()->createTexture2D(
            pTarget->getWidth(), pTarget->getHeight(), ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpIntermediateDepth = getDevice()->createTexture2D(
            pTarget->getWidth(), pTarget->getHeight(), ResourceFormat::D32Float, 1, 1, nullptr,
            ResourceBindFlags::DepthStencil | ResourceBindFlags::ShaderResource);
        mpPostProcessOutput = getDevice()->createTexture2D(
            pTarget->getWidth(), pTarget->getHeight(), ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }

    auto pInterFbo = Fbo::create(getDevice(), {mpIntermediateTexture}, mpIntermediateDepth);
    // D3D depth [0,1]: clear to 1 (far/sky) so SSAO skips background correctly
    pCtx->clearFbo(pInterFbo.get(), kClear, 1.f, 1.f, FboAttachmentType::All);

    mpScene->update(pCtx, getGlobalClock().getTime());

    mpRasterPass->getState()->setFbo(pInterFbo);
    mpScene->rasterize(pCtx, mpRasterPass->getState().get(), mpRasterPass->getVars().get());

    // Camera matrices for shadow / SSAO
    if (mpScene)
    {
        auto pCam = mpScene->getCamera();
        mFilamentSettings.invViewProj = pCam->getInvViewProjMatrix();
        mFilamentSettings.nearPlane = pCam->getNearPlane();
        mFilamentSettings.farPlane = pCam->getFarPlane();
        auto proj = pCam->getProjMatrix();
        mFilamentSettings.positionParams = float2(2.0f / proj[0][0], 2.0f / proj[1][1]);
        mFilamentSettings.invProj = inverse(proj);
    }

    // Sync FilamentSettings to FilamentPostProcess pass
    if (mpFilamentPostProcess && mFilamentSettings.postProcessingEnabled)
    {
        mpFilamentPostProcess->executeCustom(pCtx, mpIntermediateTexture, mpIntermediateDepth, mpPostProcessOutput, mFilamentSettings,
            mFilamentSettings.enableShadows ? mpShadowMapDepth : nullptr);
        pCtx->blit(mpPostProcessOutput->getSRV(), pTarget->getRTV());
    }
    else
    {
        pCtx->blit(mpIntermediateTexture->getSRV(), pTarget->getRTV());
    }

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
    
    if (auto g = w.group("Filament Settings"))
    {
        if (auto viewGroup = g.group("View")) {
            viewGroup.checkbox("Post-processing", mFilamentSettings.postProcessingEnabled);
            viewGroup.checkbox("Dithering", mFilamentSettings.dithering);
            Gui::DropdownList aaList = {{0, "None"}, {1, "FXAA"}, {2, "TAA"}};
            viewGroup.dropdown("Anti-aliasing", aaList, (uint32_t&)mFilamentSettings.antiAliasing);
            if (mFilamentSettings.antiAliasing == 2)
                viewGroup.slider("TAA Feedback", mFilamentSettings.taaFeedback, 0.0f, 0.99f);
        }

        if (auto lightGroup = g.group("Light (Sun)")) {
            lightGroup.slider("Intensity", mFilamentSettings.sunIntensity, 0.0f, 200000.0f);
            lightGroup.rgbColor("Color", mFilamentSettings.sunColor);
            lightGroup.slider("Dir X", mFilamentSettings.sunDirection.x, -1.0f, 1.0f);
            lightGroup.slider("Dir Y", mFilamentSettings.sunDirection.y, -1.0f, 1.0f);
            lightGroup.slider("Dir Z", mFilamentSettings.sunDirection.z, -1.0f, 1.0f);
            if (lightGroup.button("Normalize Direction"))
                mFilamentSettings.sunDirection = normalize(mFilamentSettings.sunDirection);
        }

        if (mpScene && mpScene->getEnvMap())
        {
            if (auto envGroup = g.group("Environment Map (IBL)"))
            {
                mpScene->getEnvMap()->renderUI(envGroup);
            }
        }

        if (auto shadowGroup = g.group("Shadows")) {
            shadowGroup.checkbox("Enable Shadows", mFilamentSettings.enableShadows);
            if (mFilamentSettings.enableShadows) {
                Gui::DropdownList shadowTypes = {{0, "PCF Hard"}, {1, "PCF Low (3x3)"}, {2, "VSM"}};
                shadowGroup.dropdown("Shadow Type", shadowTypes, (uint32_t&)mFilamentSettings.shadowType);
                shadowGroup.slider("Cascades", mFilamentSettings.shadowCascades, 1, 4);
                shadowGroup.slider("Bias", mFilamentSettings.shadowBias, 0.0f, 0.01f);
                shadowGroup.slider("Map Size", mFilamentSettings.shadowMapSize, 512u, 4096u);
            }
        }

        if (mFilamentSettings.postProcessingEnabled) {
            if (auto ppGroup = g.group("Post-processing")) {
                
                if (auto ssaoGroup = ppGroup.group("SSAO (Ambient Occlusion)")) {
                    ssaoGroup.checkbox("Enable", mFilamentSettings.enableSSAO);
                    if (mFilamentSettings.enableSSAO) {
                        ssaoGroup.slider("Radius", mFilamentSettings.ssaoRadius, 0.01f, 3.0f);
                        ssaoGroup.slider("Bias", mFilamentSettings.ssaoBias, 0.0f, 0.1f);
                        ssaoGroup.slider("Power", mFilamentSettings.ssaoPower, 0.1f, 5.0f);
                        ssaoGroup.slider("Intensity", mFilamentSettings.ssaoIntensity, 0.0f, 3.0f);
                        ssaoGroup.slider("Samples", mFilamentSettings.ssaoSampleCount, 4, 64);
                        ssaoGroup.slider("Spiral Turns", mFilamentSettings.ssaoSpiralTurns, 1, 15);
                    }
                }
                
                if (auto bloomGroup = ppGroup.group("Bloom")) {
                    bloomGroup.checkbox("Enable", mFilamentSettings.enableBloom);
                    if (mFilamentSettings.enableBloom) {
                        bloomGroup.slider("Strength", mFilamentSettings.bloomStrength, 0.0f, 1.0f);
                        bloomGroup.slider("Threshold", mFilamentSettings.bloomThreshold, 0.0f, 10.0f);
                        bloomGroup.slider("Levels", mFilamentSettings.bloomLevels, 1, 7);
                        Gui::DropdownList blendModes = {{0, "Add"}, {1, "Screen"}};
                        bloomGroup.dropdown("Blend Mode", blendModes, (uint32_t&)mFilamentSettings.bloomBlendMode);
                    }
                }
                
                if (auto dofGroup = ppGroup.group("Depth of Field")) {
                    dofGroup.checkbox("Enable", mFilamentSettings.enableDoF);
                    if (mFilamentSettings.enableDoF) {
                        dofGroup.slider("Focal Distance", mFilamentSettings.dofFocalDistance, 0.1f, 100.0f);
                        dofGroup.slider("Aperture", mFilamentSettings.dofAperture, 1.0f, 32.0f);
                        dofGroup.slider("Max CoC", mFilamentSettings.dofMaxCoC, 1.0f, 32.0f);
                    }
                }
                
                if (auto vigGroup = ppGroup.group("Vignette")) {
                    vigGroup.checkbox("Enable", mFilamentSettings.enableVignette);
                    if (mFilamentSettings.enableVignette) {
                        vigGroup.slider("Midpoint", mFilamentSettings.vignetteMidpoint, 0.0f, 1.0f);
                        vigGroup.slider("Roundness", mFilamentSettings.vignetteRoundness, 0.0f, 1.0f);
                        vigGroup.slider("Feather", mFilamentSettings.vignetteFeather, 0.0f, 1.0f);
                        vigGroup.rgbColor("Color", mFilamentSettings.vignetteColor);
                    }
                }

                if (auto cgGroup = ppGroup.group("Color Grading")) {
                    Gui::DropdownList tmModes = {{0, "ACES"}, {1, "Filmic"}, {2, "Linear"}, {3, "Display"}};
                    cgGroup.dropdown("Tone Mapping", tmModes, (uint32_t&)mFilamentSettings.toneMapping);
                    cgGroup.slider("Exposure (EV)", mFilamentSettings.exposure, -10.0f, 10.0f);
                    cgGroup.slider("Contrast", mFilamentSettings.contrast, 0.0f, 2.0f);
                    cgGroup.slider("Vibrance", mFilamentSettings.vibrance, 0.0f, 2.0f);
                    cgGroup.slider("Saturation", mFilamentSettings.saturation, 0.0f, 2.0f);
                }
            }
        }
    }

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
