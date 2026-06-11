/***************************************************************************
 # Copyright (c) 2024, Falcor SDK Test Example
 #--------------------------------------------------------------------------
 # Simple OBJ Viewer using Falcor SDK
 # Usage: Press 'L' to load a .obj/.gltf/.pyscene file
 #        Arrow keys / mouse to navigate the scene
 **************************************************************************/
#include "TestApp.h"
#include "Utils/Math/FalcorMath.h"
#include "Utils/UI/TextRenderer.h"

FALCOR_EXPORT_D3D12_AGILITY_SDK

static const float4 kClearColor(0.18f, 0.18f, 0.22f, 1.0f);

ObjViewer::ObjViewer(const SampleAppConfig& config) : SampleApp(config) {}

ObjViewer::~ObjViewer() {}

void ObjViewer::onLoad(RenderContext* pRenderContext)
{
    // Load from command-line path first
    if (!mInitialScene.empty() && std::filesystem::exists(mInitialScene))
    {
        loadScene(mInitialScene, getTargetFbo().get());
        return;
    }

    // Try to auto-load from media folder
    if (!mInitialScene.empty())
    {
        mStatusMsg = "Scene not found: " + mInitialScene.string() + ". Press L to load manually.";
        return;
    }

    // Try to load a default scene from the media folder
    std::filesystem::path mediaPath = "media";
    if (std::filesystem::exists(mediaPath))
    {
        for (auto& entry : std::filesystem::recursive_directory_iterator(mediaPath))
        {
            if (entry.path().extension() == ".pyscene" || entry.path().extension() == ".obj")
            {
                loadScene(entry.path(), getTargetFbo().get());
                break;
            }
        }
    }

    if (!mpScene)
    {
        mStatusMsg = "No scene loaded. Press L to load a .obj, .gltf or .pyscene file.";
    }
}

void ObjViewer::onResize(uint32_t width, uint32_t height)
{
    if (mpCamera)
    {
        mpCamera->setAspectRatio((float)width / (float)height);
    }
}

void ObjViewer::onFrameRender(RenderContext* pRenderContext, const ref<Fbo>& pTargetFbo)
{
    pRenderContext->clearFbo(pTargetFbo.get(), kClearColor, 1.0f, 0, FboAttachmentType::All);

    if (mpScene)
    {
        IScene::UpdateFlags updates = mpScene->update(pRenderContext, getGlobalClock().getTime());

        // Set render pass FBO and rasterize
        mpRasterPass->getState()->setFbo(pTargetFbo);
        mpScene->rasterize(pRenderContext, mpRasterPass->getState().get(), mpRasterPass->getVars().get());
    }

    // Render overlay text
    std::string msg = getFrameRate().getMsg();
    if (!mStatusMsg.empty())
        msg += "\n" + mStatusMsg;
    getTextRenderer().render(pRenderContext, msg, pTargetFbo, {20, 20});
}

void ObjViewer::onGuiRender(Gui* pGui)
{
    Gui::Window w(pGui, "OBJ Viewer", {300, 200}, {10, 80});

    if (w.button("Load Scene (L)"))
    {
        std::filesystem::path path;
        if (openFileDialog(Scene::getFileExtensionFilters(), path))
        {
            loadScene(path, getTargetFbo().get());
        }
    }

    if (mpScene)
    {
        w.text("Scene loaded: " + mpScene->getPath().filename().string());
        w.text("Meshes: " + std::to_string(mpScene->getMeshCount()));
        w.text("Lights: " + std::to_string(mpScene->getLightCount()));
        w.separator();
        mpScene->renderUI(w);
    }
}

bool ObjViewer::onKeyEvent(const KeyboardEvent& keyEvent)
{
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed)
    {
        if (keyEvent.key == Input::Key::L)
        {
            std::filesystem::path path;
            if (openFileDialog(Scene::getFileExtensionFilters(), path))
            {
                loadScene(path, getTargetFbo().get());
            }
            return true;
        }
        if (keyEvent.key == Input::Key::R)
        {
            // Reset camera to frame the model's bounding box
            if (mpScene)
            {
                float radius = mpScene->getSceneBounds().radius();
                float3 center = mpScene->getSceneBounds().center();
                mpScene->setCameraSpeed(radius * 0.25f);
                mpCamera->setDepthRange(std::max(0.1f, radius / 750.0f), radius * 10);
                float distance = std::max(radius * 2.5f, 0.1f);
                float3 camPos = center + float3(distance * 0.2f, distance * 0.35f, distance);
                mpCamera->setPosition(camPos);
                mpCamera->setTarget(center);
                mpCamera->setUpVector(float3(0, 1, 0));
                mStatusMsg = "Camera reset.";
            }
            return true;
        }
    }

    return mpScene ? mpScene->onKeyEvent(keyEvent) : false;
}

bool ObjViewer::onMouseEvent(const MouseEvent& mouseEvent)
{
    return mpScene ? mpScene->onMouseEvent(mouseEvent) : false;
}

void ObjViewer::onShutdown() {}

void ObjViewer::loadScene(const std::filesystem::path& path, const Fbo* pTargetFbo)
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

        // Update the controllers
        float radius = mpScene->getSceneBounds().radius();
        float3 center = mpScene->getSceneBounds().center();
        mpScene->setCameraSpeed(radius * 0.25f);
        float nearZ = std::max(0.1f, radius / 750.0f);
        float farZ = radius * 10;
        mpCamera->setDepthRange(nearZ, farZ);
        mpCamera->setAspectRatio((float)pTargetFbo->getWidth() / (float)pTargetFbo->getHeight());

        // Position camera to frame the model's bounding box
        // Compute a good viewing distance to encompass the bounding sphere with some margin
        float distance = std::max(radius * 2.5f, 0.1f);
        // Place camera slightly above and in front of the model center (right-handed: Z is forward)
        float3 camPos = center + float3(distance * 0.2f, distance * 0.35f, distance);
        mpCamera->setPosition(camPos);
        mpCamera->setTarget(center);
        mpCamera->setUpVector(float3(0, 1, 0));

        // Get shader modules and type conformances for the scene's material system
        auto shaderModules = mpScene->getShaderModules();
        auto typeConformances = mpScene->getTypeConformances();
        auto defines = mpScene->getSceneDefines();

        // Create raster pass with scene shader modules
        ProgramDesc rasterProgDesc;
        rasterProgDesc.addShaderModules(shaderModules);
        rasterProgDesc.addShaderLibrary("TestApp.3d.slang").vsEntry("vsMain").psEntry("psMain");
        rasterProgDesc.addTypeConformances(typeConformances);

        mpRasterPass = RasterPass::create(getDevice(), rasterProgDesc, defines);

        mStatusMsg = "Loaded: " + path.filename().string();
    }
    catch (const std::exception& e)
    {
        mStatusMsg = std::string("Failed to load scene: ") + e.what();
        mpScene = nullptr;
    }
}

int runMain(int argc, char** argv)
{
    SampleAppConfig config;
    config.windowDesc.title = "Falcor OBJ Viewer (SDK Test)";
    config.windowDesc.width = 1280;
    config.windowDesc.height = 720;
    config.windowDesc.resizableWindow = true;
    config.windowDesc.enableVSync = false;

    ObjViewer app(config);

    // Accept an optional OBJ file path from command line
    if (argc > 1)
    {
        app.setInitialScene(std::filesystem::path(argv[1]));
    }

    return app.run();
}

int main(int argc, char** argv)
{
    return catchAndReportAllExceptions([&]() { return runMain(argc, argv); });
}
