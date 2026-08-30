/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/

#ifndef LUMEN_TRANSMISSION_DATA_SLANG
#define LUMEN_TRANSMISSION_DATA_SLANG

/**
 * E1 transmission/glass diagnostic contract.
 *
 * This payload is intentionally independent of diffuseGI, resolvedDiffuseGI,
 * finalColor and RTXDI direct-lighting.  It is a reference-only scaffold until
 * a host owns refraction rays, medium transitions, and the production resolve
 * lifetime.  The default specialization is disabled and writes a deterministic
 * zero payload.  A missing directionality/validity input is also disabled.
 *
 * Inputs (all independent textures):
 *   gTransmissionRadiance   incident/transmitted RGB, alpha = source confidence
 *   gTransmissionSurface    xyz = shading normal, w = roughness
 *   gTransmissionIOR        x = eta ratio (IOR), y = path thickness, alpha = confidence
 *   gTransmissionFresnel    x = precomputed Fresnel term [0,1], alpha = confidence
 *   gTransmissionRefraction xyz = refracted direction, alpha = direction confidence
 *   gTransmissionInputValidity = producer validity bits
 *
 * Output RGB is a bounded, reference-only transmitted radiance estimate and A
 * is confidence.  It must not be presented as production glass GI.
 */

#ifndef LUMEN_GI_TRANSMISSION_ENABLED
#define LUMEN_GI_TRANSMISSION_ENABLED 0
#endif

// Keep the unsupported state explicit even if a diagnostic build enables the
// pass to inspect its reference payload.
#ifndef LUMEN_GI_TRANSMISSION_REFERENCE_ONLY
#define LUMEN_GI_TRANSMISSION_REFERENCE_ONLY 1
#endif

#ifndef is_valid_gTransmissionRadiance
#define is_valid_gTransmissionRadiance 0
#endif
#ifndef is_valid_gTransmissionSurface
#define is_valid_gTransmissionSurface 0
#endif
#ifndef is_valid_gTransmissionIOR
#define is_valid_gTransmissionIOR 0
#endif
#ifndef is_valid_gTransmissionFresnel
#define is_valid_gTransmissionFresnel 0
#endif
#ifndef is_valid_gTransmissionRefraction
#define is_valid_gTransmissionRefraction 0
#endif
#ifndef is_valid_gTransmissionInputValidity
#define is_valid_gTransmissionInputValidity 0
#endif

cbuffer LumenTransmissionCB
{
    uint2 gFrameDim;
    uint  gFrameIndex;
    float gMaxTransmissionRadiance;
};

Texture2D<float4> gTransmissionRadiance;
Texture2D<float4> gTransmissionSurface;
Texture2D<float4> gTransmissionIOR;
Texture2D<float4> gTransmissionFresnel;
Texture2D<float4> gTransmissionRefraction;
Texture2D<uint>   gTransmissionInputValidity;

RWTexture2D<float4> gTransmissionIndirect;
RWTexture2D<uint>   gTransmissionValidityOutput;

// Input validity bits are producer-owned; the output bits below describe this
// reference pass and are never a final-color contract.
static const uint kLumenTransmissionInputRadianceValid   = 1u << 0;
static const uint kLumenTransmissionInputSurfaceValid    = 1u << 1;
static const uint kLumenTransmissionInputIORValid        = 1u << 2;
static const uint kLumenTransmissionInputFresnelValid    = 1u << 3;
static const uint kLumenTransmissionInputRefractionValid = 1u << 4;

static const uint kLumenTransmissionValidityDisabled       = 1u << 0;
static const uint kLumenTransmissionValidityReferenceOnly  = 1u << 1;
static const uint kLumenTransmissionValidityRadiance       = 1u << 2;
static const uint kLumenTransmissionValidityIOR            = 1u << 3;
static const uint kLumenTransmissionValidityFresnel        = 1u << 4;
static const uint kLumenTransmissionValidityRefraction     = 1u << 5;
static const uint kLumenTransmissionValidityThickness      = 1u << 6;
static const uint kLumenTransmissionValidityOutputFinite   = 1u << 7;
static const uint kLumenTransmissionValidityTotalInternal  = 1u << 8;
static const uint kLumenTransmissionValiditySurface        = 1u << 9;

static const float kLumenTransmissionEpsilon = 1e-6f;
static const float kLumenTransmissionDefaultMaxRadiance = 10.f;

bool lumenTransmissionFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool lumenTransmissionFinite3(float3 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

bool lumenTransmissionFinite4(float4 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

bool lumenTransmissionDirectionValid(float4 value)
{
    return lumenTransmissionFinite4(value) &&
        length(value.xyz) > kLumenTransmissionEpsilon &&
        value.a > kLumenTransmissionEpsilon;
}

#endif // LUMEN_TRANSMISSION_DATA_SLANG
