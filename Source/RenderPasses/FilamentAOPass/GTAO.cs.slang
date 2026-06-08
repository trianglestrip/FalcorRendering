// Filament GTAO — gtaoImpl.fs + gtao.mat

#include "RenderPasses/FilamentAOPass/FilamentSSAOUtils.slangh"
#include "RenderPasses/FilamentAOPass/FilamentGTAOImpl.slangh"
#include "RenderPasses/FilamentAOPass/FilamentAOBilateral.slangh"

cbuffer PerFrameCB
{
    float4 gResolution;
    float2 gPositionParams;
    float  gInvFarPlane;
    int    gMaxLevel;

    float  gProjectionScaleRadius;
    float  gIntensity;
    float2 gSliceCount;
    float  gStepsPerSlice;

    float  gRadius;
    float  gInvRadiusSquared;
    float  gPower;
    float  gThicknessHeuristic;
    float  gConstThickness;
    float  gLinearThickness;
    float2 gBlurAxis;
    float  gFarPlaneOverEdgeDistance;
    int    gBlurSampleCount;
    float  gBlurKernel[16];
};

cbuffer CameraCB
{
    float4x4 gInvProj;
};

Texture2D<float> gStructureDepth;
Texture2D<float4> gAOBuffer;
RWTexture2D<float4> gDst;
RWTexture2D<float4> gBlurredAO;
SamplerState gPointSampler;

[numthreads(16, 16, 1)]
void gtaoMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= uint2(gResolution.xy))) return;

    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;

    float depth = gStructureDepth.SampleLevel(gPointSampler, uv, 0).r;
    if (FilamentIsSkyDepth(depth))
    {
        gDst[dispatchThreadId.xy] = FilamentSkyAOOutput();
        return;
    }

    float3 origin = FilamentComputeViewPosition(uv, depth, gInvProj);
    float3 normal = FilamentGTAOComputeViewSpaceNormal(uv, origin, gResolution, gInvProj);

    float obscurance = 0.0;
    float3 bentNormal = normal;
    if (gIntensity > 0.0)
    {
        FilamentGTAOGroundTruthAmbientOcclusion(
            uv, origin, normal, gResolution,
            gProjectionScaleRadius, gSliceCount, gStepsPerSlice,
            gInvRadiusSquared, gThicknessHeuristic, gConstThickness, gLinearThickness,
            gInvFarPlane, gMaxLevel, gInvProj, obscurance, bentNormal);
    }

    float aoVisibility = pow(saturate(1.0 - obscurance), gPower);
    aoVisibility = lerp(1.0, aoVisibility, gIntensity);

    float packedZ = FilamentPackNormalizedDepth(origin.z, gInvFarPlane);
    gDst[dispatchThreadId.xy] = FilamentPackAOOutput(saturate(aoVisibility), packedZ, bentNormal);
}

[numthreads(16, 16, 1)]
void gtaoBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution.xy)) return;

    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;
    gBlurredAO[dispatchThreadId.xy] = FilamentAOBilateralBlurCenter(
        uv, coord, gAOBuffer, gPointSampler, gBlurAxis, gBlurSampleCount,
        gFarPlaneOverEdgeDistance, gBlurKernel);
}
