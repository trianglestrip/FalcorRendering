// Exponential distance + height fog on HDR color (post-DoF, pre-tone-map)

#include "RenderPasses/FilamentFX/Utils/FilamentGeometry.slangh"

cbuffer PerFrameCB
{
    uint2  gResolution;
    float  gFogEnabled;
    float  gFogDensity;
    float  gFogStart;
    float3 gFogColor;
};

cbuffer CameraCB
{
    float4x4 gInvProj;
    float4x4 gInvViewProj;
    float    gNearPlane;
    float    gFarPlane;
    float3   gCameraPos;
};

Texture2D<float4> gSrc;
Texture2D<float>  gDepth;
RWTexture2D<float4> gDst;
SamplerState gLinearSampler;

[numthreads(16, 16, 1)]
void fogMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution)) return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5) / float2(gResolution);

    float4 color = gSrc.Load(int3(coord, 0));
    if (gFogEnabled < 0.5)
    {
        gDst[coord] = color;
        return;
    }

    float depth = gDepth.SampleLevel(gLinearSampler, uv, 0).r;
    if (depth > 0.9999)
    {
        gDst[coord] = color;
        return;
    }

    float3 viewPos = FilamentComputeViewPosition(uv, depth, gInvProj);
    float distance = length(viewPos);

    float3 worldPos = FilamentComputeWorldPosition(uv, depth, gInvViewProj);
    float heightFactor = exp(-max(worldPos.y - gCameraPos.y, 0.0) * gFogDensity * 0.25);

    float opticalPath = gFogDensity * max(distance - gFogStart, 0.0) * heightFactor;
    float transmittance = exp(-opticalPath);
    float fogAmount = 1.0 - transmittance;

    color.rgb = lerp(color.rgb, gFogColor, fogAmount);
    gDst[coord] = color;
}
