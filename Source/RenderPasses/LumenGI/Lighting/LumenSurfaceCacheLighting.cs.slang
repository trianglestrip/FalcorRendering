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

/** Surface Cache direct lighting compute pass (S3-B1, Agent J; S3-B2 feedback).

    Lights the captured Surface Cache pages: one threadgroup of
    16 x 16 threads (= one kLumenSurfaceCacheTileSize page) per page. Each
    texel reconstructs its shading state from the material/metadata/params
    atlases plus the page's card (AABB + face basis), evaluates single-sample
    next-event estimates for analytic lights (uniform light selection),
    environment (EnvMapSampler importance sampling) and emissive triangles
    (LightBVHSampler), averages gSamplesPerTexel estimates, and writes the
    direct radiance into the radiance atlas RGB channels plus a confidence
    value into the visibility atlas. Estimator conventions (radianceOverPdf,
    Li including 1/pdf, uniform analytic light selection) are reused from
    LumenGILighting.slang / LumenGILightSampling.slang (Agent B); the BSDF
    is the atlas Lambert model f = albedo/pi, evaluated against the decoded
    shading normal.

    S3-B2 adds the multi-bounce feedback on top of the direct term: each
    texel reads the previous frame's indirect radiance (host double-buffer
    bound as gIndirectPrev, or the radiance atlas A channel as a fallback),
    applies the recurrence indirect_curr = albedo * (direct + strength *
    indirect_prev) with an energy clamp and a bounce cap, adds it to the
    direct radiance, and writes the current indirect into gIndirectCurr plus
    the radiance atlas A channel. When the feedback is disabled (CB bit 0) or
    gIndirectPrev is unbound, the pass is byte-for-byte the S3-B1 single-bounce
    direct lighting. See the LumenSurfaceCacheLightingData.slang module header
    for the frozen recurrence and layout, and the derivation comment near the
    feedback block in main() below.

    Dispatch contract:
      - Render-list mode (default): dispatch (gRenderPageCount, 1, 1);
        threadgroup x lights gLumenRenderList[x].
      - Full-atlas mode: gLumenRenderList unbound; dispatch
        (pagesPerSide*pagesPerSide, 1, 1); threadgroup x lights page x+1;
        gRenderPageCount must equal pagesPerSide*pagesPerSide.

    Degradation matrix (all guarded, none silent-correct):
      - gLumenRenderList   unbound  -> full-atlas mode (above).
      - gLumenPageToCard   unbound  -> page skipped (world position cannot be
                                       reconstructed; host bug, not a fallback).
      - gMaterialParamsAtlas unbound -> emissive = 0.
      - envMapSampler      unbound  -> uniform-sphere sampling of
                                       gScene.envMap (unbiased, high variance).
      - emissiveSampler    unbound  -> emissive next-event = miss constant 0.
      - gIndirectPrev      unbound  -> S3-B2 feedback off (single bounce).
      - LUMEN_GI_CACHE_LIGHTING_SHADOWS = 0 -> unshadowed direct lighting.
      - fallback texels (alpha-test / unsupported) -> no radiance write
        (capture cleared the page); confidence written as 0 so the texel
        routes to hit lighting.

    Host wiring checklist (see LumenSurfaceCacheLightingData.slang for the
    frozen resource layouts and CB fields):
      - ComputePass over this file, entry "main", defines from
        EmissiveLightSampler::getDefines() (LightBVHSampler), SAMPLE_GENERATOR_TYPE
        (optional; defaults to the uniform generator), and the USE_* /
        LUMEN_GI_* defines listed in the data module.
      - mpScene->bindShaderData(rootVar["gScene"]) AND
        mpScene->bindShaderDataForRaytracing(renderContext, rootVar["gScene"], rayTypeCount)
        (the latter only when LUMEN_GI_CACHE_LIGHTING_SHADOWS = 1).
      - envMapSampler / emissiveSampler bound as in the PathTracer pass.
      - CB fields filled per dispatch; gSamplesPerTexel from
        LumenCacheLightingQuality; the S3-B2 feedback fields (enable, strength,
        maxBounces) from the host properties.
      - S3-B2 feedback (when enabled): gIndirectPrev (previous frame output),
        gIndirectCurr (this frame output) and gBounceCountAtlas bound; the host
        flips the double-buffer after every dispatch.
      - gLumenGICounters cleared before dispatch, read back after.
*/

