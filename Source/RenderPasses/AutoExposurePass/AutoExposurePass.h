/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#pragma once

#include "Falcor.h"
#include "RenderGraph/RenderPass.h"

using namespace Falcor;

class AutoExposurePass : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(AutoExposurePass, "AutoExposurePass",
        "UE-style auto exposure with histogram + eye adaptation.");

    static ref<AutoExposurePass> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<AutoExposurePass>(pDevice, props);
    }

    AutoExposurePass(ref<Device> pDevice, const Properties& props);

    Properties getProperties() const override;
    RenderPassReflection reflect(const CompileData& compileData) override;
    void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    void renderUI(Gui::Widgets& widget) override;
    void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;

    /** Direct execution for manual pipeline integration.
     *  Returns the adapted exposure value. */
    float executeDirect(RenderContext* pCtx, const ref<Texture>& pColor);

    // UE-style exposure parameters (EV100-based, matching UE defaults)
    float getExposure() const { return mCurrentExposure; }

private:
    void parseProperties(const Properties& props);
    void ensureTextures(uint32_t width, uint32_t height);

    // Compute passes
    ref<ComputePass> mpLuminancePass;        // downsample + log-average
    ref<ComputePass> mpHistogramPass;        // build histogram
    ref<ComputePass> mpExposurePass;         // compute final exposure value

    // Resources
    ref<Texture> mpLuminanceTexture;         // downsampled luminance
    ref<Buffer> mpHistogramBuffer;           // 256-bin histogram (uint)
    ref<Buffer> mpExposureBuffer;            // float4: (current exposure, target exposure, delta, pad)
    ref<Buffer> mpExposureReadbackBuffer;    // CPU readback for adapted exposure
    ref<Fence> mpReadbackFence;
    ref<Sampler> mpPointSampler;
    ref<Sampler> mpLinearSampler;

    // UE Auto-Exposure parameters (matching UE5 defaults)
    float mMinEV100 = -10.0f;    // Min EV100 (brightest)
    float mMaxEV100 = 20.0f;     // Max EV100 (darkest)
    float mSpeedUp = 2.0f;       // Adaptation speed when scene gets brighter
    float mSpeedDown = 1.0f;     // Adaptation speed when scene gets darker
    float mExposureCompensation = 0.0f;  // EV100 offset
    float mLowPercent = 0.8f;    // Histogram low percentile (UE: 80% shadows)
    float mHighPercent = 0.98f;  // Histogram high percentile (UE: 98% highlights)
    float mHistogramMin = -8.0f; // Min log luminance for histogram
    float mHistogramMax = 4.0f;  // Max log luminance for histogram
    float mCurrentExposure = 1.0f; // Running exposure value (eye-adapted)
    float mAdaptedLuminance = 0.18f; // Middle gray target (18%)
    bool mEnabled = true;

    // Per-frame delta
    float mLastFrameTime = 0.0f;
    float mDeltaTime = 0.016f;
};
