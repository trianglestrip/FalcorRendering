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
#ifndef is_valid_gFallbackGI
#define is_valid_gFallbackGI 0
#endif
#ifndef is_valid_gLinearZ
#define is_valid_gLinearZ 0
#endif
#ifndef is_valid_gRadianceCache
#define is_valid_gRadianceCache 0
#endif

cbuffer CB
{
    uint2 gFrameDim;
}

Texture2D<float4> gSourceGI;
Texture2D<float4> gFallbackGI;
Texture2D<float4> gLinearZ;
Texture2D<float4> gDiffuseOpacity;
Texture2D<float4> gRadianceCacheGI;
Texture2D<uint> gRadianceCacheValidity;
RWTexture2D<float4> gResolvedGI;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= gFrameDim))
        return;

    float4 source = gSourceGI[pixel];
    // Probe/temporal/spatial stages intentionally emit zero confidence for sky,
    // invalid tiles, and misses. Keep the same-frame HWRT result available on a
    // per-pixel basis so a sparse probe cannot turn a valid surface into a black
    // hole. The fallback is already reflected radiance, so it must not receive
    // the incident E * albedo / PI transform below.
    float4 fallback = float4(0.f);
#if is_valid_gFallbackGI
    fallback = gFallbackGI[pixel];
#endif
    bool hasSurface = true;
#if is_valid_gLinearZ
    hasSurface = gLinearZ[pixel].x > 0.f;
#endif
    // Source validity is carried by finiteness and the source alpha contract
    // (confidence for incident inputs, opaque-valid alpha for raw radiance).
    // Do not inspect source RGB magnitude here: a valid black radiance sample
    // must still keep its source kind and, for incident input, its albedo / PI
    // modulation path.
    const bool sourceValid = all(isfinite(source)) && source.a > 1e-4f;
    bool cacheValid = false;
#if is_valid_gRadianceCache
    const uint cacheFlags = gRadianceCacheValidity[pixel];
    const float4 cacheSample = gRadianceCacheGI[pixel];
    // C10 validity is independent of RGB magnitude: legal black incident
    // radiance remains usable when the producer/hit or sky bits are set.
    cacheValid = hasSurface && ((cacheFlags & (1u << 2)) != 0u) &&
        ((cacheFlags & (1u << 3)) != 0u) && all(isfinite(cacheSample)) &&
        cacheSample.a > 1e-4f;
#else
    const float4 cacheSample = float4(0.f);
#endif
    const bool useFallback = hasSurface && !sourceValid && all(isfinite(fallback));
    float3 resolved = max(source.rgb, float3(0.f));

#if LUMEN_GI_RESOLVE_INCIDENT
    float3 albedo = float3(1.f);
#if is_valid_gDiffuseOpacity
    albedo = saturate(gDiffuseOpacity[pixel].rgb);
#endif
    // E * albedo / PI is the Lambertian reflected radiance contract. The
    // HWRT path already writes modulated radiance and therefore does not take
    // this branch; this prevents a second albedo multiplication.
    if (cacheValid && !sourceValid)
        resolved = max(cacheSample.rgb, float3(0.f));
    else if (!useFallback)
        resolved *= albedo * (1.f / 3.14159265358979323846f);
    else
        resolved = max(fallback.rgb, float3(0.f));
#else
    if (cacheValid && !sourceValid)
        resolved = max(cacheSample.rgb, float3(0.f));
    else if (useFallback)
        resolved = max(fallback.rgb, float3(0.f));
#endif

    // Keep the public real-time GI contract aligned with the UE-style
    // max-ray-intensity guard used by trace/integrate/filter stages.
    resolved = min(resolved, float3(10.f));
    const float resolvedAlpha = (cacheValid && !sourceValid) ? saturate(cacheSample.a) :
        (useFallback ? saturate(fallback.a) : saturate(source.a));
    gResolvedGI[pixel] = float4(resolved, resolvedAlpha);
}
