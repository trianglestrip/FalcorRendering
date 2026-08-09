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

/** S4.2 screen probe gather (S4-A2/B2, Agent Z1).
 *
 * Three compute entry points, dispatched in order every frame (one 64-thread
 * group per 64 threads, all bounds-checked):
 *
 *   updateMain    -- 1 thread / probe: resample the probe's linearZ/GBuffer
 *                    normal, unproject to world, recompute active/dirty.
 *   traceMain     -- 1 thread / (probe, direction): low-discrepancy direction
 *                    sampling (fixed seed + cross-frame/probe rotation),
 *                    screen-first HZB march, HWRT inline fallback, unified
 *                    LumenProbeHit record + counters.
 *   finalizeMain  -- 1 thread / probe: average the probe's records into the
 *                    probeRadiance texture and refresh the aging metadata.
 *
 * The screen-first march is SELF-CONTAINED (see LumenScreenProbeData.slang):
 * the probe ray is converted to the screen-trace view convention
 * (x right, y down, z = linear depth, forward = +z) and marched against the
 * HZB max chain with the exact perspective depth model, so it never inherits
 * the S4-A1 per-pixel self-hit behavior. gScreenTraceResult is consumed only
 * as a soft prefilter and agreement statistic.
 */

#include "LumenScreenProbeData.slang"
#include "Utils/Math/MathConstants.slangh"

// -------------------------------------------------------------------------------------
// Compile-mode defines. LUMEN_GI_PROBE_SCENE_TRACE = 1 (host) imports the scene block
// and uses SceneRayQuery<0>; = 0 (default; standalone compute test) uses a raw
// RayQuery against the optional gProbeTLAS and degrades to the environment when the
// TLAS is not bound.
// -------------------------------------------------------------------------------------
#ifndef LUMEN_GI_PROBE_SCENE_TRACE
#define LUMEN_GI_PROBE_SCENE_TRACE 0
#endif
#if LUMEN_GI_PROBE_SCENE_TRACE
import Scene.Scene;
import Scene.RaytracingInline;
import Utils.Math.Ray;
import RenderPasses.LumenGI.Lighting.LumenGILighting; // lumenGIEvaluateEnvironment
#endif

#ifndef is_valid_gScreenTraceResult
#define is_valid_gScreenTraceResult 0
#endif
#ifndef is_valid_gDiffuseRadianceHitDist
#define is_valid_gDiffuseRadianceHitDist 1
#endif
#ifndef is_valid_gNormalRoughnessMaterialID
#define is_valid_gNormalRoughnessMaterialID 1
#endif
#ifndef is_valid_gProbeRadiance
#define is_valid_gProbeRadiance 1
#endif
#ifndef is_valid_gProbeTLAS
#define is_valid_gProbeTLAS 0
#endif
#ifndef is_valid_gProbeDebug
#define is_valid_gProbeDebug 0
#endif

#define is_valid(name) (is_valid_##name != 0)

static const float kLumenProbeMinProjectDenom = 1e-8f;
///< Screen-march hits closer than this many texels of equivalent world depth
///< are skipped (the probe's own 8x8 tile). Converted to meters per probe depth.
static const float kLumenProbeStartBiasTexels = 8.0f;

// -------------------------------------------------------------------------------------
// Direction sampling (fixed seed, reproducible; see the data module for the formula).
// -------------------------------------------------------------------------------------

/** Standard 32-bit integer hash (Wang). */
uint lumenProbeHash(uint x)
{
    x = (x ^ 61u) ^ (x >> 16u);
    x = x * 9u + 0x85ebca6bu;
    x = x ^ (x >> 13u);
    x = x * 0xc2b2ae35u;
    x = x ^ (x >> 16u);
    return x;
}

/** Two independent hash outputs of the probe index, normalized to [0, 1). */
float2 lumenProbeHash2(const uint probeIndex)
{
    const uint a = lumenProbeHash(probeIndex);
    const uint b = lumenProbeHash(a ^ 0x9e3779b9u);
    return float2(float(a) * (1.0 / 4294967296.0), float(b) * (1.0 / 4294967296.0));
}

/** Base-2 Van der Corput radical inverse (same sequence as FalcorMath.h). */
float lumenProbeRadicalInverse(const uint i)
{
    return float(reversebits(i)) * (1.0 / 4294967296.0);
}

/** Hammersley 2D point (i of n). */
float2 lumenProbeHammersley(const uint i, const uint n)
{
    return float2(float(i % n) / float(n), lumenProbeRadicalInverse(i));
}

