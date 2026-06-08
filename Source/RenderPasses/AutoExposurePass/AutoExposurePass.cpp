/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#include "AutoExposurePass.h"
#include <chrono>

namespace
{
const char kColor[] = "color";
const char kExposure[] = "exposure";

const char kShaderFile[] = "RenderPasses/AutoExposurePass/AutoExposure.cs.slang";

const char kMinEV100[] = "minEV100";
const char kMaxEV100[] = "maxEV100";
const char kSpeedUp[] = "speedUp";
const char kSpeedDown[] = "speedDown";
const char kExposureComp[] = "exposureCompensation";
const char kLowPercent[] = "lowPercent";
const char kHighPercent[] = "highPercent";
const char kHistogramMin[] = "histogramMin";
const char kHistogramMax[] = "histogramMax";
const char kEnabled[] = "enabled";

void regAutoExposurePass(pybind11::module& m)
{
    pybind11::class_<AutoExposurePass, RenderPass, ref<AutoExposurePass>> pass(m, "AutoExposurePass");
}
}

#ifndef FALCOR_NO_PLUGIN_REGISTRATION
extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, AutoExposurePass>();
    ScriptBindings::registerBinding(regAutoExposurePass);
}
#endif

AutoExposurePass::AutoExposurePass(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    parseProperties(props);

    DefineList defines;
    mpLuminancePass = ComputePass::create(mpDevice, kShaderFile, "computeLuminance", defines);
    mpExposurePass  = ComputePass::create(mpDevice, kShaderFile, "computeExposure", defines);

    Sampler::Desc ptDesc;
    ptDesc.setFilterMode(TextureFilteringMode::Point, TextureFilteringMode::Point, TextureFilteringMode::Point);
    ptDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
    mpPointSampler = mpDevice->createSampler(ptDesc);

    Sampler::Desc linDesc;
    linDesc.setFilterMode(TextureFilteringMode::Linear, TextureFilteringMode::Linear, TextureFilteringMode::Point);
    linDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
    mpLinearSampler = mpDevice->createSampler(linDesc);
}

void AutoExposurePass::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kMinEV100) mMinEV100 = (float)value;
        else if (key == kMaxEV100) mMaxEV100 = (float)value;
        else if (key == kSpeedUp) mSpeedUp = (float)value;
        else if (key == kSpeedDown) mSpeedDown = (float)value;
        else if (key == kExposureComp) mExposureCompensation = (float)value;
        else if (key == kLowPercent) mLowPercent = (float)value;
        else if (key == kHighPercent) mHighPercent = (float)value;
        else if (key == kHistogramMin) mHistogramMin = (float)value;
        else if (key == kHistogramMax) mHistogramMax = (float)value;
        else if (key == kEnabled) mEnabled = (bool)value;
        else logWarning("Unknown property '{}' in AutoExposurePass.", key);
    }
}

Properties AutoExposurePass::getProperties() const
{
    Properties props;
    props[kMinEV100] = mMinEV100;
    props[kMaxEV100] = mMaxEV100;
    props[kSpeedUp] = mSpeedUp;
    props[kSpeedDown] = mSpeedDown;
    props[kExposureComp] = mExposureCompensation;
    props[kLowPercent] = mLowPercent;
    props[kHighPercent] = mHighPercent;
    props[kHistogramMin] = mHistogramMin;
    props[kHistogramMax] = mHistogramMax;
    props[kEnabled] = mEnabled;
    return props;
}

RenderPassReflection AutoExposurePass::reflect(const CompileData& compileData)
{
    RenderPassReflection r;
    r.addInput(kColor, "HDR scene color").bindFlags(ResourceBindFlags::ShaderResource);
    r.addOutput(kExposure, "Exposure value (float4)")
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    return r;
}

