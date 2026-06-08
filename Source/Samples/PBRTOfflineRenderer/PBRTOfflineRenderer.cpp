/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#include "PBRTOfflineRenderer.h"
#include "Core/Platform/OS.h"
#include "Scene/Importer.h"
#include "Scene/SceneBuilder.h"
#include "Utils/Math/FalcorMath.h"
#include "Utils/Settings/Settings.h"
#include "Utils/SampleGenerators/HaltonSamplePattern.h"
#include "Utils/Timing/TimeReport.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

#include <imgui.h>

FALCOR_EXPORT_D3D12_AGILITY_SDK
static const float4 kClear = float4(0.2f, 0.2f, 0.24f, 1.f);

namespace
{
    constexpr float kDefaultIblIntensityScale = 0.35f;
    constexpr float kPreviewIblIntensityScale = 0.35f;
    constexpr float kPreviewAmbientIntensity = 0.0f;
    constexpr float kDefaultSSAORadius = 0.55f;
    constexpr float kDefaultSSAOIntensity = 0.55f;
    constexpr float kDefaultSSAOPower = 0.8f;
    constexpr float kDefaultSSAOResolution = 1.0f;
    constexpr float kDefaultSSAOBilateralThreshold = 0.05f;
    constexpr int kDefaultSSAOMode = 0;
    constexpr int kDefaultSSAOQuality = 1;
    constexpr int kDefaultSSAOSampleCount = 11;
    constexpr int kDefaultSSAOLowPass = 1;

    struct CascadeAtlasLayout { uint32_t cols; uint32_t rows; };

    CascadeAtlasLayout getAtlasLayout(uint32_t cascadeCount)
    {
        switch (cascadeCount)
        {
        case 1: return {1, 1};
        case 2: return {2, 1};
        case 3: return {2, 2};
        default: return {2, 2};
        }
    }

    float4 getAtlasTileRect(uint32_t cascadeIndex, const CascadeAtlasLayout& layout)
    {
        const float tileW = 1.f / float(layout.cols);
        const float tileH = 1.f / float(layout.rows);
        const uint32_t col = cascadeIndex % layout.cols;
        const uint32_t row = cascadeIndex / layout.cols;
        return float4(col * tileW, row * tileH, tileW, tileH);
    }

    float4x4 buildLightViewMatrix(float3 lightPos, float3 lightDir, float3 cameraFwd)
    {
        float3 f = normalize(lightDir);
        float3 s = normalize(cross(f, cameraFwd));
        if (length(s) < 1e-4f)
            s = normalize(cross(f, float3(0, 1, 0)));
        float3 u = cross(s, f);

        float4x4 view = float4x4::identity();
        view[0] = float4(s.x, u.x, -f.x, 0.f);
        view[1] = float4(s.y, u.y, -f.y, 0.f);
        view[2] = float4(s.z, u.z, -f.z, 0.f);
        view[3] = float4(-dot(s, lightPos), -dot(u, lightPos), dot(f, lightPos), 1.f);
        return view;
    }

    float4x4 buildOrthoProjMatrix(float left, float right, float bottom, float top, float nearP, float farP)
    {
        const float rcpRange = 1.f / (farP - nearP);
        float4x4 proj = float4x4::identity();
        proj[0][0] = 2.f / (right - left);
        proj[1][1] = 2.f / (top - bottom);
        proj[2][2] = rcpRange;
        proj[3][0] = -(right + left) / (right - left);
        proj[3][1] = -(top + bottom) / (top - bottom);
        proj[3][2] = -nearP * rcpRange;
        return proj;
    }

    float3 transformPoint(const float4x4& m, float3 p)
    {
        return mul(float4(p, 1.f), m).xyz();
    }

    void getFrustumSliceCorners(const ref<Camera>& pCam, float sliceNear, float sliceFar, float3 corners[8])
    {
        float3 camPos = pCam->getPosition();
        float3 fwd = normalize(pCam->getTarget() - camPos);
        float3 up = float3(0, 1, 0);
        float3 right = normalize(cross(fwd, up));
        up = cross(right, fwd);

        const float fovY = focalLengthToFovY(pCam->getFocalLength(), Camera::kDefaultFrameHeight);
        const float tanHalfY = std::tan(fovY * 0.5f);
        const float aspect = pCam->getAspectRatio();
        const float tanHalfX = tanHalfY * aspect;

        auto cornerAt = [&](float dist, int ix, int iy) -> float3
        {
            const float x = (ix == 0 ? -1.f : 1.f) * dist * tanHalfX;
            const float y = (iy == 0 ? -1.f : 1.f) * dist * tanHalfY;
            return camPos + fwd * dist + right * x + up * y;
        };

        int idx = 0;
        for (int z = 0; z < 2; ++z)
        {
            const float dist = (z == 0) ? sliceNear : sliceFar;
            for (int iy = 0; iy < 2; ++iy)
                for (int ix = 0; ix < 2; ++ix)
                    corners[idx++] = cornerAt(dist, ix, iy);
        }
    }

