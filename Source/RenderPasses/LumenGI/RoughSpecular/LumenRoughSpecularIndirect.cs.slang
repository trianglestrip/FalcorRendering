/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/

/**
 * E1 roughSpecularIndirect diagnostic producer.
 *
 * A single directional incident-radiance sample is filtered by a GGX-shaped
 * proxy lobe.  The implementation intentionally does not pretend to be a
 * full UE Lumen reflection resolve: no screen-space diffuse value, RTXDI
 * direct-light value, or unowned cache is substituted when directionality is
 * absent.  Missing/unbound directional inputs produce a zero payload and an
 * explicit Disabled/DirectionUnavailable validity mask.
 */

#include "LumenRoughSpecularData.slang"

#ifndef is_valid_gRoughSpecularIndirect
#define is_valid_gRoughSpecularIndirect 0
#endif
#ifndef is_valid_gRoughSpecularValidity
#define is_valid_gRoughSpecularValidity 0
#endif

float3 lumenRoughSpecularSafeNormalize(float3 value)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > kLumenRoughSpecularEpsilon
        ? value * rsqrt(lengthSquared)
        : float3(0.f);
}

void lumenRoughSpecularWriteZero(uint2 pixel, uint validity)
{
#if is_valid_gRoughSpecularIndirect
    gRoughSpecularIndirect[pixel] = float4(0.f);
#endif
#if is_valid_gRoughSpecularValidity
    gRoughSpecularValidity[pixel] = validity;
#endif
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= gFrameDim))
        return;

    // The default specialization is hard-disabled.  This is intentionally a
    // compile-time gate so an unbound directional resource cannot be touched.
#if !LUMEN_GI_ROUGH_SPECULAR_ENABLED
    lumenRoughSpecularWriteZero(pixel, kLumenRoughSpecularValidityDisabled);
    return;
#elif !(is_valid_gDirectionalRadiance && is_valid_gReflectionDirection &&
        is_valid_gSurfaceNormalRoughness && is_valid_gViewDirection)
    lumenRoughSpecularWriteZero(
        pixel,
        kLumenRoughSpecularValidityDisabled);
    return;
#else
    const float4 directionalRadiance = gDirectionalRadiance[pixel];
    const float4 reflectionDirection = gReflectionDirection[pixel];
    const float4 surfaceNormalRoughness = gSurfaceNormalRoughness[pixel];
    const float4 viewDirection = gViewDirection[pixel];

    uint validity = 0u;
    if (lumenRoughSpecularDirectionValid(reflectionDirection))
        validity |= kLumenRoughSpecularValidityDirectionAvailable;
    if (lumenRoughSpecularRadianceValid(directionalRadiance))
        validity |= kLumenRoughSpecularValidityRadianceAvailable;
    if (lumenRoughSpecularSurfaceValid(surfaceNormalRoughness) &&
        lumenRoughSpecularFinite4(viewDirection) &&
        length(viewDirection.xyz) > kLumenRoughSpecularEpsilon)
        validity |= kLumenRoughSpecularValiditySurfaceAvailable;

    if ((validity & (kLumenRoughSpecularValidityDirectionAvailable |
                     kLumenRoughSpecularValidityRadianceAvailable |
                     kLumenRoughSpecularValiditySurfaceAvailable)) !=
        (kLumenRoughSpecularValidityDirectionAvailable |
         kLumenRoughSpecularValidityRadianceAvailable |
         kLumenRoughSpecularValiditySurfaceAvailable))
    {
        lumenRoughSpecularWriteZero(pixel, validity | kLumenRoughSpecularValidityDisabled);
        return;
    }

    const float3 normal = lumenRoughSpecularSafeNormalize(surfaceNormalRoughness.xyz);
    const float3 view = lumenRoughSpecularSafeNormalize(viewDirection.xyz);
    const float3 reflection = lumenRoughSpecularSafeNormalize(reflectionDirection.xyz);
    const float3 idealReflection = lumenRoughSpecularSafeNormalize(reflect(-view, normal));
    const float roughness = saturate(surfaceNormalRoughness.a);
    const float alignment = saturate(dot(idealReflection, reflection));
    // A broad lobe at roughness=1 and a tight lobe at roughness=0.  This is a
    // directional diagnostic weight, not a replacement for a sampled BRDF.
    const float exponent = lerp(128.f, 2.f, roughness * roughness);
    const float lobe = pow(max(alignment, kLumenRoughSpecularEpsilon), exponent);
    const float noV = saturate(dot(normal, view));
    const float f0 = saturate(gSpecularF0 > 0.f ? gSpecularF0 : 0.04f);
    const float fresnel = f0 + (1.f - f0) * pow(1.f - noV, 5.f);
    // Surface alpha is roughness, not confidence; roughness=0 must remain a
    // valid smooth-specular sample.  Directional source confidences are the
    // only confidence factors in this diagnostic payload.
    const float confidence = saturate(directionalRadiance.a * reflectionDirection.a * lobe);
    float3 outputRadiance = max(directionalRadiance.rgb, float3(0.f)) *
        (lobe * fresnel * saturate(dot(normal, reflection)));
    outputRadiance = min(outputRadiance, float3(kLumenRoughSpecularMaxRadiance));

    if (!lumenRoughSpecularFinite3(outputRadiance) || !lumenRoughSpecularFinite(confidence))
    {
        lumenRoughSpecularWriteZero(pixel, validity | kLumenRoughSpecularValidityDisabled);
        return;
    }

    validity |= kLumenRoughSpecularValidityOutputFinite;
#if is_valid_gRoughSpecularIndirect
    gRoughSpecularIndirect[pixel] = float4(outputRadiance, confidence);
#endif
#if is_valid_gRoughSpecularValidity
    gRoughSpecularValidity[pixel] = validity;
#endif
#endif
}
