/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#include "PBRTOfflineRenderer.h"
#include "Utils/Math/FalcorMath.h"
#include "Utils/Settings/Settings.h"
#include "Utils/SampleGenerators/HaltonSamplePattern.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

FALCOR_EXPORT_D3D12_AGILITY_SDK
static const float4 kClear = float4(0.1f, 0.1f, 0.12f, 1.f);

namespace
{
    constexpr float kDefaultIblIntensityScale = 1.0f;
    constexpr float kDefaultSSAORadius = 0.3f;
    constexpr float kDefaultSSAOIntensity = 1.0f;

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
        float3 f = normalize(-lightDir);
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
}

PBRTOfflineRenderer::PBRTOfflineRenderer(const SampleAppConfig& c) : SampleApp(c), mExecutor(std::thread::hardware_concurrency())
{
    mFilamentSettings.postProcessingEnabled = true;
    mFilamentSettings.antiAliasing = 0;
    mFilamentSettings.enableSSAO = true;
    mFilamentSettings.forwardSSAO = true;
    mFilamentSettings.ssaoRadius = kDefaultSSAORadius;
    mFilamentSettings.ssaoIntensity = kDefaultSSAOIntensity;
    mFilamentSettings.ssaoPower = 1.0f;
    mFilamentSettings.ssaoResolution = 0.5f;
    mFilamentSettings.ssaoSampleCount = 11;
    mFilamentSettings.iblIntensity = kDefaultIblIntensityScale;
    mFilamentSettings.sunIntensity = 0.f;
    mFilamentSettings.enableShadows = false;
    mFilamentSettings.exposure = -1.0f;
    mFilamentSettings.toneMapping = 0; // ACES
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
    mFilamentSettings.forwardSSAO = true;
    mFilamentSettings.sunIntensity = 0.f;
}

void PBRTOfflineRenderer::setPreviewMode(bool enabled)
{
    if (!enabled)
        return;

    // Mirror filament pbrt_kitchen setPreviewPreset() defaults.
    mFilamentSettings.postProcessingEnabled = true;
    mFilamentSettings.enableSSAO = true;
    mFilamentSettings.forwardSSAO = true;
    mFilamentSettings.ssaoRadius = kDefaultSSAORadius;
    mFilamentSettings.ssaoIntensity = kDefaultSSAOIntensity;
    mFilamentSettings.ssaoPower = 1.0f;
    mFilamentSettings.ssaoResolution = 0.5f;
    mFilamentSettings.ssaoSampleCount = 11;
    mFilamentSettings.iblIntensity = kDefaultIblIntensityScale;
    mFilamentSettings.sunIntensity = 0.f;
    mFilamentSettings.enableShadows = false;
    mFilamentSettings.exposure = -1.0f; // ~ f/8, 1/125s, ISO 100
    mFilamentSettings.toneMapping = 0; // ACES (Filament ColorGrading::ToneMapping::ACES)
}