// Local default keeps this file compilable without host defines: the uniform
// generator (SAMPLE_GENERATOR_UNIFORM = 1, see SampleGeneratorType.slangh)
// needs no host-side state because it is seeded per texel. Hosts overriding
// SAMPLE_GENERATOR_TYPE (e.g. TinyUniform) must also bind the matching
// SampleGenerator object state.
#ifndef SAMPLE_GENERATOR_TYPE
#define SAMPLE_GENERATOR_TYPE 1
#endif

// Preprocessor macros do NOT cross Slang module boundaries, so the defaults
// below are re-declared here (identical values to the ones in
// LumenSurfaceCacheLightingData.slang) for this translation unit. The host
// overrides them on the program; defaults keep the file compilable standalone
// and must match the lighting module defaults.
#ifndef USE_ENV_LIGHT
#define USE_ENV_LIGHT 0
#endif
#ifndef USE_ANALYTIC_LIGHTS
#define USE_ANALYTIC_LIGHTS 1
#endif
#ifndef USE_EMISSIVE_LIGHTS
#define USE_EMISSIVE_LIGHTS 1
#endif
#ifndef LUMEN_GI_HAS_ENVIRONMENT_SAMPLER
#define LUMEN_GI_HAS_ENVIRONMENT_SAMPLER 0
#endif
#ifndef LUMEN_GI_HAS_EMISSIVE_SAMPLER
#define LUMEN_GI_HAS_EMISSIVE_SAMPLER 0
#endif
#ifndef LUMEN_GI_ANALYTIC_LIGHT_MIS
#define LUMEN_GI_ANALYTIC_LIGHT_MIS 0
#endif
#ifndef LUMEN_GI_CACHE_LIGHTING_SHADOWS
#define LUMEN_GI_CACHE_LIGHTING_SHADOWS 1
#endif

#ifndef is_valid_gLumenRenderList
#define is_valid_gLumenRenderList 1
#endif
#ifndef is_valid_gLumenPageToCard
#define is_valid_gLumenPageToCard 1
#endif
#ifndef is_valid_gLumenPageMetadata
#define is_valid_gLumenPageMetadata 1
#endif
#ifndef is_valid_gMaterialParamsAtlas
#define is_valid_gMaterialParamsAtlas 0
#endif
#ifndef is_valid_gLumenVisibilityAtlas
#define is_valid_gLumenVisibilityAtlas 1
#endif
#ifndef is_valid_gIndirectPrev
#define is_valid_gIndirectPrev 0
#endif
#ifndef is_valid_gIndirectCurr
#define is_valid_gIndirectCurr 0
#endif
#ifndef is_valid_gBounceCountAtlas
#define is_valid_gBounceCountAtlas 0
#endif
#ifndef is_valid_gLumenGICounters
#define is_valid_gLumenGICounters 0
#endif
#ifndef is_valid_gDebugTexture
#define is_valid_gDebugTexture 0
#endif

#define is_valid(name) (is_valid_##name != 0)

// Numeric constants (M_PI etc.) are macros too and are only visible when
// included in this translation unit.
#include "Utils/Math/MathConstants.slangh"

import Utils.Sampling.SampleGenerator;
import Scene.Scene;
import Scene.RaytracingInline;
import Utils.Math.MathHelpers;
import Rendering.Lights.EnvMapSampler;
import RenderPasses.LumenGI.Lighting.LumenSurfaceCacheLightingData;
import RenderPasses.LumenGI.Lighting.LumenGILightSampling;