    void appendAabbCorners(const AABB& box, float3* corners, uint32_t& count)
    {
        const float3 c = box.center();
        const float3 e = box.extent() * 0.5f;
        const float3 offsets[8] = {
            {-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
            {-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1},
        };
        for (const auto& o : offsets)
            corners[count++] = c + o * e;
    }

    bool isSupportedScenePath(const std::filesystem::path& path)
    {
        const std::string ext = getExtensionFromPath(path);
        if (ext.empty())
            return false;
        for (const auto& supported : Importer::getSupportedExtensions())
        {
            if (ext == supported)
                return true;
        }
        return false;
    }

    Settings buildSceneImportSettings(const std::filesystem::path& path, bool usePBRTMaterials)
    {
        Settings settings;
        if (getExtensionFromPath(path) == "pbrt")
        {
            settings.addOptions(nlohmann::json{
                {"PBRTImporter:rotateImageTextures90", false},
                {"PBRTImporter:rotateImageTextures180", true},
                {"PBRTImporter:flipTextureV", true},
                {"PBRTImporter:usePBRTMaterials", usePBRTMaterials},
                {"PBRTImporter:useMaterialTextures", usePBRTMaterials},
            });
        }
        return settings;
    }

    SceneBuilder::Flags buildSceneFlags(bool useCache, bool rebuildCache)
    {
        SceneBuilder::Flags flags = SceneBuilder::Flags::Default;
        if (useCache)
            flags |= SceneBuilder::Flags::UseCache;
        if (rebuildCache)
            flags |= SceneBuilder::Flags::RebuildCache;
        return flags;
    }
}

PBRTOfflineRenderer::PBRTOfflineRenderer(const SampleAppConfig& c) : SampleApp(c), mExecutor(std::thread::hardware_concurrency())
{
    mFilamentSettings.postProcessingEnabled = true;
    mFilamentSettings.antiAliasing = 0;
    mFilamentSettings.enableSSAO = true;
    mFilamentSettings.forwardSSAO = false;
    mFilamentSettings.ssaoRadius = kDefaultSSAORadius;
    mFilamentSettings.ssaoIntensity = kDefaultSSAOIntensity;
    mFilamentSettings.ssaoPower = kDefaultSSAOPower;
    mFilamentSettings.ssaoResolution = kDefaultSSAOResolution;
    mFilamentSettings.ssaoBilateralThreshold = kDefaultSSAOBilateralThreshold;
    mFilamentSettings.ssaoMode = kDefaultSSAOMode;
    mFilamentSettings.gtaoRadius = kDefaultSSAORadius;
    mFilamentSettings.ssaoQuality = kDefaultSSAOQuality;
    mFilamentSettings.ssaoSampleCount = kDefaultSSAOSampleCount;
    mFilamentSettings.ssaoLowPassFilter = kDefaultSSAOLowPass;
    mFilamentSettings.iblIntensity = kDefaultIblIntensityScale;
    mFilamentSettings.enableSunlight = false;
    mFilamentSettings.sunIntensity = 0.f;
    mFilamentSettings.sunColor = float3(1.f, 0.95f, 0.85f);
    mFilamentSettings.sunDirection = normalize(float3(0.3f, -1.f, 0.5f));
    mFilamentSettings.enableShadows = false;
    mFilamentSettings.exposure = 0.0f;
    mFilamentSettings.toneMapping = 2; // ACES
    mFilamentSettings.toneMappingFilament = 2; // ACES
    buildTaskGraph();
}
PBRTOfflineRenderer::~PBRTOfflineRenderer() { mExecutor.wait_for_all(); }

void PBRTOfflineRenderer::setHeadlessProbeMode(bool enabled)
{
    if (!enabled)
        return;

    mFilamentSettings.postProcessingEnabled = true;
    mFilamentSettings.enableShadows = false;
    mFilamentSettings.enableSSAO = true;
    mFilamentSettings.forwardSSAO = false;
    mFilamentSettings.sunIntensity = 0.f;
    mFilamentSettings.enableSunlight = false;
}

void PBRTOfflineRenderer::setPreviewMode(bool enabled)
{
    if (!enabled)
        return;

    // Mirror the interactive PBRT viewer preview preset defaults.
    mFilamentSettings.postProcessingEnabled = true;
    mFilamentSettings.enableSSAO = true;
    mFilamentSettings.forwardSSAO = false;
    mFilamentSettings.ssaoRadius = kDefaultSSAORadius;
    mFilamentSettings.ssaoIntensity = kDefaultSSAOIntensity;
    mFilamentSettings.ssaoPower = kDefaultSSAOPower;
    mFilamentSettings.ssaoResolution = kDefaultSSAOResolution;
    mFilamentSettings.ssaoBilateralThreshold = kDefaultSSAOBilateralThreshold;
    mFilamentSettings.ssaoMode = kDefaultSSAOMode;
    mFilamentSettings.gtaoRadius = kDefaultSSAORadius;
    mFilamentSettings.ssaoQuality = kDefaultSSAOQuality;
    mFilamentSettings.ssaoSampleCount = kDefaultSSAOSampleCount;
    mFilamentSettings.ssaoLowPassFilter = kDefaultSSAOLowPass;
    mFilamentSettings.iblIntensity = kPreviewIblIntensityScale;
    mFilamentSettings.enableSunlight = false;
    mFilamentSettings.sunIntensity = 0.f;
    mFilamentSettings.sunDirection = normalize(float3(0.3f, -1.f, 0.5f));
    mFilamentSettings.enableShadows = false;
    mFilamentSettings.exposure = 0.0f;
    mFilamentSettings.toneMapping = 2; // ACES
    mFilamentSettings.toneMappingFilament = 2; // ACES
    mFilamentSettings.ambientIntensity = kPreviewAmbientIntensity;
}

void PBRTOfflineRenderer::setRealtimeGIEnabled(bool enabled)
{
    applyRealtimeGIPreset(enabled);
}

void PBRTOfflineRenderer::applyRealtimeGIPreset(bool enabled)
{
    mRealtimeGIEnabled = enabled;
    if (!enabled)
        return;

    mFilamentSettings.postProcessingEnabled = true;
    mFilamentSettings.iblIntensity = std::max(mFilamentSettings.iblIntensity, mRealtimeGIBounceIntensity);
    mFilamentSettings.enableSSAO = false;
    mDeferredAOSettings.enabled = false;

    if (mRealtimeGIUseShadows)
        setEnableShadows(true);
}

void PBRTOfflineRenderer::onLoad(RenderContext* pCtx)
{
    if (!mScenePath.empty()) loadScene(pCtx);
    else logInfo("No scene. Drag a scene file (.pbrt/.obj/.gltf/...), use --scene <file>, or choose one in the UI.");
}

void PBRTOfflineRenderer::onShutdown()
{
    mExecutor.wait_for_all();
    mpGBufferPass = nullptr;
    mpLightingPass = nullptr;
    mpScene = nullptr;
}

void PBRTOfflineRenderer::loadScene(RenderContext* pCtx)
{
    logInfo("Loading: {}", mScenePath.string());
    mIsLoadingScene = true;
    mSceneLoaded = false;
    mpGBufferPass = nullptr;
    mpLightingPass = nullptr;
    mpFilamentPostProcess = nullptr;
    mpFilamentIBL = nullptr;
    mpShadowRasterPass = nullptr;
    mpDeferredAOPass = nullptr;
    mpAutoExposurePass = nullptr;
    try
    {
        setLoadingStatus("Validating scene path");
        if (!isSupportedScenePath(mScenePath))
        {
            logError("Unsupported scene extension: {}", mScenePath.extension().string());
            logError("Supported extensions include: pbrt, pyscene, obj, gltf, glb, fbx, ... (requires importer plugins in bin/Release/plugins/)");
            mIsLoadingScene = false;
            setLoadingStatus("Scene load failed");
            return;
        }

        setLoadingStatus("Importing scene");
        logInfo(
            "PBRT viewer import options: sceneCache={}, rebuildSceneCache={}, pbrtMaterials={}",
            mUseSceneCache,
            mRebuildSceneCache,
            mUsePBRTMaterials
        );
        mpScene = SceneBuilder(
            getDevice(),
            mScenePath,
            buildSceneImportSettings(mScenePath, mUsePBRTMaterials),
            buildSceneFlags(mUseSceneCache, mRebuildSceneCache)
        ).getScene();
        if (!mpScene)
        {
            logError("Failed to load scene.");
            mIsLoadingScene = false;
            setLoadingStatus("Scene load failed");
            return;
        }

        setLoadingStatus("Configuring camera");
        auto pCam = mpScene->getCamera();
        float r = mpScene->getSceneBounds().radius();
        if (!std::isfinite(r) || r <= 0.f)
            r = 1000.f;
        mpScene->setCameraSpeed(r * 0.05f);
        mpScene->setCameraController(Scene::CameraControllerType::FirstPerson);
        mpScene->setCameraControlsEnabled(true);
        pCam->setIsAnimated(false);
        pCam->setDepthRange(std::max(0.1f, r / 750.f), r * 10.f);
        mSceneLoaded = true; mLoadedScenePath = mScenePath; mFrameCount = 0; mStartTime = getGlobalClock().getTime();

        // Enable emissive lights and build light collection (needed for PBRT area lights)
        setLoadingStatus("Building light collection");
        mpScene->getRenderSettings().useEmissiveLights = true;
        mpScene->getLightCollection(pCtx);

        setLoadingStatus("Initializing scene lights");
        initSunFromScene();

        logInfo("Scene OK. Geometry: {}, Lights: a={}, e={}, env={}",
            mpScene->getGeometryCount(), mpScene->useAnalyticLights(),
            mpScene->useEmissiveLights(), mpScene->useEnvLight());

        if (!mInspectInstanceIDs.empty())
        {
            const auto pInspectCam = mpScene->getCamera();
            if (pInspectCam)
            {
                logInfo(
                    "Inspect camera: pos=({}, {}, {}) target=({}, {}, {}) up=({}, {}, {}) aspect={} focalLength={} near={} far={}",
                    pInspectCam->getPosition().x, pInspectCam->getPosition().y, pInspectCam->getPosition().z,
                    pInspectCam->getTarget().x, pInspectCam->getTarget().y, pInspectCam->getTarget().z,
                    pInspectCam->getUpVector().x, pInspectCam->getUpVector().y, pInspectCam->getUpVector().z,
                    pInspectCam->getAspectRatio(), pInspectCam->getFocalLength(), pInspectCam->getNearPlane(), pInspectCam->getFarPlane()
                );
            }
        }

        for (uint32_t instanceID : mInspectInstanceIDs)
        {
            if (instanceID >= mpScene->getGeometryInstanceCount())
            {
                logWarning("Inspect instance {} is out of range ({} instances).", instanceID, mpScene->getGeometryInstanceCount());
                continue;
            }

            const auto& instance = mpScene->getGeometryInstance(instanceID);
            const auto materialID = MaterialID::fromSlang(instance.materialID);
            const auto& pMaterial = mpScene->getMaterial(materialID);
            std::string meshName = "<non-mesh>";
            if (instance.getType() == Scene::GeometryType::TriangleMesh || instance.getType() == Scene::GeometryType::DisplacedTriangleMesh)
                meshName = mpScene->getMeshName(instance.geometryID);
            logInfo(
                "Inspect instance {}: geometryID={} materialID={} material='{}' mesh='{}'",
                instanceID,
                instance.geometryID,
                instance.materialID,
                pMaterial ? pMaterial->getName() : "<null>",
                meshName
            );
            const auto* pAnim = mpScene->getAnimationController();
            if (pAnim && instance.globalMatrixID < pAnim->getGlobalMatrices().size())
            {
                const auto& m = pAnim->getGlobalMatrices()[instance.globalMatrixID];
                logInfo(
                    "Inspect instance {} matrix rows: [{}, {}, {}, {}] [{}, {}, {}, {}] [{}, {}, {}, {}] [{}, {}, {}, {}]",
                    instanceID,
                    m[0][0], m[0][1], m[0][2], m[0][3],
                    m[1][0], m[1][1], m[1][2], m[1][3],
                    m[2][0], m[2][1], m[2][2], m[2][3],
                    m[3][0], m[3][1], m[3][2], m[3][3]
                );
            }
        }

        if (mWarmupCache)
        {
            setLoadingStatus("Warming render pass cache");
            ensureRenderPasses(pCtx);
        }
        else
        {
            logInfo("Deferring PBRT viewer render pass creation until first frame or explicit warmup.");
        }

        logInfo("Using IBL-only preview lighting by default.");
        logInfo("Scene load complete.");
        mIsLoadingScene = false;
        setLoadingStatus("Scene load complete");
    }
    catch (const std::exception& e)
    {
        logError("Exception: {}", e.what());
        mIsLoadingScene = false;
        setLoadingStatus("Scene load failed");
    }
}

void PBRTOfflineRenderer::ensureRenderPasses(RenderContext* pCtx)
{
    if (!mpScene)
        return;
    if (mpGBufferPass && mpLightingPass && mpFilamentPostProcess && mpFilamentIBL)
        return;

    TimeReport viewerTimeReport;

    if (!mpGBufferPass)
    {
        setLoadingStatus("Creating GBuffer pass");
        logInfo("Creating PBRT GBuffer raster pass.");
        ProgramDesc d;
        d.addCompilerArguments({"-Wno-30081"});
        if (mUsePBRTMaterials)
        {
            d.addShaderModules(mpScene->getShaderModules());
            d.addShaderLibrary("Samples/PBRTOfflineRenderer/PBRTGBuffer.3d.slang").vsEntry("vsMain").psEntry("psMain");
            d.addTypeConformances(mpScene->getTypeConformances());
        }
        else
        {
            d.addShaderLibrary("Samples/PBRTOfflineRenderer/PBRTGBufferFast.3d.slang").vsEntry("vsMain").psEntry("psMain");
        }
        mpGBufferPass = RasterPass::create(getDevice(), d, mpScene->getSceneDefines());
    }
    viewerTimeReport.measure("PBRT viewer create GBuffer pass");

    if (!mpLightingPass)
    {
        setLoadingStatus("Creating lighting pass");
        logInfo("Creating PBRT IBL lighting pass.");
        mpLightingPass = FullScreenPass::create(getDevice(), "Samples/PBRTOfflineRenderer/PBRTIBLLighting.ps.slang");
    }
    viewerTimeReport.measure("PBRT viewer create lighting pass");

    logInfo("Deferring shadow resources until shadows are enabled.");
    viewerTimeReport.measure("PBRT viewer defer shadow setup");

    if (!mpFilamentPostProcess)
    {
        setLoadingStatus("Creating Filament post-process");
        logInfo("Creating Filament post-process.");
        Properties props;
        mpFilamentPostProcess = FilamentPostProcess::create(getDevice(), props);
    }
    viewerTimeReport.measure("PBRT viewer create Filament post-process");

    if (!mpFilamentIBL)
    {
        setLoadingStatus("Loading Filament IBL");
        logInfo("Loading Filament IBL.");
        mpFilamentIBL = FilamentIBL::create(getDevice());
        mpFilamentIBL->loadDefault();
    }
    viewerTimeReport.measure("PBRT viewer load Filament IBL");
    viewerTimeReport.addTotal("PBRT viewer setup total");
    viewerTimeReport.printToLog();
}

void PBRTOfflineRenderer::setLoadingStatus(const std::string& status)
{
    mLoadingStatus = status;
    logInfo("PBRT viewer loading: {}", status);
    if (auto pWindow = getWindow())
    {
        const std::string title = mIsLoadingScene
            ? fmt::format("Falcor Scene Viewer - Loading: {}", status)
            : "Falcor Scene Viewer";
        pWindow->setWindowTitle(title);
    }
}

void PBRTOfflineRenderer::requestRenderTask(const std::string& name, RenderTaskQueue::Task task)
{
    mRenderTaskQueue.enqueue(name, std::move(task));
}

void PBRTOfflineRenderer::requestShadowWarmup()
{
    requestRenderTask("Warmup Shadow Resources", [this](RenderContext*)
    {
        ensureShadowPassResources();
        mFilamentSettings.enableSunlight = true;
        if (mFilamentSettings.sunIntensity <= 0.f)
            mFilamentSettings.sunIntensity = 30000.f;
        mFilamentSettings.enableShadows = true;
    });
}

void PBRTOfflineRenderer::requestDeferredAOWarmup()
{
    requestRenderTask("Warmup Deferred AO Pass", [this](RenderContext*)
    {
        if (mpFilamentPostProcess)
        {
            mpFilamentPostProcess->ensureStructurePasses();
            mpFilamentPostProcess->ensureSSAOPasses(true, false);
        }
        mDeferredAOSettings.enabled = true;
    });
}

void PBRTOfflineRenderer::requestAutoExposureWarmup()
{
    if (mpAutoExposurePass)
    {
        mAutoExposureSettings.enabled = true;
        return;
    }

    requestRenderTask("Warmup Auto Exposure Pass", [this](RenderContext*)
    {
        if (!mpAutoExposurePass)
            mpAutoExposurePass = AutoExposurePass::create(getDevice(), Properties{});
        mAutoExposureSettings.enabled = true;
    });
}

void PBRTOfflineRenderer::ensureShadowMapResources()
{
    if (mShadowMapSize != mFilamentSettings.shadowMapSize || !mpShadowMapDepth)
    {
        mShadowMapSize = mFilamentSettings.shadowMapSize;
        mpShadowMapDepth = getDevice()->createTexture2D(
            mShadowMapSize, mShadowMapSize, ResourceFormat::D32Float, 1, 1, nullptr,
            ResourceBindFlags::DepthStencil | ResourceBindFlags::ShaderResource);
        mpShadowMapMoments = getDevice()->createTexture2D(
            mShadowMapSize, mShadowMapSize, ResourceFormat::RG32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpShadowMapMomentsBlur = getDevice()->createTexture2D(
            mShadowMapSize, mShadowMapSize, ResourceFormat::RG32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpShadowFbo = Fbo::create(getDevice(), {mpShadowMapMoments}, mpShadowMapDepth);
        mpShadowMomentsSample = nullptr;
    }
}

void PBRTOfflineRenderer::ensureShadowPassResources()
{
    if (!mpScene)
        return;

    ensureShadowMapResources();

    if (!mpShadowRasterPass)
    {
        logInfo("Creating shadow raster pass.");
        ProgramDesc sd;
        sd.addCompilerArguments({"-Wno-30081"});
        sd.addShaderModules(mpScene->getShaderModules());
        sd.addShaderLibrary("Samples/PBRTOfflineRenderer/ShadowDepth.3d.slang").vsEntry("vsMain").psEntry("psMain");
        sd.addTypeConformances(mpScene->getTypeConformances());
        mpShadowRasterPass = RasterPass::create(getDevice(), sd, mpScene->getSceneDefines());
    }

    if (!mpShadowPointSampler)
    {
        Sampler::Desc shadowSamplerDesc;
        shadowSamplerDesc.setFilterMode(TextureFilteringMode::Point, TextureFilteringMode::Point, TextureFilteringMode::Point);
        shadowSamplerDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
        mpShadowPointSampler = getDevice()->createSampler(shadowSamplerDesc);
    }
}

void PBRTOfflineRenderer::initSunFromScene()
{
    if (!mpScene) return;

    for (const auto& pLight : mpScene->getLights())
    {
        if (!pLight || !pLight->isActive()) continue;
        const auto type = pLight->getType();
        if (type != LightType::Directional && type != LightType::Distant) continue;

        const float3 intensity = pLight->getIntensity();
        const float lum = std::max(dot(intensity, float3(0.2126f, 0.7152f, 0.0722f)), 1e-6f);
        mFilamentSettings.sunDirection = pLight->getData().dirW;
        mFilamentSettings.sunIntensity = lum;
        if (lum > 1e-6f)
            mFilamentSettings.sunColor = intensity / lum;
        return;
    }
}

void PBRTOfflineRenderer::syncFilamentSunLight()
{
    if (!mpScene) return;

    const float3 sunDir = normalize(mFilamentSettings.sunDirection);
    const float3 intensity = mFilamentSettings.enableSunlight ? mFilamentSettings.sunColor * mFilamentSettings.sunIntensity : float3(0.f);

    ref<Light> pSun;
    for (const auto& pLight : mpScene->getLights())
    {
        if (!pLight || !pLight->isActive()) continue;
        const auto type = pLight->getType();
        if (type == LightType::Directional || type == LightType::Distant)
        {
            pSun = pLight;
            break;
        }
    }

    if (!pSun) return;

    switch (pSun->getType())
    {
    case LightType::Directional:
        static_cast<DirectionalLight*>(pSun.get())->setWorldDirection(sunDir);
        break;
    case LightType::Distant:
        static_cast<DistantLight*>(pSun.get())->setWorldDirection(sunDir);
        break;
    default:
        break;
    }
    pSun->setIntensity(intensity);
}

void PBRTOfflineRenderer::syncFilamentCameraSettings()
{
    if (!mpScene) return;

    auto pCam = mpScene->getCamera();
    mFilamentSettings.invViewProj = pCam->getInvViewProjMatrix();
    mFilamentSettings.invView = inverse(pCam->getViewMatrix());
    mFilamentSettings.nearPlane = pCam->getNearPlane();
    mFilamentSettings.farPlane = pCam->getFarPlane();
    auto proj = pCam->getProjMatrix();
    mFilamentSettings.positionParams = float2(2.0f / proj[0][0], 2.0f / proj[1][1]);
    mFilamentSettings.invProj = inverse(proj);
    mFilamentSettings.cameraPos = pCam->getPosition();
    mFilamentSettings.cameraJitter = float2(pCam->getJitterX(), pCam->getJitterY());
}

FilamentPostProcess::FilamentSettings PBRTOfflineRenderer::getLightingAOSettings() const
{
    auto settings = mFilamentSettings;
    if (mDeferredAOSettings.enabled)
    {
        settings.postProcessingEnabled = true;
        settings.enableSSAO = true;
        settings.forwardSSAO = false;
        settings.ssaoMode = 0;
        settings.ssaoResolution = 1.0f;
        settings.ssaoRadius = mDeferredAOSettings.radius;
        settings.ssaoIntensity = mDeferredAOSettings.intensity;
        settings.ssaoBias = std::max(mDeferredAOSettings.bias, 0.0001f);
        settings.ssaoPower = mDeferredAOSettings.power;
        settings.ssaoSampleCount = int(std::clamp(mDeferredAOSettings.sampleCount, 4u, 64u));
        settings.ssaoQuality = settings.ssaoSampleCount >= 32 ? 3 : (settings.ssaoSampleCount >= 16 ? 2 : 1);
        settings.ssaoLowPassFilter = mDeferredAOSettings.blurRadius > 0 ? 1 : 0;
        settings.ssaoBilateralThreshold = 1.0f / std::max(mDeferredAOSettings.blurSharpness, 1.0f);
        settings.ssaoHighQualityUpsampling = false;
        settings.ssaoBentNormals = false;
    }
    return settings;
}

void PBRTOfflineRenderer::setAOShaderVars(const ShaderVar& var)
{
    if (!var.isValid() || !mpFilamentPostProcess) return;

    const auto aoSettings = getLightingAOSettings();
    const bool useAO = aoSettings.postProcessingEnabled && aoSettings.enableSSAO;

    auto perFrameCB = var.findMember("PerFrameCB");
    if (perFrameCB.isValid() && perFrameCB.findMember("gSSAOEnabled").isValid())
        perFrameCB["gSSAOEnabled"] = useAO ? 1u : 0u;

    if (var.findMember("AODataCB").isValid())
        mpFilamentPostProcess->bindAOShaderVars(var, aoSettings, mpIntermediateDepth);

    if (var.findMember("gSSAO").isValid())
        var["gSSAO"] = mpFilamentPostProcess->getAOTexture(aoSettings);
    if (var.findMember("gSSAOLinearSampler").isValid())
        var["gSSAOLinearSampler"] = mpFilamentPostProcess->getLinearSampler();
}

void PBRTOfflineRenderer::setShadowShaderVars(const ShaderVar& var)
{
    if (!var.isValid()) return;

    auto cb = var.findMember("ShadowCB");
    if (cb.isValid())
    {
        cb["gShadowEnabled"] = mFilamentSettings.enableShadows ? 1u : 0u;
        cb["gShadowType"] = (uint32_t)mFilamentSettings.shadowType;
        cb["gCascadeCount"] = (uint32_t)std::clamp(mFilamentSettings.shadowCascades, 1, 4);
        cb["gShadowBias"] = mFilamentSettings.shadowBias;
        cb["gCascadeSplits"] = mFilamentSettings.cascadeSplits;
        cb["gShadowAtlasSize"] = float2((float)mShadowMapSize, (float)mShadowMapSize);
        cb["gShadowSunDir"] = normalize(mFilamentSettings.sunDirection);
        cb["gVsmExponent"] = mFilamentSettings.vsmExponent;
        cb["gVsmLightBleedReduction"] = mFilamentSettings.vsmLightBleedReduction;
        if (cb.findMember("gInvViewProj").isValid())
            cb["gInvViewProj"] = mFilamentSettings.invViewProj;
        if (cb.findMember("gCameraPos").isValid())
            cb["gCameraPos"] = mFilamentSettings.cameraPos;
        for (int i = 0; i < 4; ++i)
        {
            cb["gLightViewProj"][i] = mFilamentSettings.shadowLightViewProj[i];
            cb["gCascadeAtlasRect"][i] = mFilamentSettings.cascadeAtlasRect[i];
        }
    }

    if (var.findMember("gShadowMap").isValid() && mpShadowMapDepth)
        var["gShadowMap"] = mpShadowMapDepth;
    if (var.findMember("gShadowMoments").isValid())
    {
        ref<Texture> pMoments = mpShadowMomentsSample ? mpShadowMomentsSample : mpShadowMapMoments;
        if (pMoments)
            var["gShadowMoments"] = pMoments;
    }
    if (var.findMember("gShadowPointSampler").isValid() && mpShadowPointSampler)
        var["gShadowPointSampler"] = mpShadowPointSampler;
}

void PBRTOfflineRenderer::renderShadowMap(RenderContext* pCtx)
{
    if (!mpShadowFbo || !mpShadowRasterPass || !mpScene) return;

    ensureShadowMapResources();
    const bool useVsm = (mFilamentSettings.shadowType == 2);
    pCtx->clearFbo(mpShadowFbo.get(), float4(0, 0, 0, 0), 1.f, 0, FboAttachmentType::All);

    ShaderVar depthCB = mpShadowRasterPass->getVars()->getRootVar()["ShadowDepthCB"];
    if (depthCB.isValid())
    {
        depthCB["gOutputMoments"] = useVsm ? 1u : 0u;
        depthCB["gVsmExponent"] = mFilamentSettings.vsmExponent;
        depthCB["gVsmMaxMoment"] = mFilamentSettings.vsmMaxMoment;
    }

    auto pCam = mpScene->getCamera();

    const float3 lightDir = normalize(mFilamentSettings.sunDirection);
    const float3 camPos = pCam->getPosition();
    const float3 camFwd = normalize(pCam->getTarget() - camPos);
    const AABB sceneBounds = mpScene->getSceneBounds();
    const float3 sceneCenter = sceneBounds.center();
    const float sceneRadius = sceneBounds.radius();

    const uint32_t cascadeCount = (uint32_t)std::clamp(mFilamentSettings.shadowCascades, 1, 4);
    const CascadeAtlasLayout atlasLayout = getAtlasLayout(cascadeCount);
    const float camNear = pCam->getNearPlane();
    const float camFar = std::min(pCam->getFarPlane(), mFilamentSettings.cascadeSplits[cascadeCount - 1] * 2.f);

    float splits[4] = {
        mFilamentSettings.cascadeSplits.x,
        mFilamentSettings.cascadeSplits.y,
        mFilamentSettings.cascadeSplits.z,
        mFilamentSettings.cascadeSplits.w,
    };
    for (uint32_t i = 0; i < cascadeCount; ++i)
        splits[i] = std::clamp(splits[i], camNear + 0.01f, camFar);

    for (int i = 0; i < 4; ++i)
    {
        mFilamentSettings.shadowLightViewProj[i] = float4x4::identity();
        mFilamentSettings.cascadeAtlasRect[i] = float4(0, 0, 1, 1);
    }

    mpShadowRasterPass->getState()->setFbo(mpShadowFbo);

    for (uint32_t c = 0; c < cascadeCount; ++c)
    {
        const float sliceNear = (c == 0) ? camNear : splits[c - 1];
        const float sliceFar = splits[c];

        float3 frustumCorners[8];
        getFrustumSliceCorners(pCam, sliceNear, sliceFar, frustumCorners);

        float3 samplePoints[24];
        uint32_t pointCount = 8;
        for (int i = 0; i < 8; ++i)
            samplePoints[i] = frustumCorners[i];
        appendAabbCorners(sceneBounds, samplePoints, pointCount);

        float3 sliceCenter = float3(0.f);
        for (uint32_t i = 0; i < pointCount; ++i)
            sliceCenter += samplePoints[i];
        sliceCenter /= float(pointCount);

        const float3 lightPos = sliceCenter - lightDir * (sceneRadius * 2.f + sliceFar);
        const float4x4 lightView = buildLightViewMatrix(lightPos, lightDir, camFwd);

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float minZ = std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();
        for (uint32_t i = 0; i < pointCount; ++i)
        {
            const float3 ls = transformPoint(lightView, samplePoints[i]);
            minX = std::min(minX, ls.x); maxX = std::max(maxX, ls.x);
            minY = std::min(minY, ls.y); maxY = std::max(maxY, ls.y);
            minZ = std::min(minZ, ls.z); maxZ = std::max(maxZ, ls.z);
        }

        const float margin = 0.05f * std::max(maxX - minX, maxY - minY);
        float left = minX - margin, right = maxX + margin;
        float bottom = minY - margin, top = maxY + margin;
        const float nearP = minZ - margin;
        const float farP = maxZ + margin + sceneRadius * 0.5f;

        const uint32_t tileW = mShadowMapSize / atlasLayout.cols;
        const uint32_t tileH = mShadowMapSize / atlasLayout.rows;
        if (mFilamentSettings.shadowStable)
        {
            const float extentX = right - left;
            const float extentY = top - bottom;
            const float texelX = extentX / std::max(1u, tileW);
            const float texelY = extentY / std::max(1u, tileH);
            const float centerX = std::floor(((left + right) * 0.5f) / texelX) * texelX;
            const float centerY = std::floor(((bottom + top) * 0.5f) / texelY) * texelY;
            left = centerX - extentX * 0.5f;
            right = centerX + extentX * 0.5f;
            bottom = centerY - extentY * 0.5f;
            top = centerY + extentY * 0.5f;
        }

        const float4x4 lightProj = buildOrthoProjMatrix(left, right, bottom, top, nearP, farP);
        mFilamentSettings.shadowLightViewProj[c] = mul(lightView, lightProj);
        mFilamentSettings.cascadeAtlasRect[c] = getAtlasTileRect(c, atlasLayout);

        const uint32_t col = c % atlasLayout.cols;
        const uint32_t row = c / atlasLayout.cols;
        GraphicsState::Viewport vp(float(col * tileW), float(row * tileH), float(tileW), float(tileH), 0.f, 1.f);
        mpShadowRasterPass->getState()->setViewport(0, vp, true);

        pCam->setViewMatrix(lightView);
        pCam->setProjectionMatrix(lightProj);
        mpScene->rasterize(pCtx, mpShadowRasterPass->getState().get(), mpShadowRasterPass->getVars().get());
    }

    // setViewMatrix/setProjectionMatrix enable persistent mode; restore interactive camera control afterward.
    pCam->togglePersistentViewMatrix(false);
    pCam->togglePersistentProjectionMatrix(false);
    mpShadowRasterPass->getState()->setViewport(0, GraphicsState::Viewport(0.f, 0.f, float(mShadowMapSize), float(mShadowMapSize), 0.f, 1.f), true);

    if (useVsm && mpFilamentPostProcess && mpShadowMapMoments && mpShadowMapMomentsBlur)
    {
        if (mFilamentSettings.vsmBlurWidth > 0.f)
            mpFilamentPostProcess->blurShadowMoments(pCtx, mpShadowMapMoments, mpShadowMapMomentsBlur, mFilamentSettings);
        mpShadowMomentsSample = (mFilamentSettings.vsmBlurWidth > 0.f) ? mpShadowMapMomentsBlur : mpShadowMapMoments;
    }
    else
    {
        mpShadowMomentsSample = nullptr;
    }
}

void PBRTOfflineRenderer::onFrameRender(RenderContext* pCtx, const ref<Fbo>& pFbo)
{
    pCtx->clearFbo(pFbo.get(), kClear, 1.f, 0, FboAttachmentType::All);
    if (!mSceneLoaded || !mpScene)
    {
        if (mSingleFrame && mFrameCount == 0)
        {
            mFrameCount++;
            shutdown();
        }
        return;
    }

    ensureRenderPasses(pCtx);
    if (!mpGBufferPass || !mpLightingPass)
        return;

    mRenderTaskQueue.execute(pCtx, 1);

    if (mFrameCount == 0)
        logInfo("Rendering first frame.");

    auto pTarget = pFbo->getColorTexture(0);
    const uint2 frameDim = uint2(pTarget->getWidth(), pTarget->getHeight());
    const GraphicsState::Viewport fullViewport(0.f, 0.f, float(frameDim.x), float(frameDim.y), 0.f, 1.f);

    // Halton(2,3) jitter for TAA (Filament sHaltonSamples)
    auto pCam = mpScene->getCamera();
    if (mFilamentSettings.antiAliasing == 2)
    {
        if (!mpHaltonJitter)
            mpHaltonJitter = HaltonSamplePattern::create(32);
        pCam->setPatternGenerator(mpHaltonJitter, 1.f / float2(frameDim));
    }
    else
    {
        pCam->setPatternGenerator(nullptr, float2(0.f));
    }

    // --- Stage 0: Render shadow map ---
    if (mFilamentSettings.enableShadows)
    {
        ensureShadowPassResources();
        renderShadowMap(pCtx);
    }

    // Ensure intermediate textures are correct size
    if (!mpIntermediateTexture || mpIntermediateTexture->getWidth() != frameDim.x || mpIntermediateTexture->getHeight() != frameDim.y)
    {
        mpIntermediateTexture = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpVelocityTexture = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource);
        mpIntermediateDepth = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::D32Float, 1, 1, nullptr,
            ResourceBindFlags::DepthStencil | ResourceBindFlags::ShaderResource);
        mpPostProcessOutput = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpGBufferBaseColor = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource);
        mpGBufferNormalW = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource);
        mpGBufferMaterial = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource);
        mpGBufferEmissive = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource);
        mpGBufferViewDirW = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource);
        mpGBufferIDs = getDevice()->createTexture2D(
            frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource);
    }

    syncFilamentSunLight();
    mpScene->update(pCtx, getGlobalClock().getTime());
    syncFilamentCameraSettings();

    auto pGBufferFbo = Fbo::create(
        getDevice(),
        {mpGBufferBaseColor, mpGBufferNormalW, mpGBufferMaterial, mpGBufferEmissive, mpGBufferViewDirW, mpGBufferIDs},
        mpIntermediateDepth
    );
    pCtx->clearFbo(pGBufferFbo.get(), float4(0.f, 0.f, 0.f, 0.f), 1.f, 1, FboAttachmentType::All);
    mpGBufferPass->getState()->setFbo(pGBufferFbo);
    mpGBufferPass->getState()->setViewport(0, fullViewport, true);
    mpScene->rasterize(pCtx, mpGBufferPass->getState().get(), mpGBufferPass->getVars().get());

    const auto lightingAOSettings = getLightingAOSettings();
    const bool useLightingAO = lightingAOSettings.postProcessingEnabled
        && lightingAOSettings.enableSSAO
        && mpFilamentPostProcess;
    if (useLightingAO)
    {
        mpFilamentPostProcess->executeDeferredSSAO(pCtx, mpIntermediateDepth, mpGBufferNormalW, lightingAOSettings);
    }

    auto pLightingFbo = Fbo::create(getDevice(), {mpIntermediateTexture});
    pCtx->clearFbo(pLightingFbo.get(), kClear, 1.f, 1.f, FboAttachmentType::All);
    pCtx->clearTexture(mpVelocityTexture.get(), float4(0.f, 0.f, 0.f, 0.f));

    {
        auto vars = mpLightingPass->getVars();
        if (vars)
        {
            auto root = vars->getRootVar();
            setAOShaderVars(root);
            setShadowShaderVars(root);
            auto perFrameCB = root.findMember("PerFrameCB");
            if (perFrameCB.isValid())
            {
                if (perFrameCB.findMember("gFrameDim").isValid())
                    perFrameCB["gFrameDim"] = frameDim;
                if (perFrameCB.findMember("gDebugView").isValid())
                    perFrameCB["gDebugView"] = mDebugView;
                if (perFrameCB.findMember("gSunIntensity").isValid())
                    perFrameCB["gSunIntensity"] = mFilamentSettings.enableSunlight ? mFilamentSettings.sunIntensity : 0.0f;
                if (perFrameCB.findMember("gSunColor").isValid())
                    perFrameCB["gSunColor"] = mFilamentSettings.sunColor;
                if (perFrameCB.findMember("gSunDirection").isValid())
                    perFrameCB["gSunDirection"] = normalize(mFilamentSettings.sunDirection);
                if (perFrameCB.findMember("gAmbientIntensity").isValid())
                    perFrameCB["gAmbientIntensity"] = mFilamentSettings.ambientIntensity;
            }
            root["gBaseColor"] = mpGBufferBaseColor;
            root["gNormalW"] = mpGBufferNormalW;
            root["gMaterial"] = mpGBufferMaterial;
            root["gEmissive"] = mpGBufferEmissive;
            root["gViewDirW"] = mpGBufferViewDirW;
            root["gDepth"] = mpIntermediateDepth;
            root["gIDs"] = mpGBufferIDs;
            if (mpFilamentIBL && root.findMember("FilamentIBLCB").isValid())
                mpFilamentIBL->bindShaderVars(root, mFilamentSettings);
        }
    }

    mpLightingPass->execute(pCtx, pLightingFbo);

    // --- Stage 5: Auto Exposure (UE-style histogram + eye adaptation) ---
    float exposureScale = 1.0f;
    if (mAutoExposureSettings.enabled)
    {
        if (!mpAutoExposurePass)
        {
            logInfo("Creating Auto Exposure pass.");
            mpAutoExposurePass = AutoExposurePass::create(getDevice(), Properties{});
        }

        Properties aeProps;
        aeProps["minEV100"] = mAutoExposureSettings.minEV100;
        aeProps["maxEV100"] = mAutoExposureSettings.maxEV100;
        aeProps["speedUp"] = mAutoExposureSettings.speedUp;
        aeProps["speedDown"] = mAutoExposureSettings.speedDown;
        aeProps["exposureCompensation"] = mAutoExposureSettings.exposureCompensation;
        aeProps["lowPercent"] = mAutoExposureSettings.lowPercent;
        aeProps["highPercent"] = mAutoExposureSettings.highPercent;
        aeProps["histogramMin"] = mAutoExposureSettings.histogramMin;
        aeProps["histogramMax"] = mAutoExposureSettings.histogramMax;
        aeProps["enabled"] = true;
        mpAutoExposurePass->setProperties(aeProps);

        exposureScale = mpAutoExposurePass->executeDirect(pCtx, mpIntermediateTexture);
    }

    // Sync FilamentSettings to FilamentPostProcess pass
    if (mpFilamentPostProcess && mFilamentSettings.postProcessingEnabled && mDebugView == 0)
    {
        const ref<Texture> pMotionVec = (mFilamentSettings.antiAliasing == 2) ? mpVelocityTexture : nullptr;
        auto postSettings = mFilamentSettings;
        if (mAutoExposureSettings.enabled && exposureScale > 0.0f && std::isfinite(exposureScale))
            postSettings.exposure += std::log2(exposureScale);
        mpFilamentPostProcess->executeCustom(pCtx, mpIntermediateTexture, mpIntermediateDepth, mpPostProcessOutput, postSettings,
            mFilamentSettings.enableShadows ? mpShadowMapDepth : nullptr, pMotionVec,
            mFilamentSettings.enableShadows ? mpShadowMomentsSample : nullptr);
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
        if (mSingleFrame) shutdown();
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
            mSaveTexture = nullptr;
            mSavePending = false;
        }
    });
}

