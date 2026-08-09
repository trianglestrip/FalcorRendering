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

/** S4-B3: depth / normal / material aware probe interpolation (Agent Z2).
 *
 *  One thread per output pixel. Each pixel blends the 4 nearest probe
 *  irradiance records (S4-B3 integrate output) with a weight that rejects
 *  probes sitting on a different surface than the pixel, then writes the
 *  per-pixel GI estimate (RGB irradiance + confidence) to a screen texture.
 *
 *  ============================================================================
 *  INTERPOLATION WEIGHT FORMULA (the contract this pass implements)
 *  ============================================================================
 *  For a pixel P with linear depth z_p, world normal n_p, material m_p and a
 *  candidate probe Q with depth z_q, normal n_q, material m_q, integrated
 *  irradiance E_q and confidence c_q, the weight is
 *
 *        w_Q = distW_Q · depthW_Q · normalW_Q · materialW_Q
 *
 *  with the four factors (all in (0,1]):
 *
 *    distW_Q  — screen-space bilinear weight over the 2x2 probe grid cell
 *               containing the pixel (probes sit at tile centers, so the 4
 *               nearest probes are the corners of that cell):
 *               probe coords  s = pixel.x / tileSize - 0.5,
 *                              t = pixel.y / tileSize - 0.5,
 *               base = floor((s,t)), frac = (s,t) - base,
 *               distW = (1-fx)(1-fy) | fx(1-fy) | (1-fx)fy | fx·fy
 *               for the taps (base),(base+(1,0)),(base+(0,1)),(base+(1,1)).
 *
 *    depthW_Q  — depth affinity with a dead-zone threshold:
 *               gap   = |z_p - z_q|                     (linear view depth)
 *               depthW = (gap <= gDepthThreshold) ? 1
 *                                                : exp(-(gap - gDepthThreshold) * gDepthSigmaInv)
 *               i.e. 1 within gDepthThreshold meters, exponential falloff
 *               beyond, so coplanar probes dominate and silhouettes cut.
 *
 *    normalW_Q — normal affinity: normalW = pow(saturate(dot(n_p, n_q)),
 *               gNormalExponent). Exponent > 1 sharpens the transition so
 *               orthogonal faces contribute almost nothing.
 *
 *    materialW_Q — material consistency: materialW = (m_p == m_q) ? 1
 *               : gMaterialMismatchWeight. Same GBufferRT two-bit material ID
 *               means 1; a different material gets a small residual weight
 *               (not 0, so unsupported-material pixels still get radiance).
 *
 *  Output
 *        GI_p        = Σ_Q w_Q · E_q / Σ_Q w_Q         (linear irradiance)
 *        conf_p      = Σ_Q w_Q · c_q / Σ_Q w_Q
 *
 *  This pass outputs INCIDENT IRRADIANCE (Σ over probes), not final diffuse
 *  color; the composite stage multiplies by albedo/π. The RGBA16F output
 *  texture holds RGB = GI, A = confidence.
 *
 *  ============================================================================
 *  BOUNDARY / MISSING-PROBE DEGRADATION
 *  ============================================================================
 *  1) Screen edge: probe grid coords are clamped to [0, gridDim-1] before the
 *     validity test, so edge pixels naturally duplicate the edge probes
 *     (fewer, renormalized taps) instead of sampling outside the grid.
 *  2) Grid holes / untraced probes (meta.gridState.w == 0): skipped.
 *  3) All taps rejected (total weight below epsilon, e.g. a large depth gap
 *     on every tap): fall back to a DIRECT SAMPLE of the nearest valid probe,
 *     chosen as the tap with the largest bilinear weight (the geometric
 *     nearest); its confidence is scaled by gFallbackConfidenceScale so the
 *     caller can tell a degraded sample apart.
 *  4) No valid probe within the 4 taps at all: a deterministic ring search
 *     around the pixel's tile, up to kLumenScreenProbeNearestSearchRadiusTiles
 *     tiles away, returns the first valid probe for a direct sample (confidence
 *     scaled as above). If none, the pixel outputs zero (confidence 0).
 *  5) Sky / no surface (gLinearZ.x <= 0): outputs zero GI with confidence 0
 *     (nothing to gather from).
 *
 *  ============================================================================
 *  INPUT / OUTPUT CONTRACT (host wiring, S4 root integration)
 *  ============================================================================
 *  cbuffer LumenScreenProbeInterpolateCB (bound by name):
 *      uint2 gFrameDim;           // full frame dimensions (guards dispatch)
 *      uint2 gProbeGridDim;       // (probeCols, probeRows) = ceil(frameDim/tileSize)
 *      float gDepthThreshold;     // meters; depthW dead zone (default 0.02)
 *      float gDepthSigmaInv;      // 1/meters; depthW falloff beyond the zone (default 4.0)
 *      float gNormalExponent;     // pow exponent on the normal dot (default 8.0)
 *      float gMaterialMismatchWeight; // weight of a material mismatch (default 0.05)
 *      float gFallbackConfidenceScale; // confidence scale for degraded direct samples (default 0.25)
 *      float2 gPad;
 *
 *  Resources (all guarded by is_valid_* defines; unbound optional inputs
 *  make the pass write zero):
 *      StructuredBuffer<LumenScreenProbeMeta>       gProbeMeta;       // required
 *      StructuredBuffer<LumenScreenProbeRadiance>   gProbeRadiance;   // required (integrate output)
 *      Texture2D<float2> gLinearZ;                  // GBufferRT linearZ (.x depth, .y slope; only .x used)
 *      Texture2D<float4> gNormalRoughnessMaterialID;// GBufferRT (.xy oct normal, .w 2-bit material ID)
 *      RWTexture2D<float4> gGIOutput;               // RGBA16F output (RGB = irradiance, A = confidence)
 *
 *  Dispatch: [numthreads(8,8,1)], ceil(gFrameDim / 8) per axis.
 *
 *  Deterministic: pure function of the inputs; the fallback ring search uses a
 *  fixed tap order so results do not depend on thread scheduling.
 *
 *  ALIGNMENT POINTS WITH Z1 (S4-B2 trace) / S4-A2 metadata: the probe records
 *  are defined in LumenScreenProbeData.slang with every field marked
 *  ALIGN(Z1); those structs are the Z2 assumptions and may be renamed by the
 *  root integration when Z1 lands. This pass reads meta.worldPos.w (linear
 *  view depth), meta.normalW.xyz, meta.gridState (col/row/valid), and
 *  meta.maskState.y (material ID), plus the integrate output
 *  LumenScreenProbeRadiance.
 */

