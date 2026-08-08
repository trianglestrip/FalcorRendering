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
//  LumenGI - S6-B1 Mesh SDF sampling / sphere trace implementation
//  -------------------------------------------------------------------------------------
//  Implements the contract declared in LumenMeshSDFData.slang:
//    * manual trilinear sampling with clamped cells (deterministic, no HW mip seams)
//    * explicit quantized decode (R8Snorm, R16Float) - formulas mirrored on the CPU
//      side by LumenMeshSDF.cpp (quantization error estimation)
//    * signed-distance convention read from the volume header, never hardcoded
//    * sphere trace with mip selection, hit/miss contract, central-difference normal
//
//  Sampling convention (frozen, matches the .msdf voxel-center layout):
//      voxel (x,y,z) center  <=>  texel (x,y,z) of mip 0
//      continuous voxel coord u = (p_out - bboxMin) * invVoxelSize - 0.5
//      mip m coord            u_m = u / 2^m
//  All clamping is inside the valid cell range [0, dim-1], cell base <= dim-2,
//  so Load never goes out of bounds and the outer half of the border cell
//  interpolates toward the border value (conservative for outside points).
// =====================================================================================

#include "LumenMeshSDFData.slang"

// -------------------------------------------------------------------------------------
// Quantized decode
// -------------------------------------------------------------------------------------

/// Explicit decode of an 8-bit signed code (DXGI R8_SNORM semantics):
///     d = max(int8(code) / 127, -1.0)
/// Hardware Load on an R8_SNORM resource performs exactly this conversion, so
/// this function and the Load-based path are bit-identical. The CPU-side
/// estimator in LumenMeshSDF.cpp uses the same formula.
float lumenMeshSDFDecodeSnorm8(uint code, float quantRange)
{
    int bits = int((code << 24) & 0xFF000000u) >> 24; // sign-extend the low byte
    return max(float(bits) / 127.f, -1.0f) * quantRange;
}

/// R16Float needs no decode: Load returns the float32 value (relative error
/// of the stored f16 is bounded by 2^-11). Kept as a named step for clarity
/// and for parity with the CPU error estimator.
float lumenMeshSDFDecodeR16Float(float stored)
{
    return stored;
}

// -------------------------------------------------------------------------------------
// Coordinate transforms (uniform scale, so direction vectors just rescale)
// -------------------------------------------------------------------------------------

float3 lumenMeshSDFWorldToOutput(LumenMeshSDFVolume vol, float3 worldPos)
{
    return worldPos * vol.desc.normalizationScale;
}

float3 lumenMeshSDFOutputToWorld(LumenMeshSDFVolume vol, float3 outputPos)
{
    return outputPos * vol.desc.invNormalizationScale;
}

/// Continuous voxel coordinates on mip 0; voxel i sits at u == i.
float3 lumenMeshSDFWorldToVoxel(LumenMeshSDFVolume vol, float3 worldPos)
{
    return (lumenMeshSDFWorldToOutput(vol, worldPos) - vol.desc.bboxMin) * vol.desc.invVoxelSize - 0.5f;
}

float3 lumenMeshSDFVoxelToWorld(LumenMeshSDFVolume vol, float3 voxel)
{
    return lumenMeshSDFOutputToWorld(vol, vol.desc.bboxMin + (voxel + 0.5f) * vol.desc.voxelSize);
}

/// World distance -> output distance (uniform scale: d_out = d_world * scale).
float lumenMeshSDFWorldToOutputDistance(LumenMeshSDFVolume vol, float worldDist)
{
    return worldDist * vol.desc.normalizationScale;
}

float lumenMeshSDFOutputToWorldDistance(LumenMeshSDFVolume vol, float outputDist)
{
    return outputDist * vol.desc.invNormalizationScale;
}

// -------------------------------------------------------------------------------------
// Signed-distance convention (read from the header byte - never hardcoded)
// -------------------------------------------------------------------------------------

/// Converts a stored sample into the mathematical convention (positive outside).
float lumenMeshSDFToMathConvention(LumenMeshSDFVolume vol, float stored)
{
    return stored * (vol.desc.signConvention == kLumenMeshSDFSignConventionPositiveOutside ? 1.f : -1.f);
}

// -------------------------------------------------------------------------------------
// Trilinear sampling
// -------------------------------------------------------------------------------------

uint3 lumenMeshSDFMipDims(LumenMeshSDFVolume vol, uint mip)
{
    // dims[m] = max(1, ceil(dims[m-1] / 2)); dims[0] = resolution.
    uint3 dims = vol.desc.resolution;
    for (uint m = 0; m < mip; ++m)
        dims = max(uint3(1, 1, 1), (dims + uint3(1, 1, 1)) >> 1);
    return dims;
}

