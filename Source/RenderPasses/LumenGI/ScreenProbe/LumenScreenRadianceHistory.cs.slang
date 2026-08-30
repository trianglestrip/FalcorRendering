/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/

// UE-style screen-radiance history for the ScreenProbe producer.  This pass is
// deliberately separate from the probe irradiance history: the input/output
// RGB is the per-pixel unmodulated HWRT radiance (not incident E), while A is
// the secondary hit distance.  The previous slot is reprojected with the
// same prevUV = currentUV + motion convention used by LumenTemporalFilter.

import Utils.Math.MathHelpers;

static const float kScreenRadianceHistoryMiss = 65504.f;
static const float kScreenRadianceHistoryDepthRelative = 0.05f;
static const float kScreenRadianceHistoryDepthAbsolute = 0.05f;
// UE Lumen keeps a bounded accumulated-frame count instead of using one fixed
// EMA weight forever.  The lower bound prevents a static probe from becoming
// unresponsive, while the age-dependent weight avoids over-trusting the first
// reprojection after a reset/camera transition.
static const float kScreenRadianceHistoryMinNewSampleWeight = 0.10f;
static const uint kScreenRadianceHistoryAccumulationFrames = 10u;
static const float kScreenRadianceHistoryMax = 10.f;
static const uint kScreenRadianceHistoryMaxAge = 255u;

#ifndef is_valid_gMotionVector
#define is_valid_gMotionVector 0
#endif
#ifndef is_valid_gNormalRoughnessMaterialID
#define is_valid_gNormalRoughnessMaterialID 0
#endif

cbuffer LumenScreenRadianceHistoryCB
{
    uint2 gFrameDim;
    uint gHistoryValid;
    uint gHistoryGeneration;
    uint gLightingGeneration;
    uint gResetReason;
    uint gResetActive;
};

Texture2D<float4> gCurrentRadiance;
Texture2D<float4> gPreviousRadiance;
Texture2D<float2> gCurrentLinearZ;
Texture2D<float2> gPreviousLinearZ;
#if is_valid_gMotionVector
Texture2D<float2> gMotionVector;
#endif
#if is_valid_gNormalRoughnessMaterialID
Texture2D<float4> gCurrentGuide;
#endif
Texture2D<float4> gPreviousGuide;
Texture2D<float2> gPreviousMoments;
Texture2D<uint> gPreviousLightingGeneration;
Texture2D<uint> gPreviousAge;
Texture2D<uint> gPreviousValidity;

// Keep this standalone pass ABI-compatible with LumenScreenProbeData.slang without
// importing that file (its probe CB/resources intentionally use several names that
// collide with this pass).  The first sixteen uints are the existing trace/cache
// counters; the history fields retain their fixed offsets at +64..+92 and the
// cache coverage diagnostics are appended at +96..+108.
struct LumenScreenProbeCounters
{
    uint screenHits;
    uint fallbackAttempts;
    uint fallbackHits;
    uint fallbackMisses;
    uint fallbackUnavailable;
    uint inactiveProbes;
    uint budgetSkipped;
    uint directionsTraced;
    uint gdfHits;
    uint gdfMisses;
    uint cacheLookupHits;
    uint cacheLookupAttempts;
    uint cachePageRejects;
    uint cacheCoverageRejects;
    uint cacheMetadataRejects;
    uint cacheVisibilityRejects;
    uint historyAccepted;
    uint historyRejectDepth;
    uint historyRejectGuide;
    uint historyRejectMotion;
    uint historyRejectLighting;
    uint historyRejectCurrentInvalid;
    uint historyRejectPreviousInvalid;
    uint historyReset;
    uint cacheDepthRejects;
    uint cacheAxisRejects;
    uint cacheFacingRejects;
    uint cacheOwnerValid;
};

RWStructuredBuffer<LumenScreenProbeCounters> gProbeCounters;

RWTexture2D<float4> gOutputRadiance;
RWTexture2D<float2> gOutputLinearZ;
RWTexture2D<float4> gOutputGuide;
RWTexture2D<float2> gOutputMoments;
RWTexture2D<uint> gOutputLightingGeneration;
RWTexture2D<uint> gOutputAge;
RWTexture2D<uint> gOutputValidity;