#include "LumenScreenProbeData.slang"
import Utils.Math.MathHelpers; // oct_to_ndir_unorm

// Optional-resource guards. Falcor specializes is_valid_<name> when it binds
// the resource; local defaults keep the module compilable standalone and
// every read is guarded so an unbound resource is never dereferenced.
#ifndef is_valid_gProbeMeta
#define is_valid_gProbeMeta 0
#endif
#ifndef is_valid_gProbeRadiance
#define is_valid_gProbeRadiance 0
#endif
#ifndef is_valid_gLinearZ
#define is_valid_gLinearZ 0
#endif
#ifndef is_valid_gNormalRoughnessMaterialID
#define is_valid_gNormalRoughnessMaterialID 0
#endif
#ifndef is_valid_gGIOutput
#define is_valid_gGIOutput 0
#endif

#define is_valid(name) (is_valid_##name != 0)

/** Minimum total weight below which the 4-tap blend is treated as rejected
 *  and the degradation path takes over. */
static const float kLumenScreenProbeInterpMinWeight = 1e-6f;

/** Ring-search radius (in tiles) for the "no valid tap" degradation. */
static const uint kLumenScreenProbeNearestSearchRadiusTiles = 3u;

cbuffer LumenScreenProbeInterpolateCB
{
    uint2 gFrameDim;                    ///< Full frame dimensions; guards the dispatch.
    uint2 gProbeGridDim;                ///< (probeCols, probeRows); clamps probe lookups.
    float gDepthThreshold;              ///< Meters; |z_p - z_q| below this keeps depthW = 1.
    float gDepthSigmaInv;               ///< 1/meters; exponential falloff rate beyond the threshold.
    float gNormalExponent;              ///< pow(saturate(dot(n_p, n_q)), exponent).
    float gMaterialMismatchWeight;      ///< weight for a material-ID mismatch (0 disables the material gate).
    float gFallbackConfidenceScale;     ///< confidence multiplier on degraded direct samples.
    float2 gPad;
};

StructuredBuffer<LumenScreenProbeMeta> gProbeMeta;
StructuredBuffer<LumenScreenProbeRadiance> gProbeRadiance;
Texture2D<float2> gLinearZ;
Texture2D<float4> gNormalRoughnessMaterialID;
RWTexture2D<float4> gGIOutput;

/** Depth affinity factor (see the weight formula in the header). */
float lumenScreenProbeDepthWeight(const float pixelDepth, const float probeDepth)
{
    const float gap = abs(pixelDepth - probeDepth);
    if (gap <= gDepthThreshold)
        return 1.f;
    return exp(-(gap - gDepthThreshold) * max(gDepthSigmaInv, 0.f));
}

/** Normal affinity factor. */
float lumenScreenProbeNormalWeight(const float3 pixelNormal, const float3 probeNormal)
{
    return pow(saturate(dot(pixelNormal, probeNormal)), max(gNormalExponent, 0.f));
}