/** Sample one direction of probe (probeIndex) for direction slot dirIndex at
    frame frameIndex, in the probe's world cosine hemisphere. The whole set is
    deterministic under a fixed seed; sampleIndex (0..n-1) is the rotated
    sequence slot (reported for cross-validation / stats).
*/
float3 lumenProbeSampleDirection(
    const uint probeIndex,
    const uint dirIndex,
    const uint frameIndex,
    const float3 normalW,
    out uint sampleIndex
)
{
    const uint n = max(1u, gDirectionsPerProbe);
    // Cross-frame rotation (golden-ratio stride) + per-probe rotation + fixed seed.
    const uint rot = (probeIndex * 2654435761u + frameIndex * 1140071481u + gSeed) % n;
    const uint j = (dirIndex + rot) % n;
    sampleIndex = j;
    // Low-discrepancy 2D point, jittered within one cell to decorrelate probes.
    float2 u = frac(lumenProbeHammersley(j, n) + lumenProbeHash2(probeIndex) * (1.0 / float(n)));

    // Tangent frame from the normal (branchless construction).
    const float3 N = any(isnan(normalW)) ? float3(0.f, 1.f, 0.f) : normalize(normalW);
    const float3 helper = abs(N.z) < 0.999f ? float3(0.f, 0.f, 1.f) : float3(1.f, 0.f, 0.f);
    const float3 T = normalize(cross(helper, N));
    const float3 B = cross(N, T);

    // Cosine-weighted hemisphere sample (matches FalcorMath::hammersleyCosine).
    const float z = 1.f - u.x;
    const float s = sqrt(max(0.f, 1.f - z * z));
    const float phi = 2.f * M_PI * u.y;
    const float3 localDir = float3(s * cos(phi), s * sin(phi), z);
    return normalize(T * localDir.x + B * localDir.y + N * localDir.z);
}

// -------------------------------------------------------------------------------------
// Coordinate helpers (screen-trace view convention: x right, y down, z = depth forward).
// -------------------------------------------------------------------------------------

/** World -> trace-view direction (x right, y down, z = linear depth forward).
    The trace-view frame is (gCameraRightW, -gCameraUpW, -gCameraForwardW),
    where gCameraForwardW is the view +z (from the target toward the camera).
*/
float3 lumenProbeWorldToTraceViewDir(const float3 dirW)
{
    return float3(dot(dirW, gCameraRightW), -dot(dirW, gCameraUpW), -dot(dirW, gCameraForwardW));
}

/** Trace-view position -> world (inverse of the trace-view frame). */
float3 lumenProbeTraceViewToWorld(const float3 tv)
{
    return gCameraPosW + gCameraRightW * tv.x - gCameraUpW * tv.y - gCameraForwardW * tv.z;
}

/** Unproject a probe pixel + linear depth to a world position (S4-A2 metadata). */
float3 lumenProbeUnprojectToWorld(const float2 pixel, const float z0)
{
    const float2 principal = gPrincipalPoint;
    const float focal = gCameraFocalPx;
    const float3 tv = float3(
        (pixel.x - principal.x) / focal * z0,
        (pixel.y - principal.y) / focal * z0,
        z0
    );
    return lumenProbeTraceViewToWorld(tv);
}

/** Project a trace-view point onto texel coordinates. */
float2 lumenProbeProjectToPixel(const float3 viewPos, const float focal, const float2 principal)
{
    return float2(viewPos.x / viewPos.z * focal + principal.x, viewPos.y / viewPos.z * focal + principal.y);
}

/** Project a world position to a texel coordinate (pinhole through the camera basis). */
float2 lumenProbeWorldToPixel(const float3 worldPos)
{
    const float3 rel = worldPos - gCameraPosW;
    const float3 tv = float3(dot(rel, gCameraRightW), -dot(rel, gCameraUpW), -dot(rel, gCameraForwardW));
    if (tv.z <= 1e-6f)
        return float2(-1e6f, -1e6f); // Behind the camera / degenerate.
    return lumenProbeProjectToPixel(tv, gCameraFocalPx, gPrincipalPoint);
}