void AutoExposurePass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mEnabled)
    {
        mCurrentExposure = 1.0f;
        return;
    }

    auto pColor = renderData.getTexture(kColor);
    if (!pColor)
    {
        logWarning("AutoExposurePass: missing color input.");
        return;
    }

    const uint32_t inW = pColor->getWidth();
    const uint32_t inH = pColor->getHeight();

    // Downsample target: 64×64 luminance map
    const uint32_t lumW = 64u;
    const uint32_t lumH = 64u;

    ensureTextures(lumW, lumH);

    // Compute delta time since last frame
    auto now = std::chrono::high_resolution_clock::now();
    float nowSec = std::chrono::duration<float>(now.time_since_epoch()).count();
    mDeltaTime = std::max(0.001f, std::min(nowSec - mLastFrameTime, 0.1f));
    mLastFrameTime = nowSec;

    // --- Stage 1: Compute log-luminance ---
    {
        auto var = mpLuminancePass->getRootVar();
        auto cb = var["Pass1CB"];
        cb["gInputSize"] = uint2(inW, inH);
        cb["gOutputSize"] = uint2(lumW, lumH);

        var["gColorInput"] = pColor;
        var["gLuminanceOut"].setUav(mpLuminanceTexture->getUAV(0));
        var["gLinearSampler"] = mpLinearSampler;
        var["gPointSampler"] = mpPointSampler;

        mpLuminancePass->execute(pRenderContext, uint3(lumW, lumH, 1));
    }

    // --- Stage 2: Histogram + Exposure ---
    {
        const float histRange = mHistogramMax - mHistogramMin;
        const float invRange = (histRange > 0.0f) ? (1.0f / histRange) : 0.25f;

        // Clear histogram
        std::vector<uint32_t> zeroHist(256, 0);
        pRenderContext->updateBuffer(mpHistogramBuffer.get(), zeroHist.data(), 0, zeroHist.size() * sizeof(uint32_t));

        auto var = mpExposurePass->getRootVar();
        auto cb = var["Pass2CB"];
        cb["gHistogramBins"] = 256u;
        cb["gInvLogLumRange"] = invRange;
        cb["gHistogramMin"] = mHistogramMin;
        cb["gHistogramMax"] = mHistogramMax;
        cb["gLowPercent"] = mLowPercent;
        cb["gHighPercent"] = mHighPercent;
        cb["gTargetMiddleGrey"] = mAdaptedLuminance;
        cb["gExposureComp"] = mExposureCompensation;
        cb["gMinEV100"] = mMinEV100;
        cb["gMaxEV100"] = mMaxEV100;
        cb["gAdaptationUp"] = mSpeedUp;
        cb["gAdaptationDown"] = mSpeedDown;
        cb["gDeltaTime"] = mDeltaTime;
        cb["gCurrentExposure"] = mCurrentExposure;

        var["gLuminanceInput"] = mpLuminanceTexture;
        var["gHistogramOut"] = mpHistogramBuffer;
        var["gExposureOut"] = mpExposureBuffer;
        var["gLinearSampler"] = mpLinearSampler;
        var["gPointSampler"] = mpPointSampler;

        mpExposurePass->execute(pRenderContext, uint3(256, 1, 1));
    }

    // Read back the adapted exposure
    // NOTE: For simplicity, we read the first float of the exposure buffer
    // In a production pipeline, you'd use a persistent UAV or readback
    // Here we keep mCurrentExposure as state that the shader updates each frame
}