/** Material affinity factor: 1 for equal IDs, gMaterialMismatchWeight else. */
float lumenScreenProbeMaterialWeight(const uint pixelMaterialID, const uint probeMaterialID)
{
    if (probeMaterialID == kLumenScreenProbeMaterialIDNone || pixelMaterialID == kLumenScreenProbeMaterialIDNone)
        return 1.f; // Unknown material on either side: do not gate on material.
    return (pixelMaterialID == probeMaterialID) ? 1.f : gMaterialMismatchWeight;
}

/** Bilinear screen weight of tap (dx,dy) in {0,1}x{0,1} for frac (fx,fy). */
float lumenScreenProbeBilinearWeight(const uint dx, const uint dy, const float fx, const float fy)
{
    const float wx = (dx == 0u) ? (1.f - fx) : fx;
    const float wy = (dy == 0u) ? (1.f - fy) : fy;
    return wx * wy;
}

/** Clamped probe index for a grid coordinate; returns false when out of
 *  bounds (in which case probeIndex is undefined). */
bool lumenScreenProbeClampCoord(const int2 coord, out uint2 clamped)
{
    clamped = uint2(0, 0);
    const int2 lo = int2(0, 0);
    const int2 hi = int2(gProbeGridDim) - 1;
    if (any(coord < lo) || any(coord > hi))
        return false;
    clamped = uint2(coord);
    return true;
}

/** Direct sample of the nearest valid probe for the degraded path. Returns
 *  false when no valid probe exists within the search radius.
 *
 *  Search order is deterministic: the 4 taps of the pixel's grid cell first,
 *  then concentric rings of tiles around the pixel's tile out to
 *  kLumenScreenProbeNearestSearchRadiusTiles. The first valid probe found is
 *  the result (its bilinear weight is not used here; any valid probe within
 *  the radius is acceptable for the fallback).
 */
