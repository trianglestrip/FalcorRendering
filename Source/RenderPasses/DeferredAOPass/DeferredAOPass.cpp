/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#include "DeferredAOPass.h"

namespace
{
const char kDepth[] = "depth";
const char kNormalW[] = "normalW";
const char kAO[] = "ao";

const char kRadius[] = "radius";
const char kIntensity[] = "intensity";
const char kBias[] = "bias";
const char kPower[] = "power";
const char kSampleCount[] = "sampleCount";
const char kBlurRadius[] = "blurRadius";
const char kBlurSharpness[] = "blurSharpness";
const char kNormalMode[] = "normalMode";

const char kShaderFile[] = "RenderPasses/DeferredAOPass/DeferredAOPass.cs.slang";
const ResourceBindFlags kAOBindFlags =
    ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;

void regDeferredAOPass(pybind11::module& m)
{
    pybind11::class_<DeferredAOPass, RenderPass, ref<DeferredAOPass>> pass(m, "DeferredAOPass");
}
}

#ifndef FALCOR_NO_PLUGIN_REGISTRATION
extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, DeferredAOPass>();
    ScriptBindings::registerBinding(regDeferredAOPass);
}
#endif

DeferredAOPass::DeferredAOPass(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    parseProperties(props);

    DefineList defines;
    mpAOPass = ComputePass::create(mpDevice, kShaderFile, "deferredAOMain", defines);
    mpBlurPass = ComputePass::create(mpDevice, kShaderFile, "deferredAOBlur", defines);

    Sampler::Desc pointDesc;
    pointDesc.setFilterMode(TextureFilteringMode::Point, TextureFilteringMode::Point, TextureFilteringMode::Point);
    pointDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
    mpPointSampler = mpDevice->createSampler(pointDesc);

    Sampler::Desc linearDesc;
    linearDesc.setFilterMode(TextureFilteringMode::Linear, TextureFilteringMode::Linear, TextureFilteringMode::Point);
    linearDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
    mpLinearSampler = mpDevice->createSampler(linearDesc);
}

void DeferredAOPass::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kRadius)
            mRadius = value;
        else if (key == kIntensity)
            mIntensity = value;
        else if (key == kBias)
            mBias = value;
        else if (key == kPower)
            mPower = value;
        else if (key == kSampleCount)
            mSampleCount = value;
        else if (key == kBlurRadius)
            mBlurRadius = value;
        else if (key == kBlurSharpness)
            mBlurSharpness = value;
        else if (key == kNormalMode)
            mNormalMode = value;
        else
            logWarning("Unknown property '{}' in DeferredAOPass properties.", key);
    }
}

Properties DeferredAOPass::getProperties() const
{
    Properties props;
    props[kRadius] = mRadius;
    props[kIntensity] = mIntensity;
    props[kBias] = mBias;
    props[kPower] = mPower;
    props[kSampleCount] = mSampleCount;
    props[kBlurRadius] = mBlurRadius;
    props[kBlurSharpness] = mBlurSharpness;
    props[kNormalMode] = mNormalMode;
    return props;
}

RenderPassReflection DeferredAOPass::reflect(const CompileData& compileData)
{
    RenderPassReflection r;
    r.addInput(kDepth, "Depth buffer").bindFlags(ResourceBindFlags::ShaderResource);
    r.addInput(kNormalW, "World-space GBuffer normals").bindFlags(ResourceBindFlags::ShaderResource);
    r.addOutput(kAO, "Deferred AO output: R=visibility, G=normalized view depth")
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(kAOBindFlags);
    return r;
}

void DeferredAOPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pDepth = renderData.getTexture(kDepth);
    auto pNormalW = renderData.getTexture(kNormalW);
    auto pAO = renderData.getTexture(kAO);
    if (!pDepth || !pNormalW || !pAO)
    {
        logWarning("DeferredAOPass::execute() - missing required resources.");
        return;
    }

    const uint32_t width = pAO->getWidth();
    const uint32_t height = pAO->getHeight();
    ensureTextures(width, height);
    syncCameraSettings();

    {
        auto var = mpAOPass->getRootVar();
        auto cb = var["PerFrameCB"];
        cb["gResolution"] = uint2(width, height);
        cb["gInvProj"] = mInvProj;
        cb["gInvViewProj"] = mInvViewProj;
        cb["gCameraPos"] = mCameraPos;
        cb["gNearPlane"] = mNearPlane;
        cb["gFarPlane"] = mFarPlane;
        cb["gPositionParams"] = mPositionParams;
        cb["gRadius"] = mRadius;
        cb["gIntensity"] = mIntensity;
        cb["gBias"] = mBias;
        cb["gPower"] = mPower;
        cb["gSampleCount"] = std::clamp(mSampleCount, 1u, 64u);
        cb["gNormalMode"] = mNormalMode;

        var["gDepth"] = pDepth;
        var["gNormalW"] = pNormalW;
        var["gDst"].setUav((mBlurRadius > 0 ? mpAOInternal : pAO)->getUAV(0));
        var["gPointSampler"] = mpPointSampler;
        var["gLinearSampler"] = mpLinearSampler;
        mpAOPass->execute(pRenderContext, uint3(width, height, 1));
    }

    if (mBlurRadius == 0)
        return;

    const float2 axes[2] = {float2(1.0f, 0.0f), float2(0.0f, 1.0f)};
    ref<Texture> inputs[2] = {mpAOInternal, mpBlurTemp};
    ref<Texture> outputs[2] = {mpBlurTemp, pAO};
    for (uint32_t i = 0; i < 2; ++i)
    {
        auto var = mpBlurPass->getRootVar();
        auto cb = var["BlurCB"];
        cb["gBlurResolution"] = uint2(width, height);
        cb["gBlurAxis"] = axes[i];
        cb["gBlurRadius"] = std::min(mBlurRadius, 8u);
        cb["gBlurSharpness"] = mBlurSharpness;
        var["gAOInput"] = inputs[i];
        var["gBlurredAO"].setUav(outputs[i]->getUAV(0));
        var["gPointSampler"] = mpPointSampler;
        mpBlurPass->execute(pRenderContext, uint3(width, height, 1));
    }
}

