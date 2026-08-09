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

/** S4-B3: probe directional integration (Agent Z2).
 *
 *  One thread per probe. Each thread reduces the probe's per-direction hit
 *  records (LumenScreenProbeHit, written by Z1's S4-B2 trace / S4-A3 hit
 *  lighting) into a single diffuse irradiance estimate per probe
 *  (LumenScreenProbeRadiance), which the S4-B3 interpolate pass consumes.
 *
 *  ============================================================================
 *  INTEGRATION WEIGHT FORMULA (the contract this pass implements)
 *  ============================================================================
 *  For a diffuse probe surface with world normal n, the quantity of interest
 *  is the incident diffuse irradiance
 *        E = ∫_Ω Li(ω) · max(ω·n, 0) dω
 *  where Ω is the sphere around the probe (directions below the probe plane
 *  contribute zero for an opaque surface).
 *
 *  Z1 generates the direction set by low-discrepancy / blue-noise sampling.
 *  Three standard distributions are supported, selected by gWeightMode; the
 *  per-direction weight is
 *        W_i = W0 · s_i · f(ω_i)
 *  with s_i the per-direction stratification weight from
 *  LumenScreenProbeDirection.directionW.w (1.0 for an equal-area set) and
 *
 *    Mode 0  cosine-weighted hemisphere  (default; pdf ∝ max(ω·n,0)/π):
 *        W0 = π / N,   f(ω_i) = 1
 *        The pdf cos/π exactly cancels the cosine in the MC estimator
 *        Li·cos/pdf = Li·π, so each sample carries the constant weight π/N.
 *
 *    Mode 1  uniform hemisphere           (pdf = 1/(2π)):
 *        W0 = 2π / N,  f(ω_i) = max(ω_i·n, 0)
 *
 *    Mode 2  uniform sphere               (pdf = 1/(4π), includes
 *        W0 = 4π / N,  f(ω_i) = max(ω_i·n, 0)      below-horizon samples)
 *
 *  where N = number of TRACED directions (popcount of the direction mask).
 *  Renormalizing over the traced count keeps the estimate consistent when
 *  S4-A2 adaptive masks skip directions; with the default all-traced set this
 *  is exactly N = kLumenScreenProbeDirections. (The conditional-pdf of a
 *  partial trace is approximated by the uniform 1/N subsample; S4-C3 regression
 *  validates the mask != all case.)
 *
 *  The final estimator is
 *        E = Σ_i W_i · clamp(Li_i, 0, kLumenScreenProbeMaxRadiance)
 *  over directions whose hit record is valid (flag bit0) and finite. Traced
 *  directions that missed (flag bit1, Li = 0) contribute zero but still scale
 *  the confidence via the valid-hit fraction, so a probe that lost many
 *  directions reports lower confidence instead of a darkened irradiance.
 *
 *  CONFIDENCE
 *        conf = (Σ_i W_i · hitConfidence_i) / (Σ_i W_i)   // over valid hits
 *        conf *= validHitCount / tracedCount              // miss penalty
 *
 *  ============================================================================
 *  INPUT / OUTPUT CONTRACT (host wiring, S4 root integration)
 *  ============================================================================
 *  cbuffer LumenScreenProbeIntegrateCB (bound by name):
 *      uint  gProbeCount;  // number of probes = probeCols * probeRows
 *      uint  gFrameIndex;  // frame index (diagnostics / future set validation)
 *      uint  gWeightMode;  // kLumenScreenProbeWeightMode* (0 default)
 *      uint  gPad;
 *
 *  Resources (all guarded by is_valid_* defines; every field the shader reads
 *  has an is_valid guard so unbound optional inputs compile and no-op):
 *      StructuredBuffer<LumenScreenProbeMeta>      gProbeMeta;      // required
 *      StructuredBuffer<LumenScreenProbeHit>       gProbeHits;      // required
 *      StructuredBuffer<LumenScreenProbeDirection> gProbeDirections;// required
 *      RWStructuredBuffer<LumenScreenProbeRadiance> gProbeRadiance; // required output
 *
 *  Dispatch: one thread per probe, [numthreads(64,1,1)], thread count
 *  ceil(gProbeCount / 64). The host clears gProbeRadiance before dispatch so
 *  early-out probes (invalid / untraced) leave a deterministic zero record.
 *
 *  Deterministic: pure function of the input buffers; no randomness, no
 *  shared memory, one thread writes exactly one output record.
 *
 *  ALIGNMENT POINTS WITH Z1 (S4-B2 trace): the probe/hit/direction records
 *  are defined in LumenScreenProbeData.slang with every field marked
 *  ALIGN(Z1); those three structs are the Z2 assumptions for the trace
 *  output layout and may be renamed/reshuffled by the root integration when
 *  Z1 lands. The radiance output below is Z2-owned and frozen.
 */

