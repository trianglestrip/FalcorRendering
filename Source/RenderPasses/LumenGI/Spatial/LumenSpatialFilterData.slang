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

/** S5-B2: variance-guided spatial filter data contract (Agent Z8, S5-B2 module).
 *
 *  Shared input/output contract for the S5-B2 spatial/adaptive filter compute
 *  pass (LumenSpatialFilter.cs.slang) and its S5-A1 reconstruction host. It
 *  declares ONLY data (constants / CB / resources); no functions and no entry
 *  point, so it can be `#include`d by the compute entry and, by the same field
 *  names, mirrored by the host when it wires the pass (LumenReconstruction.h
 *  carries the byte-for-byte CPU mirror + size static_assert).
 *
 *  ============================================================================
 *  WHAT THIS PASS IS
 *  ============================================================================
 *  The S5-B1 temporal filter (LumenTemporalFilter) stabilizes the noisy
 *  per-pixel probe-interpolated GI across frames and stores it in
 *  gTemporalOutput (RGB16F: RGB = irradiance, A = history length). S5-B2
 *  removes the residual spatial noise with a VARIANCE-GUIDED ADAPTIVE
 *  bilateral filter: the filter radius grows with the local radiance variance
 *  (noisy regions blur more, clean low-variance regions keep their detail),
 *  every tap is weighted by depth / normal / material affinity so the filter
 *  does NOT smear across geometry or material boundaries, and outlier pixels
 *  (fireflies) are detected against the neighborhood statistics and replaced /
 *  clamped so a single spike cannot propagate.
 *
 *  It is a PURE GPU compute pass: no host interaction, no randomization, one
 *  thread per output pixel. The host (S5-A1) only owns the buffers, the CB
 *  values, the resolution scaling (see LumenReconstruction.h) and the camera-
 *  cut / resize reset.
 *
 *  ============================================================================
 *  VARIANCE CONTRACT (S5-A1 moments, comment contract for the host)
 *  ============================================================================
 *  Two sources of per-pixel variance feed the adaptive radius:
 *
 *    1) SPATIAL (always available): the pass computes the luminance variance
 *       of the input GI over a small window (radius gNeighborhoodRadius,
 *       1 => 3x3, 2 => 5x5), gated by the depth affinity so a silhouetted
 *       background texel does not inflate the estimate:
 *           vLocal = max(0, E[lum^2] - E[lum]^2)     (depth-weighted)
 *
 *    2) TEMPORAL (optional, S5-A1 provides the running moments): either
 *           gVariance  R32F  — precomputed running per-pixel luminance
 *                              variance (variance of the irradiance luminance,
 *                              e.g. accumulated by S5-A1 from its own history
 *                              EMA, then subtracted of the signal mean), or
 *           gMoments   RG32F  — running moments: .x = running mean luminance,
 *                              .y = running mean of the squared luminance;
 *                              the shader derives
 *                                  vTemporal = max(0, .y - .x*.x)
 *       The two are mutually exclusive by contract; if both are bound the
 *       shader prefers gVariance. When neither is bound the filter is purely
 *       spatial (vTemporal = 0).
 *
 *  The COMBINED variance used for the adaptive radius and the firefly band is
 *        variance = clamp(max(vLocal, gTemporalVarianceWeight * vTemporal),
 *                        0, gMaxVarianceClamp)
 *  (a maximum, so the more pessimistic estimate wins; scaling the temporal
 *  variance lets the host trade history noise against current-frame noise).
 *  The radius is a function of the RELATIVE variance
 *        relVar = variance / max(meanLum^2, gVarianceEpsilon)
 *  so the gVarianceThresholdLow/High parameters are unit-independent
 *  "coefficient-of-variation squared" thresholds (e.g. 0.01 == 10% RMS noise).
 *  The estimated `variance` is optionally written to gFilteredVariance R32F so
 *  the host / debug views can inspect the adaptive behavior.
 *
 *  ============================================================================
 *  SPATIAL FILTER FORMULA (see LumenSpatialFilter.cs.slang for the full
 *  pseudo-code; this is the contract the host relies on)
 *  ============================================================================
 *  Per pixel P with linear depth z_p, decoded world normal n_p, material m_p:
 *    1) r = clamp(lerp(gRadiusMin, gRadiusMax, saturate((relVar - low) /
 *       (high - low))), 0, kLumenSpatialFilterMaxTapRadius)   // adaptive radius
 *    2) Bilateral gather over [-ceil(r), +ceil(r)]^2; for a tap Q at offset d:
 *         w_Q = spatialW_Q * depthW_Q * normalW_Q * materialW_Q
 *         spatialW_Q = exp(-|d|^2 / (2 sigma^2)),  sigma = gSpatialSigmaScale * r
 *         depthW_Q   = 1 if |z_p - z_Q| <= gDepthThreshold else
 *                      exp(-(|z_p - z_Q| - gDepthThreshold) * gDepthSigmaInv)
 *         normalW_Q  = pow(saturate(dot(n_p, n_Q)), gNormalExponent)   [optional]
 *         materialW_Q = 1 if equal or unknown else gMaterialMismatchWeight
 *                                                                      [optional]
 *       and the CENTER tap (Q == P) is accumulated separately with weight 1 so
 *       a clean low-variance pixel keeps most of its own value.
 *    3) Firefly handling (gFireflyClamp != 0): the center is an outlier when
 *         |lum_p - meanLum_neighbors| > gFireflyStdDevFactor * max(sqrt(vLocal),
 *         sqrt(variance)). Outlier pixels are REPLACED by the bilateral
 *         neighborhood mean ("置中值") and the result is CLAMPED so its
 *         luminance never exceeds meanLum + gFireflyStdDevFactor * std (the
 *         "clamp" variant). Non-outlier pixels blend center + neighbors.
 *    4) Output: gFilteredOutput.rgb = sanitized filtered irradiance,
 *       gFilteredOutput.a = filtered confidence.
 *
 *  ============================================================================
 *  BUFFER CONTRACT (what S5-A1 must allocate; all formats frozen)
 *  ============================================================================
 *  Required (binding without these makes the pass write zeros):
 *      gGIInput        Texture2D<float4> RGBA16F  current GI to filter: .rgb =
 *                                                  irradiance, .a = CONFIDENCE
 *                                                  (a trust scalar in [0,1], NOT
 *                                                  the S5-B1 history length).
 *                                                  Confidence SOURCE (host
 *                                                  decides; the filter only
 *                                                  reads it): the S4-B3
 *                                                  interpolate output
 *                                                  (gGIOutput.a) when no
 *                                                  temporal pass is wired, the
 *                                                  S5-B1 gTemporalConfidence
 *                                                  R32F (see gConfidenceInput
 *                                                  below; this is the Z10/S5-B2
 *                                                  chosen source, because the
 *                                                  S5-B1 gTemporalOutput.a
 *                                                  history length is only a
 *                                                  degraded proxy), or the
 *                                                  history length as a last
 *                                                  resort. It is passed through
 *                                                  to gFilteredOutput.a /
 *                                                  gFilteredConfidence.
 *      gLinearZ        Texture2D<float2> RG32F    GBufferRT linearZ, .x = linear
 *                                                  depth (m), .y slope (unused).
 *      gFilteredOutput RWTexture2D<float4> RGBA16F output: .rgb = filtered
 *                                                  irradiance, .a = filtered
 *                                                  confidence.
 *  Optional (unbound => the corresponding gating / output is skipped):
 *      gNormalRoughnessMaterialID Texture2D<float4> GBufferRT: .xy = oct normal
 *                                                  (unorm), .w = 2-bit material
 *                                                  ID (0..3). Unbound => normal
 *                                                  + material gating disabled.
 *      gVariance      Texture2D<float> R32F       S5-A1 running luminance
 *                                                  variance (see the contract).
 *      gMoments       Texture2D<float2> RG32F     S5-A1 running moments
 *                                                  (.x mean, .y mean of squares).
 *      gFilteredConfidence RWTexture2D<float> R32F filtered confidence
 *                                                  (diagnostics / composite).
 *      gFilteredVariance   RWTexture2D<float> R32F combined variance estimate
 *                                                  (diagnostics / debug view).
 *
 *  ============================================================================
 *  CONFIDENCE SOURCE (S5-B2 host wiring, Z10 decision -- read by the shader)
 *  ============================================================================
 *  gConfidenceInput Texture2D<float> R32F  optional S5-A1 per-pixel confidence.
 *      The S5-B1 temporal filter writes its updated confidence to the R32F
 *      "temporalConfidence" graph channel. When this input is valid it
 *      OVERRIDES gGIInput.a as the per-pixel confidence source for the center
 *      pixel and every gather tap (gGIInput is the S5-B1 temporal OUTPUT, whose
 *      .a channel carries the HISTORY LENGTH, not a confidence). Unbound =>
 *      the filter falls back to gGIInput.a (the degraded proxy / the S4-B3
 *      interpolate confidence when the temporal pass is not wired). The value
 *      is clamped to [0,1] at read time and passed through to gFilteredOutput.a
 *      / gFilteredConfidence.
 *
 *  Dispatch: [numthreads(8,8,1)], ceil(gFrameDim / 8) per axis. The pass is
 *  resolution-agnostic: gFrameDim is whatever GI resolution the S5-A2 host
 *  selected (full / half / quarter); the half/quarter -> full upscale is a
 *  separate step driven by LumenReconstruction.h.
 *  Deterministic: pure function of the inputs; fixed tap order (variance window
 *  then filter window), so results do not depend on thread scheduling.
 */