/** Decode the GBuffer normWRoughnessMaterialID normal. GBufferRT writes the guide
    normal OCT-ENCODED (ndir_to_oct_unorm) into the RGB10A2 xy channels; decoding it
    as a plain n*0.5+0.5 RGB (as the pre-S4.3 host did) produces a wrong, non-unit
    vector on every real GBuffer. Mirrors LumenGIDebug.cs.slang's oct_to_ndir_unorm
    (self-contained so the module also compiles standalone).
*/
float3 lumenProbeDecodeNormal(const float2 octXY)
{
    const float2 p = octXY * 2.f - 1.f;
    float3 n = float3(p.xy, 1.f - abs(p.x) - abs(p.y));
    if (n.z < 0.f)
        n.xy = (1.f - abs(n.yx)) * select(n.xy >= 0.f, 1.f, -1.f);
    return normalize(n);
}

/** Radiance at a hit: S4.2 degradation = reuse the S1 per-pixel indirect
    radiance (gDiffuseRadianceHitDist RGB). S4.3 replaces this with the surface
    cache radiance lookup (LumenScreenProbeGather/A3-B3).
*/
float3 lumenProbeScreenRadiance(const uint2 texel)
{
#if is_valid_gDiffuseRadianceHitDist
    return max(gDiffuseRadianceHitDist[texel].xyz, float3(0.f));
#else
    return float3(0.f);
#endif
}

/** Environment radiance for the fallback: scene env map in the production mode,
    CB constant otherwise (test mode has no scene block).
*/
float3 lumenProbeEnvRadiance(const float3 dirW)
{
#if LUMEN_GI_PROBE_SCENE_TRACE
    return lumenGIEvaluateEnvironment(dirW);
#else
    return gEnvFallbackRadiance;
#endif
}

/** Offset the probe origin off the surface for the fallback ray. */
float3 lumenProbeRayOrigin(const float3 posW, const float3 normalW, const float3 dirW)
{
    const float3 N = any(isnan(normalW)) ? float3(0.f, 1.f, 0.f) : normalize(normalW);
    return posW + N * kLumenScreenProbeRayBiasDefault;
}

// -------------------------------------------------------------------------------------
// Screen-first march (self-contained, correct conventions).
// -------------------------------------------------------------------------------------

/** Depth-stepping screen-space march of the probe ray (trace-view origin o, unit
    direction d, d.z > 0 = forward). The ray is advanced in WORLD DEPTH with a bounded
    per-step gain (kLumenScreenProbeRelStepDefault * currentDepth), projected to the
    screen, and tested against the surface depth. Bounding the depth gain per step is
    what makes the march robust: the exact-perspective screen-texel stepping (S4-A1)
    jumps a near-axis-parallel ray past every crossing in its first step (the crossing
    parameter t is huge when the ray is almost parallel to the view ray), so those
    directions never hit. A depth-stepped march tests every depth increment instead.
    The HZB max chain stays bound (contract) but is not required by the march.
*/
bool lumenProbeTraceScreenFirst(const float2 probePixel, const float3 o, const float3 d, out LumenProbeHit hit)
{
    hit = (LumenProbeHit)0;
    const float2 principal = gPrincipalPoint;
    const float focal = gCameraFocalPx;
    if (d.z <= 0.f)
        return false; // Toward-camera: no screen hit possible.

    // Start ahead of the probe's own surface (the probe sits on it).
    const float minHitDist = kLumenProbeStartBiasTexels * o.z / focal;
    float t = max(kLumenScreenProbeMinStepDefault, minHitDist * 0.5f);
    float prevDiff = -1.f; // Assume in front of any surface on entry.
    uint steps = 0u;

    while (steps < gMaxMarchSteps && t < kLumenScreenProbeMaxTraceDistanceDefault)
    {
        ++steps;
        const float2 q = lumenProbeProjectToPixel(o + d * t, focal, principal);
        const int2 texel = int2(q);
        if (texel.x < 0 || texel.y < 0 || texel.x >= int(gFrameDim.x) || texel.y >= int(gFrameDim.y))
            return false; // Out of the viewport: no screen hit.
        const uint2 p = uint2(texel);

        const float zt = o.z + d.z * t;
        const float step = max(kLumenScreenProbeMinStepDefault, kLumenScreenProbeRelStepDefault * max(zt, 0.01f));
        const float zSurf = gLinearZ[p].x;
        if (zSurf <= 0.f || zSurf > kLumenScreenProbeMaxSurfaceDepthDefault)
        {
            // Sky / far plane: no surface here; keep marching toward a farther one.
            prevDiff = -1.f;
            t += step;
            continue;
        }

        const float diff = zt - zSurf;
        const float thickness = max(
            kLumenScreenProbeMinThicknessDefault,
            kLumenScreenProbeThicknessScaleDefault * (d.z * step)
        );
        if (diff >= 0.f && (prevDiff < 0.f || diff <= thickness))
        {
            if (t < minHitDist)
            {
                // The probe's own surface / tile: not a GI hit, keep marching.
                prevDiff = diff;
                t += step;
                continue;
            }
            const float confidence = saturate(1.f - diff / max(thickness, 1e-6f));
            hit.radiance = lumenProbeScreenRadiance(p);
            hit.hitDistance = t;
            hit.flags = kLumenProbeHitFlagScreenHit | kLumenProbeHitFlagRadianceReused;
            hit.confidence = confidence;
            hit.hitUV = (float2(p) + 0.5f) * gInvFrameDim;
#if is_valid_gScreenTraceResult
            // Prefer the S4-A1 screen-trace hit UV when that pixel's own trace also
            // resolved a hit (soft prefilter / agreement; S4-C1 will fix the S4-A1
            // self-hit distances, after which st.xy is the true surface UV).
            if (gScreenTraceResult[p].a > 0.f)
                hit.hitUV = float2(gScreenTraceResult[p].x, gScreenTraceResult[p].y);
#endif
            return true;
        }
        prevDiff = diff;
        t += step;
    }
    return false;
}