bool finite4(const float4 v)
{
    return all(!isnan(v)) && all(!isinf(v));
}

bool finite2(const float2 v)
{
    return all(!isnan(v)) && all(!isinf(v));
}

float luminance(const float3 rgb)
{
    return max(dot(max(rgb, float3(0.f)), float3(0.2126f, 0.7152f, 0.0722f)), 0.f);
}

// Validity is intentionally independent of temporal residency.  RGB is raw Li, so
// black is a legal radiance sample (for example a fully occluded ray); only
// non-finite values and the explicit miss-distance sentinel invalidate it.
// Keep the alpha contract intact: A remains the secondary hit distance.
bool validRadiance(const float4 v)
{
    return finite4(v) && v.w > 0.f && v.w < kScreenRadianceHistoryMiss;
}

float3 clampRadiance(const float3 radiance)
{
    // Negative HDR values are not meaningful for raw Li.  Clamp only after
    // validity is established; a black (zero) sample must remain valid.
    return min(max(radiance, float3(0.f)), float3(kScreenRadianceHistoryMax));
}

bool validDepth(const float z)
{
    return !isnan(z) && !isinf(z) && z > 0.f;
}

bool validGuide(const float4 guide)
{
    return finite4(guide) && all(guide.xy >= 0.f) && all(guide.xy <= 1.f) && guide.w >= 0.f;
}

bool guideAgrees(const float4 currentGuide, const float4 previousGuide)
{
    if (!validGuide(currentGuide) || !validGuide(previousGuide))
        return false;
    const float3 currentNormal = normalize(oct_to_ndir_unorm(currentGuide.xy));
    const float3 previousNormal = normalize(oct_to_ndir_unorm(previousGuide.xy));
    const uint currentMaterial = uint(round(saturate(currentGuide.w) * 3.f));
    const uint previousMaterial = uint(round(saturate(previousGuide.w) * 3.f));
    return dot(currentNormal, previousNormal) >= 0.70710678f && currentMaterial == previousMaterial;
}

uint2 clampPixel(const int2 p)
{
    return uint2(clamp(p, int2(0), int2(gFrameDim) - 1));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= gFrameDim))
        return;

    const float4 current = gCurrentRadiance[pixel];
    const float currentDepth = gCurrentLinearZ[pixel].x;
    const bool currentValid = validRadiance(current);
    float4 currentGuide = float4(0.f);
#if is_valid_gNormalRoughnessMaterialID
    currentGuide = gCurrentGuide[pixel];
#endif

    // Falcor's motion vector follows prevUV = currentUV + mvec.  A missing
    // motion resource is represented by zero in the host and therefore keeps
    // the same-pixel history path deterministic for static scenes.
    float2 motion = float2(0.f);
#if is_valid_gMotionVector
    motion = gMotionVector[pixel];
