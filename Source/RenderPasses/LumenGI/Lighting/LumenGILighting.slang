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

/** Core lighting math for the LumenGI HWRT baseline.

    This module owns the lighting evaluation primitives used by the tracing
    path: BSDF evaluation of a pre-sampled light, MIS weighting, environment
    evaluation on miss, and the Russian roulette interface. Sampling is
    deliberately separate and lives in LumenGILightSampling.slang, which
    imports this module; the tracing adapter (LumenHitLighting.slang)
    composes the two.

    Units and conventions (identical to Falcor PathTracer):
    - Radiance is linear HDR radiance (W*sr^-1*m^-2) in the same scale as the
      PathTracer output.
    - The BSDF convention is f * dot(wo, n): for a Lambert diffuse surface
      f = albedo / PI, so materialInstance.eval() returns albedo * cos / PI.
      A cosine-weighted direction sample therefore has pdf cos / PI and a
      BSDF weight of albedo.
    - AnalyticLightSample.Li from Falcor's sampleLight() is already divided
      by the conditional solid-angle pdf for area lights (and equals the raw
      incident radiance for delta lights whose solid-angle pdf is zero).
*/

__exported import Scene.Scene;
import Scene.Shading;
import Scene.RaytracingInline;
import Rendering.Lights.LightHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Math.Ray;
import Utils.Sampling.SampleGeneratorInterface;

// The host defines these on the tracing program. Local defaults keep the
// module compilable before the host has wired them, and every code path must
// handle the 0/1 combinations the host may produce.
#ifndef USE_ENV_LIGHT
#define USE_ENV_LIGHT 0
#endif
#ifndef USE_ANALYTIC_LIGHTS
#define USE_ANALYTIC_LIGHTS 1
#endif
#ifndef USE_EMISSIVE_LIGHTS
#define USE_EMISSIVE_LIGHTS 1
#endif

/** Apply balance-heuristic MIS to the analytic light technique.

    The light-sampling technique contributes f * cos * Li * lightCount where
    Li is already divided by the conditional solid-angle pdf, so the total
    sampling pdf is pdf_area / lightCount. The complementary BSDF-sampling
    technique (sample a direction from the BSDF at the hit and find a light
    with a scatter ray) contributes the same integral over emissive geometry
    and the environment, never over analytic lights.

    The S1 single-bounce model does not execute the scatter ray from the hit,
    so a balance weight would remove part of the integral without the
    complementary technique being evaluated. This is exactly why Falcor's
    PathTracer exempts LightType::Analytic from MIS weighting. The define
    therefore defaults to 0 (weight 1, unbiased, PathTracer-comparable). Set
    it to 1 only together with a scatter technique from the hit (S3 Surface
    Cache lighting), where both strategies are executed and the balance
    weight is unbiased.
*/
#ifndef LUMEN_GI_ANALYTIC_LIGHT_MIS
#define LUMEN_GI_ANALYTIC_LIGHT_MIS 0
#endif

/** Russian roulette is provided as an interface and is disabled by default.

    The S1 baseline traces a single diffuse bounce (trace depth 0/1) and never
    continues a path, so the roulette is never invoked and enabling the define
    cannot change S1 outputs. Multi-bounce stages will call
    lumenGITerminateByRussianRoulette() on their accumulated path throughput.
*/
#ifndef LUMEN_GI_ENABLE_RUSSIAN_ROULETTE
#define LUMEN_GI_ENABLE_RUSSIAN_ROULETTE 0
#endif

static const float kLumenGIInfiniteLightDistance = 1e30f;
static const uint kLumenGIInvalidLightIndex = 0xffffffffu;

enum class LumenGILightKind : uint32_t
{
    Analytic = 0,
    Emissive = 1,
    Environment = 2,
};

/** A sampled incident-radiance estimate.

    radianceOverPdf contains the full estimator radiance factor: the emitted
    radiance divided by the total sampling pdf, including the discrete
    light-selection probability (for analytic lights this is Li * lightCount,
    where Li is already divided by the conditional solid-angle pdf, and the
    selection probability is 1/lightCount). A caller multiplies it by the
    material/BSDF value and tests visibility. This follows PathTracer's
    LightSample convention without depending on PathTracer-private types.

    Note: analytic delta lights (point/directional) have a zero solid-angle
    pdf, so pdf is zero for them and isValid() is false even though the sample
    is valid. The analytic path in lumenGIEvaluateAnalyticLightingSample()
    handles this; isValid() is only reliable for emissive/environment samples.
*/
struct LumenGILightSample
{
    float3 radianceOverPdf;
    float pdf;
    float3 rayOrigin;
    float distance;
    float3 direction;
    uint kind;
    uint sourceIndex;

