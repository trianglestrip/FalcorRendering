/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#include "Testing/UnitTest.h"

#include "NaniteBuild.h"
#include "NaniteToolAsset.h"
#include "Scene/SceneBuilder.h"
#include "Utils/Settings/Settings.h"

namespace Falcor
{
namespace
{
using namespace FalcorRendering::NaniteTool;

const std::filesystem::path kCubeObjPath = getRuntimeDirectory() / "data/framework/meshes/cube.obj";

Asset buildCubeAsset()
{
    InputScene scene = loadObjScene(kCubeObjPath);
    BuildOptions options;
    return buildNaniteAsset(scene, options);
}

WriteOptions debugWriteOptions()
{
    WriteOptions options;
    options.compressVertices = false;
    options.debugUncompressed = true;
    return options;
}
} // namespace

GPU_TEST(NaniteStreaming_InitialResidency, TAGS("Nanite"))
{
    ASSERT_TRUE(std::filesystem::exists(kCubeObjPath)) << "Missing test mesh: " << kCubeObjPath.string();

    Asset cpuAsset = buildCubeAsset();
    buildMetadataTables(cpuAsset, 32);
    ASSERT_FALSE(cpuAsset.pages.empty());

    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_streaming.fnanite");
    writeAsset(tempPath, cpuAsset, debugWriteOptions());

    ref<Scene> pScene = SceneBuilder(ctx.getDevice(), tempPath, Settings(), SceneBuilder::Flags::Default).getScene();
    ASSERT_TRUE(pScene != nullptr);
    ASSERT_TRUE(pScene->getNaniteAsset() != nullptr);

    RenderContext* pRenderContext = ctx.getRenderContext();
    pScene->uploadNaniteAsset(pRenderContext);

    const NaniteStreamingStats stats = pScene->getNaniteStreamingStats();
    EXPECT_GE(stats.residentPages, 1u);
    EXPECT_EQ(stats.totalPages, static_cast<uint32_t>(pScene->getNaniteAsset()->getCpuAsset().pages.size()));
    EXPECT_GT(stats.budgetBytes, 0u);
    EXPECT_GE(stats.residentBytes, pScene->getNaniteAsset()->getCpuAsset().pages[0].byteSize);
    EXPECT_TRUE(pScene->getNanitePageRequestBuffer() != nullptr);
    EXPECT_TRUE(pScene->getNanitePageFallbackBuffer() != nullptr);

    std::filesystem::remove(tempPath);
}

GPU_TEST(NaniteStreaming_BudgetEviction, TAGS("Nanite"))
{
    ASSERT_TRUE(std::filesystem::exists(kCubeObjPath)) << "Missing test mesh: " << kCubeObjPath.string();

    Asset cpuAsset = buildCubeAsset();
    buildMetadataTables(cpuAsset, 32);

    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_streaming_budget.fnanite");
    writeAsset(tempPath, cpuAsset, debugWriteOptions());

    ref<Scene> pScene = SceneBuilder(ctx.getDevice(), tempPath, Settings(), SceneBuilder::Flags::Default).getScene();
    ASSERT_TRUE(pScene != nullptr);

    pScene->setNaniteVramBudgetBytes(1);
    pScene->uploadNaniteAsset(ctx.getRenderContext());

    const NaniteStreamingStats stats = pScene->getNaniteStreamingStats();
    EXPECT_TRUE(stats.residentPages >= 1u);
    EXPECT_GE(stats.budgetBytes, 1u);

    std::filesystem::remove(tempPath);
}

} // namespace Falcor