/** Uniform-sphere direction sample used by the environment degradation path
    (pdf = 1/(4*pi)). Returns a unit direction.
*/
float3 lumenCacheSampleUniformSphere(const float2 u)
{
    const float z = 1.f - 2.f * u.y;
    const float r = sqrt(max(0.f, 1.f - z * z));
    const float phi = M_2PI * u.x;
    return float3(r * cos(phi), r * sin(phi), z);
}

/** Sample the environment. Importance-samples the EnvMapSampler when the
    host created and bound one; otherwise degrades to uniform-sphere sampling
    of the scene env map (same estimator form, higher variance).
*/
bool lumenCacheSampleEnvironment<S : ISampleGenerator>(
    EnvMapSampler environmentSampler,
    const float3 positionW,
    const float3 faceNormalW,
    inout S sampleGenerator,
    out LumenGILightSample result
)
{
    result = {};
#if USE_ENV_LIGHT
#if LUMEN_GI_HAS_ENVIRONMENT_SAMPLER
    return lumenGISampleEnvironmentLight(environmentSampler, positionW, faceNormalW, sampleGenerator, result);
#else
    // Degradation: EnvMapSampler object not created/bound. Uniform-sphere
    // sampling keeps the estimator unbiased; the sample is only accepted when
    // the env map is non-black (LumenGILightSample::isValid()).
    const float3 dir = lumenCacheSampleUniformSphere(sampleNext2D(sampleGenerator));
    const float pdf = 1.f / (4.f * M_PI);
    result.radianceOverPdf = max(gScene.envMap.eval(dir), float3(0.f)) / pdf;
    result.pdf = pdf;
    result.rayOrigin = lumenGIComputeRayOrigin(positionW, faceNormalW, dir);
    result.distance = kLumenGIInfiniteLightDistance;
    result.direction = dir;
    result.kind = (uint)LumenGILightKind::Environment;
    result.sourceIndex = kLumenGIInvalidLightIndex;
    return result.isValid();
#endif
#else
    return false;
#endif
}

/** Shadow test for a sampled light. Guarded by LUMEN_GI_CACHE_LIGHTING_SHADOWS
    (default 1); with the define set to 0 the test is skipped and lighting is
    unshadowed (documented degradation for hosts without ray tracing data).
*/
bool lumenCacheTraceVisibility(const LumenGILightSample lightSample)
{
#if LUMEN_GI_CACHE_LIGHTING_SHADOWS
    SceneRayQuery<1> sceneRayQuery;
    return sceneRayQuery.traceVisibilityRay(lightSample.getVisibilityRay());
#else
    return true;
#endif
}

/** Balance-heuristic MIS weight for the analytic technique, with the exact
    semantics of LUMEN_GI_ANALYTIC_LIGHT_MIS in LumenGILighting.slang:
    weight 1 by default; when the define is 1, balance the light-sampling pdf
    against the Lambert scatter pdf evaluated at the light direction. Delta
    lights (zero solid-angle pdf) stay unweighted, matching PathTracer's
    LightType::Analytic exemption.
*/
float lumenCacheMISWeight(const LumenGILightSample lightSample, const float NdotL)
{
#if LUMEN_GI_ANALYTIC_LIGHT_MIS
    if (lightSample.kind == (uint)LumenGILightKind::Analytic && lightSample.pdf > 0.f)
    {
        const float scatterPdf = max(NdotL, 0.f) * M_1_PI;
        return lumenGIEvalMIS(1.f, lightSample.pdf, 1.f, scatterPdf);
    }
#endif
    return 1.f;
}

/** Lambert direct-radiance contribution of a sampled light (pre-clamp):
    f * cos * radianceOverPdf * misWeight with f = albedo/pi, cos = max(NdotL,0).
    radianceOverPdf already contains the light-selection probability
    (analytic: Li * lightCount; emissive/env: Le / solid-angle pdf). Returns
    zero when the sampled direction is below the horizon. Visibility is NOT
    tested here; call lumenCacheTraceVisibility() on the positive result.
*/
float3 lumenCacheEvaluateDirectSample(
    const float3 albedo,
    const float3 normalW,
    const LumenGILightSample lightSample
)
{
    const float NdotL = dot(normalW, lightSample.direction);
    if (NdotL <= 0.f)
        return float3(0.f);
    return (albedo * M_1_PI) * NdotL * lightSample.radianceOverPdf * lumenCacheMISWeight(lightSample, NdotL);
}

