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

/** S5-B2: variance-guided adaptive spatial filter (Agent Z8).
 *
 *  One thread per output pixel. Removes the residual spatial noise left by the
 *  S5-B1 temporal filter with a bilateral filter whose radius adapts to the
 *  local radiance variance: noisy regions (high variance) blur more, clean
 *  low-variance regions keep their detail (radius -> gRadiusMin, down to 0 =
 *  passthrough). Every tap is gated by depth / normal / material affinity so
 *  the filter never blends across a geometry silhouette or a material
 *  boundary, and firefly outliers are detected against the neighborhood
 *  statistics and replaced / clamped so a single spike cannot propagate.
 *
 *  ============================================================================
 *  PSEUDO-CODE (the exact contract; also summarized in
 *  LumenSpatialFilterData.slang)
 *  ============================================================================
 *    per pixel P:
 *      if P out of frame: return
 *      if !is_valid(gGIInput) || !is_valid(gLinearZ) || !is_valid(gFilteredOutput):
 *          write (0,0,0,0) to gFilteredOutput (+ optional outputs 0); return
 *      pixelZ = gLinearZ[P].x
 *      if pixelZ <= 0: write (0,0,0,0); return                // sky / no surface
 *      curGI = gGIInput[P]; curRGB = sanitize(curGI.rgb); curConf = saturate(curGI.a)
 *      curNormal / curMaterial = decode(gNormalRoughnessMaterialID[P])   // optional
 *      if gFilterEnabled == 0: write (curRGB, curConf); return          // passthrough
 *
 *      // 1) Local luminance variance over a 3x3 / 5x5 window (depth-gated).
 *      (localVar, meanLum) = varianceEstimate(P, pixelZ)
 *      // 2) Optional temporal variance from S5-A1 moments (see the data file).
 *      vTemporal = gVariance[P]                      // preferred
 *                | max(0, gMoments[P].y - gMoments[P].x^2)
 *      variance  = clamp(max(localVar, gTemporalVarianceWeight * vTemporal),
 *                        0, gMaxVarianceClamp)
 *      // 3) Adaptive radius from the RELATIVE variance.
 *      relVar = variance / max(meanLum^2, gVarianceEpsilon)
 *      r = clamp(lerp(gRadiusMin, gRadiusMax, saturate((relVar - low) / (high - low))),
 *                0, kLumenSpatialFilterMaxTapRadius)
 *      sigma = gSpatialSigmaScale * max(r, 1)
 *      // 4) Bilateral gather over [-ceil(r), +ceil(r)]^2; neighbors only.
 *      for each tap Q (offset d, Q != P):
 *          w = spatialW(d) * depthW(P,Q) * normalW(P,Q) * materialW(P,Q)
 *          accRGB += w * gi(Q); accConf += w * conf(Q); accW += w
 *          neighborLumStats: sumLum, sumLumSq, sumWLum += w * ...
 *      // 5) Firefly handling + center blend.
 *      if accW <= kMinSumWeight: out = (curRGB, curConf)          // isolated
 *      else:
 *          meanRGB = accRGB/accW; meanConf = accConf/accW
 *          meanLumN = sumLum/sumWLum; varN = max(0, sumLumSq/sumWLum - meanLumN^2)
 *          band = gFireflyStdDevFactor * max(sqrt(varN), sqrt(variance))
 *          isFirefly = gFireflyClamp && variance > gVarianceEpsilon
 *                      && |curLum - meanLumN| > band
 *          if isFirefly:
 *              out = meanRGB                                   // 置中值 (replace)
 *              maxLum = meanLumN + gFireflyStdDevFactor * sqrt(varN)
 *              out *= min(1, maxLum / luminance(out))          //  clamp
 *          else:
 *              out = (accRGB + curRGB * 1) / (accW + 1)        // center weight 1
 *              conf = (accConf + curConf * 1) / (accW + 1)
 *      gFilteredOutput[P] = float4(sanitize(out), saturate(conf))
 *      gFilteredConfidence[P] = conf          // optional
 *      gFilteredVariance[P]    = variance     // optional
 *
 *  ============================================================================
 *  BILATERAL / BOUNDARY BEHAVIOR (task §10 S5-B2: keep geometry + material
 *  boundaries, do not mix across edges)
 *  ============================================================================
 *    depthW    dead-zone + exponential falloff (same form as S4-B3 interpolate);
 *              a tap on a far surface behind a silhouette gets ~0 weight, so the
 *              filter does not smear the background onto the foreground.
 *    normalW   pow(saturate(dot)) sharpens the cut at orthogonal faces.
 *    materialW 1 for equal / unknown material, gMaterialMismatchWeight else,
 *              so a matte/graphite boundary is not blended across.
 *    spatialW  pure gaussian falloff with sigma proportional to the ADAPTIVE
 *              radius, so a larger radius genuinely smooths more (the neighbor
 *              weight pool grows while the center weight stays 1).
 *
 *  ============================================================================
 *  INPUT / OUTPUT CONTRACT (S5-A1 host wiring)
 *  ============================================================================
 *  See LumenSpatialFilterData.slang for the full resource list, formats, the
 *  S5-A1 moments / variance contract and the firefly semantics. Required
 *  inputs: gGIInput, gLinearZ, gFilteredOutput. Optional inputs (normal/
 *  material, gVariance / gMoments) skip their stage when unbound. gGIInput.a is
 *  the CONFIDENCE channel (trust scalar in [0,1]); its exact source is a host
 *  decision (S4-B3 interpolate confidence, S5-B1 gTemporalConfidence, or the
 *  S5-B1 history length as a degraded proxy) -- see the data file contract.
 *
 *  Dispatch: [numthreads(8,8,1)], ceil(gFrameDim / 8) per axis. Resolution-
 *  agnostic: the S5-A2 host (LumenReconstruction.h) decides whether this pass
 *  runs at full / half / quarter resolution; the upscale to full-res is a
 *  separate step that re-uses the same bilateral weights.
 *  Deterministic: pure function of the inputs; fixed tap order.
 */

