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

/** Tracing-side adapter for LumenGI secondary-hit lighting.

    All lighting math lives in the imported modules:
    - RenderPasses.LumenGI.Lighting.LumenGILighting     (evaluation, MIS, guards)
    - RenderPasses.LumenGI.Lighting.LumenGILightSampling (sampling)
    This file only composes them and keeps the LumenHitLightingResult
    contract used by the tracing shaders. The environment contribution is
    handled by the miss shader and is not part of this adapter.
*/

import Scene.Shading;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.EmissiveLightSampler;
import RenderPasses.LumenGI.Lighting.LumenGILighting;
import RenderPasses.LumenGI.Lighting.LumenGILightSampling;

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
#ifndef LUMEN_GI_HAS_EMISSIVE_SAMPLER
#define LUMEN_GI_HAS_EMISSIVE_SAMPLER 0
#endif

/** Result of evaluating outgoing radiance at a secondary GI hit.

    This deliberately small interface is the hand-off point between the
    tracing path and the lighting modules. The trace payload and the SBT
    contract do not depend on its layout beyond the fields below.
*/
struct LumenHitLightingResult
{
    float3 radiance;
    float confidence;
};

/** Evaluate the secondary-hit lighting used by the HWRT baseline.

    Emissive lighting comes from the material system (emission) and, when the
    host binds an emissive light sampler, from next-event estimation over the
    emissive triangles. Analytic lighting is a uniform light selection with
    next-event estimation (optionally MIS weighted). The returned radiance is
    pre-clamp: the tracing shader applies the radiance ceiling and the NaN/Inf
    guards before writing the payload.
*/
LumenHitLightingResult evaluateLumenHitLighting(
    const ShadingData sd,
    const IMaterialInstance materialInstance,
    const EmissiveLightSampler emissiveSampler,
    inout SampleGenerator sampleGenerator,
    out LumenGILightingComponents components
)
{
    LumenHitLightingResult result = {};
    components.analyticRadiance = float3(0.f);
    components.emissiveRadiance = lumenGIEvaluateHitEmission(sd, materialInstance);

#if USE_EMISSIVE_LIGHTS && LUMEN_GI_HAS_EMISSIVE_SAMPLER
    LumenGILightSample emissiveSample;
    if (lumenGISampleEmissiveLight(emissiveSampler, sd.posW, sd.faceN, true, sampleGenerator, emissiveSample))
        components.emissiveRadiance += lumenGIEvaluateEmissiveLightingSample(sd, materialInstance, sampleGenerator, emissiveSample);
#endif

#if USE_ANALYTIC_LIGHTS
    LumenGILightSample lightSample;
    if (lumenGISampleAnalyticLight(sd.posW, sd.faceN, sampleGenerator, lightSample))
        components.analyticRadiance = lumenGIEvaluateAnalyticLightingSample(sd, materialInstance, sampleGenerator, lightSample);
#endif

    result.radiance = components.emissiveRadiance + components.analyticRadiance;
    result.confidence = 1.f;
    return result;
}
