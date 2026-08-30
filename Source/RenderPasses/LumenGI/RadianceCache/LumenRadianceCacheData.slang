/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/

#ifndef __LUMEN_RADIANCE_CACHE_DATA_SLANG__
#define __LUMEN_RADIANCE_CACHE_DATA_SLANG__

/**
 * C10 Wave 1 GPU contract.
 *
 * The CPU clipmap owns allocation and generation changes.  This module is the
 * intentionally small shader-side view of that state:
 *
 *   gProbeMeta[slot] = uint4(worldPositionIndex, generation, readyFrame, valid)
 *   gProbeWorldPos[index] = xyz probe position (w is reserved)
 *   gProbeRadianceHitDist[slot] = rgb incident radiance, a hit distance
 *   gIndirection[cell] = slot (0 means invalid)
 *
 * `generation` and `readyFrame` are checked before a slot is consumed.  Thus a
 * slot recycled by the CPU cannot leak its previous contents into a new frame.
 * The interpolation entry reconstructs a world position from finite GBuffer
 * depth, camera basis, and the CPU-owned clipmap center/extent. Background
 * miss-depth pixels use a bounded camera-ray/clipmap intersection so sky probes
 * remain queryable without borrowing a neighboring surface. The shader never
 * treats gViewW alpha as validity.
 */

cbuffer LumenRadianceCacheCB
{
    uint2 gFrameDim;
    uint2 gSourceDim;
    uint  gCurrentFrame;
    uint  gGeneration;
    float4x4 gViewProj;
    float4 gCacheCameraCenterBaseExtent;
    float4 gCameraPosW;
    float4 gCameraRightW;
    float4 gCameraUpW;
    float4 gCameraForwardW;
    float4 gCameraFocalPrincipal;
};

static const uint kLumenRadianceCacheValidityHit = 1u << 0;
static const uint kLumenRadianceCacheValiditySky = 1u << 1;
static const uint kLumenRadianceCacheValidityRadiance = 1u << 2;
static const uint kLumenRadianceCacheValidityProducer = 1u << 3;

Texture2D<float4> gViewW;
Texture2D<float4> gLinearZ;
Texture2D<float4> gDiffuseRadianceHitDist;

StructuredBuffer<uint4>  gProbeMeta;
StructuredBuffer<float4> gProbeWorldPos;
StructuredBuffer<float4> gProbeScreenPos;
StructuredBuffer<float4> gProbeRadianceHitDist;
StructuredBuffer<float4> gProbeRadianceHitDistPrev;
StructuredBuffer<uint>    gProbeValidity;
Texture3D<uint>           gIndirection;

RWTexture2D<float4> gRadianceCacheOutput;
RWTexture2D<float4> gRadianceCacheHitDist;
RWTexture2D<uint>    gRadianceCacheValidityOutput;
RWStructuredBuffer<float4> gProbeRadianceHitDistOut;
RWStructuredBuffer<uint>    gProbeValidityOut;
RWStructuredBuffer<uint>    gRadianceCacheQueryCounters;
RWStructuredBuffer<uint>    gRadianceCacheLevelCounters;

static const uint kLumenRadianceCacheQueryAttempts = 0u;
static const uint kLumenRadianceCacheQueryHits = 1u;
static const uint kLumenRadianceCacheQueryMisses = 2u;
static const uint kLumenRadianceCacheLevelStride = 8u;
static const uint kLumenRadianceCacheLevelAttempts = 0u;
static const uint kLumenRadianceCacheLevelHits = 1u;
static const uint kLumenRadianceCacheLevelMisses = 2u;
static const uint kLumenRadianceCacheLevelSamples = 3u;
static const uint kLumenRadianceCacheLevelValidHitDistance = 4u;
static const uint kLumenRadianceCacheLevelFallback = 5u;

static const uint kLumenRadianceCacheInvalidSlot = 0u;
static const float kLumenRadianceCacheEpsilon = 1e-6f;
static const float kLumenRadianceCacheMissHitDistance = 65504.f;

bool lumenRadianceCacheFinite(const float value)
{
    return !isnan(value) && !isinf(value);
}

bool lumenRadianceCacheFinite2(const float2 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

bool lumenRadianceCacheFinite3(const float3 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

bool lumenRadianceCacheFinite4(const float4 value)
{
    return all(!isnan(value)) && all(!isinf(value));
}

bool lumenRadianceCacheNonNegative3(const float3 value)
{
    return all(value >= float3(0.f));
}

/** The radiance payload is valid independently of cache confidence. */
bool lumenRadianceCachePayloadValid(const float4 payload)
{
    return lumenRadianceCacheFinite4(payload) &&
        lumenRadianceCacheNonNegative3(payload.rgb) &&
        payload.a >= 0.f;
}

/** Current HWRT source validity. Misses use the exact fp16 sentinel and must
 * never seed a cache payload; legal black RGB at a finite hit distance remains
 * valid. */
bool lumenRadianceCacheSourcePayloadValid(const float4 payload)
{
    return lumenRadianceCachePayloadValid(payload) &&
        payload.a > 0.f && payload.a < kLumenRadianceCacheMissHitDistance;
}

// Producer payloads are stricter than generic finite payloads: the fp16 miss
// sentinel must never become a resident interpolation sample.
bool lumenRadianceCacheHitPayloadValid(const float4 payload)
{
    return lumenRadianceCacheSourcePayloadValid(payload);
}

/**
 * Validate the complete producer record before it is published.  Hit/sky
 * provenance is deliberately independent from RGB: a legal black hit or a
 * black sky sample still carries a valid record.  The two provenance bits are
 * mutually exclusive and the hit-distance encoding is the discriminator, so
 * a malformed record cannot be mistaken for a resident interpolation sample.
 */
bool lumenRadianceCacheProducerRecordValid(const float4 payload, const uint validity)
{
    const bool isHit = (validity & kLumenRadianceCacheValidityHit) != 0u;
    const bool isSky = (validity & kLumenRadianceCacheValiditySky) != 0u;
    const bool hasRadiance = (validity & kLumenRadianceCacheValidityRadiance) != 0u;
    const bool hasProducer = (validity & kLumenRadianceCacheValidityProducer) != 0u;
    const bool payloadFinite = lumenRadianceCachePayloadValid(payload);
    const bool hitRecord = isHit && !isSky && lumenRadianceCacheHitPayloadValid(payload);
    const bool skyRecord = isSky && !isHit && payloadFinite &&
        payload.a >= kLumenRadianceCacheMissHitDistance;
    return hasProducer && hasRadiance && (hitRecord || skyRecord);
}

/**
 * Validate the CPU-owned metadata epoch.  `readyFrame <= currentFrame` is an
 * explicit next-frame visibility fence; a probe committed during this frame is
 * not visible until the following dispatch.
 */
bool lumenRadianceCacheMetaValid(const uint4 meta)
{
    return meta.w != 0u && meta.y == gGeneration && meta.z <= gCurrentFrame;
}

/** Producer-side validity.  A probe may be filled before its next-frame
 * visibility fence; interpolation alone enforces readyFrame <= currentFrame. */
bool lumenRadianceCacheMetaProducerValid(const uint4 meta)
{
    return meta.w != 0u && meta.y == gGeneration;
}

float3 lumenRadianceCacheClampCoordinate(const float3 coordinate)
{
    return saturate(coordinate);
}

#endif // __LUMEN_RADIANCE_CACHE_DATA_SLANG__