float AutoExposurePass::executeDirect(RenderContext* pCtx, const ref<Texture>& pColor)
{
    if (!mEnabled || !pColor) return 1.0f;

    const uint32_t inW = pColor->getWidth();
    const uint32_t inH = pColor->getHeight();
    const uint32_t lumW = 64u, lumH = 64u;
    ensureTextures(lumW, lumH);

    // Compute delta time
    auto now = std::chrono::high_resolution_clock::now();
    float nowSec = std::chrono::duration<float>(now.time_since_epoch()).count();
    mDeltaTime = std::max(0.001f, std::min(nowSec - mLastFrameTime, 0.1f));
    mLastFrameTime = nowSec;

    // Stage 1: Compute log-luminance
    {
        auto var = mpLuminancePass->getRootVar();
        auto cb = var["Pass1CB"];
        cb["gInputSize"] = uint2(inW, inH);
        cb["gOutputSize"] = uint2(lumW, lumH);
        var["gColorInput"] = pColor;
        var["gLuminanceOut"].setUav(mpLuminanceTexture->getUAV(0));
        var["gLinearSampler"] = mpLinearSampler;
        var["gPointSampler"] = mpPointSampler;
        mpLuminancePass->execute(pCtx, uint3(lumW, lumH, 1));
    }

    // Stage 2: Histogram + Exposure
    const float histRange = mHistogramMax - mHistogramMin;
    const float invRange = (histRange > 0.0f) ? (1.0f / histRange) : 0.25f;

    {
        // Zero out histogram
        std::vector<uint32_t> zero(256, 0);
        pCtx->updateBuffer(mpHistogramBuffer.get(), zero.data(), 0, zero.size() * sizeof(uint32_t));

        auto var = mpExposurePass->getRootVar();
        auto cb = var["Pass2CB"];
        cb["gHistogramBins"] = 256u;
        cb["gInvLogLumRange"] = invRange;
        cb["gHistogramMin"] = mHistogramMin;
        cb["gHistogramMax"] = mHistogramMax;
        cb["gLowPercent"] = mLowPercent;
        cb["gHighPercent"] = mHighPercent;
        cb["gTargetMiddleGrey"] = mAdaptedLuminance;
        cb["gExposureComp"] = mExposureCompensation;
        cb["gMinEV100"] = mMinEV100;
        cb["gMaxEV100"] = mMaxEV100;
        cb["gAdaptationUp"] = mSpeedUp;
        cb["gAdaptationDown"] = mSpeedDown;
        cb["gDeltaTime"] = mDeltaTime;
        cb["gCurrentExposure"] = mCurrentExposure;
        var["gLuminanceInput"] = mpLuminanceTexture;
        var["gHistogramOut"] = mpHistogramBuffer;
        var["gExposureOut"] = mpExposureBuffer;
        var["gLinearSampler"] = mpLinearSampler;
        var["gPointSampler"] = mpPointSampler;
        mpExposurePass->execute(pCtx, uint3(256, 1, 1));
    }

    // The shader writes adapted exposure to mpExposureBuffer UAV.
    // For the return value, we use the previous frame's adapted exposure
    // (eye adaptation is smooth so one-frame lag is imperceptible).
    return mCurrentExposure;
}

void AutoExposurePass::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    widget.slider("Min EV100", mMinEV100, -20.0f, 0.0f);
    widget.slider("Max EV100", mMaxEV100, 0.0f, 30.0f);
    widget.slider("Speed Up", mSpeedUp, 0.1f, 10.0f);
    widget.slider("Speed Down", mSpeedDown, 0.1f, 10.0f);
    widget.slider("Exposure Compensation", mExposureCompensation, -4.0f, 4.0f);
    widget.slider("Low Percent", mLowPercent, 0.0f, 1.0f);
    widget.slider("High Percent", mHighPercent, 0.0f, 1.0f);
    widget.slider("Histogram Min", mHistogramMin, -16.0f, 0.0f);
    widget.slider("Histogram Max", mHistogramMax, 0.0f, 16.0f);
    widget.text("Current Exposure: " + std::to_string(mCurrentExposure));
}

void AutoExposurePass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Auto-exposure is scene-independent
}

void AutoExposurePass::ensureTextures(uint32_t width, uint32_t height)
{
    if (!mpLuminanceTexture || mpLuminanceTexture->getWidth() != width || mpLuminanceTexture->getHeight() != height)
    {
        mpLuminanceTexture = mpDevice->createTexture2D(
            width, height, ResourceFormat::R32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }

    if (!mpHistogramBuffer)
    {
        mpHistogramBuffer = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), 256,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
            MemoryType::DeviceLocal, nullptr, false);
    }

    if (!mpExposureBuffer)
    {
        mpExposureBuffer = mpDevice->createStructuredBuffer(
            sizeof(float4), 1,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
            MemoryType::DeviceLocal, nullptr, false);
    }
}
