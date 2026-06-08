/***************************************************************************
 # PBRT DXR RTGI sample.
 **************************************************************************/
#include "PBRTRTGI.h"
#include "Utils/Math/FalcorMath.h"
#include "Utils/UI/TextRenderer.h"
#include "Utils/Settings/Settings.h"

FALCOR_EXPORT_D3D12_AGILITY_SDK

namespace
{
    const float4 kClearColor(0.02f, 0.025f, 0.03f, 1.0f);
    const std::filesystem::path kDefaultScene = "D:/models/pbrt-v4-scenes/bistro/bistro_cafe.pbrt";

    Settings buildPBRTImportSettings()
    {
        Settings settings;
        settings.addOptions(nlohmann::json::array({
            {
                {"PBRTImporter:usePBRTMaterials", true},
                {"PBRTImporter:useMaterialTextures", true},
            }
        }));
        return settings;
    }
}

PBRTRTGI::PBRTRTGI(const SampleAppConfig& config) : SampleApp(config) {}

void PBRTRTGI::onLoad(RenderContext* pRenderContext)
{
    if (!getDevice()->isFeatureSupported(Device::SupportedFeatures::Raytracing))
        FALCOR_THROW("Device does not support raytracing.");

    if (mScenePath.empty())
        mScenePath = kDefaultScene;
    loadScene(mScenePath, getTargetFbo().get());
}

void PBRTRTGI::onResize(uint32_t width, uint32_t height)
{
    if (mpCamera)
        mpCamera->setAspectRatio(float(width) / float(height));

    mpRtOut = getDevice()->createTexture2D(
        width,
        height,
        ResourceFormat::RGBA16Float,
        1,
        1,
        nullptr,
        ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
    );
}

void PBRTRTGI::loadScene(const std::filesystem::path& path, const Fbo* pTargetFbo)
{
    logInfo("PBRTRTGI loading scene: {}", path.string());

    SceneBuilder::Flags flags = SceneBuilder::Flags::Default;
    if (mUseSceneCache)
        flags |= SceneBuilder::Flags::UseCache;

    mpScene = SceneBuilder(getDevice(), path, buildPBRTImportSettings(), flags).getScene();
    if (!mpScene)
        FALCOR_THROW("Failed to load scene '{}'.", path.string());

    mpCamera = mpScene->getCamera();
    const float radius = std::max(mpScene->getSceneBounds().radius(), 1.0f);
    mpScene->setCameraSpeed(radius * 0.05f);
    mpScene->setCameraController(Scene::CameraControllerType::FirstPerson);
    mpScene->setCameraControlsEnabled(true);
    mpCamera->setDepthRange(std::max(0.1f, radius / 750.0f), radius * 10.0f);
    mpCamera->setAspectRatio(float(pTargetFbo->getWidth()) / float(pTargetFbo->getHeight()));
    mpCamera->setIsAnimated(false);

    mpScene->getRenderSettings().useEmissiveLights = true;
    mpScene->getLightCollection(getRenderContext());

    logInfo(
        "PBRTRTGI scene OK. Geometry: {}, triangleMesh={}, displacedTriangleMesh={}",
        mpScene->getGeometryCount(),
        mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh),
        mpScene->hasGeometryType(Scene::GeometryType::DisplacedTriangleMesh)
    );
    createRaytraceProgram();
    mSampleIndex = 0;
    mFrameCount = 0;
}

void PBRTRTGI::createRaytraceProgram()
{
    auto shaderModules = mpScene->getShaderModules();
    auto typeConformances = mpScene->getTypeConformances();
    auto defines = mpScene->getSceneDefines();

    ProgramDesc rtProgDesc;
    rtProgDesc.addShaderModules(shaderModules);
    rtProgDesc.addShaderLibrary("Samples/PBRTRTGI/PBRTRTGI.rt.slang");
    rtProgDesc.addTypeConformances(typeConformances);
    rtProgDesc.setMaxTraceRecursionDepth(3);
    rtProgDesc.setMaxPayloadSize(32);

    ref<RtBindingTable> sbt = RtBindingTable::create(2, 2, mpScene->getGeometryCount());
    sbt->setRayGen(rtProgDesc.addRayGen("rayGen"));
    sbt->setMiss(0, rtProgDesc.addMiss("primaryMiss"));
    sbt->setMiss(1, rtProgDesc.addMiss("shadowMiss"));
    auto primary = rtProgDesc.addHitGroup("primaryClosestHit", "primaryAnyHit");
    auto shadow = rtProgDesc.addHitGroup("", "shadowAnyHit");
    if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
    {
        sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), primary);
        sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), shadow);
    }
    if (mpScene->hasGeometryType(Scene::GeometryType::DisplacedTriangleMesh))
    {
        sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh), primary);
        sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh), shadow);
    }

    logInfo("PBRTRTGI creating DXR program.");
    mpRaytraceProgram = Program::create(getDevice(), rtProgDesc, defines);
    mpRtVars = RtProgramVars::create(getDevice(), mpRaytraceProgram, sbt);
    logInfo("PBRTRTGI DXR program ready.");
}

