/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/
#pragma once

/** S4.2 screen probe grid host component (S4-A2, Agent Z1). Header-only pure
 *  CPU: grid math, the frozen direction-sequence reference, the per-frame
 *  update budget, and the read-back stats. No GPU resources live here -- those
 *  are owned by LumenGI (mScreenProbes in LumenGI.h).
 *
 *  The shader side is LumenScreenProbeTrace.cs.slang; the frozen GPU contract
 *  is documented in LumenScreenProbeData.slang and mirrored here where the CPU
 *  must match byte-for-byte (LumenScreenProbeMeta, the CB, the seed).
 */

#include "Falcor.h"

using namespace Falcor;

namespace LumenScreenProbe
{
///< Frozen constants (mirrors of LumenScreenProbeData.slang).
static constexpr uint32_t kTileSize = 8u;                           ///< Texels per probe tile (fixed S4.2 grid).
static constexpr uint32_t kMaxDirectionsPerProbe = 32u;             ///< Fixed hit-record stride.
static constexpr uint32_t kDefaultDirectionsPerProbe = 16u;
static constexpr uint32_t kMaxMarchSteps = 512u;                    ///< Screen-march texel-step budget.
static constexpr float kMaxSurfaceDepth = 100.f;                    ///< Meters; beyond = empty space (far plane).
static constexpr float kMinThickness = 0.001f;                      ///< Meters.
static constexpr float kThicknessScale = 2.0f;
static constexpr float kStepEpsilon = 0.5f;                         ///< Texels.
static constexpr float kDepthChangeThreshold = 0.02f;               ///< Meters; dirty detection.
static constexpr uint32_t kMaxMip = 12u;
static constexpr uint32_t kSeed = 0x51B8DC0Du;                      ///< Fixed base seed (task rule 5).
static constexpr uint32_t kCounterCount = 8u;

// Surface Cache lookup acceleration contract. The host builds a world-space
// uniform grid over currently owned cards; each cell stores up to 32 card IDs.
// An overflow bit keeps the correctness fallback explicit: the shader scans
// all cards for that cell instead of silently dropping candidates.
static constexpr uint32_t kCacheCardGridDim = 16u;
static constexpr uint32_t kCacheCardGridMaxCandidates = 32u;
static constexpr uint32_t kCacheCardGridCellStride = 1u + kCacheCardGridMaxCandidates;

///< Probe grid dimensions for a frame (ceil of frame/tile), probes per axis.
static uint2 probeGridDims(uint2 frameDim)
{
    return uint2(
        std::max<uint32_t>(1u, (frameDim.x + kTileSize - 1u) / kTileSize),
        std::max<uint32_t>(1u, (frameDim.y + kTileSize - 1u) / kTileSize)
    );
}

///< Total probe count for a frame.
static uint32_t probeCount(uint2 frameDim)
{
    const uint2 dims = probeGridDims(frameDim);
    return dims.x * dims.y;
}

///< Screen (texel) position of the probe in grid tile (tx, ty): tile center + 0.5.
static float2 probeScreenPos(uint2 gridPos)
{
    return float2(
        float(gridPos.x) * float(kTileSize) + 0.5f * float(kTileSize) + 0.5f,
        float(gridPos.y) * float(kTileSize) + 0.5f * float(kTileSize) + 0.5f
    );
}

///< Probe update interval: probes traced every N frames. 0 budget = all probes.
static uint32_t updateInterval(uint32_t probeCount, uint32_t maxProbesPerFrame)
{
    if (maxProbesPerFrame == 0u)
        return 1u;
    return std::max<uint32_t>(1u, (probeCount + maxProbesPerFrame - 1u) / maxProbesPerFrame);
}

///< Expected probes traced per frame under the round-robin interval budget.
static uint32_t expectedProbesPerFrame(uint32_t probeCount, uint32_t interval)
{
    return (probeCount + interval - 1u) / interval;
}

///< CPU mirror of the frozen LumenScreenProbeMeta (LumenScreenProbeData.slang), 64 B.
struct Meta
{
    float2 screenPos;        // +0
    uint32_t active;         // +8
    uint32_t lastUpdateFrame;// +12
    float3 worldPos;         // +16
    uint32_t materialID;     // +28
    float3 normalW;          // +32
    float depth;             // +44
    uint32_t age;            // +48
    uint32_t updateInterval; // +52
    uint32_t dirty;          // +56
    uint32_t flags;          // +60
};
static_assert(sizeof(Meta) == 64, "LumenScreenProbe::Meta is 64 bytes (16B-aligned, matches the shader)");

///< CPU mirror of the frozen LumenProbeHit (LumenScreenProbeData.slang), 32 B.
struct Hit
{
    float3 radiance;     // +0
    float hitDistance;   // +12
    uint32_t flags;      // +16
    float confidence;    // +20
    float2 hitUV;        // +24
};
static_assert(sizeof(Hit) == 32, "LumenScreenProbe::Hit is 32 bytes (16B-aligned, matches the shader)");

///< Hit-record flags (mirrors of the shader constants).
static constexpr uint32_t kHitFlagScreenHit = 1u << 0;
static constexpr uint32_t kHitFlagHWRTHit = 1u << 1;
static constexpr uint32_t kHitFlagHWRTMiss = 1u << 2;
static constexpr uint32_t kHitFlagFallbackUnavailable = 1u << 3;
static constexpr uint32_t kHitFlagRadianceReused = 1u << 4;
static constexpr uint32_t kHitFlagEnvironment = 1u << 5;
static constexpr uint32_t kHitFlagGDFHit = 1u << 6;

///< Counter layout (gProbeCounters[0]). The first 16 uints are the original
///< trace/cache counters; history counters retain their frozen offsets and
///< cache-coverage diagnostics are appended after them.
struct Counters
{
    uint32_t screenHits;
    uint32_t fallbackAttempts;
    uint32_t fallbackHits;
    uint32_t fallbackMisses;
    uint32_t fallbackUnavailable;
    uint32_t inactiveProbes;
    uint32_t budgetSkipped;
    uint32_t directionsTraced;
    uint32_t gdfHits;
    uint32_t gdfMisses;
    uint32_t cacheLookupHits;
    uint32_t cacheLookupAttempts;
    uint32_t cachePageRejects;
    uint32_t cacheCoverageRejects;
    uint32_t cacheMetadataRejects;
    uint32_t cacheVisibilityRejects;
    uint32_t historyAccepted;
    uint32_t historyRejectDepth;
    uint32_t historyRejectGuide;
    uint32_t historyRejectMotion;
    uint32_t historyRejectLighting;
    uint32_t historyRejectCurrentInvalid;
    uint32_t historyRejectPreviousInvalid;
    uint32_t historyReset;
    uint32_t cacheDepthRejects;
    uint32_t cacheAxisRejects;
    uint32_t cacheFacingRejects;
    uint32_t cacheOwnerValid;
};
static_assert(sizeof(Counters) == 112, "LumenScreenProbe::Counters is 112 bytes (matches the shader)");

///< Per-frame read-back stats for the UI and scriptable gates.
struct Stats
{
    uint32_t probeCount = 0;
    uint32_t directionsPerProbe = 0;
    uint32_t updateInterval = 1;
    uint32_t expectedProbesPerFrame = 0;
    uint32_t screenHits = 0;
    uint32_t fallbackAttempts = 0;
    uint32_t fallbackHits = 0;
    uint32_t fallbackMisses = 0;
    uint32_t fallbackUnavailable = 0;
    uint32_t inactiveProbes = 0;
    uint32_t budgetSkipped = 0;
    uint32_t directionsTraced = 0;
    uint32_t gdfHits = 0;
    uint32_t gdfMisses = 0;
    uint32_t cacheLookupHits = 0;
    uint32_t cacheLookupAttempts = 0;
    uint32_t cachePageRejects = 0;
    uint32_t cacheCoverageRejects = 0;
    uint32_t cacheMetadataRejects = 0;
    uint32_t cacheVisibilityRejects = 0;
    uint32_t historyAccepted = 0;
    uint32_t historyRejectDepth = 0;
    uint32_t historyRejectGuide = 0;
    uint32_t historyRejectMotion = 0;
    uint32_t historyRejectLighting = 0;
    uint32_t historyRejectCurrentInvalid = 0;
    uint32_t historyRejectPreviousInvalid = 0;
    uint32_t historyReset = 0;
    uint32_t cacheDepthRejects = 0;
    uint32_t cacheAxisRejects = 0;
    uint32_t cacheFacingRejects = 0;
    uint32_t cacheOwnerValid = 0;

