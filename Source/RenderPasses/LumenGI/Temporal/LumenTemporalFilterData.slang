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

/** S5-B1: temporal filter data contract (Agent Z5, S5-B1 module).
 *
 *  This is the shared input/output contract for the S5-B1 temporal filter
 *  compute pass (LumenTemporalFilter.cs.slang) and its S5-A1 history host.
 *  It declares ONLY data (constants / CB / resources); no functions and no
 *  entry point, so it can be `#include`d by the compute entry and, by the
 *  same field names, mirrored by the host when it wires the pass.
 *
 *  ============================================================================
 *  WHAT THIS PASS IS
 *  ============================================================================
 *  The S4-B3 interpolate pass (LumenScreenProbeInterpolate) writes a noisy
 *  per-pixel probe-interpolated GI estimate (RGB16F: RGB = incident
 *  irradiance, A = confidence). S5-B1 stabilizes it across frames with a
 *  motion-vector reprojected exponential moving average, validating the
 *  history sample with depth / normal / material-ID / hit-distance /
 *  confidence so rejected pixels (disocclusion, occluder change, moving
 *  object) respond fast instead of ghosting.
 *
 *  It is a PURE GPU compute pass: no host interaction, no randomization, one
 *  thread per output pixel. The host (S5-A1) only owns the buffers, the CB
 *  values and the double-buffering / camera-cut reset.
 *
 *  ============================================================================
 *  MOTION VECTOR CONVENTION (frozen)
 *  ============================================================================
 *  gMotionVector is the GBufferRT RG32F screen-space motion vector produced
 *  by calcMotionVector() (Falcor MathHelpers): a value in NORMALIZED screen
 *  space [0,1]^2 (origin top-left) pointing from the current pixel to where
 *  that pixel was in the PREVIOUS frame:
 *
 *        mvec = prevUV - curUV            (normalized units)
 *        prevUV = curUV + gMotionVectorScale * mvec
 *        curUV  = (pixel + 0.5) / gFrameDim
 *
 *  The shader converts prevUV back to pixel coordinates
 *  prevPx = prevUV * gFrameDim and gathers history with manual 4-tap
 *  bilinear weights (no sampler required). If a host instead feeds pixel-space
 *  motion, set gMotionVectorScale = 1 / gFrameDim (or set the scale to the
 *  reciprocal of the motion's pixel range) — the scale is provided so the
 *  convention can be adapted without a shader change.
 *
 *  ============================================================================
 *  TEMPORAL FILTER FORMULA (see LumenTemporalFilter.cs.slang for the full
 *  pseudo-code; this is the contract the host relies on)
 *  ============================================================================
 *  Per pixel P:
 *    1) Read current data: curZ (gLinearZ.x), curNormal / curMaterial
 *       (gNormalRoughnessMaterialID, optional), curHitDist (gHitDistance,
 *       optional), curGI = (curRGB, curConf) from gCurrent.
 *    2) Reproject: prevUV = curUV + mvec; prevPx = prevUV * gFrameDim.
 *    3) Validation (per history tap): a per-tap weight w_tap in [0,1]
 *       combining hard rejects (return 0) and soft decays (see below).
 *    4) Gather: 4-tap bilinear history at prevPx, each tap weighted by its
 *       bilinear weight x w_tap, accumulating GI / history / confidence.
 *       If the renormalized sum is too small, a 3x3 cross-bilateral fallback
 *       around prevPx is tried (SVGF-style). If still nothing validates, the
 *       pixel is treated as DISOCCLUSION: history is discarded.
 *    5) EMA blend: outRGB = lerp(prevRGB, curRGB, alpha) with
 *          alpha      = max(gHistoryAlpha, 1 / max(prevHistory, 1))   (accepted)
 *          alpha      = lerp(gMaxRejectAlpha, alphaBase, validity)     (soft reject)
 *          alpha      = gMaxRejectAlpha                                 (disocclusion)
 *       and optionally the history RGB is AABB-clamped to the current 3x3
 *       neighborhood first (gClampHistory, TAA-style anti-ghost).
 *    6) Output: gTemporalOutput.rgb = sanitized outRGB,
 *       gTemporalOutput.a = NEW history length (clamped to gHistoryLengthCap).
 *
 *  Validation weight per tap (the "联合验证" of task §10 S5-B1):
 *        w_tap = wDepth * wNormal * wMaterial * wHitDist * wConf
 *    wDepth    = 1 if |curZ - prevZ| <= gDepthThreshold else
 *                exp(-(|curZ - prevZ| - gDepthThreshold) * gDepthSigmaInv)
 *                hard 0 if |curZ - prevZ| / curZ > gDepthRelativeThreshold
 *    wNormal   = pow(saturate(dot(curN, prevN)), gNormalExponent), optional
 *                hard 0 if dot < gNormalCosMin
 *    wMaterial = 1 if equal or unknown else gMaterialMismatchWeight (optional)
 *    wHitDist  = 1 if |curHit - prevHit| <= gHitDistanceThreshold else 0
 *                (optional; guards against sampling a different bounce)
 *    wConf     = saturate(1 - gConfidenceWeight * (1 - curConf))
 *
 *  HISTORY LENGTH SEMANTICS (S5-A1 host ownership)
 *  ============================================================================
 *  The host (S5-A1) maintains the per-pixel history length and the ping-pong
 *  prev buffers. The shader READS the previous history length at the
 *  reprojected location (from gHistoryLength, or gPrevGI.a when the separate
 *  buffer is unbound) and WRITES the new history length into
 *  gTemporalOutput.a. The host stores it for the next frame (this is the
 *  "history double buffer"). The effective EMA weight used for the blend is
 *  also exposed through the optional gTemporalAlpha R32F so the host can
 *  accumulate its own history as
 *        history += 1                            (accepted)
 *        history  = 1                            (disocclusion / reset)
 *  Camera-cut / resize / scene-change reset is the HOST's job (task §10 S5-A1):
 *  on cut the host must clear gPrevGI / gPrevDepth to zero and gHistoryLength
 *  to 0, which makes every pixel take the disocclusion path for one frame.
 *
 *  ============================================================================
 *  BUFFER CONTRACT (what S5-A1 must allocate; all formats frozen)
 *  ============================================================================
 *  Required (binding without these makes the pass write zeros):
 *      gCurrent        Texture2D<float4> RGBA16F   current interpolated GI
 *                                                  (S4-B3 output) .rgb = irradiance
 *                                                  .a = confidence.
 *      gLinearZ        Texture2D<float2> RG32F     GBufferRT linearZ, .x = linear
 *                                                  depth (m), .y slope (unused).
 *      gMotionVector   Texture2D<float2> RG32F     GBufferRT motion (normalized,
 *                                                  see above). Written by GBufferRT.
 *      gPrevDepth      Texture2D<float>  R32F      previous frame linear depth
 *                                                  (S5-A1 copy of gLinearZ.x).
 *      gPrevGI         Texture2D<float4> RGBA16F   previous temporal output
 *                                                  (S5-A1 double buffer): .rgb =
 *                                                  smoothed irradiance, .a = prev
 *                                                  history length.
 *      gTemporalOutput RWTexture2D<float4> RGBA16F output: .rgb = smoothed
 *                                                  irradiance, .a = new history
 *                                                  length (capped).
 *  Optional (unbound => the corresponding validation is skipped / output skipped):
 *      gNormalRoughnessMaterialID     Texture2D<float4> GBufferRT: .xy = oct
 *                                                  normal (unorm), .w = 2-bit
 *                                                  material ID (0..3).
 *      gPrevNormalRoughnessMaterialID Texture2D<float4> previous-frame copy of the
 *                                                  same; enables normal + material
 *                                                  validation.
 *      gHitDistance     Texture2D<float4> current per-pixel hit distance (reads .x;
 *                                                  bind R32F or the first channel of
 *                                                  the S4 RGBA16F hit-distance texture).
 *      gPrevHitDistance Texture2D<float4> previous hit distance (same encoding).
 *      gHistoryLength   Texture2D<float>  R32F previous history length; when unbound
 *                                                  the shader falls back to gPrevGI.a.
 *      gPrevConfidence  Texture2D<float>  R32F previous per-pixel confidence; when
 *                                                  unbound, prev confidence is assumed 1.
 *      gTemporalAlpha       RWTexture2D<float>  R32F effective EMA alpha (diagnostics /
 *                                                  host history accumulation).
 *      gTemporalConfidence  RWTexture2D<float>  R32F updated confidence for S5-B2.
 *
 *  Dispatch: [numthreads(8,8,1)], ceil(gFrameDim / 8) per axis.
 *  Deterministic: pure function of the inputs; fixed tap order everywhere.
 */