#include "LumenScreenProbeData.slang"

// Optional-resource guards. Falcor specializes is_valid_<name> when it binds
// the resource; local defaults keep the module compilable standalone and
// every read is guarded so an unbound resource is never dereferenced.
#ifndef is_valid_gProbeMeta
#define is_valid_gProbeMeta 0
#endif
#ifndef is_valid_gProbeHits
#define is_valid_gProbeHits 0
#endif
#ifndef is_valid_gProbeDirections
#define is_valid_gProbeDirections 0
#endif
#ifndef is_valid_gProbeRadiance
#define is_valid_gProbeRadiance 0
#endif

#define is_valid(name) (is_valid_##name != 0)

// Radiance ceiling mirrored from LumenGIData.slang (kLumenGIMaxRadiance),
// overridable by the host through the same define. Applied per direction
// before accumulation to prevent single-sample fireflies from dominating a
// probe, and again on the integrated output.
#ifndef LUMEN_GI_MAX_RADIANCE
#define LUMEN_GI_MAX_RADIANCE 10000.f
#endif
static const float kLumenScreenProbeMaxRadiance = LUMEN_GI_MAX_RADIANCE;

static const float kLumenScreenProbePI = 3.14159265358979f;

/** Weight-mode constants (see the formula block in the header). */
static const uint kLumenScreenProbeWeightModeCosineHemisphere = 0u;
static const uint kLumenScreenProbeWeightModeUniformHemisphere = 1u;
static const uint kLumenScreenProbeWeightModeUniformSphere = 2u;

cbuffer LumenScreenProbeIntegrateCB
{
    uint gProbeCount;   ///< Number of probes (probeCols * probeRows); guards the dispatch.
    uint gFrameIndex;   ///< Frame index (diagnostics; reserved for direction-set validation).
    uint gWeightMode;   ///< kLumenScreenProbeWeightMode* (0 = cosine hemisphere, the default).
    uint gPad;
};

StructuredBuffer<LumenScreenProbeMeta> gProbeMeta;
StructuredBuffer<LumenScreenProbeHit> gProbeHits;
StructuredBuffer<LumenScreenProbeDirection> gProbeDirections;
RWStructuredBuffer<LumenScreenProbeRadiance> gProbeRadiance;

/** Per-direction integration weight W_i (see header formula block). */
float lumenScreenProbeDirectionWeight(
    const float cosTheta,     ///< dot(direction, probeNormal).
    const float stratWeight,  ///< s_i: per-direction stratification weight (1.0 equal-area).
    const uint mode,
    const uint tracedCount
)
{
    const float n = max(float(tracedCount), 1.f);
    if (mode == kLumenScreenProbeWeightModeUniformHemisphere)
        return (2.f * kLumenScreenProbePI / n) * stratWeight * max(cosTheta, 0.f);
    if (mode == kLumenScreenProbeWeightModeUniformSphere)
        return (4.f * kLumenScreenProbePI / n) * stratWeight * max(cosTheta, 0.f);
    // Cosine-weighted hemisphere: the pdf cos/π cancels the cosine, so the
    // per-sample weight is the constant π/N.
    return (kLumenScreenProbePI / n) * stratWeight;
}

