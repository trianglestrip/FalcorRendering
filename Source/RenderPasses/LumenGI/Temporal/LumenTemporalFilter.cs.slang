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

/** S5-B1: motion-vector temporal filter (Agent Z5).
 *
 *  One thread per output pixel. Stabilizes the noisy per-pixel probe-
 *  interpolated GI (S4-B3 output) across frames by reprojecting the previous
 *  frame's history along the GBufferRT motion vector and blending with an
 *  exponential moving average whose weight depends on a joint validation of
 *  depth / normal / material ID / hit distance / confidence.
 *
 *  ============================================================================
 *  PSEUDO-CODE (the exact contract; also summarized in
 *  LumenTemporalFilterData.slang)
 *  ============================================================================
 *    per pixel P:
 *      curZ = gLinearZ[P].x
 *      if curZ <= 0: write (0, 0, 0, 0)                       // sky / no surface
 *      curGI = gCurrent[P];  curRGB = sanitize(curGI.rgb);  curConf = saturate(curGI.a)
 *      curNormal / curMaterial = decode(gNormalRoughnessMaterialID[P])   // optional
 *      curHitDist = gHitDistance[P].x                                    // optional
 *
 *      mvec   = gMotionVector[P] * gMotionVectorScale
 *      prevUV = curUV + mvec                       // curUV = (P + 0.5) / frameDim
 *      prevPx = prevUV * frameDim
 *      useHistory = (|mvec| <= gMotionLengthThreshold)                  // threshold 0 = off
 *
 *      // 4-tap bilinear history gather, each tap gated by validation weight
 *      (sumRGB, sumHist, sumConf, sumW) = gather4(prevPx, validate)
 *      if sumW < kMinSumWeight:
 *         (sumRGB, sumHist, sumConf, sumW) = crossBilateral3x3(prevPx, validate)
 *      useHistory &= (sumW >= kMinSumWeight)
 *
 *      prevHist = useHistory ? sumHist : 1
 *      prevRGB  = useHistory ? (AABB-clamp(sumRGB, current 3x3) if gClampHistory) : 0
 *      alphaBase = max(gHistoryAlpha, 1 / max(prevHist, 1))            // SVGF equal-weight startup
 *      alpha = useHistory ? lerp(gMaxRejectAlpha, alphaBase, clamp(sumW, 0, 1))
 *                         : gMaxRejectAlpha                            // disocclusion fast response
 *      outRGB = useHistory ? lerp(prevRGB, curRGB, alpha) : curRGB
 *      outHist = useHistory ? min(gHistoryLengthCap, prevHist + 1) : 1
 *      outConf = useHistory ? lerp(sumConf, curConf, alpha) : curConf
 *
 *      gTemporalOutput[P] = float4(sanitize(outRGB), outHist)
 *      gTemporalAlpha[P]        = alpha        // optional
 *      gTemporalConfidence[P]   = outConf      // optional
 *
 *  ============================================================================
 *  VALIDATION WEIGHT per history tap (joint validation of task §10 S5-B1)
 *  ============================================================================
 *        w_tap = wDepth * wNormal * wMaterial * wHitDist * wConf
 *
 *    wDepth    = 1 if gap <= gDepthThreshold else exp(-(gap-gDepthThreshold)*gDepthSigmaInv)
 *                hard 0 if gap / max(curZ, eps) > gDepthRelativeThreshold
 *                (disocclusion / occluder change → reject immediately)
 *    wNormal   = pow(saturate(dot(curN, prevN)), gNormalExponent)   [optional]
 *                hard 0 if dot < gNormalCosMin                        (normal flip)
 *    wMaterial = 1 if equal or unknown else gMaterialMismatchWeight  [optional]
 *    wHitDist  = 1 if |curHit - prevHit| <= gHitDistanceThreshold
 *                else 0                                                [optional]
 *                (different bounce distance ⇒ different surface sampled)
 *    wConf     = saturate(1 - gConfidenceWeight * (1 - curConf))
 *                (low current confidence ⇒ trust history less, respond faster)
 *
 *  ============================================================================
 *  INPUT / OUTPUT CONTRACT (S5-A1 host wiring)
 *  ============================================================================
 *  See LumenTemporalFilterData.slang for the full resource list, formats,
 *  motion-vector convention (prevUV = curUV + mvec, normalized screen space)
 *  and history-length semantics. Required inputs: gCurrent, gLinearZ,
 *  gMotionVector, gPrevDepth, gPrevGI, gTemporalOutput. All optional inputs
 *  (normal/material, hit distance, history length, confidence) skip their
 *  validation stage when unbound.
 *
 *  Dispatch: [numthreads(8,8,1)], ceil(gFrameDim / 8) per axis.
 *  Deterministic: pure function of the inputs; fixed tap order (bilinear then
 *  3x3 cross-bilateral), so results do not depend on thread scheduling.
 *
 *  Camera cut / resize / scene-change reset is owned by S5-A1 (task §10 S5-A1):
 *  it must clear the prev buffers / history to zero on cut, which forces one
 *  disocclusion frame here.
 */