#include "LumenSpatialFilterData.slang"

// Optional-resource guards. Falcor specializes is_valid_<name> when it binds
// the resource; local defaults keep the module compilable standalone and
// every read is guarded so an unbound resource is never dereferenced.
#ifndef is_valid_gGIInput
#define is_valid_gGIInput 0
#endif
#ifndef is_valid_gLinearZ
#define is_valid_gLinearZ 0
#endif
#ifndef is_valid_gNormalRoughnessMaterialID
#define is_valid_gNormalRoughnessMaterialID 0
#endif
#ifndef is_valid_gConfidenceInput
#define is_valid_gConfidenceInput 0
#endif
#ifndef is_valid_gVariance
#define is_valid_gVariance 0
#endif
#ifndef is_valid_gMoments
#define is_valid_gMoments 0
#endif
#ifndef is_valid_gFilteredOutput
#define is_valid_gFilteredOutput 0
#endif
#ifndef is_valid_gFilteredConfidence
#define is_valid_gFilteredConfidence 0
#endif
#ifndef is_valid_gFilteredVariance
#define is_valid_gFilteredVariance 0
#endif

#define is_valid(name) (is_valid_##name != 0)

/** Decode the GBuffer normWRoughnessMaterialID normal (oct-encoded xy; matches
    GBufferRT's ndir_to_oct_unorm and the S4-B3 interpolate decode). Self-contained
    mirror of Falcor MathHelpers oct_to_ndir_unorm so the module compiles standalone. */
float3 lumenSpatialFilterDecodeNormal(const float2 octXY)
{
    const float2 p = octXY * 2.f - 1.f;
    float3 n = float3(p.xy, 1.f - abs(p.x) - abs(p.y));
    if (n.z < 0.f)
        n.xy = (1.f - abs(n.yx)) * select(n.xy >= 0.f, 1.f, -1.f);
    return normalize(n);
}

