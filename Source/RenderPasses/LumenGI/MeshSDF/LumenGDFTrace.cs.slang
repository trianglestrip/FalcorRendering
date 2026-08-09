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
// =====================================================================================
//  LumenGI - S6-B4 Global Distance Field sphere trace pass (compute entry point)
//  -------------------------------------------------------------------------------------
//  Dispatches the resource-free GDF sphere-trace library (LumenGDFSphereTrace.3d.slang)
//  over the screen: one thread per pixel reconstructs the world-space view ray and
//  sphere-traces the GDF clipmap textures produced by LumenGDFCompose.cs.slang.
//
//  OUTPUT CONTRACT (S6 mainline integration)
//  -------------------------------------------------------------------------------------
//  * gDiffuseGI          RGB = hit-shaded view distance (bright near, dark far), 0 on miss;
//                        A = 1 (matches the S1 output A channel).
//  * gDiffuseRadianceHitDist  A = hit distance (world m) on hit, else kLumenGIMissHitDistance;
//                        RGB = 0. This matches the channel convention of the HWRT path so the
//                        S5 filters (which read hitDistance indirectly) keep working.
//  * gConfidence         R = hit ? 1 : 0.
//  * gGDFTraceDebug      (optional diagnostic): RGB = (t / tMax, surfaceSDF/voxel, t), A = hit.
//                        .g is the |GDF signed distance at the GBuffer surface point| / voxel -
//                        a "does the GDF represent the visible scene surface" sanity channel.
//  * gGDFTraceStats      RWStructuredBuffer<uint> counters, cleared by the host before dispatch
//                        (index layout in the LumenGDFTraceCounters struct below).
//
//  RAY RECONSTRUCTION (mirror of the S4.2 screen-probe conventions)
//  -------------------------------------------------------------------------------------
//  trace-view frame: x = right, y = down, z = depth-forward toward the camera (view +z points
//  at the camera). worldDir = normalize(right * ndc.x - up * ndc.y - forward) where
//  ndc = (pixel + 0.5 - principal) / focal. Origin = gCameraPosW. This is the inverse of
//  lumenProbeWorldToTraceViewDir in LumenScreenProbeTrace.cs.slang.
//
//  The pass is plain compute with no scene block: it needs only the GDF bindings + the
//  camera basis + the optional GBuffer linearZ for the surface-SDF sanity channel.
// =====================================================================================

#include "LumenGDFData.slang"
#include "LumenGDFSphereTrace.3d.slang"
#include "../LumenGIData.slang" // kLumenGIMissHitDistance

// Falcor emits is_valid_<name> defines for optional resources; the mandatory ones below are
// bound by the host on every dispatch. Default everything to 1 so the file is statically
// compilable before the host binds anything.
#ifndef is_valid_gGDFLevels
#define is_valid_gGDFLevels 1
#endif
#ifndef is_valid_gGDFLevelTable
#define is_valid_gGDFLevelTable 1
#endif
#ifndef is_valid_gLinearZ
#define is_valid_gLinearZ 1
#endif
#ifndef is_valid_gDiffuseGI
#define is_valid_gDiffuseGI 1
#endif
#ifndef is_valid_gDiffuseRadianceHitDist
#define is_valid_gDiffuseRadianceHitDist 1
#endif
#ifndef is_valid_gConfidence
#define is_valid_gConfidence 1
#endif
#ifndef is_valid_gGDFTraceDebug
#define is_valid_gGDFTraceDebug 0
#endif
#ifndef is_valid_gGDFTraceStats
#define is_valid_gGDFTraceStats 1
#endif

#define is_valid(name) (is_valid_##name != 0)

// -------------------------------------------------------------------------------------
// Constant buffer: the clipmap params (LumenGDFData.slang) + the camera basis the host
// derives from the active camera each frame. All fields are set by name by the host.
// -------------------------------------------------------------------------------------
cbuffer LumenGDFTraceCB
{
    LumenGDFClipmapParams gClipmap;
    float3 gCameraPosW;
    float gCameraFocalPx;
    float2 gPrincipalPoint;
    uint2 gFrameDim;
    uint gMaxSteps;            // 0 -> library default (kLumenGDFMaxSteps)
    uint gMinStepScaleBits;    // reserved (0)
    float3 gCameraRightW;
    float gCameraUpW0;         // reserved (kept to 16-byte-align the basis below)
    float3 gCameraUpW;
    float gCameraForwardW0;    // reserved
    float3 gCameraForwardW;
    float gTraceMaxDistance;   // world m; 0 -> kLumenGDFMaxDistance
    float3 gPad;
};

// -------------------------------------------------------------------------------------
// GDF bindings (shared with the compose pass; level textures + level table).
// -------------------------------------------------------------------------------------
Texture3D<float> gGDFLevels[kLumenGDFMaxLevels];
StructuredBuffer<LumenGDFLevelParams> gGDFLevelTable;