// -------------------------------------------------------------------------------------
// HWRT inline fallback.
// -------------------------------------------------------------------------------------

/** Resolve the fallback radiance for an HWRT hit at world position hitPosW:
    reuse the per-pixel GI when the hit is visible on screen, else the
    environment. Fills the record's radiance/flags.
*/
void lumenProbeResolveFallbackRadiance(const float3 dirW, const float3 hitPosW, inout LumenProbeHit hit)
{
    const float2 px = lumenProbeWorldToPixel(hitPosW);
    const int2 texel = int2(px);
    if (texel.x >= 0 && texel.y >= 0 && texel.x < int(gFrameDim.x) && texel.y < int(gFrameDim.y))
    {
        // Visibility check: the hit's linear depth must match the surface depth
        // at the projected pixel (i.e. the hit is actually visible).
        const float3 rel = hitPosW - gCameraPosW;
        const float hitDepth = -dot(rel, gCameraForwardW); // trace-view z = linear depth.
        const float surfDepth = gLinearZ[texel].x;
        if (surfDepth > 0.f && abs(hitDepth - surfDepth) <= gDepthChangeThreshold * 50.f)
        {
            hit.radiance = lumenProbeScreenRadiance(uint2(texel));
            hit.flags |= kLumenProbeHitFlagRadianceReused;
            return;
        }
    }
    hit.radiance = lumenProbeEnvRadiance(dirW);
    hit.flags |= kLumenProbeHitFlagEnvironment;
}

/** HWRT inline fallback. Returns true when a hit was resolved (flags has
    kLumenProbeHitFlagHWRTHit); false on miss or degraded fallback.
*/
bool lumenProbeTraceFallback(const float3 originW, const float3 dirW, out LumenProbeHit hit)
{
    hit = (LumenProbeHit)0;
#if LUMEN_GI_PROBE_SCENE_TRACE
    const Ray ray = Ray(originW, dirW, 0.f, 1e30f);
    float hitT = 0.f;
    SceneRayQuery<0> sceneRayQuery;
    const HitInfo sceneHit = sceneRayQuery.traceRay(ray, hitT);
    if (sceneHit.isValid())
    {
        const float3 hitPosW = originW + dirW * hitT;
        hit.hitDistance = hitT;
        hit.flags = kLumenProbeHitFlagHWRTHit;
        lumenProbeResolveFallbackRadiance(dirW, hitPosW, hit);
        return true;
    }
    hit.hitDistance = 0.f;
    hit.flags = kLumenProbeHitFlagHWRTMiss;
    hit.radiance = lumenProbeEnvRadiance(dirW);
    hit.flags |= kLumenProbeHitFlagEnvironment;
    return false;
#else
    if (is_valid(gProbeTLAS))
    {
        RayDesc ray;
        ray.Origin = originW;
        ray.Direction = dirW;
        ray.TMin = 0.f;
        ray.TMax = 1e30f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
        rayQuery.TraceRayInline(
            gProbeTLAS,
            RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
            0xff,
            ray
        );
        rayQuery.Proceed();
        if (rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            const float t = rayQuery.CommittedRayT();
            const float3 hitPosW = originW + dirW * t;
            hit.hitDistance = t;
            hit.flags = kLumenProbeHitFlagHWRTHit;
            lumenProbeResolveFallbackRadiance(dirW, hitPosW, hit);
            return true;
        }
        hit.hitDistance = 0.f;
        hit.flags = kLumenProbeHitFlagHWRTMiss;
        hit.radiance = lumenProbeEnvRadiance(dirW);
        hit.flags |= kLumenProbeHitFlagEnvironment;
        return false;
    }
    // No TLAS / no scene data: degraded fallback (radiance = environment).
    hit.hitDistance = 0.f;
    hit.flags = kLumenProbeHitFlagFallbackUnavailable;
    hit.radiance = lumenProbeEnvRadiance(dirW);
    hit.flags |= kLumenProbeHitFlagEnvironment;
    return false;
#endif
}

