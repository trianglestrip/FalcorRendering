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

#ifndef __LUMEN_GI_DATA_SLANG__
#define __LUMEN_GI_DATA_SLANG__

// Debug output modes. The host passes the same numeric values into the
// debug pass constant buffer (LumenGI::DebugMode in LumenGI.h).
static const uint kLumenDebugNone = 0;
static const uint kLumenDebugNormal = 1;
static const uint kLumenDebugLinearDepth = 2;
static const uint kLumenDebugMotion = 3;
static const uint kLumenDebugMaterialID = 4;
static const uint kLumenDebugConfidence = 5;
static const uint kLumenDebugDirectLighting = 6;
static const uint kLumenDebugEnvironmentOnly = 7;
static const uint kLumenDebugEmissiveOnly = 8;
static const uint kLumenDebugAnalyticOnly = 9;
static const uint kLumenDebugIndirectOnly = 10;
static const uint kLumenDebugFireflyMask = 11;
static const uint kLumenDebugCardsOverlay = 12;

/** Hit distance written by the miss shader. The value is the maximum
    representable float16, which is also the output format of the hit-distance
    channel, so the miss marker survives the RGBA16F write exactly.
*/
static const float kLumenGIMissHitDistance = 65504.f;

/** Per-sample radiance ceiling applied per RGB component before any output
    write. Samples above the ceiling are clamped and counted as fireflies.

    The default of 10000 is far above the typical single-bounce secondary-hit
    signal in Falcor demo scenes (analytic light intensities are <= 1e4, and
    the diffuse NEE estimator is bounded by the emitted radiance for area
    lights), so only genuine outliers are affected. The host may override the
    value through the LUMEN_GI_MAX_RADIANCE define.
*/
#ifndef LUMEN_GI_MAX_RADIANCE
#define LUMEN_GI_MAX_RADIANCE 10000.f
#endif
static const float kLumenGIMaxRadiance = LUMEN_GI_MAX_RADIANCE;

/** Layout of the optional GPU lighting counters (gLumenGICounters).

    The resource is a structured buffer of kLumenGICounterCount uint4s. The
    host clears it before each trace dispatch and reads it back into
    LumenGIStats::LumenGIFrameCounters (LumenGIStats.h). Writes are skipped
    when the resource is not bound (is_valid_gLumenGICounters == 0).
*/
enum class LumenGICounterIndex : uint32_t
{
    NaNInfSamples = 0,   ///< Radiance contained NaN/Inf and was replaced with zero.
    FireflySamples = 1,  ///< Radiance exceeded the ceiling and was clamped.
    NegativeSamples = 2, ///< Radiance had negative components and was zeroed.
    TracedRays = 3,      ///< Diffuse indirect rays traced (hit and miss).
};
static const uint kLumenGICounterCount = 4;

struct LumenGIFrameParams
{
    uint2 frameDim;
    uint frameIndex;
    uint debugMode;
};

#endif
