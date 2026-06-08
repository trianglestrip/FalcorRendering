/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#pragma once
#include "Falcor.h"
#include "Core/SampleApp.h"
#include "Core/Pass/FullScreenPass.h"
#include "Core/Pass/RasterPass.h"
#include "Scene/Scene.h"
#include "Scene/Lights/Light.h"
#include "FilamentPostProcess.h"
#include "FilamentIBL.h"
#include "DeferredAOPass.h"
#include "AutoExposurePass.h"
#include "Utils/RenderTaskQueue.h"
#include "Utils/SampleGenerators/CPUSampleGenerator.h"
#include <functional>
#include <taskflow.hpp>

using namespace Falcor;

class PBRTOfflineRenderer : public SampleApp
{
public:
    // ...existing public interface...
    PBRTOfflineRenderer(const SampleAppConfig& c);
    ~PBRTOfflineRenderer();
    void onLoad(RenderContext* pCtx) override;
    void onShutdown() override;
    void onFrameRender(RenderContext* pCtx, const ref<Fbo>& pFbo) override;
    void onGuiRender(Gui* pGui) override;
    bool onKeyEvent(const KeyboardEvent& e) override;
    bool onMouseEvent(const MouseEvent& e) override;
    void onDroppedFile(const std::filesystem::path& p) override;
    void setScenePath(const std::filesystem::path& p) { mScenePath = p; }
    void setOutputPath(const std::filesystem::path& p) { mOutputPath = p; }
    void setSingleFrame(bool enabled) { mSingleFrame = enabled; }
    void setDebugView(uint32_t view) { mDebugView = view; }
    void setUseSceneCache(bool enabled) { mUseSceneCache = enabled; }
    void setRebuildSceneCache(bool enabled) { mRebuildSceneCache = enabled; }
    void setUsePBRTMaterials(bool enabled) { mUsePBRTMaterials = enabled; }
    void setWarmupCache(bool enabled) { mWarmupCache = enabled; }
    void setEnableShadows(bool enabled)
    {
        mFilamentSettings.enableShadows = enabled;
        if (enabled)
        {
            mFilamentSettings.enableSunlight = true;
            if (mFilamentSettings.sunIntensity <= 0.f) mFilamentSettings.sunIntensity = 30000.f;
        }
    }
    void setEnableSSAO(bool enabled) { mFilamentSettings.enableSSAO = enabled; }
    void setSSAOResolution(float resolution) { mFilamentSettings.ssaoResolution = resolution; }
    void setEnableDeferredAO(bool enabled) { mDeferredAOSettings.enabled = enabled; }
    void setInspectInstanceIDs(std::vector<uint32_t> ids) { mInspectInstanceIDs = std::move(ids); }
    void setHeadlessProbeMode(bool enabled);
    void setPreviewMode(bool enabled);

private:
    void loadScene(RenderContext* pCtx);
    void saveOutput(RenderContext* pCtx);
    void buildTaskGraph();
    void setLoadingStatus(const std::string& status);
    void ensureRenderPasses(RenderContext* pCtx);
    void requestRenderTask(const std::string& name, RenderTaskQueue::Task task);
    void requestShadowWarmup();
    void requestDeferredAOWarmup();
    void requestAutoExposureWarmup();
    void renderShadowMap(RenderContext* pCtx);
    void ensureShadowMapResources();
    void ensureShadowPassResources();
    void setShadowShaderVars(const ShaderVar& var);
    void setAOShaderVars(const ShaderVar& var);
    FilamentPostProcess::FilamentSettings getLightingAOSettings() const;
    void syncFilamentCameraSettings();
    void syncFilamentSunLight();
    void initSunFromScene();

    std::filesystem::path mScenePath, mLoadedScenePath, mOutputPath;
    ref<Scene> mpScene;
    ref<RasterPass> mpGBufferPass;
    ref<FullScreenPass> mpLightingPass;
    ref<FilamentPostProcess> mpFilamentPostProcess;
    ref<FilamentIBL> mpFilamentIBL;
    ref<DeferredAOPass> mpDeferredAOPass;
    ref<AutoExposurePass> mpAutoExposurePass;
    ref<Texture> mpDeferredAOTexture;
    ref<Texture> mpExposureTexture;
    ref<Texture> mpIntermediateTexture;
    ref<Texture> mpVelocityTexture;
    ref<Texture> mpIntermediateDepth;
    ref<Texture> mpPostProcessOutput;
    ref<Texture> mpGBufferBaseColor;
    ref<Texture> mpGBufferNormalW;
    ref<Texture> mpGBufferMaterial;
    ref<Texture> mpGBufferEmissive;
    ref<Texture> mpGBufferViewDirW;
    ref<Texture> mpGBufferIDs;
    ref<CPUSampleGenerator> mpHaltonJitter;

    // Shadow map resources
    ref<Texture> mpShadowMapDepth;
    ref<Texture> mpShadowMapMoments;
    ref<Texture> mpShadowMapMomentsBlur;
    ref<Fbo> mpShadowFbo;
    ref<RasterPass> mpShadowRasterPass;
    ref<Sampler> mpShadowPointSampler;
    ref<Texture> mpShadowMomentsSample; // blurred moments bound for shading (VSM)
    uint32_t mShadowMapSize = 2048;

    uint32_t mFrameCount = 0;
    bool mSceneLoaded = false;
    bool mIsLoadingScene = false;
    std::string mLoadingStatus;
    double mStartTime = 0.0;

    // Taskflow for parallel processing
    tf::Executor mExecutor;
    tf::Taskflow mTaskflow;
    bool mSavePending = false;
    bool mSingleFrame = false;
    bool mUseSceneCache = true;
    bool mRebuildSceneCache = false;
    bool mUsePBRTMaterials = true;
    bool mWarmupCache = false;
    uint32_t mDebugView = 0;
    std::vector<uint32_t> mInspectInstanceIDs;
    std::mutex mSaveMutex;
    std::filesystem::path mSavePath;
    ref<Texture> mSaveTexture;
    RenderTaskQueue mRenderTaskQueue;

    FilamentPostProcess::FilamentSettings mFilamentSettings;

    // DeferredAOPass settings (standard deferred SSAO)
    struct DeferredAOSettings
    {
        bool enabled = false;
        float radius = 5.0f;
        float intensity = 3.0f;
        float bias = 0.0f;
        float power = 3.0f;
        uint32_t sampleCount = 48;
        uint32_t blurRadius = 4;
        float blurSharpness = 40.0f;
        uint32_t normalMode = 0;
    };
    DeferredAOSettings mDeferredAOSettings;

    // AutoExposurePass settings (UE-style histogram + eye adaptation)
    struct AutoExposureSettings
    {
        bool enabled = false;
        float minEV100 = -10.0f;
        float maxEV100 = 20.0f;
        float speedUp = 2.0f;
        float speedDown = 1.0f;
        float exposureCompensation = 0.0f;
        float lowPercent = 0.8f;
        float highPercent = 0.98f;
        float histogramMin = -8.0f;
        float histogramMax = 4.0f;
    };
    AutoExposureSettings mAutoExposureSettings;
};