/** Reconstruct the world position of a page texel from the card AABB and
    face basis (frozen in LumenCardCaptureData.slang): the orthographic card
    camera sits at center - viewDir * (0.5*extent[axis] + nearMargin) and the
    stored depth is the exact linear distance along viewDir, so the
    reconstruction is exact for captured texels.
*/
void lumenCacheReconstructPosition(
    const LumenCard card,
    const float2 localUV,  // (0..1)^2 within the page, texel centers
    const float nearMargin,
    const float depth,     // metadata R (meters)
    out float3 positionW
)
{
    const uint axis = lumenCardFaceAxis(card.faceIndex);
    const uint2 axes = lumenCardFacePerpendicularAxes(card.faceIndex);
    float3 viewDir, right, up;
    lumenCardFaceBasis(card.faceIndex, viewDir, right, up);

    const float margin = nearMargin > 0.f ? nearMargin : kLumenCardCaptureDefaultNearMargin;
    const float halfDepth = 0.5f * lumenCardComponent(card.extent, axis);
    const float3 cameraPos = card.center - viewDir * (halfDepth + margin);

    positionW = cameraPos
        + viewDir * depth
        + right * (localUV.x - 0.5f) * lumenCardComponent(card.extent, axes.x)
        + up * (localUV.y - 0.5f) * lumenCardComponent(card.extent, axes.y);
}

/** Debug view color for the cache lighting output (only called when
    gDebugTexture is bound and gDebugMode is non-zero). `radiance` is the
    final atlas RGB (direct + S3-B2 indirect); `indirect` is the feedback term
    (0 when the feedback is off).
*/
float4 lumenCacheDebugColor(
    const uint debugMode,
    const float3 radiance,
    const float3 indirect,
    const float3 analyticAverage,
    const float3 emissiveAverage,
    const float3 environmentAverage,
    const float3 emitted,
    const float confidence,
    const bool firefly
)
{
    switch (debugMode)
    {
    case kLumenDebugDirectLighting:
        return float4(radiance, 1.f);
    case kLumenDebugIndirectOnly:
        return float4(min(indirect, float3(kLumenGIMaxRadiance)), 1.f);
    case kLumenDebugAnalyticOnly:
        return float4(min(analyticAverage, float3(kLumenGIMaxRadiance)), 1.f);
    case kLumenDebugEmissiveOnly:
        return float4(min(emissiveAverage + emitted, float3(kLumenGIMaxRadiance)), 1.f);
    case kLumenDebugEnvironmentOnly:
        return float4(min(environmentAverage, float3(kLumenGIMaxRadiance)), 1.f);
    case kLumenDebugConfidence:
        return float4(confidence, confidence, confidence, 1.f);
    case kLumenDebugFireflyMask:
        return firefly ? float4(1.f, 0.f, 0.f, 1.f) : float4(0.f, 1.f, 0.f, 1.f);
    default:
        return float4(radiance, 1.f);
    }
}

/** Per-texel write of the GPU counters, batched into four atomics. Skipped
    when the buffer is unbound.
*/
void lumenCacheWriteCounters(const uint nanInfCount, const uint fireflyCount, const uint negativeCount, const uint tracedRayCount)
{
    if (is_valid(gLumenGICounters))
    {
        InterlockedAdd(gLumenGICounters[uint(LumenGICounterIndex::NaNInfSamples)].x, nanInfCount);
        InterlockedAdd(gLumenGICounters[uint(LumenGICounterIndex::FireflySamples)].x, fireflyCount);
        InterlockedAdd(gLumenGICounters[uint(LumenGICounterIndex::NegativeSamples)].x, negativeCount);
        InterlockedAdd(gLumenGICounters[uint(LumenGICounterIndex::TracedRays)].x, tracedRayCount);
    }
}