void PBRTOfflineRenderer::onGuiRender(Gui* pGui)
{
    // Filament gltf_viewer-style sidebar: no title bar, left docked, full height.
    const uint32_t sidebarHeight = (uint32_t)std::max(1.0f, ImGui::GetIO().DisplaySize.y);
    const Gui::WindowFlags kFilamentSidebarFlags = Gui::WindowFlags::AllowMove | Gui::WindowFlags::SetFocus;
    Gui::Window w(pGui, mRealtimeGIOnlyUI ? "PBRT Realtime GI" : "Filament", {380, sidebarHeight}, {0, 0}, kFilamentSidebarFlags);
    if (!mLoadingStatus.empty())
        w.text(fmt::format("Status: {}", mLoadingStatus));
    if (!mRenderTaskQueue.empty() || !mRenderTaskQueue.getCurrentTaskName().empty())
    {
        const std::string& taskName = !mRenderTaskQueue.getCurrentTaskName().empty()
            ? mRenderTaskQueue.getCurrentTaskName()
            : mRenderTaskQueue.getLastCompletedTaskName();
        w.text(fmt::format("Render Queue: {} pending ({})", mRenderTaskQueue.size(), taskName));
    }
    if (mpScene)
    {
        w.text(fmt::format("Loaded: {}", mLoadedScenePath.filename().string()));
        w.text(fmt::format("Geometry: {}", mpScene->getGeometryCount()));
    }
    else
    {
        w.text("No scene loaded.");
    }
    w.text(mScenePath.empty() ? "Selected: <none>" : fmt::format("Selected: {}", mScenePath.string()));
    w.text(fmt::format("Frame: {}", mFrameCount));

    if (mRealtimeGIOnlyUI)
    {
        auto& s = mFilamentSettings;
        auto& d = mDeferredAOSettings;

        if (auto giGroup = w.group("Realtime GI", true))
        {
            if (giGroup.checkbox("Enabled", mRealtimeGIEnabled))
            {
                if (mRealtimeGIEnabled)
                    applyRealtimeGIPreset(true);
                else
                    s.iblIntensity = kPreviewIblIntensityScale;
            }

            if (giGroup.slider("Indirect intensity", mRealtimeGIBounceIntensity, 0.0f, 4.0f) && mRealtimeGIEnabled)
                s.iblIntensity = mRealtimeGIBounceIntensity;
            giGroup.slider("IBL intensity", s.iblIntensity, 0.0f, 10.0f);
            s.iblIntensity = std::clamp(s.iblIntensity, 0.0f, 10.0f);
            s.enableSSAO = false;
            d.enabled = false;
        }

        if (auto lightingGroup = w.group("Lighting", true))
        {
            lightingGroup.slider("IBL intensity", s.iblIntensity, 0.0f, 10.0f);
            s.iblIntensity = std::clamp(s.iblIntensity, 0.0f, 10.0f);
            lightingGroup.slider("IBL rotation", s.iblRotation, -3.14159f, 3.14159f);
            if (mpFilamentIBL)
            {
                lightingGroup.text(mpFilamentIBL->usingPlaceholder()
                    ? "IBL: procedural placeholder"
                    : "IBL: data/ibl/lightroom_14b");
            }

            lightingGroup.checkbox("Sunlight", s.enableSunlight);
            lightingGroup.slider("Sun intensity", s.sunIntensity, 0.0f, 150000.0f);
            lightingGroup.slider("Sun radius [deg]", s.sunAngularRadiusDeg, 0.25f, 20.0f);
            lightingGroup.slider("Sun direction X", s.sunDirection.x, -1.0f, 1.0f);
            lightingGroup.slider("Sun direction Y", s.sunDirection.y, -1.0f, 1.0f);
            lightingGroup.slider("Sun direction Z", s.sunDirection.z, -1.0f, 1.0f);
            if (lightingGroup.button("Normalize Sun direction"))
                s.sunDirection = normalize(s.sunDirection);

            Gui::DropdownList debugViews = {
                {0, "Shaded"},
                {6, "Shadow"},
                {7, "Shadow Map"},
            };
            lightingGroup.dropdown("Debug View", debugViews, mDebugView);
        }

        if (auto shadowGroup = w.group("Shadows", true))
        {
            if (shadowGroup.checkbox("Enabled", mRealtimeGIUseShadows))
            {
                if (mRealtimeGIUseShadows)
                {
                    setEnableShadows(true);
                    requestShadowWarmup();
                }
                else
                    s.enableShadows = false;
            }
            s.enableShadows = mRealtimeGIUseShadows;

            shadowGroup.slider("Shadow map size", s.shadowMapSize, 256u, 2048u);
            shadowGroup.slider("Cascades", s.shadowCascades, 1, 4);
            shadowGroup.slider("Bias", s.shadowBias, 0.0f, 0.01f);
            shadowGroup.checkbox("Stable Shadows", s.shadowStable);
            shadowGroup.checkbox("Contact shadows", s.enableContactShadows);
            Gui::DropdownList shadowTypes = {{0, "PCF"}, {1, "VSM"}, {2, "DPCF"}, {3, "PCSS"}, {4, "PCFd"}};
            if (shadowGroup.dropdown("Shadow type", shadowTypes, (uint32_t&)s.shadowTypeFilament))
                s.shadowType = (s.shadowTypeFilament == 1) ? 2 : 1;
            if (s.shadowTypeFilament == 1)
                shadowGroup.slider("VSM blur", s.vsmBlurWidth, 0.0f, 125.0f);
            else if (s.shadowTypeFilament == 2 || s.shadowTypeFilament == 3)
                shadowGroup.slider("Penumbra scale", s.softShadowPenumbraScale, 0.0f, 100.0f);
        }

        return;
    }

    if (w.button("Choose Scene File"))
    {
        std::filesystem::path path = mScenePath;
        if (openFileDialog(Scene::getFileExtensionFilters(), path))
            mScenePath = path;
    }
    if (w.button("Load Selected Scene") && !mScenePath.empty())
        loadScene(getRenderContext());
    if (w.button("Save Screenshot")) { if (mOutputPath.empty()) { mOutputPath = mScenePath; mOutputPath.replace_extension(".exr"); } saveOutput(getRenderContext()); }
    
    if (auto g = w.group("FilamentFX", true))
    {
        auto& s = mFilamentSettings;

        if (auto viewGroup = g.group("View")) {
            viewGroup.checkbox("Post-processing", s.postProcessingEnabled);
            if (auto ppViewGroup = viewGroup.group("Post-processing")) {
                ppViewGroup.checkbox("Dithering", s.dithering);
                bool bloom = s.enableBloom;
                if (ppViewGroup.checkbox("Bloom", bloom))
                {
                    if (bloom)
                    {
                        requestRenderTask("Warmup Bloom", [this](RenderContext*)
                        {
                            if (mpFilamentPostProcess)
                                mpFilamentPostProcess->ensureBloomPasses();
                            mFilamentSettings.enableBloom = true;
                        });
                    }
                    else
                    {
                        mRenderTaskQueue.remove("Warmup Bloom");
                        s.enableBloom = false;
                    }
                }

                bool taa = s.antiAliasing == 2;
                if (ppViewGroup.checkbox("TAA", taa))
                {
                    if (taa)
                    {
                        requestRenderTask("Warmup TAA", [this](RenderContext*)
                        {
                            if (mpFilamentPostProcess)
                                mpFilamentPostProcess->ensureTAAPass();
                            mFilamentSettings.antiAliasing = 2;
                        });
                    }
                    else if (s.antiAliasing == 2)
                    {
                        mRenderTaskQueue.remove("Warmup TAA");
                        s.antiAliasing = 1;
                    }
                }

                bool fxaa = s.antiAliasing == 1;
                if (ppViewGroup.checkbox("FXAA", fxaa))
                {
                    if (fxaa)
                    {
                        requestRenderTask("Warmup FXAA", [this](RenderContext*)
                        {
                            if (mpFilamentPostProcess)
                                mpFilamentPostProcess->ensureFXAAPass();
                            mFilamentSettings.antiAliasing = 1;
                        });
                    }
                    else if (s.antiAliasing == 1)
                    {
                        mRenderTaskQueue.remove("Warmup FXAA");
                        s.antiAliasing = 0;
                    }
                }
            }
            viewGroup.checkbox("MSAA 4x", s.enableMSAA);
            if (auto msaaGroup = viewGroup.group("MSAA 4x"))
                msaaGroup.checkbox("Custom resolve", s.msaaCustomResolve);
            viewGroup.checkbox("Screen-space reflections", s.enableSSR);
            viewGroup.checkbox("Screen-space Guard Band", s.screenSpaceGuardBand);

            Gui::DropdownList debugViews = {
                {0, "Shaded"},
                {1, "Albedo"},
                {2, "Normal"},
                {3, "Material ID"},
                {4, "Instance ID"},
                {5, "AO"},
                {6, "Shadow"},
                {7, "Shadow Map"},
            };
            viewGroup.dropdown("Debug View", debugViews, mDebugView);
        }

        if (auto bloomGroup = g.group("Bloom Options")) {
            bloomGroup.slider("Strength", s.bloomStrength, 0.0f, 1.0f);
            if (bloomGroup.checkbox("Threshold", s.bloomThresholdEnabled))
                s.bloomThreshold = s.bloomThresholdEnabled ? 1.0f : 0.0f;
            bloomGroup.slider("Levels", s.bloomLevels, 3, 11);
            bloomGroup.slider("Bloom Quality", s.bloomQuality, 0, 3);
            bloomGroup.checkbox("Lens Flare", s.bloomLensFlare);
        }

        if (auto taaGroup = g.group("TAA Options")) {
            taaGroup.slider("Upscaling", s.taaUpscaling, 1.0f, 3.0f);
            taaGroup.checkbox("History Reprojection", s.taaHistoryReprojection);
            taaGroup.slider("Feedback", s.taaFeedback, 0.0f, 1.0f);
            taaGroup.checkbox("Filter History", s.taaFilterHistory);
            taaGroup.checkbox("Filter Input", s.taaFilterInput);
            taaGroup.slider("LOD bias", s.taaLodBias, -8.0f, 0.0f);
            taaGroup.checkbox("HDR", s.taaHDR);
            taaGroup.checkbox("Use YCoCg", s.taaUseYCoCg);
            taaGroup.checkbox("Prevent Flickering", s.taaPreventFlickering);
            Gui::DropdownList jitterPatterns = {{0, "RGSS x4"}, {1, "Uniform Helix x4"}, {2, "Halton x8"}, {3, "Halton x16"}, {4, "Halton x32"}};
            Gui::DropdownList boxClipping = {{0, "Accurate"}, {1, "Clamp"}, {2, "None"}};
            Gui::DropdownList boxTypes = {{0, "AABB"}, {1, "Variance"}};
            taaGroup.dropdown("Jitter Pattern", jitterPatterns, (uint32_t&)s.taaJitterPattern);
            taaGroup.dropdown("Box Clipping", boxClipping, (uint32_t&)s.taaBoxClipping);
            taaGroup.dropdown("Box Type", boxTypes, (uint32_t&)s.taaBoxType);
            taaGroup.slider("Variance Gamma", s.taaVarianceGamma, 0.75f, 1.25f);
            taaGroup.slider("RCAS", s.taaSharpness, 0.0f, 1.0f);
        }

        if (auto ssrGroup = g.group("Screen-space reflections Options")) {
            ssrGroup.slider("Ray thickness", s.ssrThickness, 0.001f, 0.2f);
            ssrGroup.slider("Bias", s.ssrBias, 0.001f, 0.5f);
            ssrGroup.slider("Max distance", s.ssrMaxDistance, 0.1f, 10.0f);
            ssrGroup.slider("Stride", s.ssrStride, 1.0f, 10.0f);
        }

        if (auto dsrGroup = g.group("Dynamic Resolution")) {
            dsrGroup.checkbox("enabled", s.dynamicResolutionEnabled);
            dsrGroup.checkbox("homogeneous", s.dynamicResolutionHomogeneous);
            dsrGroup.slider("min. scale", s.dynamicResolutionMinScale, 0.25f, 1.0f);
            dsrGroup.slider("max. scale", s.dynamicResolutionMaxScale, 0.25f, 1.0f);
            s.dynamicResolutionMinScale = std::min(s.dynamicResolutionMinScale, s.dynamicResolutionMaxScale);
            s.dynamicResolutionScale = s.dynamicResolutionMinScale;
            dsrGroup.slider("quality", s.dynamicResolutionQuality, 0, 3);
            bool fsr = s.enableFSR;
            if (dsrGroup.checkbox("FSR RCAS", fsr))
            {
                if (fsr)
                {
                    requestRenderTask("Warmup FSR", [this](RenderContext*)
                    {
                        if (mpFilamentPostProcess)
                            mpFilamentPostProcess->ensureFSRPass();
                        mFilamentSettings.enableFSR = true;
                    });
                }
                else
                {
                    mRenderTaskQueue.remove("Warmup FSR");
                    s.enableFSR = false;
                }
            }
            dsrGroup.slider("sharpness", s.fsrSharpness, 0.0f, 1.0f);
        }

        if (auto lightGroup = g.group("Light")) {
            if (auto indirectGroup = lightGroup.group("Indirect light")) {
                indirectGroup.slider("IBL intensity", s.iblIntensity, 0.0f, 10.0f);
                s.iblIntensity = std::clamp(s.iblIntensity, 0.0f, 10.0f);
                indirectGroup.slider("IBL rotation", s.iblRotation, -3.14159f, 3.14159f);
                if (mpFilamentIBL) {
                    indirectGroup.text(mpFilamentIBL->usingPlaceholder()
                        ? "Source: procedural placeholder (awaiting data/ibl assets)"
                        : "Source: data/ibl/lightroom_14b");
                }
            }
            if (auto sunlightGroup = lightGroup.group("Sunlight")) {
                sunlightGroup.checkbox("Enable sunlight", s.enableSunlight);
                sunlightGroup.slider("Sun intensity", s.sunIntensity, 0.0f, 150000.0f);
                sunlightGroup.slider("Sun radius [deg]", s.sunAngularRadiusDeg, 0.25f, 20.0f);
                sunlightGroup.slider("Halo size", s.sunHaloSize, 1.0f, 100.0f);
                sunlightGroup.slider("Halo falloff", s.sunHaloFalloff, 1.0f, 1000.0f);
                sunlightGroup.slider("Sun direction X", s.sunDirection.x, -1.0f, 1.0f);
                sunlightGroup.slider("Sun direction Y", s.sunDirection.y, -1.0f, 1.0f);
                sunlightGroup.slider("Sun direction Z", s.sunDirection.z, -1.0f, 1.0f);
                if (sunlightGroup.button("Normalize Sun direction"))
                    s.sunDirection = normalize(s.sunDirection);
                sunlightGroup.slider("Shadow Far", s.shadowFar, 0.0f, s.farPlane);
                if (auto shadowDirGroup = sunlightGroup.group("Shadow direction")) {
                    shadowDirGroup.slider("Shadow direction X", s.sunDirection.x, -1.0f, 1.0f);
                    shadowDirGroup.slider("Shadow direction Y", s.sunDirection.y, -1.0f, 1.0f);
                    shadowDirGroup.slider("Shadow direction Z", s.sunDirection.z, -1.0f, 1.0f);
                }
            }
            if (auto shadowsGroup = lightGroup.group("Shadows")) {
                bool enableShadows = s.enableShadows;
                if (shadowsGroup.checkbox("Enable shadows", enableShadows))
                {
                    if (enableShadows)
                    {
                        requestShadowWarmup();
                        requestRenderTask("Warmup Post Shadow", [this](RenderContext*)
                        {
                            if (mpFilamentPostProcess)
                                mpFilamentPostProcess->ensureShadowPasses();
                        });
                    }
                    else
                    {
                        mRenderTaskQueue.remove("Warmup Shadow Resources");
                        mRenderTaskQueue.remove("Warmup Post Shadow");
                        s.enableShadows = false;
                    }
                }
                shadowsGroup.slider("Shadow map size", s.shadowMapSize, 32u, 2048u);
                shadowsGroup.checkbox("Stable Shadows", s.shadowStable);
                shadowsGroup.checkbox("Enable LiSPSM", s.shadowLiSPSM);
                Gui::DropdownList shadowTypes = {{0, "PCF"}, {1, "VSM"}, {2, "DPCF"}, {3, "PCSS"}, {4, "PCFd"}};
                if (shadowsGroup.dropdown("Shadow type", shadowTypes, (uint32_t&)s.shadowTypeFilament))
                    s.shadowType = (s.shadowTypeFilament == 1) ? 2 : 1;
                if (s.shadowTypeFilament == 1) {
                    shadowsGroup.checkbox("High precision", s.vsmHighPrecision);
                    shadowsGroup.checkbox("ELVSM", s.vsmElvsm);
                    shadowsGroup.slider("VSM MSAA samples", s.vsmMsaaSamplesLog2, 0, 3);
                    shadowsGroup.slider("VSM anisotropy", s.vsmAnisotropy, 0, 3);
                    shadowsGroup.checkbox("VSM mipmapping", s.vsmMipmapping);
                    shadowsGroup.slider("VSM blur", s.vsmBlurWidth, 0.0f, 125.0f);
                } else if (s.shadowTypeFilament == 2 || s.shadowTypeFilament == 3) {
                    shadowsGroup.slider("Penumbra scale", s.softShadowPenumbraScale, 0.0f, 100.0f);
                    shadowsGroup.slider("Penumbra Ratio scale", s.softShadowPenumbraRatioScale, 1.0f, 100.0f);
                }
                shadowsGroup.slider("Cascades", s.shadowCascades, 1, 4);
                shadowsGroup.checkbox("Debug cascades", s.debugCascades);
                shadowsGroup.checkbox("Enable contact shadows", s.enableContactShadows);
                shadowsGroup.slider("Split pos 0", s.cascadeSplitPositions.x, 0.0f, 1.0f);
                shadowsGroup.slider("Split pos 1", s.cascadeSplitPositions.y, 0.0f, 1.0f);
                shadowsGroup.slider("Split pos 2", s.cascadeSplitPositions.z, 0.0f, 1.0f);
                shadowsGroup.slider("Bias", s.shadowBias, 0.0f, 0.01f);
                shadowsGroup.checkbox("Post-Process Shadow (debug)", s.postProcessShadow);
            }
        }

        if (auto fogGroup = g.group("Fog")) {
            bool fog = s.enableFog;
            if (fogGroup.checkbox("Enable large-scale fog", fog))
            {
                if (fog)
                {
                    requestRenderTask("Warmup Fog", [this](RenderContext*)
                    {
                        if (mpFilamentPostProcess)
                            mpFilamentPostProcess->ensureFogPass();
                        mFilamentSettings.enableFog = true;
                    });
                }
                else
                {
                    mRenderTaskQueue.remove("Warmup Fog");
                    s.enableFog = false;
                }
            }
            fogGroup.slider("Start [m]", s.fogStart, 0.0f, 100.0f);
            fogGroup.slider("Extinction [1/m]", s.fogDensity, 0.0f, 1.0f);
            fogGroup.slider("Floor [m]", s.fogHeight, 0.0f, 100.0f);
            fogGroup.slider("Height falloff [1/m]", s.fogHeightFalloff, 0.0f, 4.0f);
            fogGroup.slider("Sun Scattering start [m]", s.fogInScatteringStart, 0.0f, 100.0f);
            fogGroup.slider("Sun Scattering size", s.fogInScatteringSize, 0.1f, 100.0f);
            fogGroup.checkbox("Exclude Skybox", s.fogExcludeSkybox);
            Gui::DropdownList fogColorSources = {{0, "Constant"}, {1, "IBL"}, {2, "Skybox"}};
            fogGroup.dropdown("Color##fogColor", fogColorSources, (uint32_t&)s.fogColorSource);
            fogGroup.rgbColor("Color", s.fogColor);
        }

        if (auto sceneGroup = g.group("Scene")) {
            sceneGroup.checkbox("Scale to unit cube", s.sceneAutoScaleEnabled);
            sceneGroup.checkbox("Automatic instancing", s.sceneAutoInstancingEnabled);
            sceneGroup.checkbox("Show skybox", s.sceneSkyboxEnabled);
            sceneGroup.rgbColor("Background color", s.sceneBackgroundColor);
            sceneGroup.checkbox("Ground shadow", s.sceneGroundPlaneEnabled);
            if (s.sceneGroundPlaneEnabled)
                sceneGroup.slider("Strength", s.sceneGroundShadowStrength, 0.0f, 1.0f);
            if (mpScene && mpScene->getEnvMap()) {
                if (auto envGroup = sceneGroup.group("Environment Map (Scene)"))
                    mpScene->getEnvMap()->renderUI(envGroup);
            }
        }

        if (auto cameraGroup = g.group("Camera")) {
            cameraGroup.slider("Focal length (mm)", s.cameraFocalLength, 16.0f, 90.0f);
            cameraGroup.slider("Aperture", s.cameraAperture, 1.0f, 32.0f);
            cameraGroup.slider("Speed (1/s)", s.cameraShutterSpeed, 1.0f, 1000.0f);
            cameraGroup.slider("ISO", s.cameraSensitivity, 25.0f, 6400.0f);
            cameraGroup.slider("Near", s.nearPlane, 0.001f, 1.0f);
            cameraGroup.slider("Far", s.farPlane, 1.0f, 10000.0f);

            if (auto dofGroup = cameraGroup.group("DoF")) {
                bool dof = s.enableDoF;
                if (dofGroup.checkbox("Enabled##dofEnabled", dof))
                {
                    if (dof)
                    {
                        requestRenderTask("Warmup DoF", [this](RenderContext*)
                        {
                            if (mpFilamentPostProcess)
                                mpFilamentPostProcess->ensureDoFPass();
                            mFilamentSettings.enableDoF = true;
                        });
                    }
                    else
                    {
                        mRenderTaskQueue.remove("Warmup DoF");
                        s.enableDoF = false;
                    }
                }
                dofGroup.slider("Focus distance", s.cameraFocusDistance, 0.0f, 30.0f);
                s.dofFocalDistance = s.cameraFocusDistance;
                dofGroup.slider("Blur scale", s.dofCocScale, 0.1f, 10.0f);
                dofGroup.slider("CoC aspect-ratio", s.dofCocAspectRatio, 0.25f, 4.0f);
                dofGroup.slider("Ring count", s.dofRingCount, 1, 17);
                dofGroup.slider("Max CoC", s.dofMaxCoC, 1.0f, 32.0f);
                dofGroup.checkbox("Native Resolution", s.dofNativeResolution);
                dofGroup.checkbox("Median Filter", s.dofMedianFilter);
            }

            if (auto vignetteGroup = cameraGroup.group("Vignette")) {
                vignetteGroup.checkbox("Enabled##vignetteEnabled", s.enableVignette);
                vignetteGroup.slider("Mid point", s.vignetteMidpoint, 0.0f, 1.0f);
                vignetteGroup.slider("Roundness", s.vignetteRoundness, 0.0f, 1.0f);
                vignetteGroup.slider("Feather", s.vignetteFeather, 0.0f, 1.0f);
                vignetteGroup.rgbColor("Color##vignetteColor", s.vignetteColor);
            }
        }

        if (auto debugGroup = g.group("Debug Options")) {
            if (debugGroup.button("Skip 10 frames"))
                mFrameCount += 10;
        }

        if (auto cgGroup = g.group("Color Grading")) {
            cgGroup.checkbox("Enabled", s.colorGradingEnabled);
            cgGroup.checkbox("Linked curves", s.colorGradingLinkedCurves);
            cgGroup.checkbox("Luminance scaling", s.colorGradingLuminanceScaling);
            cgGroup.checkbox("Gamut mapping", s.colorGradingGamutMapping);
            cgGroup.slider("Quality", s.colorGradingQuality, 0, 3);
            Gui::DropdownList tmModes = {{0, "LINEAR"}, {1, "ACES_LEGACY"}, {2, "ACES"}, {3, "FILMIC"}, {4, "AGX"}, {5, "GENERIC"}, {6, "PBR_NEUTRAL"}, {7, "GT7"}, {8, "DISPLAY_RANGE"}};
            if (cgGroup.dropdown("Tone-mapping", tmModes, (uint32_t&)s.toneMappingFilament))
                s.toneMapping = s.toneMappingFilament;
            Gui::DropdownList customLuts = {{0, "None"}, {1, "Negative"}, {2, "Grayscale"}, {3, "Sepia"}, {4, "Teal and Orange"}};
            cgGroup.dropdown("Custom LUT", customLuts, (uint32_t&)s.colorGradingCustomLut);
            cgGroup.checkbox("3D LUT", s.enableColorGradingLUT);
            if (s.enableColorGradingLUT) {
                Gui::DropdownList lutSizes = {{16, "16"}, {32, "32"}};
                cgGroup.dropdown("LUT Size", lutSizes, (uint32_t&)s.lutSize);
            }
            cgGroup.slider("Exposure", s.exposure, -10.0f, 10.0f);
            cgGroup.slider("Night adaptation", s.nightAdaptation, 0.0f, 1.0f);
            cgGroup.slider("Temperature", s.temperature, -1.0f, 1.0f);
            cgGroup.slider("Tint", s.tint, -1.0f, 1.0f);
            cgGroup.slider("Contrast", s.contrast, 0.0f, 2.0f);
            cgGroup.slider("Vibrance", s.vibrance, 0.0f, 2.0f);
            cgGroup.slider("Saturation", s.saturation, 0.0f, 2.0f);
        }
    }

    if (auto aoW = w.group("FilamentAOPass", true))
    {
        auto& s = mFilamentSettings;

        bool ssao = s.enableSSAO;
        if (aoW.checkbox("Enable SSAO", ssao))
        {
            if (ssao)
            {
                requestRenderTask("Warmup Filament AO", [this](RenderContext*)
                {
                    if (mpFilamentPostProcess)
                    {
                        mpFilamentPostProcess->ensureStructurePasses();
                        mpFilamentPostProcess->ensureSSAOPasses(mFilamentSettings.ssaoMode != 1, mFilamentSettings.ssaoMode == 1);
                    }
                    mFilamentSettings.enableSSAO = true;
                });
            }
            else
            {
                mRenderTaskQueue.remove("Warmup Filament AO");
                s.enableSSAO = false;
            }
        }

        bool aoDebugView = (mDebugView == 5);
        if (aoW.checkbox("AO Debug View", aoDebugView))
            mDebugView = aoDebugView ? 5 : 0;

        if (auto ssaoGroup = aoW.group("SSAO Options")) {
            Gui::DropdownList aoTypes = {{0, "SAO"}, {1, "GTAO"}};
            if (ssaoGroup.dropdown("AO Type", aoTypes, (uint32_t&)s.ssaoMode) && s.enableSSAO)
            {
                requestRenderTask("Warmup Filament AO", [this](RenderContext*)
                {
                    if (mpFilamentPostProcess)
                    {
                        mpFilamentPostProcess->ensureStructurePasses();
                        mpFilamentPostProcess->ensureSSAOPasses(mFilamentSettings.ssaoMode != 1, mFilamentSettings.ssaoMode == 1);
                    }
                });
            }
            ssaoGroup.slider("Quality", s.ssaoQuality, 0, 3);
            ssaoGroup.slider("Low Pass", s.ssaoLowPassFilter, 0, 2);
            ssaoGroup.checkbox("Bent Normals", s.ssaoBentNormals);
            ssaoGroup.checkbox("High quality upsampling", s.ssaoHighQualityUpsampling);
            ssaoGroup.slider("Radius", s.ssaoRadius, 0.1f, 10.0f);
            ssaoGroup.slider("Intensity", s.ssaoIntensity, 0.0f, 4.0f);
            ssaoGroup.slider("Power", s.ssaoPower, 0.25f, 8.0f);

            if (s.ssaoMode == 0) {
                if (ssaoGroup.slider("Min Horizon angle", s.ssaoMinHorizonAngleRad, 0.0f, 0.785398f)) {
                    const float v = std::sin(s.ssaoMinHorizonAngleRad);
                    s.ssaoMinHorizonAngleSineSquared = v * v;
                }
            } else {
                ssaoGroup.slider("Slice Count", s.gtaoSlices, 1, 10);
                ssaoGroup.slider("Steps Per Slice", s.gtaoSteps, 1, 4);
                ssaoGroup.checkbox("Use Visibility Bitmasks", s.gtaoUseVisibilityBitmasks);
                if (s.gtaoUseVisibilityBitmasks) {
                    ssaoGroup.slider("Constant Thickness", s.gtaoConstThickness, 0.01f, 10.0f);
                    ssaoGroup.checkbox("Linear Thickness", s.gtaoLinearThickness);
                }
            }

            ssaoGroup.slider("Bilateral Threshold", s.ssaoBilateralThreshold, 0.0f, 0.1f);
            bool halfRes = s.ssaoResolution != 1.0f;
            if (ssaoGroup.checkbox("Half resolution", halfRes))
                s.ssaoResolution = halfRes ? 0.5f : 1.0f;

            if (auto dlsGroup = ssaoGroup.group("Dominant Light Shadows (experimental)")) {
                dlsGroup.checkbox("Enabled##dls", s.ssctEnabled);
                dlsGroup.slider("Cone angle", s.ssctLightConeRad, 0.0f, 1.570796f);
                dlsGroup.slider("Shadow Distance", s.ssctShadowDistance, 0.0f, 10.0f);
                dlsGroup.slider("Contact dist max", s.ssctContactDistanceMax, 0.0f, 100.0f);
                dlsGroup.slider("Intensity##dls", s.ssctIntensity, 0.0f, 10.0f);
                dlsGroup.slider("Depth bias", s.ssctDepthBias, 0.0f, 1.0f);
                dlsGroup.slider("Depth slope bias", s.ssctDepthSlopeBias, 0.0f, 1.0f);
                dlsGroup.slider("Sample Count", s.ssctSampleCount, 1, 32);
                dlsGroup.slider("Direction X##dls", s.ssctLightDirection.x, -1.0f, 1.0f);
                dlsGroup.slider("Direction Y##dls", s.ssctLightDirection.y, -1.0f, 1.0f);
                dlsGroup.slider("Direction Z##dls", s.ssctLightDirection.z, -1.0f, 1.0f);
            }
        }
    }

    if (auto daoW = w.group("Filament Deferred AO", true))
    {
        auto& d = mDeferredAOSettings;

        bool deferredAO = d.enabled;
        if (daoW.checkbox("Enable Deferred AO", deferredAO))
        {
            if (deferredAO)
                requestDeferredAOWarmup();
            else
            {
                mRenderTaskQueue.remove("Warmup Deferred AO Pass");
                d.enabled = false;
            }
        }

        if (auto daoGroup = daoW.group("Deferred AO Options"))
        {
            daoGroup.slider("Radius", d.radius, 0.1f, 10.0f);
            daoGroup.slider("Intensity", d.intensity, 0.0f, 5.0f);
            daoGroup.slider("Bias", d.bias, 0.001f, 0.5f);
            daoGroup.slider("Power", d.power, 1.0f, 8.0f);
            daoGroup.slider("Sample Count", d.sampleCount, 4u, 64u);
            daoGroup.slider("Blur Radius", d.blurRadius, 0u, 8u);
            daoGroup.slider("Blur Sharpness", d.blurSharpness, 1.0f, 200.0f);
        }
    }

    if (auto aeW = w.group("AutoExposure", true))
    {
        auto& a = mAutoExposureSettings;

        bool autoExposure = a.enabled;
        if (aeW.checkbox("Enable Auto Exposure", autoExposure))
        {
            if (autoExposure)
                requestAutoExposureWarmup();
            else
            {
                mRenderTaskQueue.remove("Warmup Auto Exposure Pass");
                a.enabled = false;
            }
        }
        if (mpAutoExposurePass)
            aeW.text(fmt::format("Current Exposure: {:.4f}", mpAutoExposurePass->getExposure()));

        if (auto aeGroup = aeW.group("Exposure Options"))
        {
            aeGroup.slider("Min EV100", a.minEV100, -20.0f, 0.0f);
            aeGroup.slider("Max EV100", a.maxEV100, 0.0f, 30.0f);
            aeGroup.slider("Speed Up", a.speedUp, 0.1f, 10.0f);
            aeGroup.slider("Speed Down", a.speedDown, 0.1f, 10.0f);
            aeGroup.slider("Exposure Compensation", a.exposureCompensation, -4.0f, 4.0f);
            aeGroup.slider("Low Percent", a.lowPercent, 0.0f, 1.0f);
            aeGroup.slider("High Percent", a.highPercent, 0.0f, 1.0f);
            aeGroup.slider("Histogram Min", a.histogramMin, -16.0f, 0.0f);
            aeGroup.slider("Histogram Max", a.histogramMax, 0.0f, 16.0f);
        }
    }

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

bool PBRTOfflineRenderer::onMouseEvent(const MouseEvent& e)
{
    if (mpScene && mpScene->onMouseEvent(e))
        return true;

    if (mpScene && e.type == MouseEvent::Type::Wheel)
    {
        auto pCam = mpScene->getCamera();
        if (!pCam)
            return false;

        const float3 pos = pCam->getPosition();
        const float3 viewDir = normalize(pCam->getTarget() - pos);
        const float distance = std::max(0.1f, mpScene->getSceneBounds().radius() * 0.15f);
        const float3 delta = viewDir * (e.wheelDelta.y * distance);
        pCam->setPosition(pos + delta);
        pCam->setTarget(pCam->getTarget() + delta);
        pCam->setIsAnimated(false);
        return true;
    }

    return false;
}

void PBRTOfflineRenderer::onDroppedFile(const std::filesystem::path& p)
{
    if (isSupportedScenePath(p))
    {
        mScenePath = p;
        loadScene(getRenderContext());
    }
    else
    {
        logWarning("Dropped file has unsupported extension: {}", p.extension().string());
    }
}

struct PBRTOfflineRendererOptions
{
    std::filesystem::path scenePath;
    std::filesystem::path outputPath;
    bool headless = false;
    bool singleFrame = false;
    bool preview = false;
    bool warmupCache = false;
    bool enableShadows = false;
    bool disableSSAO = false;
    bool enableDeferredAO = false;
    bool useSceneCache = true;
    bool rebuildSceneCache = false;
    bool usePBRTMaterials = true;
    bool explicitMaterialMode = false;
    float ssaoResolution = 0.0f;
    uint32_t debugView = 0;
    uint32_t width = 1920;
    uint32_t height = 1080;
    std::vector<uint32_t> inspectInstanceIDs;
};

static PBRTOfflineRendererOptions parseArgs(int argc, char** argv)
{
    PBRTOfflineRendererOptions options;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if ((a == "--scene" || a == "path" || a == "--path") && i + 1 < argc)
            options.scenePath = argv[++i];
        else if (a == "--output" && i + 1 < argc)
            options.outputPath = argv[++i];
        else if (a == "--headless")
            options.headless = true;
        else if (a == "--preview")
            options.preview = true;
        else if (a == "--warmup-cache")
        {
            options.warmupCache = true;
            options.headless = true;
            options.singleFrame = true;
            options.preview = true;
            options.enableShadows = true;
            options.enableDeferredAO = true;
        }
        else if (a == "--no-scene-cache")
            options.useSceneCache = false;
        else if (a == "--rebuild-scene-cache")
            options.rebuildSceneCache = true;
        else if (a == "--fast-materials")
        {
            options.usePBRTMaterials = false;
            options.explicitMaterialMode = true;
        }
        else if (a == "--pbrt-materials")
        {
            options.usePBRTMaterials = true;
            options.explicitMaterialMode = true;
        }
        else if (a == "--enable-shadows")
            options.enableShadows = true;
        else if (a == "--deferred-ao")
            options.enableDeferredAO = true;
        else if (a == "--disable-ssao")
            options.disableSSAO = true;
        else if (a == "--ssao-fullres")
            options.ssaoResolution = 1.0f;
        else if (a == "--ssao-halfres")
            options.ssaoResolution = 0.5f;
        else if (a == "--single-frame")
            options.singleFrame = true;
        else if (a == "--width" && i + 1 < argc)
            options.width = uint32_t(std::max(1, std::atoi(argv[++i])));
        else if (a == "--height" && i + 1 < argc)
            options.height = uint32_t(std::max(1, std::atoi(argv[++i])));
        else if (a == "--debug-view" && i + 1 < argc)
        {
            std::string value = argv[++i];
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (value == "albedo")
                options.debugView = 1;
            else if (value == "normal")
                options.debugView = 2;
            else if (value == "material")
                options.debugView = 3;
            else if (value == "instance")
                options.debugView = 4;
            else if (value == "ao")
                options.debugView = 5;
            else if (value == "shadow")
                options.debugView = 6;
            else if (value == "shadowmap" || value == "shadow-map")
                options.debugView = 7;
            else if (value == "shadow-cascade")
                options.debugView = 8;
            else if (value == "shadow-uv")
                options.debugView = 9;
            else if (value == "shadow-z")
                options.debugView = 10;
            else if (value == "shadow-delta")
                options.debugView = 11;
            else
                options.debugView = uint32_t(std::max(0, std::atoi(value.c_str())));
        }
        else if (a == "--inspect-instance" && i + 1 < argc)
            options.inspectInstanceIDs.push_back(uint32_t(std::max(0, std::atoi(argv[++i]))));
        else if (options.scenePath.empty() && isSupportedScenePath(a))
            options.scenePath = a;
    }
    return options;
}