#include "LumenTemporalFilterData.slang"
import Utils.Math.MathHelpers; // oct_to_ndir_unorm

// Optional-resource guards. Falcor specializes is_valid_<name> when it binds
// the resource; local defaults keep the module compilable standalone and
// every read is guarded so an unbound resource is never dereferenced.
#ifndef is_valid_gLinearZ
#define is_valid_gLinearZ 0
#endif
#ifndef is_valid_gCurrent
#define is_valid_gCurrent 0
#endif
#ifndef is_valid_gMotionVector
#define is_valid_gMotionVector 0
#endif
#ifndef is_valid_gPrevDepth
#define is_valid_gPrevDepth 0
#endif
#ifndef is_valid_gPrevGI
#define is_valid_gPrevGI 0
#endif
#ifndef is_valid_gNormalRoughnessMaterialID
#define is_valid_gNormalRoughnessMaterialID 0
#endif
#ifndef is_valid_gPrevNormalRoughnessMaterialID
#define is_valid_gPrevNormalRoughnessMaterialID 0
#endif
#ifndef is_valid_gHitDistance
#define is_valid_gHitDistance 0
#endif
#ifndef is_valid_gPrevHitDistance
#define is_valid_gPrevHitDistance 0
#endif
#ifndef is_valid_gHistoryLength
#define is_valid_gHistoryLength 0
#endif
#ifndef is_valid_gPrevConfidence
#define is_valid_gPrevConfidence 0
#endif
#ifndef is_valid_gTemporalOutput
#define is_valid_gTemporalOutput 0
#endif
#ifndef is_valid_gTemporalAlpha
#define is_valid_gTemporalAlpha 0
#endif
#ifndef is_valid_gTemporalConfidence
#define is_valid_gTemporalConfidence 0
#endif
#ifndef is_valid_gMoments
#define is_valid_gMoments 0
#endif

#define is_valid(name) (is_valid_##name != 0)

/** Maximum representable float, used for the AABB-clamp min/max seeds. */
static const float kLumenTemporalFloatMax = 3.402823466e+38f;

/** Cross-bilateral fallback radius (pixels) around the reprojected location,
 *  SVGF-style recovery when the direct bilinear taps all fail validation. */
static const uint kLumenTemporalCrossBilateralRadius = 1u;

/** Neighborhood radius for the AABB history clamp and the current-frame GI
 *  min/max (3x3 window). */
static const uint kLumenTemporalNeighborhoodRadius = 1u;

/** Sanitize a radiance estimate: clamp negative components to zero, replace
 *  NaN/Inf with zero, and clamp to the firefly ceiling. Mirrors the probe
 *  integrate/interpolate sanitization so the temporal output is guaranteed
 *  finite and non-negative (task §10 S5 gate: no NaN/Inf).
 */
float3 lumenTemporalSanitize(const float3 rgb)
{
    float3 r = max(rgb, float3(0.f));
    if (!any(isnan(r)) && !any(isinf(r)))
        return min(r, float3(max(gFireflyMaxRadiance, 0.f)));
    return float3(0.f);
}

