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

#ifndef __LUMEN_SCREEN_PROBE_DATA_SLANG__
#define __LUMEN_SCREEN_PROBE_DATA_SLANG__

// Compile mode: 0 = standalone compute (raw RayQuery fallback, tests/lumengi), 1 = production
// (SceneRayQuery<0> against gScene, set by the LumenGI host). Default 0 so this module
// compiles standalone.
#ifndef LUMEN_GI_PROBE_SCENE_TRACE
#define LUMEN_GI_PROBE_SCENE_TRACE 0
#endif
#ifndef LUMEN_GI_PROBE_HISTORY
#define LUMEN_GI_PROBE_HISTORY 0
#endif

/** Frozen data contract for the S4.2 screen probe gather (S4-A2/B2, Agent Z1).
 *
 * Probe grid (S4-A2, host component LumenScreenProbe.h):
 *   - One probe per 8x8 texel tile (kLumenScreenProbeTileSize), at the tile
 *     CENTER: probePixel = gridPos * 8 + 4 + 0.5 (texel center convention,
 *     matching the 0.5 offset used by every other screen-space shader here).
 *   - Probe metadata is a StructuredBuffer<LumenScreenProbeMeta>; every field
 *     is written by the GPU (worldPos/normal/depth/active from linearZ + the
 *     GBuffer normal), the host prefills screenPos once per resize.
 *   - Per-frame update budget: each probe is traced every gUpdateInterval
 *     frames in a round-robin (probeIndex % interval == frameIndex % interval)
 *     PLUS immediately when the probe is dirty (depth moved by more than
 *     gDepthChangeThreshold since its last update). dirty is computed by the
 *     update entry point every frame. The budget is therefore
 *     ceil(probeCount / interval) + dirtyProbes per frame (bounded).
 *   - history (S5) is reserved: the metadata carries lastUpdateFrame/age; the
 *     adaptive probe list (S4.3/S5) is reserved in the metadata flags.
 *
 * Direction sampling (S4-B2):
 *   - Per probe per frame gDirectionsPerProbe (<= kLumenScreenProbeMaxDirectionsPerProbe)
 *     directions, generated ON THE GPU from a fixed-seed low-discrepancy
 *     sequence (Hammersley over the probe's cosine hemisphere). The sequence
 *     is rotated across frames and across probes deterministically:
 *       rot = (probeIndex * 2654435761u + frameIndex * 1140071481u + gSeed) % n
 *       j   = (dirIndex + rot) % n
 *       u   = frac(hammersley(j, n) + hash2(probeIndex) / n)   // per-probe blue-noise jitter
 *     where hammersley(j,n) = (j/n, radicalInverse2(j)). Every value is a
 *     pure function of (probeIndex, dirIndex, frameIndex, gSeed), so the whole
 *     trace is reproducible under a fixed seed. LumenScreenProbe.h carries the
 *     CPU reference of this sequence for cross-validation.
 *
 * Screen-first tracing (S4-B2):
 *   - The probe ray is converted to the S4-A1 screen-trace view convention
 *     (x = right, y = down, z = forward, linear depth = view z, all positive
 *     away from the camera; forward ray has d.z > 0) using the camera view
 *     matrix passed in the CB, and marched in screen space against the HZB
 *     max-depth chain (same ceil-halving chain as LumenScreenTrace).
 *   - The march is self-contained and self-consistent (the S4-A1 per-pixel
 *     output cannot serve as a depth reference: its hit distances are
 *     self-hits, see artifacts/lumengi/S4/shader_verify.json). The optional
 *     gScreenTraceResult is consumed ONLY as a soft prefilter/agreement
 *     signal and for statistics; the authoritative screen hit is the probe's
 *     own surface-crossing test.
 *   - A screen hit records hitUV = the surface texel UV, hitDistance = the
 *     distance along the probe ray, confidence = the thickness-window quality,
 *     and radiance = the per-pixel GI at the hit texel (RGB of
 *     gDiffuseRadianceHitDist). Reusing the S1 per-pixel indirect radiance is
 *     the S4.2 degradation for radiance; S4.3 (A3/B3) replaces it with the
 *     surface-cache radiance lookup at the hit point.
 *
 * HWRT fallback (S4-B2):
 *   - When the screen march fails (exits the viewport / step budget / no
 *     crossing), the probe ray is traced inline. Two backends:
 *       * LUMEN_GI_PROBE_SCENE_TRACE = 1 (production, host): import
 *         Scene.Scene + Scene.RaytracingInline, SceneRayQuery<0> against the
 *         scene TLAS (gScene). The host binds the scene block +
 *         bindShaderDataForRaytracing() exactly like the cache-lighting pass.
 *       * LUMEN_GI_PROBE_SCENE_TRACE = 0 (default; standalone compute, used by
 *         tests/lumengi/run_screenprobe.py): raw RayQuery<...> against the
 *         optional gProbeTLAS parameter; with no TLAS bound the fallback
 *         degrades to the environment (kLumenProbeHitFlagFallbackUnavailable).
 *   - Fallback radiance: a HWRT hit that projects onto the viewport (and whose
 *     projected depth matches the HZB, i.e. the hit is visible) reuses the
 *     per-pixel GI; otherwise (and on miss) the environment radiance is used.
 *     Unified hit record: LumenProbeHit.
 *
 * All coordinates are in meters (world) / texels (screen) unless noted.
 */