bool lumenScreenProbeFindNearestValid(
    const int2 baseCoord,
    const float fx,
    const float fy,
    out uint probeIndex,
    out float distW
)
{
    probeIndex = 0u;
    distW = 0.f;

    // The 4 taps of the containing grid cell, in bilinear-weight order.
    const int2 taps[4] = {
        int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)
    };
    const float tapW[4] = {
        (1.f - fx) * (1.f - fy), fx * (1.f - fy), (1.f - fx) * fy, fx * fy
    };

    // Direct taps: prefer the geometrically nearest (largest bilinear weight).
    uint bestTap = 0u;
    float bestTapW = -1.f;
    uint foundTaps = 0u;
    for (uint i = 0u; i < 4u; ++i)
    {
        uint2 c;
        if (!lumenScreenProbeClampCoord(baseCoord + taps[i], c))
            continue;
        const uint idx = lumenScreenProbeIndex(c.x, c.y, gProbeGridDim.x);
        if (gProbeMeta[idx].gridState.w != 0u && is_valid(gProbeRadiance))
        {
            if (tapW[i] > bestTapW)
            {
                bestTapW = tapW[i];
                bestTap = i;
            }
            ++foundTaps;
        }
    }
    if (foundTaps > 0u)
    {
        uint2 c;
        lumenScreenProbeClampCoord(baseCoord + taps[bestTap], c);
        probeIndex = lumenScreenProbeIndex(c.x, c.y, gProbeGridDim.x);
        distW = tapW[bestTap];
        return true;
    }

    // No valid tap: ring search around the pixel's tile.
    const int2 tileCoord = int2(baseCoord); // baseCoord already == floor of probe coords = the tile containing the pixel's cell origin.
    for (uint radius = 1u; radius <= kLumenScreenProbeNearestSearchRadiusTiles; ++radius)
    {
        const int r = int(radius);
        // Four sides of the ring (deterministic order).
        for (int s = -r; s <= r; ++s)
        {
            int2 cand;
            // top row    (tileY = tileCoord.y - r)
            cand = tileCoord + int2(s, -r);
            uint2 c;
            if (lumenScreenProbeClampCoord(cand, c))
            {
                const uint idx = lumenScreenProbeIndex(c.x, c.y, gProbeGridDim.x);
                if (gProbeMeta[idx].gridState.w != 0u)
                {
                    probeIndex = idx;
                    distW = 0.f;
                    return true;
                }
            }
            // bottom row (tileY = tileCoord.y + r)
            cand = tileCoord + int2(s, r);
            if (lumenScreenProbeClampCoord(cand, c))
            {
                const uint idx = lumenScreenProbeIndex(c.x, c.y, gProbeGridDim.x);
                if (gProbeMeta[idx].gridState.w != 0u)
                {
                    probeIndex = idx;
                    distW = 0.f;
                    return true;
                }
            }
            // left column (x = tileCoord.x - r), skipping the corners already done.
            cand = tileCoord + int2(-r, s);
            if (s > -r && s < r && lumenScreenProbeClampCoord(cand, c))
            {
                const uint idx = lumenScreenProbeIndex(c.x, c.y, gProbeGridDim.x);
                if (gProbeMeta[idx].gridState.w != 0u)
                {
                    probeIndex = idx;
                    distW = 0.f;
                    return true;
                }
            }
            // right column (x = tileCoord.x + r)
            cand = tileCoord + int2(r, s);
            if (s > -r && s < r && lumenScreenProbeClampCoord(cand, c))
            {
                const uint idx = lumenScreenProbeIndex(c.x, c.y, gProbeGridDim.x);
                if (gProbeMeta[idx].gridState.w != 0u)
                {
                    probeIndex = idx;
                    distW = 0.f;
                    return true;
                }
            }
        }
    }

    return false;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= gFrameDim))
        return;

    if (!is_valid(gProbeMeta) || !is_valid(gProbeRadiance) || !is_valid(gLinearZ) || !is_valid(gNormalRoughnessMaterialID) || !is_valid(gGIOutput))
    {
        gGIOutput[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    const float pixelDepth = gLinearZ[pixel].x;
    if (pixelDepth <= 0.f)
    {
        // Sky / no surface: nothing to gather.
        gGIOutput[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    const float3 pixelNormal = normalize(oct_to_ndir_unorm(gNormalRoughnessMaterialID[pixel].xy));
    const uint pixelMaterialID = uint(round(saturate(gNormalRoughnessMaterialID[pixel].w) * 3.f));

    // Probe-space coordinates: probe (c,r) sits at integer (c,r), pixel texel
    // (px,py) maps to (px / tileSize - 0.5, py / tileSize - 0.5). The 4
    // nearest probes are the corners of the cell containing (s,t).
    const float s = float(pixel.x) / float(kLumenScreenProbeTileSize) - 0.5f;
    const float t = float(pixel.y) / float(kLumenScreenProbeTileSize) - 0.5f;
    const float baseS = floor(s);
    const float baseT = floor(t);
    const int2 baseCoord = int2(int(baseS), int(baseT));
    const float fx = s - baseS;
    const float fy = t - baseT;

    float3 accRadiance = float3(0.f);
    float accConfidence = 0.f;
    float accWeight = 0.f;

    for (uint dy = 0u; dy < 2u; ++dy)
    {
        for (uint dx = 0u; dx < 2u; ++dx)
        {
            uint2 c;
            if (!lumenScreenProbeClampCoord(baseCoord + int2(dx, dy), c))
                continue;
            const uint probeIndex = lumenScreenProbeIndex(c.x, c.y, gProbeGridDim.x);
            const LumenScreenProbeMeta meta = gProbeMeta[probeIndex];
            if (meta.gridState.w == 0u)
                continue; // Probe not placed / not traced this frame.

            const float distW = lumenScreenProbeBilinearWeight(dx, dy, fx, fy);
            if (distW <= 0.f)
                continue;

            const float depthW = lumenScreenProbeDepthWeight(pixelDepth, meta.worldPos.w);
            const float normalW = lumenScreenProbeNormalWeight(pixelNormal, normalize(meta.normalW.xyz));
            const float materialW = lumenScreenProbeMaterialWeight(pixelMaterialID, meta.maskState.y);

            const float w = distW * depthW * normalW * materialW;
            if (w <= kLumenScreenProbeInterpMinWeight)
                continue;

            const LumenScreenProbeRadiance pr = gProbeRadiance[probeIndex];
            accRadiance += w * pr.radiance.rgb;
            accConfidence += w * saturate(pr.radiance.a);
            accWeight += w;
        }
    }

    if (accWeight > kLumenScreenProbeInterpMinWeight)
    {
        // Normal blend path.
        gGIOutput[pixel] = float4(accRadiance / accWeight, accConfidence / accWeight);
        return;
    }

    // Degraded path: direct sample of the nearest valid probe.
    uint fallbackIndex;
    float fallbackDistW;
    if (lumenScreenProbeFindNearestValid(baseCoord, fx, fy, fallbackIndex, fallbackDistW))
    {
        const LumenScreenProbeRadiance pr = gProbeRadiance[fallbackIndex];
        const float confidence = saturate(pr.radiance.a) * max(gFallbackConfidenceScale, 0.f);
        gGIOutput[pixel] = float4(pr.radiance.rgb, confidence);
        return;
    }

    // No valid probe anywhere near: zero GI, zero confidence.
    gGIOutput[pixel] = float4(0.f, 0.f, 0.f, 0.f);
}
