/***************************************************************************
 # PBRT realtime GI example launcher.
 **************************************************************************/
#include "PBRTOfflineRenderer.h"

namespace
{
    const std::filesystem::path kDefaultScene = "D:/models/pbrt-v4-scenes/bistro/bistro_cafe.pbrt";
}

int runMain(int argc, char** argv)
{
    SampleAppConfig config;
    config.windowDesc.title = "PBRT Realtime GI - Bistro";
    config.windowDesc.resizableWindow = true;
    config.windowDesc.width = 1920;
    config.windowDesc.height = 1080;
    config.showUI = true;

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

    PBRTOfflineRenderer app(config);
    app.setScenePath(scenePath);
    app.setOutputPath(outputPath);
    app.setSingleFrame(singleFrame || headless);
    app.setPreviewMode(true);
    app.setUsePBRTMaterials(true);
    app.setUseSceneCache(true);
    app.setWarmupCache(false);
    app.setRealtimeGIOnlyUI(true);
    app.setRealtimeGIEnabled(true);
    app.setEnableShadows(true);

    return app.run();
}

int main(int argc, char** argv)
{
    return catchAndReportAllExceptions([&]() { return runMain(argc, argv); });
}
