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
#include "Utils/SampleGenerators/CPUSampleGenerator.h"
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
    void setInspectInstanceIDs(std::vector<uint32_t> ids) { mInspectInstanceIDs = std::move(ids); }
    void setHeadlessProbeMode(bool enabled);
    void setPreviewMode(bool enabled);

private:
    void loadScene(RenderContext* pCtx);
    void saveOutput(RenderContext* pCtx);
    void buildTaskGraph();
    void renderShadowMap(RenderContext* pCtx);
    void ensureShadowMapResources();
    void setShadowShaderVars(const ShaderVar& var);
    void setAOShaderVars(const ShaderVar& var);
    void syncFilamentCameraSettings();
    void syncFilamentSunLight();
    void initSunFromScene();

    std::filesystem::path mScenePath, mOutputPath;
    ref<Scene> mpScene;
    ref<RasterPass> mpGBufferPass;
    ref<FullScreenPass> mpLightingPass;
    ref<FilamentPostProcess> mpFilamentPostProcess;
    ref<FilamentIBL> mpFilamentIBL;
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
    double mStartTime = 0.0;

    // Taskflow for parallel processing
    tf::Executor mExecutor;
    tf::Taskflow mTaskflow;
    bool mSavePending = false;
    bool mSingleFrame = false;
    uint32_t mDebugView = 0;
    std::vector<uint32_t> mInspectInstanceIDs;
    std::mutex mSaveMutex;
    std::filesystem::path mSavePath;
    ref<Texture> mSaveTexture;

    FilamentPostProcess::FilamentSettings mFilamentSettings;
};
