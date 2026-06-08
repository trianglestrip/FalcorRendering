// Filament-style screen-space shadow visibility (debug / optional post-process path)
#include "RenderPasses/FilamentFX/Shadow/FilamentShadow.slangh"

cbuffer PerFrameCB
{
    uint2 gResolution;
    uint gShadowType; // 0: PCF Hard, 1: PCF Low (3x3 gaussian), 2: VSM
    uint gCascadeCount;
    float4 gCascadeSplits;
    float gShadowBias;
    float gShadowSlopeScaleBias;
    uint2 gShadowAtlasSize;
    float gVsmExponent;
    float gVsmLightBleedReduction;
};

cbuffer CameraCB
{
    float4x4 gInvViewProj;
    float3 gCameraPos;
    float _pad0;
};

Texture2D<float> gDepth;
Texture2D<float> gShadowMap;
Texture2D<float2> gShadowMoments;
RWTexture2D<float> gDst;

SamplerState gPointSampler;

cbuffer ShadowCameraCB
{
    float4x4 gLightViewProj[4];
    float4 gCascadeAtlasRect[4];
};

[numthreads(16, 16, 1)]
void shadowMapMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution))
        return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5f) / float2(gResolution);

    float depth = gDepth.SampleLevel(gPointSampler, uv, 0).r;
    if (depth > 0.9999)
    {
        gDst[coord] = 1.0;
        return;
    }

    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 world = mul(gInvViewProj, clip);
    world.xyz /= world.w;

    float visibility = evaluateShadowVisibility(
        world.xyz,
        gCameraPos,
        1u,
        gShadowType,
        gCascadeCount,
        gShadowBias,
        gCascadeSplits,
        gLightViewProj,
        gCascadeAtlasRect,
        float2(gShadowAtlasSize),
        gShadowMap,
        gShadowMoments,
        gPointSampler,
        gVsmExponent,
        gVsmLightBleedReduction);

    gDst[coord] = visibility;
}
