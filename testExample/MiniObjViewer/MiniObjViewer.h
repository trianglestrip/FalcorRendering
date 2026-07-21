/***************************************************************************
 # Copyright (c) 2024, MiniFalcorSDK Test Example
 #--------------------------------------------------------------------------
 # Minimal OBJ Viewer using MiniFalcorSDK
 # Usage: Press 'L' to load a .obj/.gltf/.pyscene file
 #        Arrow keys / mouse to navigate the scene
 **************************************************************************/
#pragma once
#include <Falcor/Falcor.h>
#include <Falcor/Core/SampleApp.h>
#include <Falcor/Core/Pass/RasterPass.h>

using namespace Falcor;

class MiniObjViewer : public SampleApp
{
public:
    MiniObjViewer(const SampleAppConfig& config);
    ~MiniObjViewer();

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

    std::string mStatusMsg = "MiniFalcorSDK - Press L to load an OBJ file.";
    std::filesystem::path mInitialScene;
};
