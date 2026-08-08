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

/** S4-A1: hierarchical Z-buffer (HZB) builder.
 *
 * Pure compute (no imports), one dispatch per mip level. The host (S4-A1
 * integration) binds sources, target UAV and the CB before every dispatch
 * and derives all sizes from LumenHZB.h — this shader makes no assumption
 * about the full-resolution frame size beyond what the CB says.
 *
 * Mip chain contract (frozen; mirrors LumenHZB.h, which the S4-B1 screen
 * trace consumes through hzbSampleMaxDepth in LumenScreenTrace.cs.slang):
 *
 *   - mip m  dims  = max(frameDim >> m, 1x1).
 *   - mip 0  holds the full-resolution linear depth, identical to
 *     GBufferRT linearZ.x. It is produced by the gSourceIsLinearZ == 1
 *     dispatch, which copies gLinearZSource[pixel].x (a no-op pool).
 *   - mip m+1 texel (x, y) = MAX of mip m texels
 *         (2x, 2y), (2x+1, 2y), (2x, 2y+1), (2x+1, 2y+1).
 *   - Boundary rule (explicit): out-of-range source coordinates are
 *     clamped to the source dims - 1, i.e. the edge texel is duplicated
 *     and no coordinate below 0 or above the source is ever sampled.
 *     Under target = max(source >> 1, 1) the clamp only ever fires when
 *     a source dim is 1, where it makes the block an identity copy of the
 *     single texel — so "clamp" and "skip out-of-range" produce identical
 *     results for every allowed size pair.
 *
 * Per-dispatch binding contract (host):
 *   - gSourceIsLinearZ == 1 (mip 0): bind gLinearZSource = full-resolution
 *     RG32F linearZ texture (only .x is read); gHZBSource is left unbound
 *     and is never dereferenced. gSourceMipSize = gTargetMipSize = frame
 *     dims.
 *   - gSourceIsLinearZ == 0 (mip m > 0): bind gHZBSource = R32F SRV of
 *     mip m-1; gLinearZSource is left unbound. gSourceMipSize = mip m-1
 *     dims, gTargetMipSize = mip m dims.
 *   - gHZBTarget is always the R32F UAV of the target mip.
 *   - Dispatch: numthreads(16, 16, 1), group count
 *     ceil(gTargetMipSize / 16) per axis (see LumenHZB.h).
 *
 * Deterministic: every thread writes exactly one target texel from at most
 * four source texels; no atomics, no shared memory, no barriers.
 */

cbuffer LumenHZBBuildCB
{
    uint2 gSourceMipSize;    ///< Source mip dims (frame dims when gSourceIsLinearZ).
    uint2 gTargetMipSize;    ///< Target mip dims; guards the output write.
    uint  gSourceIsLinearZ;  ///< 1: copy gLinearZSource.x; 0: 2x2 max of gHZBSource.
    uint  gPad;
};

Texture2D<float>   gHZBSource;     ///< R32F source mip (mip m-1), bound for mip m > 0.
Texture2D<float2>  gLinearZSource; ///< RG32F full-res linearZ (GBufferRT), bound for mip 0.
RWTexture2D<float> gHZBTarget;     ///< R32F target mip UAV.

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= gTargetMipSize))
    {
        return;
    }

    float depth;
    if (gSourceIsLinearZ)
    {
        // Mip 0: copy the linear depth channel (GBufferRT linearZ.x).
        depth = gLinearZSource[pixel].x;
    }
    else
    {
        // 2x2 max reduction. Source coords are clamped to the source
        // dims - 1 so out-of-range samples repeat the edge texel; see the
        // boundary rule above.
        const uint2 base = pixel * 2u;
        const uint2 clampCoord = gSourceMipSize - 1u;
        const float v00 = gHZBSource[min(base + uint2(0u, 0u), clampCoord)];
        const float v10 = gHZBSource[min(base + uint2(1u, 0u), clampCoord)];
        const float v01 = gHZBSource[min(base + uint2(0u, 1u), clampCoord)];
        const float v11 = gHZBSource[min(base + uint2(1u, 1u), clampCoord)];
        depth = max(max(v00, v10), max(v01, v11));
    }

    gHZBTarget[pixel] = depth;
}
