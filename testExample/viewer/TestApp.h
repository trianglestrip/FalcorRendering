/***************************************************************************
 # Copyright (c) 2024, Falcor SDK Test Example
 #--------------------------------------------------------------------------
 # Simple OBJ Viewer using Falcor SDK
 **************************************************************************/
#pragma once
#include <Falcor/Falcor.h>
#include <Falcor/Core/SampleApp.h>
#include <Falcor/Core/Pass/RasterPass.h>

using namespace Falcor;

class ObjViewer : public SampleApp
{
public:
    ObjViewer(const SampleAppConfig& config);
    ~ObjViewer();

    void onLoad(RenderContext* pRenderContext) override;
    void onFrameRender(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo) override;
    void onGuiRender(Gui* pGui) override;
    bool onKeyEvent(const KeyboardEvent& keyEvent) override;
    bool onMouseEvent(const MouseEvent& mouseEvent) override;
    void onResize(uint32_t width, uint32_t height) override;
    void onShutdown() override;

    void setInitialScene(const std::filesystem::path& path) { mInitialScene = path; }

private:
    void loadScene(const std::filesystem::path& path, const Fbo* pTargetFbo);

    ref<Scene> mpScene;
    ref<Camera> mpCamera;
    ref<RasterPass> mpRasterPass;

    std::string mStatusMsg = "Ready. Press L to load an OBJ file.";
    std::filesystem::path mInitialScene;
};