    bool isValid()
    {
        return pdf > 0.f && distance > 0.f && dot(direction, direction) > 0.f && any(radianceOverPdf > 0.f);
    }

    Ray getVisibilityRay()
    {
        return Ray(rayOrigin, direction, 0.f, distance);
    }
};

/** Separated radiance contributions at a secondary hit (pre-clamp).

    The environment contribution is produced by the miss shader and is not
    part of this struct. The tracing shader uses these components for debug
    views and the firefly mask.
*/
struct LumenGILightingComponents
{
    float3 analyticRadiance; ///< Next-event estimate from analytic scene lights.
    float3 emissiveRadiance; ///< Material emission.
};

/** Offset a ray origin to the side of the geometric normal containing rayDirectionW. */
float3 lumenGIComputeRayOrigin(const float3 positionW, const float3 faceNormalW, const float3 rayDirectionW)
{
    return computeRayOrigin(positionW, dot(faceNormalW, rayDirectionW) >= 0.f ? faceNormalW : -faceNormalW);
}

/** Evaluate environment radiance on a miss without an importance sampler.

    The environment texture and transform are part of gScene and are bound by
    Falcor's scene shader-data path. Importance-map resources are not
    required. The environment direction is already unit length at the miss.
*/
float3 lumenGIEvaluateEnvironment(const float3 directionW, const float lod = 0.f)
{
#if USE_ENV_LIGHT
    return max(gScene.envMap.eval(directionW, max(lod, 0.f)), float3(0.f));
#else
    return float3(0.f);
#endif
}

/** Evaluate material emission at a secondary hit. */
float3 lumenGIEvaluateHitEmission(const ShadingData sd, const IMaterialInstance materialInstance)
{
#if USE_EMISSIVE_LIGHTS
    return max(materialInstance.getProperties(sd).emission, float3(0.f));
#else
    return float3(0.f);
#endif
}

/** Balance-heuristic MIS weight, matching PathTracer::evalMIS with the
    balance heuristic: w = (n0 * p0) / (n0 * p0 + n1 * p1).
    If both pdfs vanish (Dirac delta case) the sample is left unweighted.
*/
float lumenGIEvalMIS(const float n0, const float p0, const float n1, const float p1)
{
    const float q0 = n0 * p0;
    const float q1 = n1 * p1;
    if (q0 + q1 <= 0.f)
        return 1.f;
    return q0 / (q0 + q1);
}

/** Russian roulette termination probability for a radiance estimate.
    Mirrors PathTracer::terminatePathByRussianRoulette() using the max
    component as the luminance estimate.
*/
float lumenGIRussianRouletteProbability(const float3 radiance)
{
    const float value = max(max(radiance.x, radiance.y), radiance.z);
    return max(0.f, 1.f - value);
}

/** Apply Russian roulette to a radiance estimate.
    \return True if the estimate was terminated (compensatedRadiance is zero).
    With the LUMEN_GI_ENABLE_RUSSIAN_ROULETTE define off (default), the
    estimate is returned unchanged and the result is always false.
*/
bool lumenGITerminateByRussianRoulette<S : ISampleGenerator>(
    const float3 radiance,
    inout S sampleGenerator,
    out float3 compensatedRadiance
)
{
    compensatedRadiance = radiance;
#if LUMEN_GI_ENABLE_RUSSIAN_ROULETTE
    const float probability = lumenGIRussianRouletteProbability(radiance);
    if (sampleNext1D(sampleGenerator) < probability)
    {
        compensatedRadiance = float3(0.f);
        return true;
    }
    compensatedRadiance = radiance / (1.f - probability);
#endif
    return false;
}

