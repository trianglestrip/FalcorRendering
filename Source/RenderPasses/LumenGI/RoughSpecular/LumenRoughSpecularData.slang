/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/

#ifndef LUMEN_ROUGH_SPECULAR_DATA_SLANG
#define LUMEN_ROUGH_SPECULAR_DATA_SLANG

/**
 * E1 roughSpecularIndirect diagnostic contract.
 *
 * This payload is deliberately separate from the diffuseGI chain and from
 * RTXDI direct-lighting.  `gDirectionalRadiance` is an incident radiance
 * sample tagged with confidence; `gReflectionDirection` supplies the sampled
 * direction and its directionality confidence.  A host that cannot provide a
 * directionality signal must leave the pass disabled/unbound: the output is a
 * deterministic zero with the Disabled/DirectionUnavailable validity bits.
 *
 * RGB = reflected indirect specular radiance (linear, non-negative).
 * A   = confidence in [0,1], not a colour-channel validity sentinel.
 *
 * The pass is diagnostic-only until a host owns the directional input,
 * lifetime, and final resolve endpoint.  This module never reads diffuseGI.
 */

#ifndef LUMEN_GI_ROUGH_SPECULAR_ENABLED
#define LUMEN_GI_ROUGH_SPECULAR_ENABLED 0
#endif

#ifndef is_valid_gDirectionalRadiance
#define is_valid_gDirectionalRadiance 0
#endif
#ifndef is_valid_gReflectionDirection
#define is_valid_gReflectionDirection 0
#endif
#ifndef is_valid_gSurfaceNormalRoughness
#define is_valid_gSurfaceNormalRoughness 0
#endif
#ifndef is_valid_gViewDirection
#define is_valid_gViewDirection 0
#endif

cbuffer LumenRoughSpecularCB
{
    uint2 gFrameDim;
    uint  gFrameIndex;
    float gSpecularF0;
};

// Independent directional inputs.  These are not aliases for diffuseGI.
Texture2D<float4> gDirectionalRadiance;
Texture2D<float4> gReflectionDirection;
Texture2D<float4> gSurfaceNormalRoughness;
Texture2D<float4> gViewDirection;

RWTexture2D<float4> gRoughSpecularIndirect;
RWTexture2D<uint>   gRoughSpecularValidity;

static const uint kLumenRoughSpecularValidityDisabled            = 1u << 0;
static const uint kLumenRoughSpecularValidityDirectionAvailable = 1u << 1;
static const uint kLumenRoughSpecularValidityRadianceAvailable  = 1u << 2;
static const uint kLumenRoughSpecularValidityOutputFinite       = 1u << 3;
static const uint kLumenRoughSpecularValiditySurfaceAvailable   = 1u << 4;

static const float kLumenRoughSpecularMaxRadiance = 10.f;
static const float kLumenRoughSpecularEpsilon = 1e-6f;

bool lumenRoughSpecularFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool lumenRoughSpecularFinite3(float3 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

bool lumenRoughSpecularFinite4(float4 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

bool lumenRoughSpecularRadianceValid(float4 payload)
{
    // Black radiance is legal; alpha carries source confidence.
    return lumenRoughSpecularFinite4(payload) &&
        all(payload.rgb >= float3(0.f)) &&
        payload.a > kLumenRoughSpecularEpsilon;
}

bool lumenRoughSpecularDirectionValid(float4 payload)
{
    const float directionLength = length(payload.xyz);
    return lumenRoughSpecularFinite4(payload) &&
        directionLength > kLumenRoughSpecularEpsilon &&
        payload.a > kLumenRoughSpecularEpsilon;
}

bool lumenRoughSpecularSurfaceValid(float4 payload)
{
    const float normalLength = length(payload.xyz);
    return lumenRoughSpecularFinite4(payload) &&
        normalLength > kLumenRoughSpecularEpsilon &&
        payload.a >= 0.f && payload.a <= 1.f;
}

#endif // LUMEN_ROUGH_SPECULAR_DATA_SLANG
