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

/** S4-B1: hierarchical screen-space ray march (exact perspective model).
 *
 * Marching model (view space, y down to match pixel space):
 *
 *   - The ray starts at the surface point of the input pixel with linear
 *     depth z0 = linearZ[p].xy.x. The view-space origin is
 *       o = ( (p.x - cx)/f * z0, (p.y - cy)/f * z0, z0 ).
 *   - The march parameter s is the screen displacement in texels along the
 *     screen direction sdir (projection of the view direction d onto the
 *     screen). For a screen point q(s) = p0 + sdir * s, the view-space
 *     point where the ray crosses the view ray through q is the unique
 *     t with project(o + t*d) = q:
 *       h = (q - principal) / f
 *       t = (o.x - h.x * o.z) / (h.x * d.z - d.x)      (x equation)
 *       t = (o.y - h.y * o.z) / (h.y * d.z - d.y)      (y equation, fallback)
 *     and the ray depth at that point is z(t) = o.z + t * d.z. This is the
 *     exact perspective solution, so the march never accumulates screen-
 *     space error (the thickness window absorbs the discrete step).
 *
 *   - gHZB is a max-depth hierarchy: mip m texel covers 2^m x 2^m texels
 *     and stores the MAXIMUM linear depth of the block. Since z grows away
 *     from the camera, "the ray is in front of everything in this block" is
 *     exactly z(t) >= maxDepth(cell). Only z(t) < maxDepth can contain an
 *     occluder, so the trace descends one mip without advancing; when the
 *     ray is in front, the whole cell is skipped and the mip is re-selected
 *     from the marched distance (coarser farther away).
 *
 *   - Thickness test at mip 0 (conservative):
 *       | z(t) - linearZ[texel].x | <= thickness(texel, step)
 *     with thickness(p, step) = max(gMinThickness,
 *                                    gThicknessScale * maxSlope * step)
 *     using the linearZ slope stored in .y. The slope term scales the
 *     acceptance window with the march step so steep silhouettes do not
 *     tunnel; the step enters in texels of the current mip.
 *
 *   - confidence (hit) = saturate(1 - |z(t) - zSurface| / thickness),
 *     1 on a dead-center hit decaying to 0 at the window edge.
 *
 * Deterministic: no randomness, no divergence-based early-outs, all
 * decisions are pure functions of the inputs.
 */

#include "LumenScreenTraceData.slang"

#ifndef is_valid_gHZBMips
#define is_valid_gHZBMips 0
#endif
#ifndef is_valid_gRayDirection
#define is_valid_gRayDirection 0
#endif
#ifndef is_valid_gLinearZ
#define is_valid_gLinearZ 0
#endif

#define is_valid(name) (is_valid_##name != 0)

static const float kLumenScreenTraceMinProjectDenom = 1e-8f;

/** Sample the maximum depth of the HZB cell containing pixel p at mip m. */
float hzbSampleMaxDepth(uint2 p, uint mip)
{
    // Ceil-halving, matching LumenHZB.h / LumenHZBBuild.cs.slang: every mip
    // fully covers its parent so the max chain is conservative.
    const uint2 scale = uint2(1u << min(mip, 31u), 1u << min(mip, 31u));
    const uint2 mipSize = max((gFrameDim + scale - 1u) / scale, uint2(1u, 1u));
    const uint2 cell = clamp(p / scale, uint2(0u, 0u), mipSize - 1u);
    return gHZBMips[mip].Load(int3(cell, 0)).x;
}

/** Project a view-space point onto pixel coordinates (float, texel space). */
float2 projectToPixel(const float3 viewPos, const float focal, const float2 principal)
{
    return float2(viewPos.x / viewPos.z * focal + principal.x, viewPos.y / viewPos.z * focal + principal.y);
}

/** Conservative thickness at a screen texel for a given step length. */
float thicknessAt(const uint2 texel, const float stepLen)
{
    const float slope = gLinearZ[texel].y;
    return max(gMinThickness, gThicknessScale * abs(slope) * stepLen);
}