#ifndef PBRT_OFFLINE_RENDERER_LIBRARY
int runMain(int argc, char** argv)
{
    auto options = parseArgs(argc, argv);
    SampleAppConfig c;
    c.windowDesc.title = "Falcor Scene Viewer"; c.windowDesc.resizableWindow = true; c.windowDesc.width = options.width; c.windowDesc.height = options.height;
    c.headless = options.headless;
    c.showUI = !options.headless;
    PBRTOfflineRenderer app(c);
    logInfo(
        "Options: headless={}, singleFrame={}, scene='{}', output='{}', sceneCache={}, rebuildSceneCache={}, pbrtMaterials={}, warmupCache={}",
        options.headless,
        options.singleFrame || options.headless,
        options.scenePath.string(),
        options.outputPath.string(),
        options.useSceneCache,
        options.rebuildSceneCache,
        options.usePBRTMaterials,
        options.warmupCache
    );
    if (options.warmupCache)
        logInfo("PBRT viewer cache warmup enabled: prewarming scene cache, GBuffer, lighting, shadow, Filament deferred AO, and post-process shaders.");
    if (options.preview && !options.explicitMaterialMode && options.usePBRTMaterials)
        logInfo("Preview mode defaults to PBRT material shaders. Pass --fast-materials to use the fast shader path.");
    if (options.headless)
    {
        app.setHeadlessProbeMode(true);
    }
    app.setPreviewMode(options.preview);
    if (options.enableShadows)
        app.setEnableShadows(true);
    if (options.disableSSAO)
        app.setEnableSSAO(false);
    if (options.enableDeferredAO)
        app.setEnableDeferredAO(true);
    if (options.ssaoResolution > 0.0f)
        app.setSSAOResolution(options.ssaoResolution);
    app.setScenePath(options.scenePath);
    app.setOutputPath(options.outputPath);
    app.setSingleFrame(options.singleFrame || options.headless);
    app.setUseSceneCache(options.useSceneCache);
    app.setRebuildSceneCache(options.rebuildSceneCache);
    app.setUsePBRTMaterials(options.usePBRTMaterials);
    app.setWarmupCache(options.warmupCache);
    app.setDebugView(options.debugView);
    app.setInspectInstanceIDs(options.inspectInstanceIDs);
    return app.run();
}
int main(int argc, char** argv) { return catchAndReportAllExceptions([&]() { return runMain(argc, argv); }); }
#endif
