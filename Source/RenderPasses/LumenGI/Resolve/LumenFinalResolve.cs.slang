/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/

// C9 production resolve. Probe/temporal/spatial outputs are incident diffuse
// irradiance; the public LumenGI diffuseGI output is reflected linear radiance.

#ifndef LUMEN_GI_RESOLVE_INCIDENT
#define LUMEN_GI_RESOLVE_INCIDENT 0
#endif
#ifndef is_valid_gDiffuseOpacity
#define is_valid_gDiffuseOpacity 0
#endif

cbuffer CB
{
    uint2 gFrameDim;
}

Texture2D<float4> gSourceGI;
Texture2D<float4> gDiffuseOpacity;
RWTexture2D<float4> gResolvedGI;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= gFrameDim))
        return;

    float4 source = gSourceGI[pixel];
    float3 resolved = max(source.rgb, float3(0.f));

#if LUMEN_GI_RESOLVE_INCIDENT
    float3 albedo = float3(1.f);
#if is_valid_gDiffuseOpacity
    albedo = saturate(gDiffuseOpacity[pixel].rgb);
#endif
    // E * albedo / PI is the Lambertian reflected radiance contract. The
    // HWRT path already writes modulated radiance and therefore does not take
    // this branch; this prevents a second albedo multiplication.
    resolved *= albedo * (1.f / 3.14159265358979323846f);
#endif

    resolved = min(resolved, float3(10000.f));
    gResolvedGI[pixel] = float4(resolved, saturate(source.a));
}