#ifndef __LUMEN_TEMPORAL_FILTER_DATA_SLANG__
#define __LUMEN_TEMPORAL_FILTER_DATA_SLANG__

// Radiance ceiling mirror from LumenGIData.slang (kLumenGIMaxRadiance);
// overridable by the host through the same define. Used as the firefly clamp
// default (gFireflyMaxRadiance) and to sanitize the output.
#ifndef LUMEN_GI_MAX_RADIANCE
#define LUMEN_GI_MAX_RADIANCE 10.f
#endif

/** Minimum accumulated validation weight for a history gather to be accepted.
 *  Below this the pixel is treated as disocclusion. */
static const float kLumenTemporalMinSumWeight = 1e-4f;

/** Material-ID value treated as "unknown / no material" (matches the GBufferRT
 *  two-bit ID convention where 0 is the default / unset). Material gating is
 *  skipped when either side is unknown. */
static const uint kLumenTemporalMaterialIDNone = 0u;

/** Constant buffer filled by the host every dispatch. All values are named and
 *  set through the ShaderVar binding (Falcor sets by name, not raw copy). The
 *  "+N" comments give the byte offset used by the D3D constant-buffer layout so
 *  a C++ mirror struct can be kept in sync if desired.
 */
cbuffer LumenTemporalFilterCB
{
    uint2  gFrameDim;                // +0   Frame dims in pixels; guards the dispatch.
    uint   gFrameIndex;              // +8   Frame index (diagnostics / reserved).
    uint   gClampHistory;            // +12  != 0: AABB-clamp history RGB to the current 3x3 neighborhood (TAA anti-ghost). Default 1.
    float  gHistoryAlpha;            // +16  Base EMA weight toward the current frame (0.1). Larger = faster, noisier.
    float  gHistoryLengthCap;        // +20  Cap for the output history length (UE-style default 10).
    float  gClampBoxMargin;          // +24  AABB clamp margin as a fraction of the neighborhood range (0.0 = exact min/max).
    float  gDepthThreshold;          // +28  Meters; |curZ - prevZ| dead zone below which depthW = 1 (0.05).
    float  gDepthSigmaInv;           // +32  1/meters; depthW exponential falloff rate beyond the dead zone (8.0).
    float  gDepthRelativeThreshold;  // +36  Hard reject when |curZ - prevZ| / curZ exceeds this (0.05). 0 disables.
    float  gNormalCosMin;            // +40  Hard reject when dot(curNormal, prevNormal) < this (0.8). 0 disables.
    float  gNormalExponent;          // +44  pow(saturate(dot(norms)), exponent) for the normal decay (8.0).
    float  gMaterialMismatchWeight;  // +48  Weight of a material-ID mismatch (0.05). 0 disables material gating entirely.
    float  gHitDistanceThreshold;    // +52  Meters; hard reject when |curHit - prevHit| exceeds this (0.5). 0 disables.
    float  gConfidenceWeight;        // +56  Confidence gating strength in wConf (1.0). 0 disables.
    float  gMaxRejectAlpha;          // +60  Blend alpha used on disocclusion / soft reject (1.0 = use the current frame immediately).
    float  gMotionVectorScale;       // +64  Multiplies the motion vector before reprojection (1.0 = Falcor normalized convention).
    float  gMotionLengthThreshold;   // +68  Hard reject when |mvec| exceeds this (normalized units, 0.5 = half screen). 0 disables.
    float  gFireflyMaxRadiance;      // +72  Firefly ceiling on the output RGB (default LUMEN_GI_MAX_RADIANCE = 10).
    float2 gInvFrameDim;             // +76  1 / frame dims (UV math).
    uint   gPad0;                    // +84
    uint   gPad1;                    // +88
    uint   gPad2;                    // +92
};  // 96 bytes.