void PBRTRTGI::setPerFrameVars(const Fbo* pTargetFbo)
{
    auto var = mpRtVars->getRootVar();
    var["PerFrameCB"]["viewportDims"] = float2(pTargetFbo->getWidth(), pTargetFbo->getHeight());
    var["PerFrameCB"]["sampleIndex"] = mSampleIndex++;
    var["PerFrameCB"]["enableRTGI"] = mEnableRTGI ? 1u : 0u;
    var["PerFrameCB"]["enableDirectLighting"] = mEnableDirectLighting ? 1u : 0u;
    var["PerFrameCB"]["enableShadows"] = mEnableShadows ? 1u : 0u;
    var["PerFrameCB"]["indirectIntensity"] = mIndirectIntensity;
    var["PerFrameCB"]["maxBounces"] = mMaxBounces;
    var["gOutput"] = mpRtOut;
}

void PBRTRTGI::onFrameRender(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo)
{
    pRenderContext->clearFbo(pTargetFbo.get(), kClearColor, 1.0f, 0, FboAttachmentType::All);
    if (!mpScene || !mpRaytraceProgram || !mpRtVars)
        return;

    if (!mpRtOut || mpRtOut->getWidth() != pTargetFbo->getWidth() || mpRtOut->getHeight() != pTargetFbo->getHeight())
    {
        mpRtOut = getDevice()->createTexture2D(
            pTargetFbo->getWidth(),
            pTargetFbo->getHeight(),
            ResourceFormat::RGBA16Float,
            1,
            1,
            nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
    }

    auto updates = mpScene->update(pRenderContext, getGlobalClock().getTime());
    if (is_set(updates, IScene::UpdateFlags::GeometryChanged) || is_set(updates, IScene::UpdateFlags::RecompileNeeded))
        createRaytraceProgram();

    setPerFrameVars(pTargetFbo.get());
    pRenderContext->clearUAV(mpRtOut->getUAV().get(), kClearColor);
    if (mFrameCount == 0)
        logInfo("PBRTRTGI tracing first frame {}x{}.", pTargetFbo->getWidth(), pTargetFbo->getHeight());
    mpScene->raytrace(pRenderContext, mpRaytraceProgram.get(), mpRtVars, uint3(pTargetFbo->getWidth(), pTargetFbo->getHeight(), 1));
    pRenderContext->blit(mpRtOut->getSRV(), pTargetFbo->getRenderTargetView(0));
    if (mFrameCount == 0)
        logInfo("PBRTRTGI first frame raytrace complete.");
    getTextRenderer().render(pRenderContext, getFrameRate().getMsg(), pTargetFbo, {20, 20});

    mFrameCount++;
    if (mFrameCount == 1 && !mOutputPath.empty())
        saveOutput(pRenderContext);
    if (mSingleFrame && mFrameCount == 1)
        shutdown();
}

void PBRTRTGI::onGuiRender(Gui* pGui)
{
    Gui::Window w(pGui, "PBRT RTGI", {340, 430}, {10, 80});
    if (mpScene)
    {
        w.text(fmt::format("Scene: {}", mScenePath.filename().string()));
        w.text(fmt::format("Geometry: {}", mpScene->getGeometryCount()));
    }

    if (auto gi = w.group("Realtime GI", true))
    {
        if (gi.checkbox("Enable RTGI", mEnableRTGI))
            mSampleIndex = 0;
        if (gi.slider("Indirect intensity", mIndirectIntensity, 0.0f, 4.0f))
            mSampleIndex = 0;
        if (gi.slider("Max bounces", mMaxBounces, 0u, 2u))
            mSampleIndex = 0;
    }

    if (auto lighting = w.group("Lighting", true))
    {
        if (lighting.checkbox("Direct lighting", mEnableDirectLighting))
            mSampleIndex = 0;
    }

    if (auto shadows = w.group("Shadows", true))
    {
        if (shadows.checkbox("Ray traced shadows", mEnableShadows))
            mSampleIndex = 0;
    }

    if (w.button("Reset samples"))
        mSampleIndex = 0;
}

bool PBRTRTGI::onKeyEvent(const KeyboardEvent& keyEvent)
{
    if (mpScene && mpScene->onKeyEvent(keyEvent))
        return true;
    return false;
}

bool PBRTRTGI::onMouseEvent(const MouseEvent& mouseEvent)
{
    return mpScene && mpScene->onMouseEvent(mouseEvent);
}

void PBRTRTGI::saveOutput(RenderContext* pRenderContext)
{
    if (mOutputPath.empty())
        return;
    logInfo("Saving: {}", mOutputPath.string());
    pRenderContext->submit(true);
    getTargetFbo()->getColorTexture(0)->captureToFile(0, 0, mOutputPath, Bitmap::FileFormat::PngFile, Bitmap::ExportFlags::None, false);
    logInfo("Saved OK.");
}

int runMain(int argc, char** argv)
{
    SampleAppConfig config;
    config.windowDesc.title = "PBRT RTGI - Bistro";
    config.windowDesc.resizableWindow = true;
    config.windowDesc.width = 1280;
    config.windowDesc.height = 720;

    std::filesystem::path scenePath = kDefaultScene;
    std::filesystem::path outputPath;
    bool headless = false;
    bool singleFrame = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--scene" || arg == "--path" || arg == "path") && i + 1 < argc)
            scenePath = argv[++i];
        else if (arg == "--output" && i + 1 < argc)
            outputPath = argv[++i];
        else if (arg == "--headless")
            headless = true;
        else if (arg == "--single-frame")
            singleFrame = true;
    }
    config.headless = headless;
    config.showUI = !headless;

    PBRTRTGI app(config);
    app.setScenePath(scenePath);
    app.setOutputPath(outputPath);
    app.setSingleFrame(singleFrame || headless);
    return app.run();
}

int main(int argc, char** argv)
{
    return catchAndReportAllExceptions([&]() { return runMain(argc, argv); });
}