// -------------------------------------------------------------------------------------
// Frozen parameters (the host may override every one of these through the CB).
// -------------------------------------------------------------------------------------
static const uint kLumenScreenProbeTileSize = 8u;                  ///< Texels per probe tile (fixed S4.2 grid).
static const uint kLumenScreenProbeMaxDirectionsPerProbe = 32u;    ///< Fixed hit-record stride (record i = probe*stride + dir).
static const uint kLumenScreenProbeDefaultDirectionsPerProbe = 16u;
static const uint kLumenScreenProbeMaxMarchStepsDefault = 512u;    ///< Screen-march step budget (texel steps).
static const float kLumenScreenProbeMaxSurfaceDepthDefault = 100.f; ///< Meters; beyond this linearZ is empty space (far plane).
static const float kLumenScreenProbeMinThicknessDefault = 0.01f;   ///< Meters, floor of the thickness test.
static const float kLumenScreenProbeThicknessScaleDefault = 4.0f;  ///< Multiplies (slope | rayDepthStep).
static const float kLumenScreenProbeStepEpsilonDefault = 0.5f;     ///< Texels; forward bias that skips the start surface.
static const float kLumenScreenProbeMaxTraceDistanceDefault = 20.f; ///< Meters; screen-first rays beyond this go to HWRT.
static const float kLumenScreenProbeRelStepDefault = 0.02f;        ///< Depth-relative march step (fraction of the current depth).
static const float kLumenScreenProbeMinStepDefault = 0.005f;       ///< Meters, floor of the depth march step.
static const float kLumenScreenProbeDepthChangeThresholdDefault = 0.02f; ///< Meters; dirty detection.
static const float kLumenScreenProbeRayBiasDefault = 0.01f;        ///< Meters; normal offset for the HWRT fallback origin.
static const uint kLumenScreenProbeSeedDefault = 0x51B8DC0Du;      ///< Fixed base seed (task rule 5).
static const uint kLumenScreenProbeMaxMipDefault = 12u;
static const uint kLumenScreenProbeCounterCount = 8u;

// LumenScreenProbeMeta.flags
static const uint kLumenScreenProbeMetaFlagActive = 1u << 0;  ///< linearZ at the probe > 0.
static const uint kLumenScreenProbeMetaFlagDirty = 1u << 1;   ///< Depth moved by > gDepthChangeThreshold since last update.

// LumenScreenProbeMeta.materialID: the GBuffer normWRoughnessMaterialID A channel carries a
// 2-bit material ID (0..3). kLumenScreenProbeMaterialIDNone marks an unknown/unset material
// (impossible with a valid 2-bit GBuffer ID; reserved for the interpolation material gate).
static const uint kLumenScreenProbeMaterialIDNone = 0xFFFFFFFFu;