void PBRTOfflineRenderer::onLoad(RenderContext* pCtx)
{
    if (!mScenePath.empty()) loadScene(pCtx);
    else logInfo("No scene. Drag .pbrt file or use --scene <path>");
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
    try
    {
        Settings settings;
        settings.addOptions(nlohmann::json{
            {"PBRTImporter:rotateImageTextures90", false},
            {"PBRTImporter:flipTextureV", true},
            {"PBRTImporter:usePBRTMaterials", true},
        });
        mpScene = Scene::create(getDevice(), mScenePath, settings);
        if (!mpScene) { logError("Failed to load scene."); return; }

        auto pCam = mpScene->getCamera();
        float r = mpScene->getSceneBounds().radius();
        if (!std::isfinite(r) || r <= 0.f)
            r = 1000.f;
        mpScene->setCameraSpeed(r * 0.05f);
        mpScene->setCameraController(Scene::CameraControllerType::FirstPerson);
        mpScene->setCameraControlsEnabled(true);
        pCam->setIsAnimated(false);
        pCam->setDepthRange(std::max(0.1f, r / 750.f), r * 10.f);
        mSceneLoaded = true; mFrameCount = 0; mStartTime = getGlobalClock().getTime();

        // Enable emissive lights and build light collection (needed for PBRT area lights)
        mpScene->getRenderSettings().useEmissiveLights = true;
        mpScene->getLightCollection(pCtx);

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

        logInfo("Creating PBRT GBuffer raster pass.");
        ProgramDesc d;
        d.addCompilerArguments({"-Wno-30081"});
        d.addShaderModules(mpScene->getShaderModules());
        d.addShaderLibrary("Samples/PBRTOfflineRenderer/PBRTGBuffer.3d.slang").vsEntry("vsMain").psEntry("psMain");
        d.addTypeConformances(mpScene->getTypeConformances());
        mpGBufferPass = RasterPass::create(getDevice(), d, mpScene->getSceneDefines());

        logInfo("Creating PBRT IBL lighting pass.");
        mpLightingPass = FullScreenPass::create(getDevice(), "Samples/PBRTOfflineRenderer/PBRTIBLLighting.ps.slang");

        logInfo("Creating shadow resources.");
        ensureShadowMapResources();

        // Create shadow depth raster pass (depth-only from light POV)
        logInfo("Creating shadow raster pass.");
        ProgramDesc sd;
        sd.addCompilerArguments({"-Wno-30081"});
        sd.addShaderModules(mpScene->getShaderModules());
        sd.addShaderLibrary("Samples/PBRTOfflineRenderer/ShadowDepth.3d.slang").vsEntry("vsMain").psEntry("psMain");
        sd.addTypeConformances(mpScene->getTypeConformances());
        mpShadowRasterPass = RasterPass::create(getDevice(), sd, mpScene->getSceneDefines());

        Sampler::Desc shadowSamplerDesc;
        shadowSamplerDesc.setFilterMode(TextureFilteringMode::Point, TextureFilteringMode::Point, TextureFilteringMode::Point);
        shadowSamplerDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
        mpShadowPointSampler = getDevice()->createSampler(shadowSamplerDesc);

        logInfo("Creating Filament post-process.");
        Properties props;
        mpFilamentPostProcess = FilamentPostProcess::create(getDevice(), props);

        logInfo("Loading Filament IBL.");
        mpFilamentIBL = FilamentIBL::create(getDevice());
        mpFilamentIBL->loadDefault();

        logInfo("Using IBL-only preview lighting by default.");
        logInfo("Scene load complete.");
    }
    catch (const std::exception& e)
    {
        logError("Exception: {}", e.what());
    }
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
    const float3 intensity = mFilamentSettings.sunColor * mFilamentSettings.sunIntensity;

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

void PBRTOfflineRenderer::setAOShaderVars(const ShaderVar& var)
{
    if (!var.isValid() || !mpFilamentPostProcess) return;

    const bool useForwardSSAO = mFilamentSettings.postProcessingEnabled
        && mFilamentSettings.enableSSAO
        && mFilamentSettings.forwardSSAO;

    auto perFrameCB = var.findMember("PerFrameCB");
    if (perFrameCB.isValid() && perFrameCB.findMember("gSSAOEnabled").isValid())
        perFrameCB["gSSAOEnabled"] = useForwardSSAO ? 1u : 0u;

    if (var.findMember("gSSAO").isValid())
        var["gSSAO"] = mpFilamentPostProcess->getAOTexture(mFilamentSettings);
    if (var.findMember("gSSAOLinearSampler").isValid())
        var["gSSAOLinearSampler"] = mpFilamentPostProcess->getLinearSampler();

    if (var.findMember("AODataCB").isValid())
        mpFilamentPostProcess->bindAOShaderVars(var, mFilamentSettings, mpIntermediateDepth);
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
        const float left = minX - margin, right = maxX + margin;
        const float bottom = minY - margin, top = maxY + margin;
        const float nearP = minZ - margin;
        const float farP = maxZ + margin + sceneRadius * 0.5f;

        const float4x4 lightProj = buildOrthoProjMatrix(left, right, bottom, top, nearP, farP);
        mFilamentSettings.shadowLightViewProj[c] = mul(lightProj, lightView);
        mFilamentSettings.cascadeAtlasRect[c] = getAtlasTileRect(c, atlasLayout);

        const uint32_t tileW = mShadowMapSize / atlasLayout.cols;
        const uint32_t tileH = mShadowMapSize / atlasLayout.rows;
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
    if (!mSceneLoaded || !mpScene || !mpGBufferPass || !mpLightingPass)
    {
        if (mSingleFrame && mFrameCount == 0)
        {
            mFrameCount++;
            shutdown();
        }
        return;
    }

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
    if (mFilamentSettings.enableShadows && mpShadowRasterPass)
    {
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

    // GBuffer depth -> SSAO.
    if (mFilamentSettings.postProcessingEnabled && mFilamentSettings.enableSSAO && mFilamentSettings.forwardSSAO && mpFilamentPostProcess)
    {
        mpFilamentPostProcess->executePrePassSSAO(pCtx, mpIntermediateDepth, mFilamentSettings);
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
            auto perFrameCB = root.findMember("PerFrameCB");
            if (perFrameCB.isValid())
            {
                if (perFrameCB.findMember("gFrameDim").isValid())
                    perFrameCB["gFrameDim"] = frameDim;
                if (perFrameCB.findMember("gDebugView").isValid())
                    perFrameCB["gDebugView"] = mDebugView;
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

    // Sync FilamentSettings to FilamentPostProcess pass
    if (mpFilamentPostProcess && mFilamentSettings.postProcessingEnabled)
    {
        const ref<Texture> pMotionVec = (mFilamentSettings.antiAliasing == 2) ? mpVelocityTexture : nullptr;
        mpFilamentPostProcess->executeCustom(pCtx, mpIntermediateTexture, mpIntermediateDepth, mpPostProcessOutput, mFilamentSettings,
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
            Gui::DropdownList debugViews = {
                {0, "Shaded"},
                {1, "Albedo"},
                {2, "Normal"},
                {3, "Material ID"},
                {4, "Instance ID"},
                {5, "AO"},
            };
            viewGroup.dropdown("Debug View", debugViews, mDebugView);
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

        if (auto iblGroup = g.group("Filament IBL"))
        {
            iblGroup.slider("IBL Intensity Scale", mFilamentSettings.iblIntensity, 0.0f, 1.0f);
            iblGroup.slider("IBL Rotation", mFilamentSettings.iblRotation, -3.14159f, 3.14159f);
            if (mpFilamentIBL)
            {
                iblGroup.text(mpFilamentIBL->usingPlaceholder()
                    ? "Source: procedural placeholder (awaiting data/ibl assets)"
                    : "Source: data/ibl/lightroom_14b");
            }
        }

        if (mpScene && mpScene->getEnvMap())
        {
            if (auto envGroup = g.group("Environment Map (Scene)"))
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
                shadowGroup.slider("Split 1", mFilamentSettings.cascadeSplits.x, 1.0f, 500.0f);
                if (mFilamentSettings.shadowCascades >= 2)
                    shadowGroup.slider("Split 2", mFilamentSettings.cascadeSplits.y, 1.0f, 500.0f);
                if (mFilamentSettings.shadowCascades >= 3)
                    shadowGroup.slider("Split 3", mFilamentSettings.cascadeSplits.z, 1.0f, 500.0f);
                if (mFilamentSettings.shadowCascades >= 4)
                    shadowGroup.slider("Split 4", mFilamentSettings.cascadeSplits.w, 1.0f, 500.0f);
                shadowGroup.slider("Bias", mFilamentSettings.shadowBias, 0.0f, 0.01f);
                shadowGroup.slider("Map Size", mFilamentSettings.shadowMapSize, 512u, 4096u);
                shadowGroup.checkbox("Post-Process Shadow (debug)", mFilamentSettings.postProcessShadow);
            }
        }

        if (mFilamentSettings.postProcessingEnabled) {
            if (auto ppGroup = g.group("Post-processing")) {
                
                if (auto ssaoGroup = ppGroup.group("SSAO (Ambient Occlusion)")) {
                    ssaoGroup.checkbox("Enable", mFilamentSettings.enableSSAO);
                    if (mFilamentSettings.enableSSAO) {
                        ssaoGroup.slider("Radius", mFilamentSettings.ssaoRadius, 0.01f, 2.0f);
                        ssaoGroup.slider("Bias", mFilamentSettings.ssaoBias, 0.0f, 0.1f);
                        ssaoGroup.slider("Power", mFilamentSettings.ssaoPower, 0.1f, 5.0f);
                        ssaoGroup.slider("Intensity", mFilamentSettings.ssaoIntensity, 0.0f, 3.0f);
                        ssaoGroup.slider("Samples", mFilamentSettings.ssaoSampleCount, 4, 64);
                        ssaoGroup.slider("Spiral Turns", mFilamentSettings.ssaoSpiralTurns, 1, 15);
                        ssaoGroup.slider("Resolution", mFilamentSettings.ssaoResolution, 0.25f, 1.0f);
                        ssaoGroup.slider("Upsample Edge", mFilamentSettings.ssaoBilateralEdgeDistance, 0.01f, 1.0f);
                        Gui::DropdownList aoModes = {{0, "SAO"}, {1, "GTAO"}};
                        ssaoGroup.dropdown("AO Mode", aoModes, (uint32_t&)mFilamentSettings.ssaoMode);
                        if (mFilamentSettings.ssaoMode == 1) {
                            ssaoGroup.slider("GTAO Radius", mFilamentSettings.gtaoRadius, 0.05f, 2.0f);
                            ssaoGroup.slider("GTAO Slices", mFilamentSettings.gtaoSlices, 1, 8);
                            ssaoGroup.slider("GTAO Steps", mFilamentSettings.gtaoSteps, 1, 8);
                            ssaoGroup.slider("GTAO Thickness", mFilamentSettings.gtaoThicknessHeuristic, 0.0f, 0.05f);
                        }
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

                if (auto fogGroup = ppGroup.group("Fog")) {
                    fogGroup.checkbox("Enable", mFilamentSettings.enableFog);
                    if (mFilamentSettings.enableFog) {
                        fogGroup.slider("Density", mFilamentSettings.fogDensity, 0.0f, 0.2f);
                        fogGroup.slider("Start", mFilamentSettings.fogStart, 0.0f, 100.0f);
                        fogGroup.rgbColor("Color", mFilamentSettings.fogColor);
                    }
                }

                if (auto ssrGroup = ppGroup.group("SSR (stub)")) {
                    ssrGroup.checkbox("Enable", mFilamentSettings.enableSSR);
                }

                if (auto fsrGroup = ppGroup.group("FSR (RCAS)")) {
                    fsrGroup.checkbox("Enable", mFilamentSettings.enableFSR);
                    if (mFilamentSettings.enableFSR)
                        fsrGroup.slider("Sharpness", mFilamentSettings.fsrSharpness, 0.0f, 2.0f);
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
                    cgGroup.checkbox("3D LUT", mFilamentSettings.enableColorGradingLUT);
                    if (mFilamentSettings.enableColorGradingLUT) {
                        Gui::DropdownList lutSizes = {{16, "16"}, {32, "32"}};
                        cgGroup.dropdown("LUT Size", lutSizes, (uint32_t&)mFilamentSettings.lutSize);
                    }
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
    auto ext = p.extension().string();
    if (ext == ".pbrt" || ext == ".pyscene") { mScenePath = p; loadScene(getRenderContext()); }
}

struct PBRTOfflineRendererOptions
{
    std::filesystem::path scenePath;
    std::filesystem::path outputPath;
    bool headless = false;
    bool singleFrame = false;
    bool preview = false;
    uint32_t debugView = 0;
    uint32_t width = 1920;
    uint32_t height = 840;
    std::vector<uint32_t> inspectInstanceIDs;
};

static PBRTOfflineRendererOptions parseArgs(int argc, char** argv)
{
    PBRTOfflineRendererOptions options;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--scene" && i + 1 < argc)
            options.scenePath = argv[++i];
        else if (a == "--output" && i + 1 < argc)
            options.outputPath = argv[++i];
        else if (a == "--headless")
            options.headless = true;
        else if (a == "--preview")
            options.preview = true;
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
            else
                options.debugView = uint32_t(std::max(0, std::atoi(value.c_str())));
        }
        else if (a == "--inspect-instance" && i + 1 < argc)
            options.inspectInstanceIDs.push_back(uint32_t(std::max(0, std::atoi(argv[++i]))));
    }
    return options;
}

int runMain(int argc, char** argv)
{
    const auto options = parseArgs(argc, argv);
    SampleAppConfig c;
    c.windowDesc.title = "PBRT Renderer - Falcor"; c.windowDesc.resizableWindow = true; c.windowDesc.width = options.width; c.windowDesc.height = options.height;
    c.headless = options.headless;
    c.showUI = !options.headless;
    PBRTOfflineRenderer app(c);
    logInfo("Options: headless={}, singleFrame={}, scene='{}', output='{}'",
        options.headless, options.singleFrame || options.headless, options.scenePath.string(), options.outputPath.string());
    if (options.headless)
    {
        app.setHeadlessProbeMode(true);
    }
    app.setPreviewMode(options.preview);
    app.setScenePath(options.scenePath);
    app.setOutputPath(options.outputPath);
    app.setSingleFrame(options.singleFrame || options.headless);
    app.setDebugView(options.debugView);
    app.setInspectInstanceIDs(options.inspectInstanceIDs);
    return app.run();
}
int main(int argc, char** argv) { return catchAndReportAllExceptions([&]() { return runMain(argc, argv); }); }