// -------------------------------------------------------------------------------------
// Entry points.
// -------------------------------------------------------------------------------------

/** Entry: updateMain -- refresh probe metadata (1 thread / probe). */
void updateProbe(const uint probeIndex)
{
    const uint2 probeGridDims = gProbeGridDims;
    const uint2 gridPos = uint2(probeIndex % probeGridDims.x, probeIndex / probeGridDims.x);
    const float2 probePixel = float2(gridPos) * float(kLumenScreenProbeTileSize) +
                              0.5f * float(kLumenScreenProbeTileSize) + 0.5f;

    const uint2 pixel = uint2(probePixel);
    const float z = gLinearZ[pixel].x;
    const bool active = z > 0.f;

    LumenScreenProbeMeta meta = gProbeMeta[probeIndex];
    const float lastDepth = meta.depth;
    const uint wasActive = meta.active;

    meta.screenPos = probePixel;
    meta.active = active ? kLumenScreenProbeMetaFlagActive : 0u;
    meta.flags = meta.flags & ~kLumenScreenProbeMetaFlagDirty;

    if (active)
    {
        meta.depth = z;
        meta.worldPos = lumenProbeUnprojectToWorld(probePixel, z);
#if is_valid_gNormalRoughnessMaterialID
        const float4 gb = gNormalRoughnessMaterialID[pixel];
        float3 n = lumenProbeDecodeNormal(gb.xy);
        meta.normalW = any(isnan(n)) ? float3(0.f, 1.f, 0.f) : normalize(n);
        meta.materialID = uint(round(saturate(gb.w) * 3.f));
#else
        meta.normalW = float3(0.f, 1.f, 0.f);
        meta.materialID = 0u;
#endif
        // Dirty when the depth moved since the last update or on first activation.
        const bool moved = wasActive != 0u && lastDepth > 0.f && abs(z - lastDepth) > gDepthChangeThreshold;
        if (moved || wasActive == 0u)
            meta.flags |= kLumenScreenProbeMetaFlagDirty;
        meta.age = gFrameIndex - min(meta.lastUpdateFrame, gFrameIndex);
    }
    else
    {
        meta.depth = 0.f;
        meta.worldPos = float3(0.f);
        meta.normalW = float3(0.f, 1.f, 0.f);
        meta.materialID = 0u;
    }
    gProbeMeta[probeIndex] = meta;
}