/** Rec.709 luma of a linear radiance sample, used for all variance / firefly
 *  statistics so the filter decisions are color-insensitive. */
float lumenSpatialFilterLuminance(const float3 rgb)
{
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

/** Sanitize a radiance estimate: clamp negative components to zero, replace
 *  NaN/Inf with zero, and clamp to the firefly ceiling (gFireflyMaxRadiance).
 *  Mirrors the temporal filter sanitization so the spatial output is guaranteed
 *  finite and non-negative (task §10 S5 gate: no NaN/Inf).
 */
float3 lumenSpatialFilterSanitize(const float3 rgb)
{
    float3 r = max(rgb, float3(0.f));
    if (!any(isnan(r)) && !any(isinf(r)))
        return min(r, float3(max(gFireflyMaxRadiance, 0.f)));
    return float3(0.f);
}

/** Confidence source (see the data file contract): the S5-A1 per-pixel
 *  confidence R32F (gConfidenceInput, the "temporalConfidence" graph channel)
 *  OVERRIDES gGIInput.a when valid -- gGIInput is the S5-B1 temporal output
 *  whose .a channel carries the HISTORY LENGTH, not a confidence. Unbound =>
 *  fall back to gGIInput.a (degraded proxy / S4-B3 interpolate confidence). */
float lumenSpatialFilterConfidence(const int2 pixel, const float fallbackA)
{
    if (is_valid(gConfidenceInput))
        return saturate(gConfidenceInput.Load(int3(pixel, 0)).x);
    return saturate(fallbackA);
}

/** A neighbor may contribute to the spatial statistics only when its sample and
 *  confidence are finite and the confidence is non-zero.  RGB is deliberately
 *  not inspected here: a valid black radiance sample still carries information
 *  when its confidence is above the validity epsilon.
 */
static const float kLumenSpatialFilterValidityEpsilon = 1e-4f;

bool lumenSpatialFilterSampleIsValid(
    const int2 pixel,
    const float4 sample,
    out float confidence
)
{
    confidence = 0.f;
    if (any(isnan(sample)) || any(isinf(sample)))
        return false;
    confidence = lumenSpatialFilterConfidence(pixel, sample.a);
    return !isnan(confidence) && !isinf(confidence) &&
           confidence > kLumenSpatialFilterValidityEpsilon;
}

/** Depth affinity factor (same dead-zone + exponential form as the S4-B3
 *  interpolate weight): 1 within gDepthThreshold meters, exponential falloff
 *  beyond, so coplanar taps dominate and silhouettes cut.
 */
float lumenSpatialFilterDepthWeight(const float pixelZ, const float tapZ)
{
    const float gap = abs(pixelZ - tapZ);
    if (gap <= gDepthThreshold)
        return 1.f;
    return exp(-(gap - gDepthThreshold) * max(gDepthSigmaInv, 0.f));
}

/** Normal affinity factor: pow(saturate(dot(n_p, n_Q)), gNormalExponent). */
float lumenSpatialFilterNormalWeight(const float3 pixelNormal, const float3 tapNormal)
{
    return pow(saturate(dot(pixelNormal, tapNormal)), max(gNormalExponent, 0.f));
}

/** Material affinity factor: 1 for equal IDs or an unknown ID on either side,
 *  gMaterialMismatchWeight else (a small residual, never 0, so unsupported-
 *  material pixels still receive radiance).
 */
float lumenSpatialFilterMaterialWeight(const uint pixelMaterialID, const uint tapMaterialID)
{
    if (pixelMaterialID == kLumenSpatialFilterMaterialIDNone || tapMaterialID == kLumenSpatialFilterMaterialIDNone)
        return 1.f;
    return (pixelMaterialID == tapMaterialID) ? 1.f : gMaterialMismatchWeight;
}

/** Spatial gaussian term for a tap offset: exp(-|d|^2 / (2 sigma^2)). The
 *  center tap (offset 0) always returns 1. sigma = 0 => only the center.
 */
float lumenSpatialFilterSpatialWeight(const int2 offset, const float sigma)
{
    if (sigma <= 0.f)
        return (all(offset == int2(0, 0))) ? 1.f : 0.f;
    const float d2 = float(dot(offset, offset));
    return exp(-d2 / (2.f * sigma * sigma));
}

/** Local luminance variance of the input GI over the window of radius
 *  gNeighborhoodRadius (1 = 3x3, 2 = 5x5), depth-gated so a silhouetted
 *  background texel cannot inflate the estimate.
 *
 *  @param variance   max(0, E[lum^2] - E[lum]^2) over the depth-weighted taps.
 *  @param meanLum    depth-weighted mean luminance (used to normalize the
 *                    variance into a relative quantity).
 */
void lumenSpatialFilterVarianceEstimate(
    const int2 pixel,
    const float pixelZ,
    out float variance,
    out float meanLum
)
{
    variance = 0.f;
    meanLum = 0.f;

    float sumLum = 0.f;
    float sumLumSq = 0.f;
    float sumW = 0.f;

    const int radius = min(int(gNeighborhoodRadius), int(kLumenSpatialFilterMaxVarianceRadius));
    for (int dy = -radius; dy <= radius; ++dy)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            const int2 p = pixel + int2(dx, dy);
            if (any(p < int2(0, 0)) || any(p >= int2(gFrameDim)))
                continue;
            const float z = gLinearZ.Load(int3(p, 0)).x;
            if (z <= 0.f)
                continue;
            // Depth gate: a tap on a different surface contributes ~0 weight.
            const float w = lumenSpatialFilterDepthWeight(pixelZ, z);
            if (w < 1e-3f)
                continue;
            const float4 tapGI = gGIInput.Load(int3(p, 0));
            float tapConfidence;
            if (!lumenSpatialFilterSampleIsValid(p, tapGI, tapConfidence))
                continue;
            const float3 gi = lumenSpatialFilterSanitize(tapGI.rgb);
            const float lum = lumenSpatialFilterLuminance(gi);
            sumLum += w * lum;
            sumLumSq += w * lum * lum;
            sumW += w;
        }
    }

    if (sumW <= kLumenSpatialFilterMinSumWeight)
        return;
    meanLum = sumLum / sumW;
    variance = max(0.f, sumLumSq / sumW - meanLum * meanLum);
}