/** Joint validation weight for one history tap. Combines soft decays with hard
 *  rejects (return 0). See the header formula block.
 *
 *  @param haveNormal/haveMaterial/haveHitDist  whether the CURRENT pixel has
 *         the corresponding optional channel (and hence whether to enforce it).
 */
float lumenTemporalValidationWeight(
    const float curZ,
    const float3 curNormal,
    const bool haveNormal,
    const uint curMaterial,
    const bool haveMaterial,
    const float curHitDist,
    const bool haveHitDist,
    const float curConf,
    const float prevZ,
    const float3 prevNormal,
    const bool havePrevNormal,
    const uint prevMaterial,
    const bool havePrevMaterial,
    const float prevHitDist,
    const bool havePrevHitDist
)
{
    if (prevZ <= 0.f)
        return 0.f; // Previous frame had no surface at this location.

    // Hard reject: relative depth jump — the reprojected location sampled a
    // different surface (disocclusion / occluder change).
    if (gDepthRelativeThreshold > 0.f)
    {
        const float gapRel = abs(curZ - prevZ) / max(curZ, 1e-4f);
        if (gapRel > gDepthRelativeThreshold)
            return 0.f;
    }

    // Soft decay: absolute depth gap with a dead zone + exponential falloff.
    const float gap = abs(curZ - prevZ);
    float w = (gap <= gDepthThreshold) ? 1.f : exp(-(gap - gDepthThreshold) * max(gDepthSigmaInv, 0.f));

    // Normal affinity (optional). Hard reject on a normal flip.
    if (haveNormal && havePrevNormal)
    {
        const float ndot = saturate(dot(curNormal, prevNormal));
        if (gNormalCosMin > 0.f && ndot < gNormalCosMin)
            return 0.f;
        w *= pow(ndot, max(gNormalExponent, 0.f));
    }

    // Material-ID consistency (optional). Unknown on either side: no gating.
    if (haveMaterial && havePrevMaterial && gMaterialMismatchWeight > 0.f &&
        curMaterial != kLumenTemporalMaterialIDNone &&
        prevMaterial != kLumenTemporalMaterialIDNone)
    {
        w *= (curMaterial == prevMaterial) ? 1.f : gMaterialMismatchWeight;
    }

    // Hit-distance continuity (optional). A large change in the traced bounce
    // distance means the history sampled a different surface → ghost risk.
    if (haveHitDist && havePrevHitDist && gHitDistanceThreshold > 0.f)
    {
        if (abs(curHitDist - prevHitDist) > gHitDistanceThreshold)
            return 0.f;
    }

    // Confidence gating: low current confidence decays history trust.
    w *= saturate(1.f - max(gConfidenceWeight, 0.f) * (1.f - saturate(curConf)));

    return clamp(w, 0.f, 1.f);
}

/** One validated previous-frame tap loaded at a pixel location. */
struct LumenTemporalPrevTap
{
    float3  giRGB;      ///< Previous smoothed irradiance.
    float   history;    ///< Previous history length at this location.
    float   confidence; ///< Previous confidence.
    float   weight;     ///< Validation weight w_tap (0 = invalid).
    bool    valid;      ///< false when out of screen, no surface, or w_tap == 0.
};

/** Load and validate one previous-frame tap at integer pixel `loc`.
 *  Returns valid=false when the tap is unusable (no surface / rejected).
 */
