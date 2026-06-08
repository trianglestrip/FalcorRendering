/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#pragma once

#include "Falcor.h"
#include "RenderGraph/RenderPass.h"

using namespace Falcor;

class DeferredAOPass : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(DeferredAOPass, "DeferredAOPass", "Standard deferred SSAO for depth + GBuffer normals.");

    static ref<DeferredAOPass> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<DeferredAOPass>(pDevice, props);
    }

    DeferredAOPass(ref<Device> pDevice, const Properties& props);

    Properties getProperties() const override;
    RenderPassReflection reflect(const CompileData& compileData) override;
    void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    void renderUI(Gui::Widgets& widget) override;
    void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;

    /** Direct execution without RenderGraph — for manual pipeline integration. */
    void executeDirect(RenderContext* pCtx, const ref<Texture>& pDepth,
        const ref<Texture>& pNormalW, const ref<Texture>& pAOTarget);

private:
    void parseProperties(const Properties& props);
    void syncCameraSettings();
    void ensureTextures(uint32_t width, uint32_t height);

    ref<Scene> mpScene;
    ref<ComputePass> mpAOPass;
    ref<ComputePass> mpBlurPass;
    ref<Sampler> mpPointSampler;
    ref<Sampler> mpLinearSampler;
    ref<Texture> mpAOInternal;
    ref<Texture> mpBlurTemp;

    float4x4 mInvProj = float4x4();
    float4x4 mInvViewProj = float4x4();
    float3 mCameraPos = float3(0.0f);
    float mNearPlane = 0.1f;
    float mFarPlane = 1000.0f;
    float2 mPositionParams = float2(1.0f);

    float mRadius = 1.5f;
    float mIntensity = 0.8f;
    float mBias = 0.01f;
    float mPower = 2.0f;
    uint32_t mSampleCount = 32;
    uint32_t mBlurRadius = 4;
    float mBlurSharpness = 40.0f;
    uint32_t mNormalMode = 0; // 0: packed [0,1], 1: signed [-1,1]
};