#endif
    const float2 prevUV = (float2(pixel) + 0.5f) / float2(gFrameDim) + motion;
    const int2 prevPixelI = int2(floor(prevUV * float2(gFrameDim)));
    const bool prevInBounds = all(prevPixelI >= int2(0)) && all(prevPixelI < int2(gFrameDim));
    const uint2 prevPixel = clampPixel(prevPixelI);
    const float4 previous = gPreviousRadiance[prevPixel];
    const float previousDepth = gPreviousLinearZ[prevPixel].x;
    const uint previousLightingGeneration = gPreviousLightingGeneration[prevPixel];
    const uint previousAge = gPreviousAge[prevPixel];
    const bool previousValidity = gPreviousValidity[prevPixel] != 0u;
    const bool lightingGenerationAgrees = previousLightingGeneration == gLightingGeneration;
    const bool previousRadianceValid = validRadiance(previous);
    const bool previousDepthValid = validDepth(previousDepth);
    const bool currentDepthValid = validDepth(currentDepth);
    const bool previousValid = gHistoryValid != 0u && prevInBounds && previousValidity && lightingGenerationAgrees &&
        previousRadianceValid && previousDepthValid;
    const float4 previousGuide = gPreviousGuide[prevPixel];
    const float2 previousMoments = gPreviousMoments[prevPixel];
    const bool previousMomentsValid = previousValid && finite2(previousMoments) &&
        previousMoments.x >= 0.f && previousMoments.y >= previousMoments.x * previousMoments.x;
    const bool guideValid = is_valid_gNormalRoughnessMaterialID != 0 && guideAgrees(currentGuide, previousGuide);

    bool depthMatches = false;
    if (previousValid && currentDepthValid)
    {
        const float absDelta = abs(previousDepth - currentDepth);
        const float relDelta = absDelta / max(currentDepth, 1e-3f);
        depthMatches = absDelta <= kScreenRadianceHistoryDepthAbsolute || relDelta <= kScreenRadianceHistoryDepthRelative;
    }
    const bool depthAgree = depthMatches && guideValid;

    float4 output = float4(0.f);
    float2 outputMoments = float2(0.f);
    uint outputAge = 0u;
    if (currentValid)
    {
        float3 currentRGB = clampRadiance(current.rgb);
        const float currentLuminance = luminance(currentRGB);
        outputMoments = float2(currentLuminance, currentLuminance * currentLuminance);
        if (depthAgree)
        {
            const float3 previousRGB = clampRadiance(previous.rgb);
            const uint accumulatedFrames = min(previousAge + 1u, kScreenRadianceHistoryAccumulationFrames);
            const float ageWeight = 1.f / max(float(accumulatedFrames), 1.f);
            const float newSampleWeight = max(kScreenRadianceHistoryMinNewSampleWeight, ageWeight);
            currentRGB = lerp(previousRGB, currentRGB, newSampleWeight);
            if (previousMomentsValid)
            {
                const float2 currentSampleMoments = float2(currentLuminance, currentLuminance * currentLuminance);
                outputMoments = lerp(previousMoments, currentSampleMoments, newSampleWeight);
            }
            outputAge = min(previousAge + 1u, kScreenRadianceHistoryMaxAge);
        }
        else
            outputAge = 1u;
        output = float4(currentRGB, current.w);
    }
    else if (depthAgree && validDepth(currentDepth))
    {
        // Preserve a valid previous sample across a transient current-frame
        // miss.  The probe trace still applies its own surface/depth gates.
        output = float4(clampRadiance(previous.rgb), previous.w);
        outputMoments = previousMomentsValid ? previousMoments : float2(0.f);
        outputAge = min(previousAge + 1u, kScreenRadianceHistoryMaxAge);
    }

    // Record one mutually-exclusive primary outcome per pixel.  The host sends
    // an edge-triggered reset bit; gResetReason itself is latched for UI/debug
    // and must not cause every later frame to be counted as a reset.
    const bool historyResetFrame = gResetActive != 0u || gHistoryValid == 0u;
    const bool historyUsed = depthAgree && (currentValid || currentDepthValid);
    if (historyResetFrame)
        InterlockedAdd(gProbeCounters[0].historyReset, 1u);
    else if (historyUsed)
        InterlockedAdd(gProbeCounters[0].historyAccepted, 1u);
    else if (!currentValid)
        InterlockedAdd(gProbeCounters[0].historyRejectCurrentInvalid, 1u);
    else if (!prevInBounds)
        InterlockedAdd(gProbeCounters[0].historyRejectMotion, 1u);
    else if (!previousValid)
    {
        if (!lightingGenerationAgrees)
            InterlockedAdd(gProbeCounters[0].historyRejectLighting, 1u);
        else
            InterlockedAdd(gProbeCounters[0].historyRejectPreviousInvalid, 1u);
    }
    else if (!currentDepthValid)
        InterlockedAdd(gProbeCounters[0].historyRejectCurrentInvalid, 1u);
    else if (!guideValid)
        InterlockedAdd(gProbeCounters[0].historyRejectGuide, 1u);
    else if (!depthMatches)
        InterlockedAdd(gProbeCounters[0].historyRejectDepth, 1u);

    gOutputRadiance[pixel] = output;
    gOutputLinearZ[pixel] = float2(validDepth(currentDepth) ? currentDepth : 0.f, 0.f);
    gOutputGuide[pixel] = currentGuide;
    gOutputMoments[pixel] = outputMoments;
    gOutputLightingGeneration[pixel] = validRadiance(output) ? gLightingGeneration : 0u;
    gOutputAge[pixel] = validRadiance(output) ? outputAge : 0u;
    gOutputValidity[pixel] = validRadiance(output) ? 1u : 0u;
}