#ifndef __LUMEN_SPATIAL_FILTER_DATA_SLANG__
#define __LUMEN_SPATIAL_FILTER_DATA_SLANG__

// Radiance ceiling mirror from LumenGIData.slang (kLumenGIMaxRadiance);
// overridable by the host through the same define. Used as the firefly absolute
// ceiling (gFireflyMaxRadiance) and to sanitize the output.
#ifndef LUMEN_GI_MAX_RADIANCE
#define LUMEN_GI_MAX_RADIANCE 10.f
#endif

/** Minimum accumulated weight for a tap to contribute to the bilateral gather
 *  (also the floor for the renormalization sums). */
static const float kLumenSpatialFilterMinSumWeight = 1e-4f;

/** Compile-time bound on the adaptive radius in pixels (loop bound of the
 *  bilateral gather: [-4, +4]^2 == a 9x9 window). The CB radius is clamped to
 *  this so the loop is statically bounded and deterministic. */
static const uint kLumenSpatialFilterMaxTapRadius = 4u;

/** Compile-time bound on the variance-window radius (loop bound; supports
 *  3x3 for gNeighborhoodRadius = 1 and 5x5 for = 2). */
static const uint kLumenSpatialFilterMaxVarianceRadius = 2u;

/** Material-ID value treated as "unknown / no material" (matches the GBufferRT
 *  two-bit ID convention: real IDs are 0..3, this sentinel is outside that
 *  range). Material gating is skipped when either side is unknown. */
