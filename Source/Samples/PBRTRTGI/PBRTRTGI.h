/***************************************************************************
 # PBRT DXR RTGI sample.
 **************************************************************************/
#pragma once
#include "Falcor.h"
#include "Core/SampleApp.h"
#include "Scene/SceneBuilder.h"

using namespace Falcor;

class PBRTRTGI : public SampleApp
{
public:
    PBRTRTGI(const SampleAppConfig& config);

    void onLoad(RenderContext* pRenderContext) override;
    void onResize(uint32_t width, uint32_t height) override;
    void onFrameRender(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo) override;
    void onGuiRender(Gui* pGui) override;
    bool onKeyEvent(const KeyboardEvent& keyEvent) override;
    bool onMouseEvent(const MouseEvent& mouseEvent) override;

    void setScenePath(const std::filesystem::path& path) { mScenePath = path; }
    void setOutputPath(const std::filesystem::path& path) { mOutputPath = path; }
    void setSingleFrame(bool enabled) { mSingleFrame = enabled; }

private:
    void loadScene(const std::filesystem::path& path, const Fbo* pTargetFbo);
    void createRaytraceProgram();
    void setPerFrameVars(const Fbo* pTargetFbo);
    void saveOutput(RenderContext* pRenderContext);

    std::filesystem::path mScenePath;
    std::filesystem::path mOutputPath;
    ref<Scene> mpScene;
    ref<Camera> mpCamera;
    ref<Program> mpRaytraceProgram;
    ref<RtProgramVars> mpRtVars;
    ref<Texture> mpRtOut;

    bool mSingleFrame = false;
    bool mEnableRTGI = true;
    bool mEnableDirectLighting = true;
    bool mEnableShadows = true;
    bool mUseSceneCache = true;
    float mIndirectIntensity = 1.0f;
    uint32_t mMaxBounces = 1;
    uint32_t mSampleIndex = 0;
    uint32_t mFrameCount = 0;
};

