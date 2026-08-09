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
//  LumenGI - S6-B4 Global Distance Field sphere trace (sampling / tracing library)
//  -------------------------------------------------------------------------------------
//  Samples the S6-B3 GDF clipmap and sphere-traces it:
//      step = distanceField value at the current point (world units, clamped to a
//      min step), hit when the field <= hitEps = hitEpsScale * voxelSize of the level
//      that owns the sample point. Outputs hit / t / missReason / stepCount / minDistance.
//
//  LEVEL SELECTION DURING TRACE (mirror of the CPU levelIndexForWorld)
//  -------------------------------------------------------------------------------------
//  Every sample picks the level that owns the query point: finest dynamic level whose
//  footprint contains the point, else finest static level, else the coarsest level
//  (where worldToIndex reports inBounds == false and the sample returns the level's empty
//  distance - treated as "empty space" so the ray keeps marching). All world <-> voxel
//  math is shared with the compose pass (LumenGDFData.slang) and mirrors the CPU clipmap.
//
//  HIT TYPE SEMANTICS (interop)
//  -------------------------------------------------------------------------------------
//  On a hit, LumenGDFTraceResult::hitType is set to kLumenGDFHitType == 6, which equals
//  Falcor's HitType::SDFGrid (Source/Falcor/Scene/HitInfoType.slang). A consumer that
//  routes by Falcor hit types can therefore treat a GDF hit as HitType::SDFGrid directly;
//  a consumer that keeps the GDF hit distinct can compare against kLumenGDFHitType. The
//  GDF trace is intentionally independent of the Falcor ray-tracing types (it has no
//  dependency on Scene/Raster or the ray-generation intrinsics).
//
//  RESOURCE-FREE LIBRARY (import-safe)
//  -------------------------------------------------------------------------------------
//  This module declares NO global resources and NO constant buffer, so it can be `#include`d
//  by any number of consumer shaders without name clashes (same design as its sibling
//  LumenMeshSDFSampling.slang, and the opposite of LumenScreenTraceData.slang which owns its
//  globals). The consumer owns the bindings:
//      Texture3D<float> gGDFLevels[kLumenGDFMaxLevels];       // SRVs (R16Float / R8Snorm)
//      StructuredBuffer<LumenGDFLevelParams> gGDFLevelTable;
//      cbuffer LumenGDFSphereTraceCB { LumenGDFClipmapParams gClipmap; }
//      ... LumenGDFBindings g = { gGDFLevels, gGDFLevelTable }; ...
//  Validity is guarded by VALUE (c.levelCount == 0 -> NoGrid), which is the correct guard
//  for a parameterized library; the caller's is_valid_* guards cover unbound resources.
// =====================================================================================

#include "LumenGDFData.slang"

// -------------------------------------------------------------------------------------
// Bindings passed to the trace functions. The consumer declares its own cbuffer + SRVs and
// constructs this value with aggregate initialization, e.g.
//     LumenGDFBindings g = { gGDFLevels, gGDFLevelTable };
// Kept as a value struct so the trace entry points stay host-global-free (same style as
// LumenMeshSDFSampling.slang's LumenMeshSDFVolume).
// -------------------------------------------------------------------------------------
struct LumenGDFBindings
{
    Texture3D<float> levels[kLumenGDFMaxLevels];
    StructuredBuffer<LumenGDFLevelParams> levelTable;
};

// -------------------------------------------------------------------------------------
// Sampling
// -------------------------------------------------------------------------------------