static const uint kLumenSpatialFilterMaterialIDNone = 0xFFFFFFFFu;

/** Constant buffer filled by the host every dispatch. All values are named and
 *  set through the ShaderVar binding (Falcor sets by name, not raw copy). The
 *  "+N" comments give the byte offset used by the D3D constant-buffer layout so
 *  the C++ mirror (LumenReconstruction::SpatialFilterConstantBuffer) can be
 *  kept in sync; a static_assert checks the size.
 */
cbuffer LumenSpatialFilterCB
{
    uint2  gFrameDim;                // +0   Frame (GI) dims in pixels; guards the dispatch.
    uint   gFilterEnabled;           // +8   != 0: run the filter; 0: pass-through copy (debug).
    uint   gFireflyClamp;            // +12  != 0: enable firefly replace + clamp. Default 1.
    float  gFireflyMaxRadiance;      // +16  Absolute per-channel ceiling on the output (default LUMEN_GI_MAX_RADIANCE = 10).
    float  gFireflyStdDevFactor;     // +20  Outlier band: |lum - mean| > factor * std (default 4.0).
    float  gVarianceThresholdLow;    // +24  Relative variance below which radius = gRadiusMin (default 0.0).
    float  gVarianceThresholdHigh;   // +28  Relative variance above which radius = gRadiusMax (default 0.20).
    float  gRadiusMin;               // +32  Minimum filter radius in pixels (default 0 = keep detail in clean regions).
    float  gRadiusMax;               // +36  Maximum filter radius in pixels (default 4, clamped to kLumenSpatialFilterMaxTapRadius).
    float  gSpatialSigmaScale;       // +40  Gaussian sigma of the spatial term = scale * radius (default 0.75).
    float  gDepthThreshold;          // +44  Meters; |z_p - z_Q| dead zone below which depthW = 1 (default 0.05).
    float  gDepthSigmaInv;           // +48  1/meters; depthW exponential falloff rate beyond the dead zone (default 8.0).
    float  gNormalExponent;          // +52  pow(saturate(dot(n_p, n_Q)), exponent) (default 8.0).
    float  gMaterialMismatchWeight;  // +56  Weight of a material-ID mismatch (default 0.05). 0 disables material gating.
    float  gTemporalVarianceWeight;  // +60  Multiplier on the S5-A1 temporal variance before max-merge (default 1.0). 0 = spatial only.
    float  gMaxVarianceClamp;        // +64  Clamp on the combined variance estimate (numerical guard, default 1e4).
    float  gVarianceEpsilon;         // +68  Floor for variance / mean^2 division and the firefly band (default 1e-6).
    float2 gInvFrameDim;             // +72  1 / frame dims (UV math).
    uint   gNeighborhoodRadius;      // +80  Variance window radius (default 2 = 5x5; clamped to kLumenSpatialFilterMaxVarianceRadius).
    uint   gPad0;                    // +84
    uint   gPad1;                    // +88
    uint   gPad2;                    // +92
};  // 96 bytes.

/** Input / output resources. Every field the shader reads has an is_valid
 *  guard (defaults defined in the compute entry) so unbound optional inputs
 *  compile and no-op; required ones make the pass write zeros.
 */
Texture2D<float4>  gGIInput;                    // required; current GI to filter (RGB + confidence in .a).
Texture2D<float2>  gLinearZ;                    // required; current depth (.x).
Texture2D<float4>  gNormalRoughnessMaterialID;  // optional; current normal (.xy oct) / material (.w).
Texture2D<float>   gConfidenceInput;            // optional; S5-A1 per-pixel confidence R32F (temporalConfidence graph channel). Overrides gGIInput.a when valid.
Texture2D<float>   gVariance;                   // optional; S5-A1 running per-pixel luminance variance R32F.
Texture2D<float2>  gMoments;                    // optional; S5-A1 running moments (.x mean, .y mean of squares).
RWTexture2D<float4> gFilteredOutput;            // required; filtered GI (.a = filtered confidence).
RWTexture2D<float>  gFilteredConfidence;        // optional; filtered confidence R32F.
RWTexture2D<float>  gFilteredVariance;          // optional; combined variance estimate R32F (diagnostics).

#endif // __LUMEN_SPATIAL_FILTER_DATA_SLANG__