/** Probe index for a clamped grid coordinate (used by the S4-B3 interpolate pass).
    Row-major: index = gridPos.y * gridDim.x + gridPos.x (identical to the host's grid math).
 */
uint lumenScreenProbeIndex(const uint2 gridPos, const uint gridDimX)
{
    return gridPos.y * gridDimX + gridPos.x;
}

// LumenProbeHit.flags
static const uint kLumenProbeHitFlagNone = 0u;
static const uint kLumenProbeHitFlagScreenHit = 1u << 0;           ///< Screen-first march confirmed a surface crossing.
static const uint kLumenProbeHitFlagHWRTHit = 1u << 1;             ///< HWRT fallback hit.
static const uint kLumenProbeHitFlagHWRTMiss = 1u << 2;            ///< HWRT fallback miss.
static const uint kLumenProbeHitFlagFallbackUnavailable = 1u << 3; ///< No scene/TLAS data (degraded fallback).
static const uint kLumenProbeHitFlagRadianceReused = 1u << 4;      ///< Radiance sampled from per-pixel GI (S4.2 degradation).
static const uint kLumenProbeHitFlagEnvironment = 1u << 5;         ///< Radiance is the environment.

///< LumenScreenProbeMeta, 64 bytes, 16-byte aligned. Host mirror in LumenScreenProbe.h.
struct LumenScreenProbeMeta
{
    float2 screenPos;     ///< +0  Probe pixel (tile center texel + 0.5). Host-prefilled per resize.
    uint   active;        ///< +8  kLumenScreenProbeMetaFlagActive.
    uint   lastUpdateFrame; ///< +12 Frame of the last trace (S5 history / age).
    float3 worldPos;      ///< +16 Probe world position (linearZ unprojection).
    uint   materialID;    ///< +28 GBuffer material ID (RGB10A2 A), reserved for S4.3 interpolation.
    float3 normalW;       ///< +32 Probe world normal (GBuffer, decoded).
    float  depth;         ///< +44 Probe linear depth (meters).
    uint   age;           ///< +48 Frames since the last trace (frameIndex - lastUpdateFrame).
    uint   updateInterval; ///< +52 gUpdateInterval used for the last trace.
    uint   dirty;         ///< +56 kLumenScreenProbeMetaFlagDirty.
    uint   flags;         ///< +60 Reserved (adaptive list, S4.3+).
};
// Layout verified by LumenScreenProbe::Meta (64 B, C++ static_assert) and tests/lumengi/run_screenprobe.py.

///< LumenProbeHit, 32 bytes, 16-byte aligned. Unified per-direction hit record.
struct LumenProbeHit
{
    float3 radiance;      ///< +0  Radiance at the hit (per-pixel GI reuse or environment).
    float  hitDistance;   ///< +12 Distance along the probe ray (meters); 0 on degraded fallback.
    uint   flags;         ///< +16 kLumenProbeHitFlag*.
    float  confidence;    ///< +20 Screen-hit thickness-window quality; 0 otherwise.
    float2 hitUV;         ///< +24 Hit UV (radiance sample location; consumed by S4-B3 interpolation).
};
// Layout verified by LumenScreenProbe::Hit (32 B, C++ static_assert) and tests/lumengi/run_screenprobe.py.

///< LumenScreenProbeCounters, 32 bytes, one element. InterlockedAdd targets.
struct LumenScreenProbeCounters
{
    uint screenHits;          ///< +0  Screen-first confirmed hits.
    uint fallbackAttempts;    ///< +4  Screen-first failed -> fallback traced.
    uint fallbackHits;        ///< +8  Fallback resolved a hit.
    uint fallbackMisses;      ///< +12 Fallback missed (environment).
    uint fallbackUnavailable; ///< +16 Fallback degraded (no scene/TLAS data).
    uint inactiveProbes;      ///< +20 Probes with no surface this frame (per-probe count, dir 0 only).
    uint budgetSkipped;       ///< +24 Probes skipped by the update interval budget (dir 0 only).
    uint directionsTraced;    ///< +28 Total directions actually traced.
};
// Layout verified by LumenScreenProbe::Counters (32 B, C++ static_assert) and tests/lumengi/run_screenprobe.py.

