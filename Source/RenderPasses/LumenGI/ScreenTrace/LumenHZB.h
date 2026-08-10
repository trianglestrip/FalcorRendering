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
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Falcor
{
/**
 * @brief S4-A1 hierarchical Z-buffer (HZB) build component (pure CPU, no Device).
 *
 * Host-side companion of the LumenHZBBuild.cs.slang compute pass. This class
 * only produces numbers and CPU mirror data; it never touches a Device, a
 * texture or a render context. The S4-A1 integration wires the outputs into
 * Falcor resources exactly as documented on each struct/function.
 *
 * FROZEN MIP CHAIN CONTRACT (shared with LumenHZBBuild.cs.slang and consumed
 * by the S4-B1 screen trace through hzbSampleMaxDepth in
 * LumenScreenTrace.cs.slang):
 *
 *   - mip m  dims  = max(ceil(dim / 2^m), 1x1) (ceil-halving, see
 *     mipDimension: every mip fully covers its parent, so the max chain is
 *     conservative for hierarchical screen tracing).
 *   - mip 0  holds the full-resolution linear depth, identical to
 *     GBufferRT linearZ.x (RG32F, .x only).
 *   - mip m+1 texel (x, y) = MAX of mip m texels
 *         (2x, 2y), (2x+1, 2y), (2x, 2y+1), (2x+1, 2y+1).
 *   - Boundary rule: out-of-range source coordinates are clamped to the
 *     source dims - 1 (edge texel duplicated). Under target =
 *     max(source >> 1, 1) the clamp fires only when a source dim is 1
 *     (identity copy), so clamp == "skip out-of-range" for every allowed
 *     size pair; both rules are implemented in buildMipCPU and in the
 *     shader and produce bit-identical results.
 *
 * Edge property (by design, keep the formula frozen): with dims
 * max(dim >> m, 1), the last odd row/column of every mip is not covered by
 * any 2x2 block of the next mip (e.g. a 5-wide mip yields a 2-wide next
 * mip whose blocks cover texels 0..3; texel 4 is not pooled). The S4-B1
 * trace therefore always descends to mip 0, which holds the full
 * resolution, so the dropped texels never create a visible hole; they only
 * make a one-texel border region slightly less conservative at coarse mips.
 *
 * Dispatch model: exactly one compute dispatch per mip level, in ascending
 * mip order. Level m > 0 reads mip m-1 (SRV) and writes mip m (UAV) of the
 * same texture; level 0 reads the RG32F linearZ. Level 0 uses the
 * gSourceIsLinearZ == 1 mode of the shader, all others the 2x2-max mode.
 * Group counts are ceil(mip dims / 16) per axis (16x16 threads, matching
 * [numthreads(16, 16, 1)]).
 *
 * Host texture creation (for the S4-A1 integration; shown here only as the
 * contract, this class does not depend on Device). One INDEPENDENT texture per
 * level (ceil-halving dims; D3D12 native mip chains are floor-sized and cannot
 * hold a ceil chain):
 *   for mip in [0, mipCount):
 *     Texture::create2D(mipDimension(w, mip), mipDimension(h, mip),
 *                       ResourceFormat::R32Float, 1, 1, nullptr,
 *                       ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess)
 * Bind gHZBTarget = pLevel[mip]->getUAV(0), gHZBSource = pLevel[mip-1]->getSRV(0, 1),
 * gLinearZSource = the GBufferRT linearZ texture.
 *
 * Thread safety: stateless; all methods are const/static and safe to call
 * from any thread.
 */
class LumenHZB
{
public:
    ///< Threads per axis of the build dispatch (must match [numthreads(16, 16, 1)]).
    static constexpr uint32_t kBuildThreads = 16;

    ///< Upper bound on a full-res dimension. Keeps every shift and size
    ///< computation well inside uint32_t while covering any real frame size.
    static constexpr uint32_t kMaxDimension = (1u << 24);

    /** Resource creation parameters for the whole HZB chain. */
    struct CreateParams
    {
        uint32_t width = 0;    ///< Full-resolution width.
        uint32_t height = 0;   ///< Full-resolution height.
        uint32_t mipCount = 0; ///< 1 + floor(log2(max(width, height))).

        bool isValid() const
        {
            return width >= 1 && height >= 1 && width <= kMaxDimension && height <= kMaxDimension &&
                   mipCount == LumenHZB::mipCount(width, height);
        }
    };

    /** Scheduling parameters for one mip-level dispatch. */
    struct DispatchParams
    {
        uint32_t mip = 0;               ///< Target mip level being built.
        uint32_t sourceWidth = 0;       ///< Source mip dims (frame dims for mip 0).
        uint32_t sourceHeight = 0;      ///< Source mip dims (frame dims for mip 0).
        uint32_t targetWidth = 0;       ///< Target mip dims: max(source >> 1, 1), frame dims for mip 0.
        uint32_t targetHeight = 0;      ///< Target mip dims.
        uint32_t groupsX = 0;           ///< ceil(targetWidth / kBuildThreads).
        uint32_t groupsY = 0;           ///< ceil(targetHeight / kBuildThreads).
        bool sourceIsLinearZ = false;   ///< true only for mip 0: read gLinearZSource.x.

        bool isValid() const
        {
            return sourceWidth >= 1 && sourceHeight >= 1 && targetWidth >= 1 && targetHeight >= 1 &&
                   groupsX >= 1 && groupsY >= 1;
        }
    };

    /** mip m dimension of a full-res dimension: ceil-halving so that every
        mip fully covers its parent (no discarded odd tail row/column). This
        keeps the max chain conservative for hierarchical screen tracing:
        max(dim >> m, 1) (floor) drops the last texel of odd-sized mips and
        under-estimates the block max. */
    static uint32_t mipDimension(uint32_t fullDim, uint32_t mip)
    {
        return std::max((fullDim + ((1u << mip) - 1u)) >> mip, 1u);
    }

    /** Number of mip levels: 1 + ceil(log2(max(width, height))), i.e. the
        number of ceil-halvings needed to reach 1x1. */
    static uint32_t mipCount(uint32_t width, uint32_t height)
    {
        uint32_t dim = std::max(width, height);
        uint32_t count = 1;
        while (dim > 1)
        {
            dim = (dim + 1u) / 2u;
            ++count;
        }
        return count;
    }

    /** Build create params for a full-res input; returns invalid params
        (isValid() == false) for width/height == 0 or > kMaxDimension. */
    static CreateParams makeCreateParams(uint32_t width, uint32_t height)
    {
        CreateParams params;
        if (width >= 1 && height >= 1 && width <= kMaxDimension && height <= kMaxDimension)
        {
            params.width = width;
            params.height = height;
            params.mipCount = mipCount(width, height);
        }
        return params;
    }

    /** Scheduling parameters of the single dispatch that builds mip `mip`.
        Returns invalid params when `params` is invalid or mip >= mipCount.
        Mip 0 reads full-res linearZ (sourceIsLinearZ == true); mip m > 0
        reads mip m-1 (sourceIsLinearZ == false). */
    static DispatchParams makeDispatchParams(const CreateParams& params, uint32_t mip)
    {
        DispatchParams dispatch;
        if (!params.isValid() || mip >= params.mipCount)
        {
            return dispatch;
        }

        dispatch.mip = mip;
        dispatch.sourceIsLinearZ = (mip == 0);
        dispatch.sourceWidth = dispatch.sourceIsLinearZ ? params.width : mipDimension(params.width, mip - 1);
        dispatch.sourceHeight = dispatch.sourceIsLinearZ ? params.height : mipDimension(params.height, mip - 1);
        dispatch.targetWidth = mipDimension(params.width, mip);
        dispatch.targetHeight = mipDimension(params.height, mip);
        dispatch.groupsX = static_cast<uint32_t>(
            (static_cast<uint64_t>(dispatch.targetWidth) + kBuildThreads - 1) / kBuildThreads
        );
        dispatch.groupsY = static_cast<uint32_t>(
            (static_cast<uint64_t>(dispatch.targetHeight) + kBuildThreads - 1) / kBuildThreads
        );
        return dispatch;
    }

    /** One DispatchParams entry per mip, in ascending mip order (the exact
        dispatch sequence the host must issue). Empty when `params` is
        invalid. */
    static std::vector<DispatchParams> makeAllDispatchParams(const CreateParams& params)
    {
        std::vector<DispatchParams> dispatches;
        if (params.isValid())
        {
            dispatches.reserve(params.mipCount);
            for (uint32_t mip = 0; mip < params.mipCount; ++mip)
            {
                dispatches.push_back(makeDispatchParams(params, mip));
            }
        }
        return dispatches;
    }

    /** Host-side error check: verifies that `dispatch` is consistent with
        `params` and with the frozen chain formulas. */
    static bool validateDispatchParams(const CreateParams& params, const DispatchParams& dispatch)
    {
        if (!params.isValid() || !dispatch.isValid())
        {
            return false;
        }
        const DispatchParams expected = makeDispatchParams(params, dispatch.mip);
        return dispatch.sourceWidth == expected.sourceWidth &&
               dispatch.sourceHeight == expected.sourceHeight &&
               dispatch.targetWidth == expected.targetWidth &&
               dispatch.targetHeight == expected.targetHeight &&
               dispatch.groupsX == expected.groupsX && dispatch.groupsY == expected.groupsY &&
               dispatch.sourceIsLinearZ == expected.sourceIsLinearZ;
    }

    /** Texel count of mip m (for chain layout math). */
    static size_t mipTexelCount(uint32_t width, uint32_t height, uint32_t mip)
    {
        return static_cast<size_t>(mipDimension(width, mip)) * mipDimension(height, mip);
    }

    /** Byte/texel offset of mip m in the contiguous chain layout produced by
        buildChainCPU (mips 0..m-1 precede it, row-major within a mip). */
    static size_t mipOffset(const CreateParams& params, uint32_t mip)
    {
        size_t offset = 0;
        for (uint32_t m = 0; m < mip; ++m)
        {
            offset += mipTexelCount(params.width, params.height, m);
        }
        return offset;
    }

    /** Total texel count of the whole chain (size of buildChainCPU output). */
    static size_t chainTexelCount(const CreateParams& params)
    {
        return mipOffset(params, params.mipCount);
    }

    /** CPU mirror of one shader mip build: max-pool `src` (row-major) into
        `dst`. Preconditions: srcW/srcH >= 1, dstW == max(srcW >> 1, 1),
        dstH == max(srcH >> 1, 1). Implements exactly the shader rule —
        out-of-range 2x2 coords clamp to the source dims - 1 — so the
        output is bit-identical to the GPU pass and can be used as the
        reference for readback validation. */
    static void buildMipCPU(const float* src, uint32_t srcW, uint32_t srcH, float* dst, uint32_t dstW, uint32_t dstH)
    {
        const uint32_t clampX = srcW - 1u;
        const uint32_t clampY = srcH - 1u;
        for (uint32_t y = 0; y < dstH; ++y)
        {
            const uint32_t by = std::min(y * 2u, clampY);
            const uint32_t ey = std::min(y * 2u + 1u, clampY);
            for (uint32_t x = 0; x < dstW; ++x)
            {
                const uint32_t bx = std::min(x * 2u, clampX);
                const uint32_t ex = std::min(x * 2u + 1u, clampX);
                const float v00 = src[by * srcW + bx];
                const float v10 = src[by * srcW + ex];
                const float v01 = src[ey * srcW + bx];
                const float v11 = src[ey * srcW + ex];
                dst[y * dstW + x] = std::max(std::max(v00, v10), std::max(v01, v11));
            }
        }
    }

    /** CPU mirror of the whole chain: mip 0 is an identity copy of the
        full-res linear depth array (matching the gSourceIsLinearZ dispatch),
        every following mip is one buildMipCPU level. Returns a contiguous
        row-major array with mips laid out in ascending order; per-mip views
        are obtained with mipOffset/mipTexelCount. Returns an empty vector
        when `params` is invalid. */
    static std::vector<float> buildChainCPU(const float* linearZ, const CreateParams& params)
    {
        std::vector<float> chain;
        if (!params.isValid() || linearZ == nullptr)
        {
            return chain;
        }

        chain.resize(chainTexelCount(params));
        const size_t mip0Count = mipTexelCount(params.width, params.height, 0);
        for (size_t i = 0; i < mip0Count; ++i)
        {
            chain[i] = linearZ[i];
        }

        for (uint32_t mip = 1; mip < params.mipCount; ++mip)
        {
            const size_t srcOffset = mipOffset(params, mip - 1);
            const size_t dstOffset = mipOffset(params, mip);
            buildMipCPU(
                chain.data() + srcOffset,
                mipDimension(params.width, mip - 1),
                mipDimension(params.height, mip - 1),
                chain.data() + dstOffset,
                mipDimension(params.width, mip),
                mipDimension(params.height, mip)
            );
        }
        return chain;
    }
};
} // namespace Falcor
