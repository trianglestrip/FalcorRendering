// Deferred SAO for MRT preview paths — normals from GBuffer.

#include "RenderPasses/FilamentFX/Utils/FilamentNoise.slangh"
#include "RenderPasses/FilamentAOPass/FilamentSSAOUtils.slangh"
#include "RenderPasses/FilamentAOPass/FilamentSAOImpl.slangh"

cbuffer PerFrameCB
{
    float4 gResolution;
    float2 gPositionParams;
    float  gInvRadiusSquared;
    float  gMinHorizonAngleSineSquared;
    float  gBias;
    float  gPeak2;
    float  gProjectionScaleRadius;
    float  gIntensity;
    float  gPower;
    float  gSpiralTurns;
    float2 gSampleCount;
    float2 gAngleIncCosSin;
    float  gInvFarPlane;
    int    gMaxLevel;
};

cbuffer CameraCB
{
    float4x4 gInvProj;
    float4x4 gInvView;
};

Texture2D<float> gStructureDepth;
Texture2D<float4> gNormalW;
RWTexture2D<float4> gDst;
SamplerState gPointSampler;

float3 sampleWorldNormal(float2 uv)
{
    float3 normalW = gNormalW.SampleLevel(gPointSampler, uv, 0).xyz * 2.0 - 1.0;
    float len2 = dot(normalW, normalW);
    return (len2 > 1e-8) ? normalW * rsqrt(len2) : float3(0.0, 1.0, 0.0);
}

[numthreads(16, 16, 1)]
void deferredSSAO(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution.xy)) return;

    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;

    float depth = gStructureDepth.SampleLevel(gPointSampler, uv, 0).r;
    if (FilamentIsSkyDepth(depth))
    {
        gDst[dispatchThreadId.xy] = FilamentSkyAOOutput();
        return;
    }

    float3 originW = FilamentComputeWorldPositionFromView(
        FilamentComputeViewPosition(uv, depth, gInvProj), gInvView);
    float3 normalW = sampleWorldNormal(uv);
    float viewDepth = abs(FilamentComputeViewPosition(uv, depth, gInvProj).z);

    float noise = FilamentInterleavedGradientNoise(coord);
    float2 tapPosition = FilamentSAOStartPosition(noise);
    float2x2 angleStep = FilamentSAOTapAngleStep(gAngleIncCosSin);
    float ssDiskRadius = gProjectionScaleRadius / max(viewDepth, 1e-4);

    float obscurance = 0.0;
    float3 bentNormal = normalW;
    for (float i = 0.0; i < gSampleCount.x; i += 1.0)
    {
        FilamentSAOComputeSampleWorld(
            obscurance,
#if COMPUTE_BENT_NORMAL
            bentNormal,
#endif
            i, ssDiskRadius, uv, originW, normalW, viewDepth,
            tapPosition, noise, gSampleCount.y,
            gInvRadiusSquared, gMinHorizonAngleSineSquared, gBias, gPeak2,
            gMaxLevel, gResolution, gInvProj, gInvView);
        tapPosition = mul(angleStep, tapPosition);
    }

    obscurance = sqrt(obscurance * gIntensity);
    float aoVisibility = pow(saturate(1.0 - obscurance), gPower);
#if COMPUTE_BENT_NORMAL
    bentNormal = normalize(bentNormal);
#endif

    float packedZ = FilamentPackNormalizedDepth(-viewDepth, gInvFarPlane);
    gDst[dispatchThreadId.xy] = FilamentPackAOOutput(aoVisibility, packedZ, bentNormal);
}