void writeMiss(uint2 pixel, uint reason)
{
    gScreenTraceResult[pixel] = float4(0.f, 0.f, 0.f, -((float)reason + 1.f));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= gFrameDim))
    {
        return;
    }

    if (!is_valid(gHZBMips) || !is_valid(gLinearZ))
    {
        writeMiss(pixel, kLumenScreenTraceMissReasonOutOfScreen);
        return;
    }

    const float z0 = gLinearZ[pixel].x;
    if (z0 <= 0.f)
    {
        // No surface at this pixel (sky / background): nothing to trace from.
        writeMiss(pixel, kLumenScreenTraceMissReasonOutOfScreen);
        return;
    }

    const float2 principal = gPrincipalPoint;
    const float focal = gCameraFocalPx;

    // View-space ray origin at the surface of this pixel (y down).
    const float3 o = float3(
        (float(pixel.x) + 0.5f - principal.x) / focal * z0,
        (float(pixel.y) + 0.5f - principal.y) / focal * z0,
        z0
    );

    float3 d = float3(0.f, 0.f, -1.f);
    if (is_valid(gRayDirection))
    {
        d = normalize(gRayDirection[pixel].xyz);
    }
    // Only rays moving away from the camera (negative view z) can be traced:
    // toward-camera rays immediately re-hit the starting surface.
    if (d.z >= 0.f)
    {
        writeMiss(pixel, kLumenScreenTraceMissReasonOutOfScreen);
        return;
    }

    // Screen-space march direction from one unit of the view ray.
    const float2 p0 = float2(pixel) + 0.5f;
    const float2 q1 = projectToPixel(o + d, focal, principal);
    const float2 sdir = q1 - p0;
    const float sdirLen = length(sdir);
    if (sdirLen < 1e-4f)
    {
        // Ray projects to a single pixel (straight ahead): it either exits
        // through depth or never leaves the screen; march it in depth steps
        // against the exact HZB at this pixel.
        float t = 1.f;
        uint steps = 0u;
        while (steps < gMaxSteps)
        {
            ++steps;
            const float zt = o.z + d.z * t;
            const uint mip = min(gMaxMip, uint(floor(log2(max(1.0, t)))));
            const float maxDepth = hzbSampleMaxDepth(pixel, mip);
            if (zt >= maxDepth)
            {
                t += float(1u << mip);
                continue;
            }
            const float zSurface = gLinearZ[pixel].x;
            const float thickness = thicknessAt(pixel, 1.f);
            if (abs(zt - zSurface) <= thickness)
            {
                const float confidence = saturate(1.f - abs(zt - zSurface) / max(thickness, 1e-6f));
                const float2 hitUV = (float2(pixel) + 0.5f) * gInvFrameDim;
                gScreenTraceResult[pixel] = float4(hitUV, t, confidence);
                return;
            }
            t += 1.f;
        }
        writeMiss(pixel, kLumenScreenTraceMissReasonMaxSteps);
        return;
    }
    const float2 dir2 = sdir / sdirLen;

    // March.
    float s = gStepEpsilon; // Texels; biased forward to skip the start surface.
    uint mip = gStartMip;
    uint steps = 0u;

    while (steps < gMaxSteps)
    {
        ++steps;

        const float2 q = p0 + dir2 * s;
        const int2 texel = int2(q);
        if (texel.x < 0 || texel.y < 0 || texel.x >= int(gFrameDim.x) || texel.y >= int(gFrameDim.y))
        {
            writeMiss(pixel, kLumenScreenTraceMissReasonOutOfScreen);
            return;
        }

        // Exact ray depth at screen point q (see header for the derivation).
        const float2 h = (q - principal) / focal;
        const float denomX = h.x * d.z - d.x;
        const float denomY = h.y * d.z - d.y;
        float t;
        if (abs(denomX) > kLumenScreenTraceMinProjectDenom)
        {
            t = (o.x - h.x * o.z) / denomX;
        }
        else if (abs(denomY) > kLumenScreenTraceMinProjectDenom)
        {
            t = (o.y - h.y * o.z) / denomY;
        }
        else
        {
            // View ray collinear with the screen ray: degenerate, bail out.
            writeMiss(pixel, kLumenScreenTraceMissReasonOutOfScreen);
            return;
        }
        if (t < 0.f)
        {
            // Behind the camera: cannot be a screen hit.
            writeMiss(pixel, kLumenScreenTraceMissReasonOutOfScreen);
            return;
        }
        const float zt = o.z + d.z * t;

        const uint2 p = uint2(texel);
        const float maxDepth = hzbSampleMaxDepth(p, mip);
        const float cellWidth = float(1u << mip);

        if (zt >= maxDepth)
        {
            // In front of everything in this cell: skip the whole cell and
            // re-select the mip from the marched distance (coarser farther).
            s += cellWidth;
            mip = min(gMaxMip, gStartMip + uint(floor(log2(max(1.0, s)))));
            continue;
        }

        if (mip > 0u)
        {
            // Potential occluder in the cell: descend one mip without
            // advancing, so the same region is re-tested at finer detail.
            --mip;
            continue;
        }

        // Mip 0: exact thickness test against the full-resolution depth.
        const float zSurface = gLinearZ[p].x;
        const float thickness = thicknessAt(p, 1.f);
        const float depthDiff = zt - zSurface;
        if (abs(depthDiff) <= thickness)
        {
            const float confidence = saturate(1.f - abs(depthDiff) / max(thickness, 1e-6f));
            const float2 hitUV = (float2(p) + 0.5f) * gInvFrameDim;
            gScreenTraceResult[pixel] = float4(hitUV, t, confidence);
            return;
        }
        // Not a hit here: advance one texel (the surface is either behind
        // the ray or still ahead but outside the acceptance window).
        s += 1.f;
        mip = min(gMaxMip, gStartMip + uint(floor(log2(max(1.0, s)))));
    }

    writeMiss(pixel, kLumenScreenTraceMissReasonMaxSteps);
}