/** Adaptive filter radius from the relative variance. relVar is the
 *  coefficient-of-variation squared (variance / mean^2), so the thresholds are
 *  unit-independent: relVar <= gVarianceThresholdLow  => gRadiusMin (keep
 *  detail), relVar >= gVarianceThresholdHigh => gRadiusMax (smooth noise).
 */
float lumenSpatialFilterAdaptiveRadius(const float variance, const float meanLum)
{
    const float relVar = variance / max(meanLum * meanLum, max(gVarianceEpsilon, 1e-6f));
    const float range = max(gVarianceThresholdHigh - gVarianceThresholdLow, 1e-6f);
    const float t = clamp((relVar - gVarianceThresholdLow) / range, 0.f, 1.f);
    return lerp(gRadiusMin, gRadiusMax, t);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pixel = int2(dispatchThreadID.xy);
    if (any(pixel >= int2(gFrameDim)))
        return;

    // Required-resource guard: without the core inputs the pass writes zero.
    if (!is_valid(gGIInput) || !is_valid(gLinearZ) || !is_valid(gFilteredOutput))
    {
        if (is_valid(gFilteredOutput))
            gFilteredOutput[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        if (is_valid(gFilteredConfidence))
            gFilteredConfidence[pixel] = 0.f;
        if (is_valid(gFilteredVariance))
            gFilteredVariance[pixel] = 0.f;
        return;
    }

    const float pixelZ = gLinearZ.Load(int3(pixel, 0)).x;
    if (pixelZ <= 0.f)
    {
        // Sky / no surface: nothing to filter.
        gFilteredOutput[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        if (is_valid(gFilteredConfidence))
            gFilteredConfidence[pixel] = 0.f;
        if (is_valid(gFilteredVariance))
            gFilteredVariance[pixel] = 0.f;
        return;
    }

    const float4 curGI = gGIInput.Load(int3(pixel, 0));
    const float3 curRGB = lumenSpatialFilterSanitize(curGI.rgb);
    const float curConf = lumenSpatialFilterConfidence(pixel, curGI.a);
    const float curLum = lumenSpatialFilterLuminance(curRGB);

    // Pass-through mode (disabled / debug): the output is the input, unchanged.
    if (gFilterEnabled == 0u)
    {
        gFilteredOutput[pixel] = float4(curRGB, curConf);
        if (is_valid(gFilteredConfidence))
            gFilteredConfidence[pixel] = curConf;
        if (is_valid(gFilteredVariance))
            gFilteredVariance[pixel] = 0.f;
        return;
    }

    // Optional decode of the current normal / material.
    float3 curNormal = float3(0.f);
    uint curMaterial = kLumenSpatialFilterMaterialIDNone;
    const bool haveNormal = is_valid(gNormalRoughnessMaterialID);
    if (haveNormal)
    {
        const float4 nm = gNormalRoughnessMaterialID.Load(int3(pixel, 0));
        curNormal = lumenSpatialFilterDecodeNormal(nm.xy);
        curMaterial = uint(round(saturate(nm.w) * 3.f));
    }

    // 1) Local spatial variance + mean luminance.
    float localVar;
    float meanLum;
    lumenSpatialFilterVarianceEstimate(pixel, pixelZ, localVar, meanLum);

    // 2) Optional temporal variance from the S5-A1 running moments (contract in
    //    LumenSpatialFilterData.slang): gVariance R32F preferred, else
    //    gMoments RG32F (.x = mean, .y = mean of squares).
    float temporalVar = 0.f;
    if (is_valid(gVariance))
    {
        temporalVar = max(0.f, gVariance.Load(int3(pixel, 0)));
    }
    else if (is_valid(gMoments))
    {
        const float2 m = gMoments.Load(int3(pixel, 0));
        temporalVar = max(0.f, m.y - m.x * m.x);
    }
    const float variance = min(
        max(localVar, max(gTemporalVarianceWeight, 0.f) * temporalVar),
        max(gMaxVarianceClamp, 0.f)
    );

    // 3) Adaptive radius (clamped to the static loop bound).
    const float radius = clamp(lumenSpatialFilterAdaptiveRadius(variance, meanLum),
        0.f, float(kLumenSpatialFilterMaxTapRadius));
    const int radiusInt = int(ceil(radius));
    const float sigma = max(gSpatialSigmaScale, 0.f) * max(radius, 1.f);

    // 4) Bilateral gather over [-radiusInt, +radiusInt]^2. The CENTER tap is
    //    accumulated separately (weight 1) so it can be dropped for fireflies
    //    and its statistics excluded from the firefly detection.
    float3 accRGB = float3(0.f);
    float accConf = 0.f;
    float accW = 0.f;
    float sumLum = 0.f;
    float sumLumSq = 0.f;
    float sumWLum = 0.f;

    for (int dy = -radiusInt; dy <= radiusInt; ++dy)
    {
        for (int dx = -radiusInt; dx <= radiusInt; ++dx)
        {
            const int2 offset = int2(dx, dy);
            if (all(offset == int2(0, 0)))
                continue; // Center handled after the loop.
            const int2 p = pixel + offset;
            if (any(p < int2(0, 0)) || any(p >= int2(gFrameDim)))
                continue;
            const float z = gLinearZ.Load(int3(p, 0)).x;
            if (z <= 0.f)
                continue;

            float w = lumenSpatialFilterSpatialWeight(offset, sigma);
            if (w <= 0.f)
                continue;
            w *= lumenSpatialFilterDepthWeight(pixelZ, z);
            if (haveNormal)
            {
                const float4 nm = gNormalRoughnessMaterialID.Load(int3(p, 0));
                w *= lumenSpatialFilterNormalWeight(curNormal, lumenSpatialFilterDecodeNormal(nm.xy));
                w *= lumenSpatialFilterMaterialWeight(curMaterial, uint(round(saturate(nm.w) * 3.f)));
            }
            if (w <= kLumenSpatialFilterMinSumWeight)
                continue;

            const float4 tapGI = gGIInput.Load(int3(p, 0));
            float conf;
            if (!lumenSpatialFilterSampleIsValid(p, tapGI, conf))
                continue;
            const float3 gi = lumenSpatialFilterSanitize(tapGI.rgb);
            const float lum = lumenSpatialFilterLuminance(gi);

            accRGB += w * gi;
            accConf += w * conf;
            accW += w;
            sumLum += w * lum;
            sumLumSq += w * lum * lum;
            sumWLum += w;
        }
    }

    float3 outRGB;
    float outConf;

    if (accW > kLumenSpatialFilterMinSumWeight)
    {
        const float3 meanRGB = accRGB / accW;
        const float meanConf = accConf / accW;
        const float meanLumN = sumLum / max(sumWLum, 1e-6f);
        const float varN = max(0.f, sumLumSq / max(sumWLum, 1e-6f) - meanLumN * meanLumN);

        // Firefly detection: the center is an outlier when its luminance
        // deviates from the NEIGHBOR mean by more than factor * std. The band
        // uses the larger of the local window std and the combined (spatial +
        // temporal) variance, so a single spike never gets a free pass.
        bool isFirefly = false;
        if (gFireflyClamp != 0u && variance > gVarianceEpsilon)
        {
            const float band = gFireflyStdDevFactor * max(sqrt(varN), sqrt(variance));
            isFirefly = (abs(curLum - meanLumN) > band);
        }

        if (isFirefly)
        {
            // 置中值: replace the outlier with the bilateral neighborhood mean
            // (the outlier's own value is excluded because the center tap was
            // never accumulated).
            outRGB = meanRGB;
            outConf = meanConf;
            // clamp: additionally never exceed the neighborhood mean + factor *
            // std in luminance, so a surviving outlier is bounded.
            const float maxLum = max(meanLumN + gFireflyStdDevFactor * max(sqrt(varN), gVarianceEpsilon), gVarianceEpsilon);
            const float outLum = lumenSpatialFilterLuminance(outRGB);
            if (outLum > maxLum)
                outRGB *= maxLum / max(outLum, 1e-6f);
        }
        else
        {
            // Normal path: blend the center (weight 1) with the neighbors, so a
            // clean low-variance pixel keeps most of its own value (detail
            // preservation) while noisy regions are dominated by the pool.
            const float centerWeight = lumenSpatialFilterSpatialWeight(int2(0, 0), sigma); // == 1
            outRGB = (accRGB + curRGB * centerWeight) / (accW + centerWeight);
            outConf = (accConf + curConf * centerWeight) / (accW + centerWeight);
        }
    }
    else
    {
        // No usable neighbor (isolated pixel, e.g. a 1-texel island or all
        // neighbors sky): keep the input, sanitized.
        outRGB = curRGB;
        outConf = curConf;
    }

    gFilteredOutput[pixel] = float4(lumenSpatialFilterSanitize(outRGB), saturate(outConf));
    if (is_valid(gFilteredConfidence))
        gFilteredConfidence[pixel] = saturate(outConf);
    if (is_valid(gFilteredVariance))
        gFilteredVariance[pixel] = variance;
}