/** True when every component of a radiance estimate is finite. */
bool lumenScreenProbeRadianceIsFinite(const float3 radiance)
{
    return !any(isnan(radiance)) && !any(isinf(radiance));
}

/** Clamp a radiance estimate to the firefly ceiling and zero non-finite /
 *  negative components. Mirrors the LumenGI sample sanitization.
 */
float3 lumenScreenProbeSanitizeRadiance(const float3 radiance)
{
    float3 r = max(radiance, float3(0.f));
    if (!lumenScreenProbeRadianceIsFinite(r))
        return float3(0.f);
    return min(r, float3(kLumenScreenProbeMaxRadiance));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint probeIndex = dispatchThreadID.x;
    if (probeIndex >= gProbeCount)
        return;

    // Zero output record: valid even when this probe is not traced (the host
    // may still clear the buffer; this keeps the write independent of that).
    LumenScreenProbeRadiance outRecord = (LumenScreenProbeRadiance)0;

    if (!is_valid(gProbeMeta) || !is_valid(gProbeHits) || !is_valid(gProbeDirections) || !is_valid(gProbeRadiance))
    {
        gProbeRadiance[probeIndex] = outRecord;
        return;
    }

    const LumenScreenProbeMeta meta = gProbeMeta[probeIndex];

    // Untraced probe: leave a zero record (host clears / or consume zero).
    if (meta.gridState.w == 0u || meta.gridState.x == kLumenScreenProbeInvalidID)
    {
        gProbeRadiance[probeIndex] = outRecord;
        return;
    }

    const float3 probeNormal = normalize(meta.normalW.xyz);
    const uint directionSet = meta.gridState.z;
    const uint mask = meta.maskState.x & kLumenScreenProbeDirectionMaskAll;
    const uint tracedCount = lumenScreenProbePopcount(mask);
    if (tracedCount == 0u)
    {
        gProbeRadiance[probeIndex] = outRecord;
        return;
    }

    float3 accRadiance = float3(0.f);
    float accConfidence = 0.f;
    float accDistance = 0.f;
    float accWeight = 0.f;
    uint validHitCount = 0u;

    for (uint dir = 0u; dir < kLumenScreenProbeDirections; ++dir)
    {
        if ((mask & (1u << dir)) == 0u)
            continue; // Direction not traced (adaptive mask): not part of the estimator.

        const LumenScreenProbeHit hit = gProbeHits[probeIndex * kLumenScreenProbeDirections + dir];
        const uint flags = uint(hit.hitInfo.w);
        if ((flags & kLumenScreenProbeHitFlagValid) == 0u)
            continue; // Traced but no radiance (screen miss without fallback): Li = 0, lowers confidence via hit fraction.

        const LumenScreenProbeDirection d = gProbeDirections[directionSet * kLumenScreenProbeDirections + dir];
        const float3 dirW = normalize(d.directionW.xyz);

        const float3 li = lumenScreenProbeSanitizeRadiance(hit.radiance.rgb);
        if (!any(li > float3(0.f)))
            continue; // Zero / sanitized radiance: same as a miss for the estimator and confidence.
        ++validHitCount;

        const float cosTheta = dot(dirW, probeNormal);
        const float weight = lumenScreenProbeDirectionWeight(cosTheta, d.directionW.w, gWeightMode, tracedCount);
        if (weight <= 0.f)
            continue;

        accRadiance += weight * li;
        accConfidence += weight * saturate(hit.radiance.a);
        accDistance += weight * max(hit.hitInfo.x, 0.f);
        accWeight += weight;
    }

    if (accWeight > 0.f)
    {
        const float confidence = (accConfidence / accWeight) * (float(validHitCount) / float(tracedCount));
        outRecord.radiance = float4(lumenScreenProbeSanitizeRadiance(accRadiance / accWeight), confidence);
        outRecord.stats = float4(accDistance / accWeight, float(validHitCount) / float(tracedCount), 0.f, 0.f);
    }

    gProbeRadiance[probeIndex] = outRecord;
}