/** Evaluate next-event direct lighting from a pre-sampled analytic light at
    a secondary hit with optional balance-heuristic MIS.

    The estimator is f * cos * radianceOverPdf * misWeight with a visibility
    test. The MIS weight is 1 by default (see LUMEN_GI_ANALYTIC_LIGHT_MIS);
    when enabled it balances the light-sampling pdf against the BSDF sampling
    pdf evaluated at the light direction, with delta lights (zero pdf) kept
    unweighted, matching PathTracer's LightType::Analytic exemption.
*/
float3 lumenGIEvaluateAnalyticLightingSample<S : ISampleGenerator>(
    const ShadingData sd,
    const IMaterialInstance materialInstance,
    inout S sampleGenerator,
    const LumenGILightSample lightSample
)
{
    // The sampled direction must lie in a hemisphere supported by the BSDF
    // lobes (mirrors the test in PathTracer's NEE path).
    const uint lobeTypes = materialInstance.getLobeTypes(sd);
    const bool hasReflection = (lobeTypes & uint(LobeType::Reflection)) != 0;
    const bool hasTransmission = (lobeTypes & uint(LobeType::Transmission)) != 0;
    const float NdotL = dot(sd.getOrientedFaceNormal(), lightSample.direction);
    const bool validHemisphere = (NdotL > kMinCosTheta || hasTransmission) && (NdotL < -kMinCosTheta || hasReflection);
    if (!validHemisphere)
        return float3(0.f);

    // Alpha-aware inline visibility query.
    SceneRayQuery<1> sceneRayQuery;
    if (!sceneRayQuery.traceVisibilityRay(lightSample.getVisibilityRay()))
        return float3(0.f);

    // MIS weight between the light-sampling technique and the BSDF-sampling
    // technique evaluated at the light direction.
    float misWeight = 1.f;
#if LUMEN_GI_ANALYTIC_LIGHT_MIS
    if (lightSample.pdf > 0.f)
    {
        const float scatterPdf = materialInstance.evalPdf(sd, lightSample.direction, true);
        misWeight = lumenGIEvalMIS(1.f, lightSample.pdf, 1.f, scatterPdf);
    }
#endif

    return materialInstance.eval(sd, lightSample.direction, sampleGenerator) * lightSample.radianceOverPdf * misWeight;
}

/** Evaluate next-event direct lighting from a pre-sampled emissive triangle at
    a secondary hit.

    The estimator is f * cos * radianceOverPdf (radianceOverPdf = Le / pdf,
    pdf with respect to solid angle from the shading point) with a visibility
    test. The complementary BSDF-sampling technique (scatter a ray from the
    hit to find emissive geometry) is not executed by the S1 single-bounce
    model, so a balance weight would remove part of the integral without the
    complementary technique being evaluated (identical reasoning to the
    analytic path and to PathTracer's MIS exemption); the weight is therefore
    1 by default. Set LUMEN_GI_ANALYTIC_LIGHT_MIS (shared with the analytic
    path) to 1 only together with a scatter technique (S3 Surface Cache
    lighting), where both strategies are executed and the balance weight is
    unbiased.
*/
float3 lumenGIEvaluateEmissiveLightingSample<S : ISampleGenerator>(
    const ShadingData sd,
    const IMaterialInstance materialInstance,
    inout S sampleGenerator,
    const LumenGILightSample lightSample
)
{
    // The sampled direction must lie in a hemisphere supported by the BSDF
    // lobes (mirrors the analytic path and PathTracer's NEE test).
    const uint lobeTypes = materialInstance.getLobeTypes(sd);
    const bool hasReflection = (lobeTypes & uint(LobeType::Reflection)) != 0;
    const bool hasTransmission = (lobeTypes & uint(LobeType::Transmission)) != 0;
    const float NdotL = dot(sd.getOrientedFaceNormal(), lightSample.direction);
    const bool validHemisphere = (NdotL > kMinCosTheta || hasTransmission) && (NdotL < -kMinCosTheta || hasReflection);
    if (!validHemisphere)
        return float3(0.f);

    // Alpha-aware inline visibility query.
    SceneRayQuery<1> sceneRayQuery;
    if (!sceneRayQuery.traceVisibilityRay(lightSample.getVisibilityRay()))
        return float3(0.f);

    // MIS weight between the light-sampling technique and the BSDF-sampling
    // technique evaluated at the light direction. Emissive sampling always
    // has a positive pdf, so delta-light exemption is not needed here.
    float misWeight = 1.f;
#if LUMEN_GI_ANALYTIC_LIGHT_MIS
    {
        const float scatterPdf = materialInstance.evalPdf(sd, lightSample.direction, true);
        misWeight = lumenGIEvalMIS(1.f, lightSample.pdf, 1.f, scatterPdf);
    }
#endif

    return materialInstance.eval(sd, lightSample.direction, sampleGenerator) * lightSample.radianceOverPdf * misWeight;
}