LumenTemporalPrevTap lumenTemporalLoadPrevTap(
    const int2 loc,
    const float curZ,
    const float3 curNormal,
    const uint curMaterial,
    const float curHitDist,
    const float curConf
)
{
    LumenTemporalPrevTap t = (LumenTemporalPrevTap)0;

    if (any(loc < int2(0, 0)) || any(loc >= int2(gFrameDim)))
        return t;

    const float prevZ = is_valid(gPrevDepth) ? gPrevDepth.Load(int3(loc, 0)) : 0.f;
    if (prevZ <= 0.f)
        return t;

    float3 prevNormal = float3(0.f);
    uint prevMaterial = kLumenTemporalMaterialIDNone;
    const bool havePrevNormal = is_valid(gPrevNormalRoughnessMaterialID);
    if (havePrevNormal)
    {
        const float4 nm = gPrevNormalRoughnessMaterialID.Load(int3(loc, 0));
        prevNormal = normalize(oct_to_ndir_unorm(nm.xy));
        prevMaterial = uint(round(saturate(nm.w) * 3.f));
    }

    const float prevHitDist = is_valid(gPrevHitDistance) ? gPrevHitDistance.Load(int3(loc, 0)).x : 0.f;

    const float w = lumenTemporalValidationWeight(
        curZ, curNormal, is_valid(gNormalRoughnessMaterialID), curMaterial, is_valid(gNormalRoughnessMaterialID),
        curHitDist, is_valid(gHitDistance), curConf,
        prevZ, prevNormal, havePrevNormal, prevMaterial, havePrevNormal,
        prevHitDist, is_valid(gPrevHitDistance));

    if (w <= 0.f)
        return t;

    const float4 prevGI = is_valid(gPrevGI) ? gPrevGI.Load(int3(loc, 0)) : float4(0.f);
    t.giRGB = lumenTemporalSanitize(prevGI.rgb);
    t.history = is_valid(gHistoryLength) ? gHistoryLength.Load(int3(loc, 0))
                                         : prevGI.a; // fallback: history carried in gPrevGI.a
    t.confidence = is_valid(gPrevConfidence) ? gPrevConfidence.Load(int3(loc, 0)) : 1.f;
    t.weight = w;
    t.valid = true;
    return t;
}

/** Compute the per-channel AABB of the current-frame GI in a neighborhood
 *  around `pixel`. Only texels with a valid surface (linearZ > 0) contribute;
 *  returns false when no such texel exists.
 */
bool lumenTemporalCurrentNeighborhoodBox(
    const int2 pixel,
    out float3 boxMin,
    out float3 boxMax
)
{
    boxMin = float3(kLumenTemporalFloatMax);
    boxMax = float3(-kLumenTemporalFloatMax);
    bool found = false;

    for (int dy = -int(kLumenTemporalNeighborhoodRadius); dy <= int(kLumenTemporalNeighborhoodRadius); ++dy)
    {
        for (int dx = -int(kLumenTemporalNeighborhoodRadius); dx <= int(kLumenTemporalNeighborhoodRadius); ++dx)
        {
            const int2 p = pixel + int2(dx, dy);
            if (any(p < int2(0, 0)) || any(p >= int2(gFrameDim)))
                continue;
            if (!is_valid(gLinearZ) || gLinearZ.Load(int3(p, 0)).x <= 0.f)
                continue;
            if (!is_valid(gCurrent))
                continue;
            const float3 gi = lumenTemporalSanitize(gCurrent.Load(int3(p, 0)).rgb);
            boxMin = min(boxMin, gi);
            boxMax = max(boxMax, gi);
            found = true;
        }
    }
    return found;
}

/** AABB-clamp the history RGB to the current-frame neighborhood (TAA-style
 *  anti-ghost). Guards against the history carrying a stale surface / light
 *  change that the per-tap validation missed.
 */
float3 lumenTemporalClampHistory(const int2 pixel, const float3 historyRGB)
{
    float3 boxMin, boxMax;
    if (!lumenTemporalCurrentNeighborhoodBox(pixel, boxMin, boxMax))
        return historyRGB;

    const float3 margin = (boxMax - boxMin) * max(gClampBoxMargin, 0.f);
    return clamp(historyRGB, boxMin - margin, boxMax + margin);
}

/** Gather the previous-frame history at the reprojected pixel coordinate
 *  `prevPx` (fractional, in pixels). Uses a 4-tap bilinear gather where each
 *  tap is gated by its validation weight; falls back to a 3x3 cross-bilateral
 *  search when the direct taps all reject. Accumulates GI / history /
 *  confidence / total weight; `sumW` is the renormalized mean per-tap
 *  validation weight (≈ bilinear sum ∈ [0,1]) and doubles as the "validity"
 *  fraction that raises alpha toward the disocclusion value.
 */