/** Input / output resources. Every field the shader reads has an is_valid
 *  guard (defaults defined in the compute entry) so unbound optional inputs
 *  compile and no-op; required ones make the pass write zeros.
 */
Texture2D<float2>  gLinearZ;                    // required; current depth (.x).
Texture2D<float4>  gCurrent;                    // required; current interpolated GI (RGB + confidence).
Texture2D<float2>  gMotionVector;               // required; screen-space motion (normalized).
Texture2D<float>   gPrevDepth;                  // required; previous depth.
Texture2D<float4>  gPrevGI;                     // required; previous temporal output (RGB + history length in .a).
Texture2D<float4>  gNormalRoughnessMaterialID;  // optional; current normal (.xy oct) / material (.w).
Texture2D<float4>  gPrevNormalRoughnessMaterialID; // optional; previous normal / material.
Texture2D<float4>  gHitDistance;                // optional; current hit distance (.x).
Texture2D<float4>  gPrevHitDistance;            // optional; previous hit distance (.x).
Texture2D<float>   gHistoryLength;              // optional; previous history length R32F.
Texture2D<float>   gPrevConfidence;             // optional; previous confidence R32F.
RWTexture2D<float4> gTemporalOutput;            // required; smoothed GI (.a = new history length).
RWTexture2D<float>  gTemporalAlpha;             // optional; effective EMA alpha.
RWTexture2D<float>  gTemporalConfidence;        // optional; updated confidence.
RWTexture2D<float2> gMoments;                   // optional; running luminance mean / mean square.

#endif // __LUMEN_TEMPORAL_FILTER_DATA_SLANG__
