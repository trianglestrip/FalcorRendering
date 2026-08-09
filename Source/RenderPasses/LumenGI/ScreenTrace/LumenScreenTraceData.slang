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

/** Shared data contract for the S4-B1 hierarchical screen-space ray tracer.
 *
 * Coordinate conventions (frozen for the S4 wave; the host wires resources
 * exactly as described here):
 *
 * - Linear depth z is positive and increases away from the camera (Falcor
 *   convention, GBufferRT linearZ.x). A ray marches from z0 to larger z.
 * - gLinearZ is the full-resolution RG32F texture from GBufferRT:
 *   .x = linear depth in meters, .y = depth slope (derivative of z w.r.t.
 *   the dominant screen axis, used for conservative thickness).
 * - gHZBMips is an array of independent R32F textures, one per level. Mip 0
 *   is the full-resolution linear depth, each subsequent level stores the
 *   MAXIMUM linear depth of a 2x2 block of the previous level. Levels use
 *   ceil-halving dimensions (max((d + 2^m - 1) >> m, 1)) so every level fully
 *   covers its parent and the max chain is conservative; independent
 *   textures are required because D3D12 native mip chains are floor-sized.
 *   The host builds them with the LumenHZBBuild compute pass (S4-A1).
 * - gRayDirection is an RGBA16F texture holding the VIEW-SPACE ray
 *   direction (positive z toward the camera, i.e. a forward GI ray has
 *   negative z). The host derives it from gViewW / camera matrices.
 * - The march starts at every input pixel's surface point and follows the
 *   view-space direction. Screen space is reached by perspective division
 *   with the camera focal lengths passed in the constant buffer.
 *
 * Output encoding (gScreenTraceResult, RGBA16F):
 * - Hit:      RGB = float3(hitUV, hitLinearDistance), A = confidence in (0, 1].
 * - Miss:     RGB = 0, A = -((float)missReason + 1.0) (always negative,
 *             distinct from every confidence value; reasons are the
 *             kLumenScreenTraceMissReason* constants).
 * This lets a consumer test `result.a > 0.f` for a hit and decode the
 * reason as `uint(-result.a) - 1` for a miss.
 */

#ifndef __LUMEN_SCREEN_TRACE_DATA_SLANG__
#define __LUMEN_SCREEN_TRACE_DATA_SLANG__

/** Miss reasons, also used as stats categories by the host (S4-C1). */
static const uint kLumenScreenTraceMissReasonNone = 0u;        ///< Hit.
static const uint kLumenScreenTraceMissReasonOutOfScreen = 1u; ///< Ray left the viewport.
static const uint kLumenScreenTraceMissReasonBehindSurface = 2u; ///< Ray passed through geometry without a stable hit.
static const uint kLumenScreenTraceMissReasonMaxSteps = 3u;    ///< Step budget exhausted.
static const uint kLumenScreenTraceMissReasonThinGeometry = 4u; ///< Reserved: unstable thin-geometry region.

static const uint kLumenScreenTraceMissReasonCount = 5u;

/** Frozen algorithm parameters. The host may override every one of these
 * through the CB, so quality presets (S8) can retune the trace without a
 * shader change.
 */
static const uint   kLumenScreenTraceMaxStepsDefault = 64u;
static const float  kLumenScreenTraceMinThicknessDefault = 0.001f; ///< Meters.
static const float  kLumenScreenTraceThicknessScaleDefault = 2.0f;
static const float  kLumenScreenTraceStepEpsilonDefault = 1e-4f;   ///< Texel-space forward bias.
static const uint   kLumenScreenTraceMaxMipDefault = 12u;

/** Constant buffer filled by the host every dispatch. */
cbuffer LumenScreenTraceCB
{
    uint2  gFrameDim;
    uint   gMaxSteps;
    uint   gStartMip;          ///< First HZB mip used by the march (0 = full res).
    float  gMinThickness;      ///< Meters, floor of the thickness test.
    float  gThicknessScale;    ///< Multiplies slope * stepLength.
    float  gStepEpsilon;       ///< Forward bias in texels to avoid self-intersection.
    float  gCameraFocalPx;     ///< Focal length in pixels (f * heightPx / sensorHeight).
    float2 gPrincipalPoint;    ///< Principal point in pixels.
    float2 gInvFrameDim;       ///< 1 / frame dims (for UV math).
    uint   gMaxMip;
    uint   gPad;
};

/** Input resources (optional ones are guarded by is_valid_* defines). */
Texture2D<float2>   gLinearZ;
Texture2D<float>    gHZBMips[16];
Texture2D<float4>   gRayDirection;
RWTexture2D<float4> gScreenTraceResult;

#endif