void lumenTemporalGatherHistory(
    const int2 pixel,
    const float2 prevPx,
    const float curZ,
    const float3 curNormal,
    const uint curMaterial,
    const float curHitDist,
    const float curConf,
    out float3 sumRGB,
    out float sumHist,
    out float sumConf,
    out float sumW
)
{
    sumRGB = float3(0.f);
    sumHist = 0.f;
    sumConf = 0.f;
    sumW = 0.f;

    // 4-tap bilinear gather. basePx is the texel-center of the lower-left tap
    // of the 2x2 block containing prevPx.
    const float2 basePx = floor(prevPx - 0.5f) + 0.5f;
    const float2 f = prevPx - basePx;

    for (uint dy = 0u; dy < 2u; ++dy)
    {
        for (uint dx = 0u; dx < 2u; ++dx)
        {
            const float bw = ((dx == 0u) ? (1.f - f.x) : f.x) * ((dy == 0u) ? (1.f - f.y) : f.y);
            if (bw <= 0.f)
                continue;
            const LumenTemporalPrevTap t = lumenTemporalLoadPrevTap(
                int2(basePx) + int2(int(dx), int(dy)), curZ, curNormal, curMaterial, curHitDist, curConf);
            if (!t.valid)
                continue;
            const float wt = bw * t.weight;
            sumRGB += wt * t.giRGB;
            sumHist += wt * t.history;
            sumConf += wt * t.confidence;
            sumW += wt;
        }
    }

    if (sumW >= kLumenTemporalMinSumWeight)
    {
        sumRGB /= sumW;
        sumHist /= sumW;
        sumConf /= sumW;
        return;
    }

    // All direct taps rejected: cross-bilateral recovery around the reprojected
    // pixel (SVGF-style), each neighbor gated by its validation weight only.
    sumRGB = float3(0.f);
    sumHist = 0.f;
    sumConf = 0.f;
    sumW = 0.f;

    const int2 center = int2(prevPx);
    for (int yy = -int(kLumenTemporalCrossBilateralRadius); yy <= int(kLumenTemporalCrossBilateralRadius); ++yy)
    {
        for (int xx = -int(kLumenTemporalCrossBilateralRadius); xx <= int(kLumenTemporalCrossBilateralRadius); ++xx)
        {
            const LumenTemporalPrevTap t = lumenTemporalLoadPrevTap(
                center + int2(xx, yy), curZ, curNormal, curMaterial, curHitDist, curConf);
            if (!t.valid)
                continue;
            sumRGB += t.weight * t.giRGB;
            sumHist += t.weight * t.history;
            sumConf += t.weight * t.confidence;
            sumW += t.weight;
        }
    }

    if (sumW >= kLumenTemporalMinSumWeight)
    {
        sumRGB /= sumW;
        sumHist /= sumW;
        sumConf /= sumW;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pixel = int2(dispatchThreadID.xy);
    if (any(pixel >= int2(gFrameDim)))
        return;

    // Required-resource guard: without the core inputs the pass writes zero.
    if (!is_valid(gLinearZ) || !is_valid(gCurrent) || !is_valid(gMotionVector) ||
        !is_valid(gPrevDepth) || !is_valid(gPrevGI) || !is_valid(gTemporalOutput))
    {
        if (is_valid(gTemporalOutput))
            gTemporalOutput[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        if (is_valid(gTemporalAlpha))
            gTemporalAlpha[pixel] = 1.f;
        if (is_valid(gTemporalConfidence))
            gTemporalConfidence[pixel] = 0.f;
        if (is_valid(gMoments))
            gMoments[pixel] = float2(0.f);
        return;
    }

    const float curZ = gLinearZ.Load(int3(pixel, 0)).x;
    if (curZ <= 0.f)
    {
        // Sky / no surface: nothing to stabilize (history is meaningless for a
        // non-surface pixel; the interpolate pass also wrote zero here).
        gTemporalOutput[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        if (is_valid(gTemporalAlpha))
            gTemporalAlpha[pixel] = 1.f;
        if (is_valid(gTemporalConfidence))
            gTemporalConfidence[pixel] = 0.f;
        if (is_valid(gMoments))
            gMoments[pixel] = float2(0.f);
        return;
    }

    // Current-frame pixel data.
    const float4 curGI = gCurrent.Load(int3(pixel, 0));
    const float3 curRGB = lumenTemporalSanitize(curGI.rgb);
    const float curConf = saturate(curGI.a);

    float3 curNormal = float3(0.f);
    uint curMaterial = kLumenTemporalMaterialIDNone;
    if (is_valid(gNormalRoughnessMaterialID))
    {
        const float4 nm = gNormalRoughnessMaterialID.Load(int3(pixel, 0));
        curNormal = normalize(oct_to_ndir_unorm(nm.xy));
        curMaterial = uint(round(saturate(nm.w) * 3.f));
    }

    const float curHitDist = is_valid(gHitDistance) ? gHitDistance.Load(int3(pixel, 0)).x : 0.f;

    // Reproject: prevUV = curUV + mvec (normalized screen space).
    const float2 mvec = gMotionVector.Load(int3(pixel, 0)).xy * max(gMotionVectorScale, 0.f);
    const bool motionReject = (gMotionLengthThreshold > 0.f) && (length(mvec) > gMotionLengthThreshold);

    bool useHistory = !motionReject;

    float3 sumRGB = float3(0.f);
    float sumHist = 1.f;
    float sumConf = 1.f;
    float sumW = 0.f;

    if (useHistory)
    {
        const float2 prevUV = (float2(pixel) + 0.5f) * gInvFrameDim + mvec;
        const float2 prevPx = prevUV * float2(gFrameDim);
        lumenTemporalGatherHistory(pixel, prevPx, curZ, curNormal, curMaterial, curHitDist, curConf,
            sumRGB, sumHist, sumConf, sumW);
        useHistory = (sumW >= kLumenTemporalMinSumWeight);
    }

    // EMA blend weight. Accepted history: equal-weight startup over a short
    // history (SVGF), then the user base alpha; degraded / disoccluded pixels
    // move toward gMaxRejectAlpha so the current frame responds immediately.
    const float prevHist = useHistory ? sumHist : 1.f;
    const float alphaBase = max(gHistoryAlpha, 1.f / max(prevHist, 1.f));
    const float alpha = useHistory
        ? clamp(lerp(gMaxRejectAlpha, alphaBase, clamp(sumW, 0.f, 1.f)), 0.f, 1.f)
        : gMaxRejectAlpha;

    float3 prevRGB = useHistory ? sumRGB : float3(0.f);
    if (useHistory && gClampHistory != 0u)
        prevRGB = lumenTemporalClampHistory(pixel, prevRGB);

    const float3 outRGB = useHistory ? lerp(prevRGB, curRGB, alpha) : curRGB;
    const float outHist = useHistory ? min(gHistoryLengthCap, prevHist + 1.f) : 1.f;
    const float outConf = useHistory ? lerp(sumConf, curConf, alpha) : curConf;

    // Output: RGB = smoothed irradiance, A = new history length (host stores it
    // for the next frame; the host accumulates its own history from gTemporalAlpha).
    gTemporalOutput[pixel] = float4(lumenTemporalSanitize(outRGB), outHist);
    // C8 running luminance moments. The temporal output alpha is the accepted
    // history length, so a reset (outHist == 1) starts a fresh population and
    // subsequent frames update an online mean / second moment. This resource is
    // intentionally separate from RGBA16F history to preserve variance precision.
    if (is_valid(gMoments))
    {
        const float lum = dot(outRGB, float3(0.2126f, 0.7152f, 0.0722f));
        const float previousCount = max(outHist - 1.f, 0.f);
        if (previousCount <= 0.f)
        {
            gMoments[pixel] = float2(lum, lum * lum);
        }
        else
        {
            const float2 previous = gMoments[pixel];
            const float invCount = 1.f / max(outHist, 1.f);
            gMoments[pixel] = float2(
                (previous.x * previousCount + lum) * invCount,
                (previous.y * previousCount + lum * lum) * invCount
            );
        }
    }
    if (is_valid(gTemporalAlpha))
        gTemporalAlpha[pixel] = alpha;
    if (is_valid(gTemporalConfidence))
        gTemporalConfidence[pixel] = outConf;
}