/** Constant buffer filled by the host every dispatch. All values are named
    (set through the ShaderVar binding by both the C++ host and the Python
    test); camera orientation is passed as three world basis vectors instead
    of matrices so the CB is fully settable from the script bindings.
*/
cbuffer LumenScreenProbeCB
{
    uint2   gFrameDim;          // +0   Frame dims in pixels.
    uint    gFrameIndex;        // +8   Frame counter (rotates the direction set).
    uint    gDirectionsPerProbe;// +12  Directions traced per probe this frame (<= stride).
    uint2   gProbeGridDims;     // +16  Probes per axis (ceil(frameDim / tileSize)).
    uint    gMaxHitRecordStride;// +24  kLumenScreenProbeMaxDirectionsPerProbe (record indexing).
    float   gMinThickness;      // +28  Meters.
    float   gThicknessScale;    // +32  Slope multiplier.
    uint    gMaxMarchSteps;     // +36  Screen-march step budget.
    float   gStepEpsilon;       // +40  Texels, forward bias.
    float   gCameraFocalPx;     // +44  Focal length in pixels.
    float2  gPrincipalPoint;    // +48  Principal point in pixels (frame center; Falcor has no offset).
    float2  gInvFrameDim;       // +56  1 / frame dims.
    uint    gUpdateInterval;    // +64  Probe trace interval in frames (1 = all probes).
    uint    gSeed;              // +68  Fixed base seed.
    float   gDepthChangeThreshold; // +72 Dirty detection threshold (meters).
    uint    gMaxMip;            // +76  Highest HZB mip index.
    // Camera world basis (view +x = right, view +y = up, view +z = forward
    // toward the camera); the trace-view frame is (right, -up, -forward).
    float3  gCameraPosW;        // +80  Camera world position.
    uint    gPad0;              // +92
    float3  gCameraRightW;      // +96
    uint    gPad1;              // +108
    float3  gCameraUpW;         // +112
    uint    gPad2;              // +124
    float3  gCameraForwardW;    // +128
    uint    gPad3;              // +140
    float3  gEnvFallbackRadiance; // +144 Environment radiance used when no scene data (test mode).
    uint    gDebugMode;         // +156 Reserved.
    uint    gProbeCount;        // +160 Total probes.
    uint    gPad4;              // +164
    uint    gPad5;              // +168
    uint    gPad6;              // +172
    uint    gWeightMode;        // +176 S4.3 integrate weight mode (kLumenScreenProbeWeightMode*, 0 = cosine hemisphere default). Host-bound per dispatch; all pre-S4.3 offsets preserved.
};  // 192 bytes (gWeightMode sits in the previously-padded tail).

/** Input / output resources. Optional ones are guarded by is_valid_* defines
    (all default 0). gProbeTLAS is only declared in the non-scene fallback mode.

    gHZBMips is a SINGLE native-mip R32F texture (floor-halved D3D12 mip chain,
    mip 0 = full-res linear depth, max-reduction per level), built by the Lumen
    HZB build pass with floor dims. The probe march indexes it with explicit
    mip levels: Load(int3(cell, mip)).
*/
Texture2D<float2> gLinearZ;
Texture2D<float>  gHZBMips;
Texture2D<float4> gNormalRoughnessMaterialID;
Texture2D<float4> gDiffuseRadianceHitDist;
Texture2D<float4> gScreenTraceResult;
#if !LUMEN_GI_PROBE_SCENE_TRACE
RaytracingAccelerationStructure gProbeTLAS;
#endif
RWStructuredBuffer<LumenScreenProbeMeta> gProbeMeta;
RWStructuredBuffer<LumenProbeHit> gProbeHitRecords;
RWStructuredBuffer<LumenScreenProbeCounters> gProbeCounters;
RWTexture2D<float4> gProbeRadiance;
#if LUMEN_GI_PROBE_HISTORY
RWTexture2D<float4> gProbeRadianceHistory;
#endif
RWStructuredBuffer<float4> gProbeDebug; // Optional (tests only): per-probe march diagnostics.

#endif // __LUMEN_SCREEN_PROBE_DATA_SLANG__
