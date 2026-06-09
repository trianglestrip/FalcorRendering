#include "NaniteGBufferComposite.h"

#include "RenderGraph/RenderPassHelpers.h"
#include "Utils/Timing/Profiler.h"

namespace
{
const char kShaderFile[] = "RenderPasses/NaniteRaster/NaniteGBufferComposite.cs.slang";

const char kSceneDepth[] = "sceneDepth";
const char kScenePosW[] = "scenePosW";
const char kSceneNormW[] = "sceneNormW";
const char kSceneFaceNormalW[] = "sceneFaceNormalW";
const char kSceneTexC[] = "sceneTexC";
const char kSceneMtlData[] = "sceneMtlData";

const char kNaniteDepth[] = "naniteDepth";
const char kNanitePosition[] = "nanitePosition";
const char kNaniteNormal[] = "naniteNormal";
const char kNaniteTexCoord[] = "naniteTexCoord";
const char kNaniteBaseColor[] = "naniteBaseColor";

const char kDepth[] = "depth";
const char kPosW[] = "posW";
const char kNormW[] = "normW";
const char kFaceNormalW[] = "faceNormalW";
const char kTexC[] = "texC";
const char kMtlData[] = "mtlData";
const char kOutput[] = "output";
} // namespace

NaniteGBufferComposite::NaniteGBufferComposite(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    (void)props;
    mpCompositePass = ComputePass::create(mpDevice, kShaderFile, "main");
}

RenderPassReflection NaniteGBufferComposite::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    const uint2 sz = compileData.defaultTexDims;

    reflector.addInput(kSceneDepth, "GBuffer depth (NDC)").format(ResourceFormat::D32Float).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kScenePosW, "GBuffer world position").format(ResourceFormat::RGBA32Float).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kSceneNormW, "GBuffer shading normal").format(ResourceFormat::RGBA32Float).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kSceneFaceNormalW, "GBuffer face normal").format(ResourceFormat::RGBA32Float).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kSceneTexC, "GBuffer texture coordinates").format(ResourceFormat::RG32Float).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kSceneMtlData, "GBuffer material data").format(ResourceFormat::RGBA32Uint).flags(RenderPassReflection::Field::Flags::Optional);

    reflector.addInput(kNaniteDepth, "Nanite depth (NDC)").format(ResourceFormat::R32Float);
    reflector.addInput(kNanitePosition, "Nanite resolved position").format(ResourceFormat::RGBA32Float);
    reflector.addInput(kNaniteNormal, "Nanite resolved normal").format(ResourceFormat::RGBA32Float);
    reflector.addInput(kNaniteTexCoord, "Nanite resolved texture coordinates").format(ResourceFormat::RGBA32Float);
    reflector.addInput(kNaniteBaseColor, "Nanite resolved base color").format(ResourceFormat::RGBA32Float);

    reflector.addOutput(kDepth, "Merged depth (NDC)").format(ResourceFormat::R32Float);
    reflector.addOutput(kPosW, "Merged world position").format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kNormW, "Merged shading normal").format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kFaceNormalW, "Merged face normal").format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kTexC, "Merged texture coordinates").format(ResourceFormat::RG32Float);
    reflector.addOutput(kMtlData, "Merged material data").format(ResourceFormat::RGBA32Uint);
    reflector.addOutput(kOutput, "Debug shaded output").format(ResourceFormat::RGBA32Float);

    (void)sz;
    return reflector;
}

void NaniteGBufferComposite::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mFrameDim = compileData.defaultTexDims;
    (void)pRenderContext;
}

void NaniteGBufferComposite::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "NaniteGBufferComposite::execute");

    auto var = mpCompositePass->getRootVar();
    var["gScreenSize"] = float2(mFrameDim);

    if (auto pTex = renderData.getTexture(kSceneDepth)) var["gSceneDepth"] = pTex;
    if (auto pTex = renderData.getTexture(kScenePosW)) var["gScenePosW"] = pTex;
    if (auto pTex = renderData.getTexture(kSceneNormW)) var["gSceneNormW"] = pTex;
    if (auto pTex = renderData.getTexture(kSceneFaceNormalW)) var["gSceneFaceNormalW"] = pTex;
    if (auto pTex = renderData.getTexture(kSceneTexC)) var["gSceneTexC"] = pTex;
    if (auto pTex = renderData.getTexture(kSceneMtlData)) var["gSceneMtlData"] = pTex;

    var["gHasSceneGBuffer"] = renderData.getTexture(kSceneDepth) != nullptr;

    var["gNaniteDepth"] = renderData.getTexture(kNaniteDepth);
    var["gNanitePosition"] = renderData.getTexture(kNanitePosition);
    var["gNaniteNormal"] = renderData.getTexture(kNaniteNormal);
    var["gNaniteTexCoord"] = renderData.getTexture(kNaniteTexCoord);
    var["gNaniteBaseColor"] = renderData.getTexture(kNaniteBaseColor);

    var["gDepth"] = renderData.getTexture(kDepth);
    var["gPosW"] = renderData.getTexture(kPosW);
    var["gNormW"] = renderData.getTexture(kNormW);
    var["gFaceNormalW"] = renderData.getTexture(kFaceNormalW);
    var["gTexC"] = renderData.getTexture(kTexC);
    var["gMtlData"] = renderData.getTexture(kMtlData);
    var["gOutput"] = renderData.getTexture(kOutput);

    mpCompositePass->execute(pRenderContext, mFrameDim.x, mFrameDim.y, 1);
}