Texture2D<float> gLinearZ;                    // optional: surface depth for the sanity channel.

RWTexture2D<float4> gDiffuseGI;               // output (S6 MeshSDF main path).
RWTexture2D<float4> gDiffuseRadianceHitDist;  // output hit-distance channel.
RWTexture2D<float> gConfidence;               // output confidence.
RWTexture2D<float4> gGDFTraceDebug;           // optional diagnostic.
RWStructuredBuffer<uint> gGDFTraceStats;      // counters (LumenGDFTraceCounters layout).

// -------------------------------------------------------------------------------------
// Per-frame counters. The host clears the buffer before dispatch and reads it back after.
// -------------------------------------------------------------------------------------
static const uint kGDFTraceStatTraced = 0;
static const uint kGDFTraceStatHit = 1;
static const uint kGDFTraceStatMiss = 2;
static const uint kGDFTraceStatMaxSteps = 3;
static const uint kGDFTraceStatNoGrid = 4;
static const uint kGDFTraceStatCount = 5;

// -------------------------------------------------------------------------------------
// Entry point (8x8 threads).
// -------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= gFrameDim))
        return;

    if (is_valid(gGDFTraceStats))
        InterlockedAdd(gGDFTraceStats[kGDFTraceStatTraced], 1u);

    // Reconstruct the world-space view ray (trace-view convention, mirror of S4.2).
    const float2 ndc = (float2(pixel) + 0.5f - gPrincipalPoint) / max(gCameraFocalPx, 1e-6f);
    const float3 dirW = normalize(gCameraRightW * ndc.x - gCameraUpW * ndc.y - gCameraForwardW);
    const float3 originW = gCameraPosW;

    // Surface-point sanity channel: |GDF signed distance at the visible surface| / voxel.
    float surfaceSDFNorm = -1.f;
    if (is_valid(gLinearZ))
    {
        const float z0 = gLinearZ[pixel].x;
        if (z0 > 0.f)
        {
            const float3 pSurface = originW + dirW * z0;
            uint level = 0;
            bool valid = false;
            const float dSurf = lumenGDFSample(
                LumenGDFBindings(gGDFLevels, gGDFLevelTable), gClipmap, pSurface, level, valid
            );
            const float vs = max(gGDFLevelTable[level].voxelSize, 1e-12f);
            surfaceSDFNorm = valid ? abs(dSurf) / vs : 0.f;
        }
    }

    // Sphere-trace the GDF along the view ray.
    LumenGDFTraceParams params;
    params.maxSteps = gMaxSteps;
    params.minStepScale = 0.f;
    params.hitEpsScale = 0.f;
    params.tMax = gTraceMaxDistance;

    LumenGDFTraceResult result;
    const bool hit = traceGDF(
        LumenGDFBindings(gGDFLevels, gGDFLevelTable), gClipmap, originW, dirW, params, result
    );

    if (is_valid(gGDFTraceStats))
    {
        if (hit)
            InterlockedAdd(gGDFTraceStats[kGDFTraceStatHit], 1u);
        else if (result.missReason == LumenGDFMissReason::MaxSteps)
            InterlockedAdd(gGDFTraceStats[kGDFTraceStatMaxSteps], 1u);
        else if (result.missReason == LumenGDFMissReason::NoGrid)
            InterlockedAdd(gGDFTraceStats[kGDFTraceStatNoGrid], 1u);
        else
            InterlockedAdd(gGDFTraceStats[kGDFTraceStatMiss], 1u);
    }

    const float tMax = gTraceMaxDistance > 0.f ? gTraceMaxDistance : kLumenGDFMaxDistance;

    // ---- Main outputs (S6 MeshSDF path). ----
    if (is_valid(gDiffuseGI))
    {
        // Bright near, dark far; 0 on miss (the S6 pass output is a hit/occlusion signal).
        const float3 gi = hit ? float3(0.8f * (1.f - saturate(result.t / tMax)) + 0.2f) : float3(0.f);
        gDiffuseGI[pixel] = float4(gi, 1.f);
    }
    if (is_valid(gDiffuseRadianceHitDist))
    {
        const float dist = hit ? result.t : kLumenGIMissHitDistance;
        gDiffuseRadianceHitDist[pixel] = float4(0.f, 0.f, 0.f, dist);
    }
    if (is_valid(gConfidence))
    {
        gConfidence[pixel] = hit ? 1.f : 0.f;
    }

    // ---- Optional diagnostic channel. ----
    if (is_valid(gGDFTraceDebug))
    {
        const float3 debug = float3(
            hit ? saturate(result.t / tMax) : 1.f,
            saturate(surfaceSDFNorm),
            hit ? result.t : 0.f
        );
        gGDFTraceDebug[pixel] = float4(debug, hit ? 1.f : 0.f);
    }
}
