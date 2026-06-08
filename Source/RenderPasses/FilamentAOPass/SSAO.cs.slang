// Filament SAO (Scalable Ambient Obscurance) - strict alignment with saoImpl.fs + sao.mat

#include "RenderPasses/FilamentFX/Utils/FilamentNoise.slangh"
#include "RenderPasses/FilamentAOPass/FilamentSSAOUtils.slangh"
#include "RenderPasses/FilamentAOPass/FilamentSAOImpl.slangh"
#include "RenderPasses/FilamentAOPass/FilamentAOBilateral.slangh"

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
    float2 gBlurAxis;
    float  gFarPlaneOverEdgeDistance;
    int    gBlurSampleCount;
    float  gBlurKernel[16];
};

cbuffer CameraCB
{
    float4x4 gInvProj;
    float    gNearPlane;
    float    gFarPlane;
};

Texture2D<float> gDepth;
Texture2D<float> gStructureDepth;
Texture2D<float4> gAOBuffer;
RWTexture2D<float4> gDst;
RWTexture2D<float4> gBlurredAO;
SamplerState gPointSampler;

[numthreads(16, 16, 1)]
void ssaoMain(uint3 dispatchThreadId : SV_DispatchThreadID)
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

    float3 origin = FilamentComputeViewPosition(uv, depth, gInvProj);
    float3 normal = FilamentSAOComputeViewSpaceNormalMediumQ(uv, origin, gResolution.zw, gInvProj);

    float noise = FilamentInterleavedGradientNoise(coord);
    float2 tapPosition = FilamentSAOStartPosition(noise);
    float2x2 angleStep = FilamentSAOTapAngleStep(gAngleIncCosSin);
    float ssDiskRadius = gProjectionScaleRadius / max(abs(origin.z), 1e-4);

    float obscurance = 0.0;
    float3 bentNormal = normal;
    for (float i = 0.0; i < gSampleCount.x; i += 1.0)
    {
        FilamentSAOComputeSampleView(
            obscurance,
#if COMPUTE_BENT_NORMAL
            bentNormal,
#endif
            i, ssDiskRadius, uv, origin, normal, abs(origin.z),
            tapPosition, noise, gSampleCount.y,
            gInvRadiusSquared, gMinHorizonAngleSineSquared, gBias, gPeak2,
            gMaxLevel, gResolution, gInvProj);
        tapPosition = mul(angleStep, tapPosition);
    }

    obscurance = sqrt(obscurance * gIntensity);
    float aoVisibility = pow(saturate(1.0 - obscurance), gPower);
#if COMPUTE_BENT_NORMAL
    bentNormal = normalize(bentNormal);
#endif

    float packedZ = FilamentPackNormalizedDepth(origin.z, gInvFarPlane);
    gDst[dispatchThreadId.xy] = FilamentPackAOOutput(aoVisibility, packedZ, bentNormal);
}

[numthreads(16, 16, 1)]
void ssaoBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution.xy)) return;

    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;
    gBlurredAO[dispatchThreadId.xy] = FilamentAOBilateralBlurCenter(
        uv, coord, gAOBuffer, gPointSampler, gBlurAxis, gBlurSampleCount,
        gFarPlaneOverEdgeDistance, gBlurKernel);
}