/** Surface Cache direct lighting entry point. One threadgroup per page:
    [numthreads(16,16,1)] with the page size frozen to
    kLumenSurfaceCacheTileSize in LumenCardCaptureData.slang.
*/
[numthreads(16, 16, 1)]
void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
    // ------------------------------------------------------------------
    // Page resolution
    // ------------------------------------------------------------------
    if (groupID.x >= gRenderPageCount)
        return;

    uint pageID;
#if is_valid(gLumenRenderList)
    pageID = gLumenRenderList[groupID.x];
#else
    pageID = groupID.x + 1u; // full-atlas fallback mode
#endif
    if (pageID == kInvalidPageID)
        return;
    const uint pagesPerSide = max(1u, gPagesPerSide);
    if (pageID > pagesPerSide * pagesPerSide)
        return;

    // ------------------------------------------------------------------
    // Texel location and atlas data
    // ------------------------------------------------------------------
    const uint2 origin = lumenCardPageTexelOrigin(pageID, pagesPerSide);
    const uint2 texel = origin + uint2(groupThreadID.xy);
    if (any(texel >= gAtlasSize))
        return;
    if (is_valid(gLumenVisibilityAtlas))
        gLumenVisibilityAtlas[texel] = 0.f;

    const float4 material = gMaterialAtlas[texel];
    const float4 metadata = gMetadataAtlas[texel];
    const uint flags = uint(metadata.g);

    // No surface: the page slot was never captured (capture clears the page
    // region, so stale radiance cannot survive a re-capture).
    if (flags == 0u)
        return;

    // Fallback texels (alpha-test failure, unsupported material) route to hit
    // lighting: write zero confidence and leave the radiance cleared by the
    // capture pass.
    if ((flags & kLumenCardMetadataFlagFallbackAny) != 0u)
    {
        if (is_valid(gLumenVisibilityAtlas))
            gLumenVisibilityAtlas[texel] = 0.f;
        return;
    }
    if ((flags & kLumenCardMetadataFlagValid) == 0u)
        return;

    // ------------------------------------------------------------------
    // Surface reconstruction from the page's card
    // ------------------------------------------------------------------
    const uint cardIndex = is_valid(gLumenPageToCard) ? gLumenPageToCard[pageID] : kLumenCardInvalidID;
    if (cardIndex == kLumenCardInvalidID)
        return; // page without a card (evicted/host bug): cannot light.

    // Generation/state ownership check. A page ID can be reused after eviction;
    // reject stale or non-resident entries before reading the card or lighting data.
    const uint4 pageMetadata = is_valid(gLumenPageMetadata) ? gLumenPageMetadata[pageID] : uint4(0u);
    const uint pageState = pageMetadata.y;
    if (pageMetadata.x == 0u || (pageState != 1u && pageState != 2u))
        return;

    const LumenCard card = gCards[cardIndex];
    const float2 localUV = (float2(groupThreadID.xy) + 0.5f) / float(kLumenSurfaceCacheTileSize);

    float3 positionW;
    lumenCacheReconstructPosition(card, localUV, gNearMargin, metadata.r, positionW);
    float3 normalW = oct_to_ndir_unorm(metadata.ba);
    normalW = any(isnan(normalW)) ? float3(0.f, 1.f, 0.f) : normalize(normalW);

    const float3 albedo = max(material.rgb, float3(0.f));
    const float opacity = saturate(material.a);

    // ------------------------------------------------------------------
    // Accumulators and random state
    // ------------------------------------------------------------------
    uint nanInfCount = 0u;
    uint fireflyCount = 0u;
    uint negativeCount = 0u;
    uint tracedRayCount = 0u;
    bool texelFirefly = false;

    const uint sampleCount = max(1u, gSamplesPerTexel);
    SampleGenerator sampleGenerator = SampleGenerator(texel, gSeed + gFrameIndex * kLumenGICacheSeedFrameStride);

    float3 directSum = float3(0.f);
    float3 analyticSum = float3(0.f);
    float3 emissiveSum = float3(0.f);
    float3 environmentSum = float3(0.f);
    uint visibleSampleCount = 0u;

    for (uint s = 0u; s < sampleCount; ++s)
    {
        float3 sampleTotal = float3(0.f);
        bool anyVisible = false;

        // Analytic next-event estimation: one uniform light selection.
#if USE_ANALYTIC_LIGHTS
        {
            LumenGILightSample lightSample;
            if (lumenGISampleAnalyticLight(positionW, normalW, sampleGenerator, lightSample))
            {
                float3 contribution = lumenCacheEvaluateDirectSample(albedo, normalW, lightSample);
                if (any(contribution > 0.f))
                {
                    if (lumenCacheTraceVisibility(lightSample))
                    {
                        ++tracedRayCount;
                        if (any(isnan(contribution)) || any(isinf(contribution)))
                        {
                            contribution = float3(0.f);
                            ++nanInfCount;
                        }
                        sampleTotal += contribution;
                        analyticSum += contribution;
                        anyVisible = true;
                    }
                }
            }
        }
#endif

        // Environment next-event estimation.
#if USE_ENV_LIGHT && LUMEN_GI_HAS_ENVIRONMENT_SAMPLER
        {
            LumenGILightSample lightSample;
            if (lumenCacheSampleEnvironment(envMapSampler, positionW, normalW, sampleGenerator, lightSample))
            {
                float3 contribution = lumenCacheEvaluateDirectSample(albedo, normalW, lightSample);
                if (any(contribution > 0.f))
                {
                    if (lumenCacheTraceVisibility(lightSample))
                    {
                        ++tracedRayCount;
                        if (any(isnan(contribution)) || any(isinf(contribution)))
                        {
                            contribution = float3(0.f);
                            ++nanInfCount;
                        }
                        sampleTotal += contribution;
                        environmentSum += contribution;
                        anyVisible = true;
                    }
                }
            }
        }
#endif

        // Emissive triangle next-event estimation. With no sampler bound the
        // estimate degrades to a miss constant (see the data module).
#if USE_EMISSIVE_LIGHTS && LUMEN_GI_HAS_EMISSIVE_SAMPLER
        {
            LumenGILightSample lightSample;
            if (lumenGISampleEmissiveLight(emissiveSampler, positionW, normalW, true, sampleGenerator, lightSample))
            {
                float3 contribution = lumenCacheEvaluateDirectSample(albedo, normalW, lightSample);
                if (any(contribution > 0.f))
                {
                    if (lumenCacheTraceVisibility(lightSample))
                    {
                        ++tracedRayCount;
                        if (any(isnan(contribution)) || any(isinf(contribution)))
                        {
                            contribution = float3(0.f);
                            ++nanInfCount;
                        }
                        sampleTotal += contribution;
                        emissiveSum += contribution;
                        anyVisible = true;
                    }
                }
            }
        }
#endif

        // Per-sample guards: NaN/Inf -> zero, firefly -> clamp to the ceiling.
        if (any(isnan(sampleTotal)) || any(isinf(sampleTotal)))
        {
            sampleTotal = float3(0.f);
            ++nanInfCount;
        }
        if (any(sampleTotal > float3(kLumenGIMaxRadiance)))
        {
            sampleTotal = min(sampleTotal, float3(kLumenGIMaxRadiance));
            ++fireflyCount;
            texelFirefly = true;
        }

        directSum += sampleTotal;
        if (anyVisible)
            ++visibleSampleCount;
    }

    // ------------------------------------------------------------------
    // Final radiance: emission (material property, exact) + averaged NEE.
    // ------------------------------------------------------------------
    float3 emitted = is_valid(gMaterialParamsAtlas)
        ? max(gMaterialParamsAtlas[texel].gba, float3(0.f))
        : float3(0.f);
    if (any(isnan(emitted)) || any(isinf(emitted)))
    {
        emitted = float3(0.f);
        ++nanInfCount;
    }

    const float invSampleCount = 1.f / float(sampleCount);
    const float3 neeAverage = directSum * invSampleCount;

    float3 direct = emitted + neeAverage;
    if (any(isnan(direct)) || any(isinf(direct)))
    {
        direct = float3(0.f);
        ++nanInfCount;
    }
    if (any(direct < 0.f))
    {
        direct = max(direct, float3(0.f));
        ++negativeCount;
    }
    // Defensive final clamp: the sampled part is already per-sample clamped
    // to the ceiling; emission is a material property and is not clamped.
    if (any(direct > float3(kLumenGIMaxRadiance)))
    {
        direct = emitted + min(neeAverage, float3(kLumenGIMaxRadiance));
        ++fireflyCount;
        texelFirefly = true;
    }

    const float confidence = float(visibleSampleCount) * invSampleCount * opacity;

    // ------------------------------------------------------------------
    // S3-B2: multi-bounce feedback (recurrence frozen here + in the data module).
    //
    //   indirect_curr = albedo * (direct + strength * indirect_prev)
    //   radiance      = direct + indirect_curr
    //
    // Derivation: treat the previous frame's accumulated indirect as an
    // incoming radiance field and reflect it with the surface albedo. For a
    // texel driven by a constant direct signal D with albedo a and feedback
    // strength s (factor r = a*s), every frame adds one more power of r:
    //
    //   indirect_k = a*D + r*indirect_{k-1} = a*D * (1 + r + ... + r^{k-1})
    //   radiance_k = D * (1 + r + ... + r^k)
    //
    // so the loop telescopes onto the analytic white-furnace equilibrium
    // radiance = D / (1 - r) as k -> inf when r < 1. The bounce cap below
    // stops the accumulation at gCacheLightingFeedbackMaxBounces terms
    // (indirect saturates and no longer grows, judged from the previous
    // frame's counter), which keeps r >= 1 texels (e.g. an albedo-1 emissive
    // panel) bounded as well. Every term is additionally clamped to
    // kLumenGIMaxRadiance (energy clamp; exposure-independent, so the result
    // does not depend on the camera/exposure).
    //
    // is_valid guard: feedback requires the previous-indirect buffer. When it
    // is unbound (or the enable bit is 0) the pass degrades to the plain
    // single-bounce direct result, identical to the S3-B1 behavior.
    const bool cacheFeedbackEnabled = (gCacheLightingFeedbackEnabled != 0u) && is_valid(gIndirectPrev);
    float3 indirect = float3(0.f);
    if (cacheFeedbackEnabled)
    {
        // Previous indirect: the double-buffered atlas when the host binds it;
        // otherwise fall back to the radiance atlas A channel. The capture
        // pass clears A to zero on every (re)capture, so the A == 0 test below
        // also detects freshly captured texels and restarts their accumulation
        // instead of re-feeding stale pre-capture indirect.
        float3 indirectPrev = is_valid(gIndirectPrev)
            ? gIndirectPrev[texel].rgb
            : float3(gRadianceAtlas[texel].a);

        const float3 albedoC = max(albedo, float3(0.f));
        const float strength = max(gCacheLightingFeedbackStrength, 0.f);
        const uint maxBounces = max(1u, gCacheLightingFeedbackMaxBounces);

        uint prevBounces = is_valid(gBounceCountAtlas) ? gBounceCountAtlas[texel] : 0u;
        if (gRadianceAtlas[texel].a == 0.f)
        {
            // Capture-reset: the page was re-captured this frame; restart the
            // accumulation from the single bounce and clear the bounce count.
            indirectPrev = float3(0.f);
            prevBounces = 0u;
        }

        if (prevBounces < maxBounces)
        {
            indirect = albedoC * (direct + strength * indirectPrev);
            // Energy clamp (kLumenGIMaxRadiance, exposure-independent).
            indirect = min(indirect, float3(kLumenGIMaxRadiance));
        }
        else
        {
            // Bounce cap reached: indirect saturates at the previous value and
            // no longer grows (judged from the previous frame's counter).
            indirect = min(indirectPrev, float3(kLumenGIMaxRadiance));
        }

        // Analytic saturation bound for hosts that cannot bind the counter
        // atlas: after maxBounces bounces indirect cannot exceed the closed
        // form a*D*(r^B-1)/(r-1) (== B*a*D when r == 1), which is exactly the
        // value the counter-based cap freezes at for a constant direct. This
        // keeps the feedback safe even without the counter resource, for r < 1
        // (converging series) and r > 1 (diverging series) alike.
        {
            const float3 r = albedoC * strength;
            [unroll]
            for (uint c = 0u; c < 3u; ++c)
            {
                const float rc = r[c];
                const float dc = albedoC[c] * max(direct[c], 0.f);
                const float cap = (abs(rc - 1.f) < 1e-3f)
                    ? float(maxBounces) * dc
                    : dc * (pow(rc, float(maxBounces)) - 1.f) / (rc - 1.f);
                indirect[c] = min(indirect[c], cap);
            }
        }

        if (any(isnan(indirect)) || any(isinf(indirect)))
        {
            indirect = float3(0.f);
            ++nanInfCount;
        }
        if (any(indirect < 0.f))
        {
            indirect = max(indirect, float3(0.f));
            ++negativeCount;
        }

        if (is_valid(gBounceCountAtlas))
            gBounceCountAtlas[texel] = prevBounces + (prevBounces < maxBounces ? 1u : 0u);
        if (is_valid(gIndirectCurr))
            gIndirectCurr[texel] = float4(indirect, 0.f);
    }
    else
    {
        // Feedback off (or no previous-indirect buffer): single-bounce result.
        // Clear the feedback history so the double-buffer and the bounce
        // counter self-clean and re-enabling starts from the single bounce.
        if (is_valid(gIndirectCurr))
            gIndirectCurr[texel] = float4(0.f);
        if (is_valid(gBounceCountAtlas))
            gBounceCountAtlas[texel] = 0u;
    }

    // Final atlas radiance: direct + indirect (single bounce when the feedback
    // is off), energy-clamped to the ceiling.
    float3 radiance = direct;
    if (cacheFeedbackEnabled)
    {
        radiance = direct + indirect;
        if (any(isnan(radiance)) || any(isinf(radiance)))
        {
            radiance = float3(0.f);
            ++nanInfCount;
        }
        if (any(radiance > float3(kLumenGIMaxRadiance)))
        {
            radiance = min(radiance, float3(kLumenGIMaxRadiance));
            ++fireflyCount;
            texelFirefly = true;
        }
    }

    // ------------------------------------------------------------------
    // Outputs
    // ------------------------------------------------------------------
    // Radiance atlas: RGB = total radiance (direct + indirect); A = scalar
    // indirect-radiance proxy (luminance) written by the S3-B2 feedback, or 0
    // when the feedback is off. The capture pass clears both on re-capture.
    gRadianceAtlas[texel] = float4(radiance, dot(indirect, float3(0.2126f, 0.7152f, 0.0722f)));

    if (is_valid(gLumenVisibilityAtlas))
        gLumenVisibilityAtlas[texel] = confidence;

    if (is_valid(gDebugTexture) && gDebugMode != 0u)
    {
        gDebugTexture[texel] = lumenCacheDebugColor(
            gDebugMode,
            radiance,
            indirect,
            analyticSum * invSampleCount,
            emissiveSum * invSampleCount,
            environmentSum * invSampleCount,
            emitted,
            confidence,
            texelFirefly
        );
    }

    lumenCacheWriteCounters(nanInfCount, fireflyCount, negativeCount, tracedRayCount);
}
