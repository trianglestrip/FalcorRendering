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

/** Light sampling adapters for LumenGI lighting.

    This module imports LumenGILighting.slang and adds the sampling
    techniques: uniform analytic light selection (the migrated naive
    single-light sampling from the S1 baseline, now reporting the full
    sampling pdf for MIS), and optional environment/emissive importance
    sampling guarded by host defines.

    The environment and emissive importance samplers are host-created objects
    (EnvMapSampler / EmissiveLightSampler) whose shader fields are only
    required when the corresponding define is 1. The host sets
    LUMEN_GI_HAS_ENVIRONMENT_SAMPLER / LUMEN_GI_HAS_EMISSIVE_SAMPLER to 1
    only when those objects are created and bound; until then the functions
    return false without touching the sampler parameters.
*/

__exported import RenderPasses.LumenGI.Lighting.LumenGILighting;
import Rendering.Lights.LightHelpers;
import Rendering.Lights.EnvMapSampler;
import Rendering.Lights.EmissiveLightSampler;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGeneratorInterface;

#ifndef LUMEN_GI_HAS_ENVIRONMENT_SAMPLER
#define LUMEN_GI_HAS_ENVIRONMENT_SAMPLER 0
#endif

#ifndef LUMEN_GI_HAS_EMISSIVE_SAMPLER
#define LUMEN_GI_HAS_EMISSIVE_SAMPLER 0
#endif

// The host defines these on the tracing program. Local defaults keep this
// file compilable standalone and must match the lighting module defaults.
#ifndef USE_ENV_LIGHT
#define USE_ENV_LIGHT 0
#endif
#ifndef USE_ANALYTIC_LIGHTS
#define USE_ANALYTIC_LIGHTS 1
#endif
#ifndef USE_EMISSIVE_LIGHTS
#define USE_EMISSIVE_LIGHTS 1
#endif

/** Sample one analytic light uniformly from Falcor's scene light list.

    This is the migrated S1 baseline sampling: a single light index is chosen
    uniformly, then Falcor's sampleLight() generates a point on the light.
    radianceOverPdf is Li * lightCount (Li already includes the conditional
    solid-angle pdf division for area lights), so the total sampling pdf is
    lightSample.pdf / lightCount. Delta lights (point/directional) have a
    zero solid-angle pdf and therefore zero total pdf; the caller must not
    reject such samples (see LumenGILightSample::isValid()).
*/
bool lumenGISampleAnalyticLight<S : ISampleGenerator>(
    const float3 positionW,
    const float3 faceNormalW,
    inout S sampleGenerator,
    out LumenGILightSample result
)
{
    result = {};

#if USE_ANALYTIC_LIGHTS
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0u)
        return false;

    // Select a light source uniformly from the light list. This matches
    // PathTracer's selection scheme; the per-light selection probability is
    // 1 / lightCount.
    const uint lightIndex = min(uint(sampleNext1D(sampleGenerator) * float(lightCount)), lightCount - 1u);

    AnalyticLightSample lightSample;
    if (!sampleLight(positionW, gScene.getLight(lightIndex), sampleGenerator, lightSample))
        return false;

    result.radianceOverPdf = lightSample.Li * float(lightCount);
    result.pdf = lightSample.pdf / float(lightCount);
    result.rayOrigin = lumenGIComputeRayOrigin(positionW, faceNormalW, lightSample.dir);
    result.distance = min(lightSample.distance, kLumenGIInfiniteLightDistance);
    result.direction = lightSample.dir;
    result.kind = (uint)LumenGILightKind::Analytic;
    result.sourceIndex = lightIndex;
    return true;
#else
    return false;
#endif
}

/** Importance-sample the environment map prepared by Falcor's EnvMapSampler.

    Only available when the host creates the EnvMapSampler object and sets
    LUMEN_GI_HAS_ENVIRONMENT_SAMPLER to 1.
*/
bool lumenGISampleEnvironmentLight<S : ISampleGenerator>(
    EnvMapSampler environmentSampler,
    const float3 positionW,
    const float3 faceNormalW,
    inout S sampleGenerator,
    out LumenGILightSample result
)
{
    result = {};

#if USE_ENV_LIGHT && LUMEN_GI_HAS_ENVIRONMENT_SAMPLER
    EnvMapSample lightSample;
    if (!environmentSampler.sample(sampleNext2D(sampleGenerator), lightSample))
        return false;
    if (lightSample.pdf <= 0.f)
        return false;

    result.radianceOverPdf = max(lightSample.Le / lightSample.pdf, float3(0.f));
    result.pdf = lightSample.pdf;
    result.rayOrigin = lumenGIComputeRayOrigin(positionW, faceNormalW, lightSample.dir);
    result.distance = kLumenGIInfiniteLightDistance;
    result.direction = lightSample.dir;
    result.kind = (uint)LumenGILightKind::Environment;
    result.sourceIndex = kLumenGIInvalidLightIndex;
    return result.isValid();
#else
    return false;
#endif
}

/** Importance-sample emissive triangles using a host-selected Falcor sampler.

    Only available when the host creates the EmissiveLightSampler object and
    sets LUMEN_GI_HAS_EMISSIVE_SAMPLER to 1.
*/
bool lumenGISampleEmissiveLight<S : ISampleGenerator>(
    EmissiveLightSampler emissiveSampler,
    const float3 positionW,
    const float3 faceNormalW,
    const bool upperHemisphereOnly,
    inout S sampleGenerator,
    out LumenGILightSample result
)
{
    result = {};

#if USE_EMISSIVE_LIGHTS && LUMEN_GI_HAS_EMISSIVE_SAMPLER
    TriangleLightSample lightSample;
    if (!emissiveSampler.sampleLight(positionW, faceNormalW, upperHemisphereOnly, sampleGenerator, lightSample))
        return false;
    if (lightSample.pdf <= 0.f)
        return false;

    const float3 lightPositionW = computeRayOrigin(lightSample.posW, lightSample.normalW);
    const float3 rayOrigin = lumenGIComputeRayOrigin(positionW, faceNormalW, lightPositionW - positionW);
    const float3 toLight = lightPositionW - rayOrigin;
    const float distance = length(toLight);
    if (distance <= 0.f)
        return false;

    result.radianceOverPdf = max(lightSample.Le / lightSample.pdf, float3(0.f));
    result.pdf = lightSample.pdf;
    result.rayOrigin = rayOrigin;
    result.distance = distance;
    result.direction = toLight / distance;
    result.kind = (uint)LumenGILightKind::Emissive;
    result.sourceIndex = lightSample.triangleIndex;
    return result.isValid();
#else
    return false;
#endif
}