/// Trilinear sample of the GDF at `worldPos`. Picks the owning level (CPU-levelIndexForWorld
/// rule), maps world -> continuous voxel coords, and trilinearly interpolates the quantized
/// field with clamped cells (deterministic; identical clamp semantics to the S6-B1 sampler).
/// Returns the world-space distance (positive outside). `sampleLevel` reports the level that
/// owned the sample (needed for the hit epsilon). `valid` is false when the point is outside
/// every level footprint; the returned value is then that level's empty distance (the caller
/// keeps marching).
float lumenGDFSample(LumenGDFBindings g, LumenGDFClipmapParams c, float3 worldPos, out uint sampleLevel, out bool valid)
{
    if (c.levelCount == 0u)
    {
        valid = false;
        sampleLevel = 0;
        return c.emptyDistance > 0.f ? c.emptyDistance : kLumenGDFMaxDistance;
    }

    const uint level = lumenGDFLevelIndexForWorld(g.levelTable, c, worldPos);
    sampleLevel = level;
    const LumenGDFLevelParams lp = g.levelTable[level];
    const float emptyDistance = lumenGDFEmptyDistanceFor(lp, c);

    int3 index;
    bool inBounds;
    lumenGDFWorldToIndex(g.levelTable, c, level, worldPos, index, inBounds);
    if (!inBounds)
    {
        // Outside every level footprint (the coarsest level's worldToIndex reported
        // out of bounds): treat as empty space so sphere tracing does not stop here.
        valid = false;
        return emptyDistance;
    }

    // Continuous voxel coordinates: value at voxel center i sits at v == i (voxel-center
    // convention). Cells clamp to [0, R-1], base <= R-2, so Load never goes out of bounds
    // and the outer half of the border cell interpolates toward the border value
    // (conservative for outside points - same rule as the S6-B1 sampler).
    const uint R = lp.resolution;
    const float3 v = (worldPos - lumenGDFGridMin(g.levelTable, c, level)) / max(lp.voxelSize, 1e-12f);
    const uint3 maxCoord = uint3(R - 1u);
    const float3 cell = clamp(v, float3(0.f, 0.f, 0.f), float3(maxCoord));
    const uint3 base = min(uint3(cell), maxCoord);
    const uint3 bx = uint3(base.x < maxCoord.x ? 1u : 0u, base.y < maxCoord.y ? 1u : 0u, base.z < maxCoord.z ? 1u : 0u);
    const float3 f = cell - float3(base);

    Texture3D<float> tex = g.levels[level];
    const float v000 = tex.Load(int4(int3(base + uint3(0, 0, 0)), 0)).x;
    const float v100 = tex.Load(int4(int3(base + uint3(bx.x, 0, 0)), 0)).x;
    const float v010 = tex.Load(int4(int3(base + uint3(0, bx.y, 0)), 0)).x;
    const float v110 = tex.Load(int4(int3(base + uint3(bx.x, bx.y, 0)), 0)).x;
    const float v001 = tex.Load(int4(int3(base + uint3(0, 0, bx.z)), 0)).x;
    const float v101 = tex.Load(int4(int3(base + uint3(bx.x, 0, bx.z)), 0)).x;
    const float v011 = tex.Load(int4(int3(base + uint3(0, bx.y, bx.z)), 0)).x;
    const float v111 = tex.Load(int4(int3(base + uint3(bx.x, bx.y, bx.z)), 0)).x;

    const float c0 = lerp(lerp(v000, v100, f.x), lerp(v010, v110, f.x), f.y);
    const float c1 = lerp(lerp(v001, v101, f.x), lerp(v011, v111, f.x), f.y);
    const float d = lerp(c0, c1, f.z);

    valid = true;
    return lumenGDFDecode(lp, d);
}

// -------------------------------------------------------------------------------------
// Sphere trace
// -------------------------------------------------------------------------------------