/** Entry: traceMain -- trace one direction of one probe (1 thread / (probe, dir)). */
void traceProbeDirection(const uint probeIndex, const uint dirIndex)
{
    const LumenScreenProbeMeta meta = gProbeMeta[probeIndex];
    const uint recordIndex = probeIndex * gMaxHitRecordStride + dirIndex;

    if (meta.active == 0u)
    {
        if (dirIndex == 0u)
            InterlockedAdd(gProbeCounters[0].inactiveProbes, 1u);
        return; // No surface at this probe: leave the record stale.
    }

    // Per-frame budget: round-robin over gUpdateInterval plus immediate dirty.
    const uint interval = max(1u, gUpdateInterval);
    const bool due = (meta.flags & kLumenScreenProbeMetaFlagDirty) != 0u ||
                     (probeIndex % interval) == (gFrameIndex % interval);
    if (!due)
    {
        if (dirIndex == 0u)
            InterlockedAdd(gProbeCounters[0].budgetSkipped, 1u);
        return;
    }

    InterlockedAdd(gProbeCounters[0].directionsTraced, 1u);

    uint sampleIndex = 0u;
    const float3 dirW = lumenProbeSampleDirection(probeIndex, dirIndex, gFrameIndex, meta.normalW, sampleIndex);

    // Convert the world direction to the screen-trace view convention.
    const float3 d = normalize(lumenProbeWorldToTraceViewDir(dirW));
    const float2 principal = gPrincipalPoint;
    const float focal = gCameraFocalPx;
    const float3 o = float3(
        (meta.screenPos.x - principal.x) / focal * meta.depth,
        (meta.screenPos.y - principal.y) / focal * meta.depth,
        meta.depth
    );

    LumenProbeHit hit = {};
    hit.hitUV = float2(-1.f, -1.f);

    bool screenHit = false;
    if (d.z > 0.f) // Forward rays only can screen-hit; toward-camera rays go straight to HWRT.
        screenHit = lumenProbeTraceScreenFirst(meta.screenPos, o, d, hit);

    if (screenHit)
    {
        InterlockedAdd(gProbeCounters[0].screenHits, 1u);
    }
    else
    {
        InterlockedAdd(gProbeCounters[0].fallbackAttempts, 1u);
        const float3 originW = lumenProbeRayOrigin(meta.worldPos, meta.normalW, dirW);
        if (lumenProbeTraceFallback(originW, dirW, hit))
            InterlockedAdd(gProbeCounters[0].fallbackHits, 1u);
        else if ((hit.flags & kLumenProbeHitFlagFallbackUnavailable) != 0u)
            InterlockedAdd(gProbeCounters[0].fallbackUnavailable, 1u);
        else
            InterlockedAdd(gProbeCounters[0].fallbackMisses, 1u);
    }

    gProbeHitRecords[recordIndex] = hit;

#if is_valid_gProbeDebug
    // Per-probe march diagnostics (tests only): probe 0 slot holds probe 0, etc.
    if (dirIndex == 0u)
    {
        gProbeDebug[probeIndex * 4u + 0u] = float4(d, meta.normalW.y);
        gProbeDebug[probeIndex * 4u + 1u] = float4(
            float(screenHit ? 1u : 2u), float(d.z > 0.f ? 1u : 0u), meta.depth, hit.hitDistance
        );
        gProbeDebug[probeIndex * 4u + 2u] = float4(hit.radiance, hit.confidence);
        gProbeDebug[probeIndex * 4u + 3u] = float4(hit.hitUV, float(hit.flags), 0.f);
    }
#endif
}

/** Entry: finalizeMain -- average a probe's records into the radiance texture
    and refresh the aging metadata (1 thread / probe).
*/
void finalizeProbe(const uint probeIndex)
{
    LumenScreenProbeMeta meta = gProbeMeta[probeIndex];
    const uint n = max(1u, gDirectionsPerProbe);
    if (meta.active == 0u)
        return;
    const uint interval = max(1u, gUpdateInterval);
    const bool due = (meta.flags & kLumenScreenProbeMetaFlagDirty) != 0u ||
                     (probeIndex % interval) == (gFrameIndex % interval);
    if (!due)
        return;

    float3 sum = float3(0.f);
    uint hitCount = 0u;
    for (uint i = 0u; i < n; ++i)
    {
        const LumenProbeHit hit = gProbeHitRecords[probeIndex * gMaxHitRecordStride + i];
        if ((hit.flags & (kLumenProbeHitFlagScreenHit | kLumenProbeHitFlagHWRTHit)) != 0u)
            ++hitCount;
        sum += hit.radiance;
    }

#if is_valid_gProbeRadiance
    const int2 pixel = int2(meta.screenPos);
    if (pixel.x >= 0 && pixel.y >= 0 && pixel.x < int(gFrameDim.x) && pixel.y < int(gFrameDim.y))
        gProbeRadiance[pixel] = float4(sum / float(n), float(hitCount) / float(n));
#endif

    meta.lastUpdateFrame = gFrameIndex;
    meta.age = 0u;
    meta.updateInterval = interval;
    gProbeMeta[probeIndex] = meta;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void updateMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x < gProbeCount)
        updateProbe(dispatchThreadID.x);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void traceMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint n = max(1u, gDirectionsPerProbe);
    const uint total = gProbeCount * n;
    const uint threadIndex = dispatchThreadID.x;
    if (threadIndex >= total)
        return;
    traceProbeDirection(threadIndex / n, threadIndex % n);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void finalizeMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x < gProbeCount)
        finalizeProbe(dispatchThreadID.x);
}
