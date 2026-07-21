/***************************************************************************
 # Copyright (c) 2024, MiniFalcorSDK Test Example
 #--------------------------------------------------------------------------
 # Minimal OBJ Viewer using MiniFalcorSDK
 # Usage: Press 'L' to load a .obj/.gltf/.pyscene file
 #        Arrow keys / mouse to navigate the scene
 #        Press 'R' to reset camera to frame the model
 **************************************************************************/
#include "MiniObjViewer.h"
#include "Utils/Math/FalcorMath.h"
#include "Utils/UI/TextRenderer.h"

FALCOR_EXPORT_D3D12_AGILITY_SDK

static const float4 kClearColor(0.18f, 0.18f, 0.22f, 1.0f);

MiniObjViewer::MiniObjViewer(const SampleAppConfig& config) : SampleApp(config) {}

MiniObjViewer::~MiniObjViewer() {}

void MiniObjViewer::onLoad(RenderContext* pRenderContext)
{
    if (!mInitialScene.empty() && std::filesystem::exists(mInitialScene))
    {
        loadScene(mInitialScene, getTargetFbo().get());
        return;
    }
    mStatusMsg = "No scene loaded. Press L to load a .obj, .gltf or .pyscene file.";
}

void MiniObjViewer::onResize(uint32_t width, uint32_t height)
{
    if (mpCamera)
        mpCamera->setAspectRatio((float)width / (float)height);
}

void MiniObjViewer::onFrameRender(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo)
{
    pRenderContext->clearFbo(pTargetFbo.get(), kClearColor, 1.0f, 0, FboAttachmentType::All);

    if (mpScene)
    {
        IScene::UpdateFlags updates = mpScene->update(pRenderContext, getGlobalClock().getTime());
        mpRasterPass->getState()->setFbo(pTargetFbo);
        mpScene->rasterize(pRenderContext, mpRasterPass->getState().get(), mpRasterPass->getVars().get());
    }

    std::string msg = getFrameRate().getMsg();
    if (!mStatusMsg.empty())
        msg += "\n" + mStatusMsg;
    getTextRenderer().render(pRenderContext, msg, pTargetFbo, {20, 20});
}

void MiniObjViewer::onGuiRender(Gui* pGui)
{
    Gui::Window w(pGui, "Mini OBJ Viewer", {300, 200}, {10, 80});

    if (w.button("Load Scene (L)"))
    {
        std::filesystem::path path;
        if (openFileDialog(Scene::getFileExtensionFilters(), path))
            loadScene(path, getTargetFbo().get());
    }

    if (mpScene)
    {
        w.text("Scene: " + mpScene->getPath().filename().string());
        w.text("Meshes: " + std::to_string(mpScene->getMeshCount()));
        w.text("Lights: " + std::to_string(mpScene->getLightCount()));
        w.separator();
        mpScene->renderUI(w);
    }
    else
    {
        w.text("MiniFalcorSDK - Minimal build");
        w.text("Core + RT GI (no Nanite/SDF/etc.)");
    }
}

bool MiniObjViewer::onKeyEvent(const KeyboardEvent& keyEvent)
{
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed)
    {
        if (keyEvent.key == Input::Key::L)
        {
            std::filesystem::path path;
            if (openFileDialog(Scene::getFileExtensionFilters(), path))
                loadScene(path, getTargetFbo().get());
            return true;
        }
        if (keyEvent.key == Input::Key::R && mpScene)
        {
            float radius = mpScene->getSceneBounds().radius();
            float3 center = mpScene->getSceneBounds().center();
            mpScene->setCameraSpeed(radius * 0.25f);
            mpCamera->setDepthRange(std::max(0.1f, radius / 750.0f), radius * 10);
            float distance = std::max(radius * 2.5f, 0.1f);
            mpCamera->setPosition(center + float3(distance * 0.2f, distance * 0.35f, distance));
            mpCamera->setTarget(center);
            mpCamera->setUpVector(float3(0, 1, 0));
            mStatusMsg = "Camera reset.";
            return true;
        }
    }
    return mpScene ? mpScene->onKeyEvent(keyEvent) : false;
}

bool MiniObjViewer::onMouseEvent(const MouseEvent& mouseEvent)
{
    return mpScene ? mpScene->onMouseEvent(mouseEvent) : false;
}

void MiniObjViewer::onShutdown() {}

void MiniObjViewer::loadScene(const std::filesystem::path& path, const Fbo* pTargetFbo)
{
    if (!std::filesystem::exists(path))
    {
        mStatusMsg = "File not found: " + path.string();
        return;
    }

    try
    {
        mpScene = Scene::create(getDevice(), path);
        mpCamera = mpScene->getCamera();

        float radius = mpScene->getSceneBounds().radius();
        float3 center = mpScene->getSceneBounds().center();
        mpScene->setCameraSpeed(radius * 0.25f);
        float nearZ = std::max(0.1f, radius / 750.0f);
        float farZ = radius * 10;
        mpCamera->setDepthRange(nearZ, farZ);
        mpCamera->setAspectRatio((float)pTargetFbo->getWidth() / (float)pTargetFbo->getHeight());

        // Frame the model
        float distance = std::max(radius * 2.5f, 0.1f);
        mpCamera->setPosition(center + float3(distance * 0.2f, distance * 0.35f, distance));
        mpCamera->setTarget(center);
        mpCamera->setUpVector(float3(0, 1, 0));

        auto shaderModules = mpScene->getShaderModules();
        auto typeConformances = mpScene->getTypeConformances();
        auto defines = mpScene->getSceneDefines();

        ProgramDesc rasterProgDesc;
        rasterProgDesc.addShaderModules(shaderModules);
        rasterProgDesc.addShaderLibrary("MiniObjViewer.3d.slang").vsEntry("vsMain").psEntry("psMain");
        rasterProgDesc.addTypeConformances(typeConformances);

        mpRasterPass = RasterPass::create(getDevice(), rasterProgDesc, defines);
        mStatusMsg = "Loaded: " + path.filename().string();
    }
    catch (const std::exception& e)
    {
        mStatusMsg = std::string("Failed: ") + e.what();
        mpScene = nullptr;
    }
}

int runMain(int argc, char** argv)
{
    SampleAppConfig config;
    config.windowDesc.title = "MiniFalcorSDK - OBJ Viewer";
    config.windowDesc.width = 1280;
    config.windowDesc.height = 720;
    config.windowDesc.resizableWindow = true;
    config.windowDesc.enableVSync = false;

    MiniObjViewer app(config);
    if (argc > 1)
        app.setInitialScene(std::filesystem::path(argv[1]));
    return app.run();
}

int main(int argc, char** argv)
{
    return catchAndReportAllExceptions([&]() { return runMain(argc, argv); });
}