/// Sphere-trace the GDF. Sphere march: at the current point, step by the (non-negative)
/// field value clamped to minStep; a crossing is a hit when d <= hitEps = hitEpsScale *
/// voxelSize of the level that owns the sample point. t is world distance along the unit
/// direction. All miss reasons are reported in LumenGDFMissReason; the caller can fall
/// back to HWRT / detail-SDF on any miss.
LumenGDFTraceResult lumenGDFTrace(
    LumenGDFBindings g,
    LumenGDFClipmapParams c,
    float3 rayOriginWorld,
    float3 rayDirWorld,
    float tMin,
    float tMax,
    LumenGDFTraceParams params)
{
    LumenGDFTraceResult result;
    result.hit = false;
    result.t = 0.f;
    result.d = 0.f;
    result.steps = 0;
    result.minDistance = 3.4e38f;
    result.hitType = 0;
    result.missReason = LumenGDFMissReason::None;

    // Early outs (all deterministic miss reasons; never silent).
    if (c.levelCount == 0u)
    {
        result.missReason = LumenGDFMissReason::NoGrid;
        return result;
    }
    if (tMax <= tMin)
    {
        result.missReason = LumenGDFMissReason::RayMissedBounds;
        return result;
    }
    const float dirLen = length(rayDirWorld);
    if (dirLen <= 1e-12f)
    {
        result.missReason = LumenGDFMissReason::DegenerateRay;
        return result;
    }
    const float3 dir = rayDirWorld / dirLen; // unit direction; t is world distance

    const uint maxSteps = params.maxSteps == 0u ? kLumenGDFMaxSteps : params.maxSteps;
    const float minStepScale = params.minStepScale == 0.f ? kLumenGDFMinStepScale : params.minStepScale;
    const float hitEpsScale = params.hitEpsScale == 0.f ? kLumenGDFHitEpsScale : params.hitEpsScale;

    float t = tMin;
    uint steps = 0;

    // Probe at the segment start.
    uint level = 0;
    bool valid = false;
    float d = lumenGDFSample(g, c, rayOriginWorld + t * dir, level, valid);
    result.minDistance = min(result.minDistance, abs(d));

    if (valid && d <= hitEpsScale * max(g.levelTable[level].voxelSize, 1e-12f))
    {
        // Already on / inside the surface at tMin.
        result.hit = true;
        result.t = t;
        result.d = d;
        result.hitType = kLumenGDFHitType;
        result.steps = 1;
        return result;
    }

    // Sphere march. Step = |d| clamped to minStep; never overshoot tMax. A sample that
    // went invalid (out of footprint) returns the level's empty distance, which is large
    // enough to keep marching through empty space.
    float tPrev = t;
    float dPrev = d;
    for (; steps < maxSteps; ++steps)
    {
        const float absD = abs(d);
        result.minDistance = min(result.minDistance, absD);

        float step = max(absD, minStepScale * max(g.levelTable[level].voxelSize, 1e-12f));
        if (t + step > tMax)
        {
            result.missReason = LumenGDFMissReason::RayMissedBounds;
            return result;
        }
        tPrev = t;
        dPrev = d;
        t += step;

        d = lumenGDFSample(g, c, rayOriginWorld + t * dir, level, valid);
        result.minDistance = min(result.minDistance, abs(d));

        if (valid && d <= hitEpsScale * max(g.levelTable[level].voxelSize, 1e-12f))
        {
            // Crossing (or landing within tolerance): refine t by linear secant
            // interpolation between the last two samples (identical to the S6-B1 sampler).
            const float denom = dPrev - d;
            if (abs(denom) > 1e-12f)
                t = tPrev + (t - tPrev) * (dPrev / denom);
            result.hit = true;
            result.d = d;
            result.t = t;
            result.hitType = kLumenGDFHitType;
            break;
        }
        // Inside but coarse-overshot (|d| > hitEps): keep walking with min steps.
    }

    result.steps = steps + 1;
    if (!result.hit && steps >= maxSteps)
        result.missReason = LumenGDFMissReason::MaxSteps;
    return result;
}

/// Convenience entry (the S6-B4 `traceGDF(origin, dir, params, out hit)` contract): traces
/// from tMin = 0 to params.tMax (0 -> kLumenGDFMaxDistance). Returns hit in `result` and
/// returns the bool for early-outs.
bool traceGDF(
    LumenGDFBindings g,
    LumenGDFClipmapParams c,
    float3 rayOriginWorld,
    float3 rayDirWorld,
    LumenGDFTraceParams params,
    out LumenGDFTraceResult result)
{
    const float tMax = params.tMax > 0.f ? params.tMax : kLumenGDFMaxDistance;
    result = lumenGDFTrace(g, c, rayOriginWorld, rayDirWorld, 0.f, tMax, params);
    return result.hit;
}
