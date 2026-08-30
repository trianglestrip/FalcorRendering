/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/

/**
 * E1 transmission/glass reference-only diagnostic producer.
 *
 * This shader is not connected to finalColor or any production LumenGI
 * resolve.  It evaluates an intentionally small Beer-Lambert/Fresnel proxy so
 * that direction, IOR, thickness and validity contracts can be tested before a
 * real medium-aware refraction producer exists.  Never alias diffuseGI or
 * RTXDI direct light into this pass.
 */

#include "LumenTransmissionData.slang"

#ifndef is_valid_gTransmissionIndirect
#define is_valid_gTransmissionIndirect 0
#endif
#ifndef is_valid_gTransmissionValidityOutput
#define is_valid_gTransmissionValidityOutput 0
#endif

float3 lumenTransmissionSafeNormalize(float3 value)
{
    const float len2 = dot(value, value);
    return len2 > kLumenTransmissionEpsilon ? value * rsqrt(len2) : float3(0.f);
}

void lumenTransmissionWriteZero(uint2 pixel, uint validity)
{
#if is_valid_gTransmissionIndirect
    gTransmissionIndirect[pixel] = float4(0.f);
#endif
#if is_valid_gTransmissionValidityOutput
    gTransmissionValidityOutput[pixel] = validity;
#endif
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= gFrameDim))
        return;

#if !LUMEN_GI_TRANSMISSION_ENABLED
    lumenTransmissionWriteZero(pixel, kLumenTransmissionValidityDisabled);
    return;
#elif !(is_valid_gTransmissionRadiance && is_valid_gTransmissionSurface &&
        is_valid_gTransmissionIOR && is_valid_gTransmissionFresnel &&
        is_valid_gTransmissionRefraction && is_valid_gTransmissionInputValidity)
    // No directionality/medium validity means no approximation is allowed.
    lumenTransmissionWriteZero(pixel, kLumenTransmissionValidityDisabled);
    return;
#else
    const float4 source = gTransmissionRadiance[pixel];
    const float4 surface = gTransmissionSurface[pixel];
    const float4 iorThickness = gTransmissionIOR[pixel];
    const float4 fresnelInput = gTransmissionFresnel[pixel];
    const float4 refraction = gTransmissionRefraction[pixel];
    const uint inputValidity = gTransmissionInputValidity[pixel];

    uint validity = kLumenTransmissionValidityReferenceOnly;
    const bool sourceValid = lumenTransmissionFinite4(source) &&
        all(source.rgb >= float3(0.f)) && source.a > kLumenTransmissionEpsilon &&
        (inputValidity & kLumenTransmissionInputRadianceValid) != 0u;
    const bool surfaceValid = lumenTransmissionFinite4(surface) &&
        length(surface.xyz) > kLumenTransmissionEpsilon &&
        surface.a >= 0.f && surface.a <= 1.f &&
        (inputValidity & kLumenTransmissionInputSurfaceValid) != 0u;
    // IOR is kept as a ratio to support air->glass and glass->air transitions.
    const bool iorValid = lumenTransmissionFinite4(iorThickness) &&
        iorThickness.x > 0.1f && iorThickness.x < 4.f &&
        iorThickness.a > kLumenTransmissionEpsilon &&
        (inputValidity & kLumenTransmissionInputIORValid) != 0u;
    const bool fresnelValid = lumenTransmissionFinite4(fresnelInput) &&
        fresnelInput.x >= 0.f && fresnelInput.x <= 1.f &&
        fresnelInput.a > kLumenTransmissionEpsilon &&
        (inputValidity & kLumenTransmissionInputFresnelValid) != 0u;
    const bool refractionValid = lumenTransmissionDirectionValid(refraction) &&
        (inputValidity & kLumenTransmissionInputRefractionValid) != 0u;

    if (sourceValid) validity |= kLumenTransmissionValidityRadiance;
    if (surfaceValid) validity |= kLumenTransmissionValiditySurface;
    if (iorValid) validity |= kLumenTransmissionValidityIOR;
    if (fresnelValid) validity |= kLumenTransmissionValidityFresnel;
    if (refractionValid) validity |= kLumenTransmissionValidityRefraction;
    if (iorValid && iorThickness.y >= 0.f) validity |= kLumenTransmissionValidityThickness;

    if (!sourceValid || !surfaceValid || !iorValid || !fresnelValid || !refractionValid ||
        iorThickness.y < 0.f)
    {
        lumenTransmissionWriteZero(pixel, validity | kLumenTransmissionValidityDisabled);
        return;
    }

    const float3 normal = lumenTransmissionSafeNormalize(surface.xyz);
    const float3 transmitted = lumenTransmissionSafeNormalize(refraction.xyz);
    const float normalCosine = dot(normal, transmitted);
    // The refraction producer must classify total internal reflection. A
    // back-facing/invalid direction is rejected rather than relabeled as GI.
    if (normalCosine <= kLumenTransmissionEpsilon)
    {
        lumenTransmissionWriteZero(pixel, validity | kLumenTransmissionValidityTotalInternal |
            kLumenTransmissionValidityDisabled);
        return;
    }

    const float thickness = max(iorThickness.y, 0.f);
    const float attenuation = exp(-thickness * 0.1f); // reference-only density
    const float fresnel = saturate(fresnelInput.x);
    const float maxRadiance = gMaxTransmissionRadiance > 0.f
        ? gMaxTransmissionRadiance : kLumenTransmissionDefaultMaxRadiance;
    float3 outputRadiance = max(source.rgb, float3(0.f)) *
        ((1.f - fresnel) * attenuation * saturate(normalCosine));
    outputRadiance = min(outputRadiance, float3(maxRadiance));
    const float confidence = saturate(source.a * refraction.a * iorThickness.a * fresnelInput.a);

    if (!lumenTransmissionFinite3(outputRadiance) || !lumenTransmissionFinite(confidence))
    {
        lumenTransmissionWriteZero(pixel, validity | kLumenTransmissionValidityDisabled);
        return;
    }

    validity |= kLumenTransmissionValidityOutputFinite;
#if is_valid_gTransmissionIndirect
    gTransmissionIndirect[pixel] = float4(outputRadiance, confidence);
#endif
#if is_valid_gTransmissionValidityOutput
    gTransmissionValidityOutput[pixel] = validity;
#endif
#endif
}