/// Stored (unconverted) value at continuous voxel coords uMip on mip `mip`,
/// manual trilinear with clamped cells. Returns the value in output space,
/// still scaled by quantRange for snorm mips and in stored sign convention.
/// Cell/corner clamping is dims-safe (also for 1-texel mips): every corner
/// index is clamped to [0, dims-1], so Load never goes out of bounds and the
/// outer half of the border cell interpolates toward the border value
/// (conservative for outside points).
float lumenMeshSDFSampleStored(LumenMeshSDFVolume vol, float3 uMip, uint mip)
{
    const uint3 dims = lumenMeshSDFMipDims(vol, mip);
    const uint3 maxCoord = dims - uint3(1, 1, 1); // dims >= 1 guaranteed
    const float3 cell = clamp(uMip, float3(0.f, 0.f, 0.f), float3(maxCoord));
    const uint3 base = min(uint3(cell), maxCoord);
    const uint3 bx = uint3(base.x < maxCoord.x ? 1u : 0u, base.y < maxCoord.y ? 1u : 0u,
                           base.z < maxCoord.z ? 1u : 0u);
    const float3 f = cell - float3(base);

    Texture3D<float> tex = vol.mips[mip];
    const float v000 = tex.Load(int4(base + uint3(0, 0, 0), 0)).x;
    const float v100 = tex.Load(int4(base + uint3(bx.x, 0, 0), 0)).x;
    const float v010 = tex.Load(int4(base + uint3(0, bx.y, 0), 0)).x;
    const float v110 = tex.Load(int4(base + uint3(bx.x, bx.y, 0), 0)).x;
    const float v001 = tex.Load(int4(base + uint3(0, 0, bx.z), 0)).x;
    const float v101 = tex.Load(int4(base + uint3(bx.x, 0, bx.z), 0)).x;
    const float v011 = tex.Load(int4(base + uint3(0, bx.y, bx.z), 0)).x;
    const float v111 = tex.Load(int4(base + uint3(bx.x, bx.y, bx.z), 0)).x;

    const float c0 = lerp(lerp(v000, v100, f.x), lerp(v010, v110, f.x), f.y);
    const float c1 = lerp(lerp(v001, v101, f.x), lerp(v011, v111, f.x), f.y);
    return lerp(c0, c1, f.z);
}

/// Signed distance (math convention, output space) at an explicit mip.
float lumenMeshSDFSignedDistanceOutput(LumenMeshSDFVolume vol, float3 outputPos, uint mip)
{
    const float3 u = (outputPos - vol.desc.bboxMin) * vol.desc.invVoxelSize - 0.5f;
    return lumenMeshSDFToMathConvention(vol, lumenMeshSDFSampleStored(vol, u / float(1u << mip), mip));
}

/// Signed distance (math convention, world space) at mip 0.
float lumenMeshSDFSignedDistanceWorld(LumenMeshSDFVolume vol, float3 worldPos)
{
    return lumenMeshSDFSignedDistanceOutput(vol, lumenMeshSDFWorldToOutput(vol, worldPos), 0);
}

// -------------------------------------------------------------------------------------
// Mip selection
// -------------------------------------------------------------------------------------

/// Coarsest mip with 2^mip fine voxels <= |d|: |d| < 2 voxels -> mip 0,
/// |d| in [2, 4) voxels -> mip 1, ... (frozen rule, see Data.slang).
uint lumenMeshSDFSelectMip(LumenMeshSDFVolume vol, float absDistanceOutput)
{
    const float fineVoxels = absDistanceOutput * vol.desc.invVoxelSize;
    const float m = floor(log2(max(fineVoxels, 1.f)));
    return min(uint(m), vol.desc.mipCount - 1u);
}

// -------------------------------------------------------------------------------------
// Normal estimation (central differences on mip 0, world space)
// -------------------------------------------------------------------------------------

float3 lumenMeshSDFNormal(LumenMeshSDFVolume vol, float3 worldPos)
{
    const float deltaWorld = kLumenMeshSDFNormalDeltaScale *
        lumenMeshSDFOutputToWorldDistance(vol, vol.desc.voxelSize);
    const float3 p = worldPos;
    const float3 n = float3(
        lumenMeshSDFSignedDistanceWorld(vol, p + float3(deltaWorld, 0.f, 0.f)) -
        lumenMeshSDFSignedDistanceWorld(vol, p - float3(deltaWorld, 0.f, 0.f)),
        lumenMeshSDFSignedDistanceWorld(vol, p + float3(0.f, deltaWorld, 0.f)) -
        lumenMeshSDFSignedDistanceWorld(vol, p - float3(0.f, deltaWorld, 0.f)),
        lumenMeshSDFSignedDistanceWorld(vol, p + float3(0.f, 0.f, deltaWorld)) -
        lumenMeshSDFSignedDistanceWorld(vol, p - float3(0.f, 0.f, deltaWorld))
    );
    const float len = length(n);
    return len > 1e-12f ? n / len : float3(0.f, 0.f, 1.f);
}

// -------------------------------------------------------------------------------------
// Sphere trace
// -------------------------------------------------------------------------------------