    float screenHitRate() const
    {
        return directionsTraced > 0 ? float(screenHits) / float(directionsTraced) : 0.f;
    }

    float fallbackHitRate() const
    {
        return fallbackAttempts > 0 ? float(fallbackHits) / float(fallbackAttempts) : 0.f;
    }
};

///< Base-2 Van der Corput radical inverse (identical to FalcorMath.h / the shader).
static inline float radicalInverse(uint32_t i)
{
    i = (i & 0x55555555u) << 1 | (i & 0xAAAAAAAAu) >> 1;
    i = (i & 0x33333333u) << 2 | (i & 0xCCCCCCCCu) >> 2;
    i = (i & 0x0F0F0F0Fu) << 4 | (i & 0xF0F0F0F0u) >> 4;
    i = (i & 0x00FF00FFu) << 8 | (i & 0xFF00FF00u) >> 8;
    i = (i << 16) | (i >> 16);
    return float(i) * 2.3283064365386963e-10f;
}

///< Frozen direction-sequence reference (mirrors lumenProbeSampleDirection in the
///< shader): returns the rotated sequence slot j in [0, n) for (probe, dir, frame).
static inline uint32_t directionSlot(uint32_t probeIndex, uint32_t dirIndex, uint32_t frameIndex, uint32_t n, uint32_t seed)
{
    const uint32_t rot = (probeIndex * 2654435761u + frameIndex * 1140071481u + seed) % n;
    return (dirIndex + rot) % n;
}

///< Frozen direction-sample point u (mirrors the shader) for cross-validation.
static inline float2 directionSample2D(uint32_t probeIndex, uint32_t dirIndex, uint32_t frameIndex, uint32_t n, uint32_t seed)
{
    const uint32_t j = directionSlot(probeIndex, dirIndex, frameIndex, n, seed);
    // Per-probe, per-frame jitter. The frame term mirrors the shader's
    // Cranley-Patterson rotation so the direction union grows over time.
    uint32_t a = probeIndex ^ (frameIndex * 0x9e3779b9u);
    a = (a ^ 61u) ^ (a >> 16u);
    a = a * 9u + 0x85ebca6bu;
    a = a ^ (a >> 13u);
    a = a * 0xc2b2ae35u;
    a = a ^ (a >> 16u);
    uint32_t b = a ^ 0x9e3779b9u;
    b = (b ^ 61u) ^ (b >> 16u);
    b = b * 9u + 0x85ebca6bu;
    b = b ^ (b >> 13u);
    b = b * 0xc2b2ae35u;
    b = b ^ (b >> 16u);
    const float2 jitter = float2(float(a) * (1.f / 4294967296.f), float(b) * (1.f / 4294967296.f));
    float2 u = float2(float(j % n) / float(n), radicalInverse(j));
    u += jitter * (1.f / float(n));
    return u - float2(floor(u.x), floor(u.y));
}
} // namespace LumenScreenProbe