void DeferredAOPass::executeDirect(RenderContext* pCtx, const ref<Texture>& pDepth,
    const ref<Texture>& pNormalW, const ref<Texture>& pAOTarget)
{
    if (!pDepth || !pNormalW || !pAOTarget) return;

    const uint32_t width = pAOTarget->getWidth();
    const uint32_t height = pAOTarget->getHeight();
    ensureTextures(width, height);
    syncCameraSettings();

    {
        auto var = mpAOPass->getRootVar();
        auto cb = var["PerFrameCB"];
        cb["gResolution"] = uint2(width, height);
        cb["gInvProj"] = mInvProj;
        cb["gInvViewProj"] = mInvViewProj;
        cb["gCameraPos"] = mCameraPos;
        cb["gNearPlane"] = mNearPlane;
        cb["gFarPlane"] = mFarPlane;
        cb["gPositionParams"] = mPositionParams;
        cb["gRadius"] = mRadius;
        cb["gIntensity"] = mIntensity;
        cb["gBias"] = mBias;
        cb["gPower"] = mPower;
        cb["gSampleCount"] = std::clamp(mSampleCount, 1u, 64u);
        cb["gNormalMode"] = mNormalMode;

        var["gDepth"] = pDepth;
        var["gNormalW"] = pNormalW;
        var["gDst"].setUav((mBlurRadius > 0 ? mpAOInternal : pAOTarget)->getUAV(0));
        var["gPointSampler"] = mpPointSampler;
        var["gLinearSampler"] = mpLinearSampler;
        mpAOPass->execute(pCtx, uint3(width, height, 1));
    }

    if (mBlurRadius == 0) return;

    const float2 axes[2] = {float2(1.0f, 0.0f), float2(0.0f, 1.0f)};
    ref<Texture> inputs[2] = {mpAOInternal, mpBlurTemp};
    ref<Texture> outputs[2] = {mpBlurTemp, pAOTarget};
    for (uint32_t i = 0; i < 2; ++i)
    {
        auto var = mpBlurPass->getRootVar();
        auto cb = var["BlurCB"];
        cb["gBlurResolution"] = uint2(width, height);
        cb["gBlurAxis"] = axes[i];
        cb["gBlurRadius"] = std::min(mBlurRadius, 8u);
        cb["gBlurSharpness"] = mBlurSharpness;
        var["gAOInput"] = inputs[i];
        var["gBlurredAO"].setUav(outputs[i]->getUAV(0));
        var["gPointSampler"] = mpPointSampler;
        mpBlurPass->execute(pCtx, uint3(width, height, 1));
    }
}

void DeferredAOPass::renderUI(Gui::Widgets& widget)
{
    widget.slider("Radius", mRadius, 0.05f, 10.0f);
    widget.slider("Intensity", mIntensity, 0.0f, 4.0f);
    widget.slider("Bias", mBias, 0.0f, 0.2f);
    widget.slider("Power", mPower, 0.25f, 8.0f);
    widget.slider("Sample Count", mSampleCount, 1u, 64u);
    widget.slider("Blur Radius", mBlurRadius, 0u, 8u);
    widget.slider("Blur Sharpness", mBlurSharpness, 1.0f, 200.0f);
    Gui::DropdownList normalModes = {{0, "Packed [0,1]"}, {1, "Signed [-1,1]"}};
    widget.dropdown("Normal Mode", normalModes, mNormalMode);
}

void DeferredAOPass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
}

void DeferredAOPass::syncCameraSettings()
{
    if (!mpScene || !mpScene->getCamera()) return;

    const auto& pCamera = mpScene->getCamera();
    const float4x4 proj = pCamera->getProjMatrix();
    mInvProj = inverse(proj);
    mInvViewProj = pCamera->getInvViewProjMatrix();
    mCameraPos = pCamera->getPosition();
    mNearPlane = pCamera->getNearPlane();
    mFarPlane = pCamera->getFarPlane();
    mPositionParams = float2(2.0f / proj[0][0], 2.0f / proj[1][1]);
}

void DeferredAOPass::ensureTextures(uint32_t width, uint32_t height)
{
    auto ensure = [&](ref<Texture>& pTexture)
    {
        if (!pTexture || pTexture->getWidth() != width || pTexture->getHeight() != height)
            pTexture = mpDevice->createTexture2D(width, height, ResourceFormat::RGBA32Float, 1, 1, nullptr, kAOBindFlags);
    };
    ensure(mpAOInternal);
    ensure(mpBlurTemp);
}