LumenMeshSDFTraceResult lumenMeshSDFTrace(
    LumenMeshSDFVolume vol,
    LumenMeshSDFTraceParams params,
    float3 rayOriginWorld,
    float3 rayDirWorld,
    float tMin,
    float tMax
)
{
    LumenMeshSDFTraceResult result;
    result.hit = false;
    result.t = 0.f;
    result.d = 0.f;
    result.normal = float3(0.f, 0.f, 1.f);
    result.steps = 0;
    result.minAbsDistance = 3.4e38f;
    result.missReason = LumenMeshSDFMissReason::None;

    // Early outs: no grid, unreliable sign.
    if (vol.desc.mipCount == 0)
    {
        result.missReason = LumenMeshSDFMissReason::NoGrid;
        return result;
    }
    if ((vol.desc.flags & kLumenMeshSDFFlagSignReliable) == 0u)
    {
        result.missReason = LumenMeshSDFMissReason::SignUnreliable;
        return result;
    }
    if (tMax <= tMin)
    {
        result.missReason = LumenMeshSDFMissReason::RayMissedBounds;
        return result;
    }

    const float dirLen = length(rayDirWorld);
    if (dirLen <= 1e-12f)
    {
        result.missReason = LumenMeshSDFMissReason::RayMissedBounds;
        return result;
    }
    const float3 dir = rayDirWorld / dirLen; // unit world direction; t is world distance

    const float maxSteps = params.maxSteps == 0 ? kLumenMeshSDFMaxSteps : params.maxSteps;
    const float minStepScale = params.minStepScale == 0.f ? kLumenMeshSDFMinStepScale : params.minStepScale;
    const float hitEpsScale = params.hitEpsScale == 0.f ? kLumenMeshSDFHitEpsScale : params.hitEpsScale;
    const float normalDeltaScale = params.normalDeltaScale == 0.f ? kLumenMeshSDFNormalDeltaScale : params.normalDeltaScale;

    // Work in continuous voxel space (mip 0) along the ray: u(t) = u0 + t * du.
    const float3 u0 = lumenMeshSDFWorldToVoxel(vol, rayOriginWorld);
    const float3 du = dir * vol.desc.normalizationScale * vol.desc.invVoxelSize;

    // World-space step constants derived from the volume voxel size.
    const float voxelWorld = lumenMeshSDFOutputToWorldDistance(vol, vol.desc.voxelSize);
    const float minStep = minStepScale * voxelWorld;
    const float hitEps = hitEpsScale * voxelWorld;

    float t = tMin;
    uint steps = 0;
    float dPrev = 1.f;
    float tPrev = tMin;

    // Probe at the segment start.
    float d = lumenMeshSDFToMathConvention(vol, lumenMeshSDFSampleStored(vol, u0, 0));
    result.minAbsDistance = min(result.minAbsDistance, abs(d));

    if (d <= 0.f && abs(d) <= hitEps)
    {
        // Already on / inside the surface at tMin.
        result.hit = true;
        result.t = t;
        result.d = d;
        result.normal = lumenMeshSDFNormal(vol, rayOriginWorld + t * dir);
        return result;
    }

    for (; steps < maxSteps; ++steps)
    {
        const float absD = abs(d);
        result.minAbsDistance = min(result.minAbsDistance, absD);

        // Step size: |d| clamped to the minimum; never overshoot tMax.
        float step = max(absD, minStep);
        if (t + step > tMax)
        {
            result.missReason = LumenMeshSDFMissReason::RayMissedBounds;
            return result;
        }

        // Track the previous sample for hit refinement.
        tPrev = t;
        dPrev = d;
        t += step;

        // Probe at the new position, picking a mip from the step scale so that
        // the |d| we just used covers >= 2^mip voxels (see lumenMeshSDFSelectMip).
        const float3 u = u0 + t * du;
        const uint mip = lumenMeshSDFSelectMip(vol, lumenMeshSDFWorldToOutputDistance(vol, absD));
        d = lumenMeshSDFToMathConvention(vol, lumenMeshSDFSampleStored(vol, u / float(1u << mip), mip));
        result.minAbsDistance = min(result.minAbsDistance, abs(d));

        if (d <= 0.f && abs(d) <= hitEps)
        {
            // Crossing (or landing inside within tolerance): refine t by linear
            // secant interpolation between the last two samples.
            const float denom = dPrev - d;
            if (abs(denom) > 1e-12f)
                t = tPrev + (t - tPrev) * (dPrev / denom);
            result.hit = true;
            result.d = d;
            result.normal = lumenMeshSDFNormal(vol, rayOriginWorld + t * dir);
            break;
        }
        // Inside but coarse-overshot (|d| > hitEps): keep walking with min steps.
    }

    result.steps = steps + 1;
    if (!result.hit && steps >= maxSteps)
        result.missReason = LumenMeshSDFMissReason::MaxSteps;
    return result;
}

LumenMeshSDFTraceResult lumenMeshSDFTrace(
    LumenMeshSDFVolume vol,
    float3 rayOriginWorld,
    float3 rayDirWorld,
    float tMin,
    float tMax
)
{
    LumenMeshSDFTraceParams defaults;
    defaults.maxSteps = 0;
    defaults.minStepScale = 0.f;
    defaults.hitEpsScale = 0.f;
    defaults.normalDeltaScale = 0.f;
    return lumenMeshSDFTrace(vol, defaults, rayOriginWorld, rayDirWorld, tMin, tMax);
}
