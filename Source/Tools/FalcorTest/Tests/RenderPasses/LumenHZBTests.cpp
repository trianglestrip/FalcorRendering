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
//  LumenHZBTests.cpp - CPU tests for the S4-A1 HZB build component.
//  -------------------------------------------------------------------------------------
//  Structure mirrors LumenSurfaceCacheTests.cpp (CPU_TEST + EXPECT_*). NOT registered
//  in CMake by design (S4-A1 pure-code round); register in
//  Source/Tools/FalcorTest/CMakeLists.txt next to LumenSurfaceCacheTests.cpp when the
//  HZB pass is integrated.
//
//  COVERAGE (maps to task.md S4-A1 / S4 gate):
//  1. Mip chain formula: dim(m) = max(fullDim >> m, 1), mipCount = 1 + floor(log2(max
//     dim)), for power-of-two, non-power-of-two and tiny sizes
//  2. Dispatch scheduling: one dispatch per mip, source/target dims per level,
//     group counts ceil(dim/16), mip0 uses the linearZ source mode
//  3. Max pooling correctness on hand-computed golden inputs (power of two and
//     non-power-of-two, including the frozen edge rule: out-of-range coords clamp,
//     last odd row/col of a mip is not pooled into the next one)
//  4. Clamp rule == skip rule under the frozen dim formula (verified on random data)
//  5. Random-input full chain vs an independent brute-force block-max reference
//     over mip 0 (fixed seed)
//  6. 1x1 / 2x1 / 1x2 boundary chains and identity mip0
//  7. Host-side validation: reject zero/oversized dims, out-of-range mip, and
//     dispatch params inconsistent with the formulas
// =====================================================================================

#include "Testing/UnitTest.h"
#include "../../../../RenderPasses/LumenGI/ScreenTrace/LumenHZB.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace Falcor
{
namespace
{
// EXPECT_EQ has no float comparator; compare exact bit patterns instead. Max
// pooling returns one of its inputs unchanged, so bit-exact equality is the
// right assertion for this component.
uint32_t floatBits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

#define LUMEN_EXPECT_FLOAT_EQ(actual, expected) \
    EXPECT_MSG(floatBits(static_cast<float>(actual)) == floatBits(static_cast<float>(expected)), #actual " bits == " #expected " bits")

// Independent reference: max of every mip-0 texel whose block-aligned parent
// is (tx, ty) at level `mip`, i.e. max over t with (t >> mip) == (tx, ty).
// This is the semantics of the frozen chain: dims max(dim >> m, 1), and a
// mip-m texel covers the 2^m-aligned block it belongs to.
float bruteForceBlockMax(const std::vector<float>& mip0, uint32_t width, uint32_t height, uint32_t mip, uint32_t tx, uint32_t ty)
{
    float best = -std::numeric_limits<float>::infinity();
    const uint32_t x0 = tx << mip;
    const uint32_t y0 = ty << mip;
    const uint32_t x1 = std::min(x0 + (1u << mip), width);
    const uint32_t y1 = std::min(y0 + (1u << mip), height);
    for (uint32_t y = y0; y < y1; ++y)
    {
        for (uint32_t x = x0; x < x1; ++x)
        {
            best = std::max(best, mip0[y * width + x]);
        }
    }
    return best;
}

// Independent "skip out-of-range" formulation of one 2x2 max level. Under the
// frozen dim formula it must equal buildMipCPU (clamp rule) everywhere.
float skipRuleMipValue(const std::vector<float>& src, uint32_t srcW, uint32_t srcH, uint32_t tx, uint32_t ty)
{
    float best = -std::numeric_limits<float>::infinity();
    const uint32_t bx = tx * 2u;
    const uint32_t by = ty * 2u;
    for (uint32_t dy = 0; dy < 2; ++dy)
    {
        for (uint32_t dx = 0; dx < 2; ++dx)
        {
            const uint32_t x = bx + dx;
            const uint32_t y = by + dy;
            if (x < srcW && y < srcH)
            {
                best = std::max(best, src[y * srcW + x]);
            }
        }
    }
    return best;
}

// Fixed-seed fill used by the random tests (deterministic across runs).
void fillRandom(std::vector<float>& values, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.5f, 500.0f);
    for (float& value : values)
    {
        value = dist(rng);
    }
}
} // namespace

CPU_TEST(LumenHZB_MipChainFormulaAndDispatchParams)
{
    // 1920x1080: 1 + floor(log2(1920)) = 11 levels.
    const LumenHZB::CreateParams params = LumenHZB::makeCreateParams(1920, 1080);
    EXPECT_TRUE(params.isValid());
    EXPECT_EQ(params.width, 1920u);
    EXPECT_EQ(params.height, 1080u);
    EXPECT_EQ(params.mipCount, 11u);

    // dim(m) = max(fullDim >> m, 1) for both axes, ending in 1x1.
    const uint32_t expectedWidths[11] = {1920, 960, 480, 240, 120, 60, 30, 15, 7, 3, 1};
    const uint32_t expectedHeights[11] = {1080, 540, 270, 135, 67, 33, 16, 8, 4, 2, 1};
    for (uint32_t mip = 0; mip < params.mipCount; ++mip)
    {
        EXPECT_EQ(LumenHZB::mipDimension(1920, mip), expectedWidths[mip]);
        EXPECT_EQ(LumenHZB::mipDimension(1080, mip), expectedHeights[mip]);
    }

    // One dispatch per mip, in ascending order, all consistent with the formulas.
    const std::vector<LumenHZB::DispatchParams> dispatches = LumenHZB::makeAllDispatchParams(params);
    EXPECT_EQ(dispatches.size(), params.mipCount);
    EXPECT_TRUE(LumenHZB::validateDispatchParams(params, dispatches[0]));
    for (uint32_t mip = 0; mip < params.mipCount; ++mip)
    {
        const LumenHZB::DispatchParams& dispatch = dispatches[mip];
        EXPECT_EQ(dispatch.mip, mip);
        EXPECT_TRUE(dispatch.isValid());
        EXPECT_EQ(dispatch.targetWidth, expectedWidths[mip]);
        EXPECT_EQ(dispatch.targetHeight, expectedHeights[mip]);
        if (mip == 0)
        {
            EXPECT_TRUE(dispatch.sourceIsLinearZ);
            EXPECT_EQ(dispatch.sourceWidth, 1920u);
            EXPECT_EQ(dispatch.sourceHeight, 1080u);
        }
        else
        {
            EXPECT_FALSE(dispatch.sourceIsLinearZ);
            EXPECT_EQ(dispatch.sourceWidth, expectedWidths[mip - 1]);
            EXPECT_EQ(dispatch.sourceHeight, expectedHeights[mip - 1]);
        }
        EXPECT_TRUE(LumenHZB::validateDispatchParams(params, dispatch));
    }

    // Group counts: ceil(dim / 16) per axis.
    EXPECT_EQ(dispatches[0].groupsX, (1920u + 15u) / 16u); // 120
    EXPECT_EQ(dispatches[0].groupsY, (1080u + 15u) / 16u); // 68
    EXPECT_EQ(dispatches[1].groupsX, (960u + 15u) / 16u);  // 60
    EXPECT_EQ(dispatches[1].groupsY, (540u + 15u) / 16u);  // 34
    EXPECT_EQ(dispatches[10].groupsX, 1u);                 // 1x1 last mip
    EXPECT_EQ(dispatches[10].groupsY, 1u);

    // Chain layout: per-mip texel counts sum to the chain total, offsets are
    // non-overlapping and contiguous.
    size_t texelSum = 0;
    for (uint32_t mip = 0; mip < params.mipCount; ++mip)
    {
        texelSum += LumenHZB::mipTexelCount(1920, 1080, mip);
        EXPECT_EQ(LumenHZB::mipOffset(params, mip), texelSum - LumenHZB::mipTexelCount(1920, 1080, mip));
    }
    EXPECT_EQ(LumenHZB::chainTexelCount(params), texelSum);

    // Non-power-of-two and tiny mip counts.
    EXPECT_EQ(LumenHZB::makeCreateParams(1, 1).mipCount, 1u);
    EXPECT_EQ(LumenHZB::makeCreateParams(2, 2).mipCount, 2u);
    EXPECT_EQ(LumenHZB::makeCreateParams(3, 3).mipCount, 2u);
    EXPECT_EQ(LumenHZB::makeCreateParams(5, 5).mipCount, 3u);
    EXPECT_EQ(LumenHZB::makeCreateParams(33, 17).mipCount, 6u); // 1 + floor(log2(33)) = 6
    EXPECT_EQ(LumenHZB::mipDimension(33, 4), 2u);
    EXPECT_EQ(LumenHZB::mipDimension(33, 5), 1u);
    EXPECT_EQ(LumenHZB::mipDimension(33, 6), 1u); // clamps at 1
}

CPU_TEST(LumenHZB_MaxPoolingGolden)
{
    // 4x4 power-of-two: exact 2x2 max at mip 1, single texel at mip 2.
    const std::vector<float> input4x4 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const LumenHZB::CreateParams params4x4 = LumenHZB::makeCreateParams(4, 4);
    EXPECT_EQ(params4x4.mipCount, 3u);
    const std::vector<float> chain4x4 = LumenHZB::buildChainCPU(input4x4.data(), params4x4);
    EXPECT_EQ(chain4x4.size(), 16u + 4u + 1u);
    EXPECT_EQ(chain4x4[0], 1.f);
    EXPECT_EQ(chain4x4[15], 16.f); // mip 0 is an identity copy of the input
    EXPECT_EQ(chain4x4[16], 6.f);  // mip 1 (0,0) = max(1,2,5,6)
    EXPECT_EQ(chain4x4[17], 8.f);  // mip 1 (1,0) = max(3,4,7,8)
    EXPECT_EQ(chain4x4[18], 14.f); // mip 1 (0,1) = max(9,10,13,14)
    EXPECT_EQ(chain4x4[19], 16.f); // mip 1 (1,1) = max(11,12,15,16)
    EXPECT_EQ(chain4x4[20], 16.f); // mip 2 (0,0) = max of mip 1

    // 5x3 non-power-of-two: mip 1 is 2x1, mip 2 is 1x1. The frozen edge rule
    // pools texels (2x,2x+1) x (2y,2y+1); source column 4 and row 2 are not
    // covered by any 2x2 block and are dropped.
    //  1  2  3  4  5
    //  6  7  8  9 10
    // 11 12 13 14 15
    const std::vector<float> input5x3 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const LumenHZB::CreateParams params5x3 = LumenHZB::makeCreateParams(5, 3);
    EXPECT_EQ(params5x3.mipCount, 3u);
    const std::vector<float> chain5x3 = LumenHZB::buildChainCPU(input5x3.data(), params5x3);
    EXPECT_EQ(chain5x3.size(), 15u + 2u + 1u);
    EXPECT_EQ(chain5x3[15], 7.f);  // mip 1 (0,0) = max(1,2,6,7)
    EXPECT_EQ(chain5x3[16], 9.f);  // mip 1 (1,0) = max(3,4,8,9)
    EXPECT_EQ(chain5x3[17], 9.f);  // mip 2 (0,0) = max of mip 1

    // A 3x1 input whose global maximum sits in the dropped column: mip 1 is
    // 1x1 and must NOT include texel 2 (99).
    //  1  2 99
    const std::vector<float> input3x1 = {1, 2, 99};
    const LumenHZB::CreateParams params3x1 = LumenHZB::makeCreateParams(3, 1);
    EXPECT_EQ(params3x1.mipCount, 2u);
    const std::vector<float> chain3x1 = LumenHZB::buildChainCPU(input3x1.data(), params3x1);
    EXPECT_EQ(chain3x1.size(), 3u + 1u);
    EXPECT_EQ(chain3x1[3], 2.f); // 99 is in the dropped column
}

CPU_TEST(LumenHZB_TinyAndBoundarySizes)
{
    // 1x1: single level, identity chain.
    const std::vector<float> input1x1 = {42.f};
    const LumenHZB::CreateParams params1x1 = LumenHZB::makeCreateParams(1, 1);
    EXPECT_EQ(params1x1.mipCount, 1u);
    const std::vector<float> chain1x1 = LumenHZB::buildChainCPU(input1x1.data(), params1x1);
    EXPECT_EQ(chain1x1.size(), 1u);
    EXPECT_EQ(chain1x1[0], 42.f);

    // 2x1 and 1x2: one max level down to 1x1 (both texels pooled).
    const std::vector<float> input2x1 = {3.f, 9.f};
    const std::vector<float> chain2x1 = LumenHZB::buildChainCPU(input2x1.data(), LumenHZB::makeCreateParams(2, 1));
    EXPECT_EQ(chain2x1.size(), 2u + 1u);
    EXPECT_EQ(chain2x1[2], 9.f);

    const std::vector<float> input1x2 = {3.f, 9.f};
    const std::vector<float> chain1x2 = LumenHZB::buildChainCPU(input1x2.data(), LumenHZB::makeCreateParams(1, 2));
    EXPECT_EQ(chain1x2.size(), 2u + 1u);
    EXPECT_EQ(chain1x2[2], 9.f);

    // 2x2: two levels, max at the top.
    const std::vector<float> input2x2 = {1.f, 4.f, 2.f, 3.f};
    const std::vector<float> chain2x2 = LumenHZB::buildChainCPU(input2x2.data(), LumenHZB::makeCreateParams(2, 2));
    EXPECT_EQ(chain2x2.size(), 4u + 1u);
    EXPECT_EQ(chain2x2[4], 4.f);
}

CPU_TEST(LumenHZB_ClampRuleEqualsSkipRule)
{
    // Under the frozen dim formula (target = max(src >> 1, 1)) the clamp
    // boundary rule and the skip-out-of-range rule must agree everywhere.
    // Random sizes and values, fixed seed.
    const uint32_t kSizes[][2] = {{1, 1}, {2, 2}, {3, 3}, {5, 3}, {7, 9}, {16, 16}, {17, 33}, {33, 17}};
    for (const auto& size : kSizes)
    {
        const uint32_t srcW = size[0];
        const uint32_t srcH = size[1];
        std::vector<float> src(srcW * srcH);
        fillRandom(src, 0xC10C11u + srcW * 31u + srcH);

        const uint32_t dstW = LumenHZB::mipDimension(srcW, 1);
        const uint32_t dstH = LumenHZB::mipDimension(srcH, 1);
        std::vector<float> dst(dstW * dstH);
        LumenHZB::buildMipCPU(src.data(), srcW, srcH, dst.data(), dstW, dstH);

        for (uint32_t y = 0; y < dstH; ++y)
        {
            for (uint32_t x = 0; x < dstW; ++x)
            {
                LUMEN_EXPECT_FLOAT_EQ(dst[y * dstW + x], skipRuleMipValue(src, srcW, srcH, x, y));
            }
        }
    }
}

CPU_TEST(LumenHZB_RandomChainMatchesBruteForceBlockMax)
{
    // Fixed seed; sizes in [1, 64] so several mip levels exist (1x1 included).
    std::mt19937 rng(0x5EED42A7u);
    const uint32_t kRounds = 32;
    for (uint32_t round = 0; round < kRounds; ++round)
    {
        const uint32_t width = 1 + (rng() % 64u);
        const uint32_t height = 1 + (rng() % 64u);

        std::vector<float> linearZ(width * height);
        fillRandom(linearZ, 0xABCDu + round * 7919u);

        const LumenHZB::CreateParams params = LumenHZB::makeCreateParams(width, height);
        EXPECT_TRUE(params.isValid());

        const std::vector<float> chain = LumenHZB::buildChainCPU(linearZ.data(), params);
        EXPECT_EQ(chain.size(), LumenHZB::chainTexelCount(params));

        for (uint32_t mip = 0; mip < params.mipCount; ++mip)
        {
            const uint32_t mipW = LumenHZB::mipDimension(width, mip);
            const uint32_t mipH = LumenHZB::mipDimension(height, mip);
            const size_t offset = LumenHZB::mipOffset(params, mip);

            if (mip == 0)
            {
                // Mip 0 is an identity copy of the linear-depth input.
                for (size_t i = 0; i < linearZ.size(); ++i)
                {
                    LUMEN_EXPECT_FLOAT_EQ(chain[offset + i], linearZ[i]);
                }
                continue;
            }

            for (uint32_t y = 0; y < mipH; ++y)
            {
                for (uint32_t x = 0; x < mipW; ++x)
                {
                    // The chain value is the max of the aligned 2^mip block
                    // in mip 0 (independent reference).
                    const float expected = bruteForceBlockMax(linearZ, width, height, mip, x, y);
                    const float actual = chain[offset + y * mipW + x];
                    LUMEN_EXPECT_FLOAT_EQ(actual, expected);
                }
            }
        }
    }
}

CPU_TEST(LumenHZB_ValidationRejectsInvalidInput)
{
    // Zero and oversized dimensions are rejected at creation.
    EXPECT_FALSE(LumenHZB::makeCreateParams(0, 1080).isValid());
    EXPECT_FALSE(LumenHZB::makeCreateParams(1920, 0).isValid());
    EXPECT_FALSE(LumenHZB::makeCreateParams(0, 0).isValid());
    EXPECT_FALSE(LumenHZB::makeCreateParams(LumenHZB::kMaxDimension + 1, 1080).isValid());
    EXPECT_FALSE(LumenHZB::makeCreateParams(1920, LumenHZB::kMaxDimension + 1).isValid());
    EXPECT_TRUE(LumenHZB::makeCreateParams(LumenHZB::kMaxDimension, LumenHZB::kMaxDimension).isValid());

    // Dispatch params for out-of-range mips are invalid, and the empty
    // default dispatch is invalid.
    const LumenHZB::CreateParams params = LumenHZB::makeCreateParams(1920, 1080);
    EXPECT_FALSE(LumenHZB::makeDispatchParams(params, params.mipCount).isValid());
    EXPECT_FALSE(LumenHZB::makeDispatchParams(params, params.mipCount + 100).isValid());
    EXPECT_FALSE(LumenHZB::makeDispatchParams(LumenHZB::CreateParams{}, 0).isValid());
    EXPECT_FALSE(LumenHZB::DispatchParams{}.isValid());
    EXPECT_TRUE(LumenHZB::makeAllDispatchParams(LumenHZB::CreateParams{}).empty());

    // A tampered dispatch no longer validates.
    LumenHZB::DispatchParams tampered = LumenHZB::makeDispatchParams(params, 3);
    EXPECT_TRUE(LumenHZB::validateDispatchParams(params, tampered));
    tampered.targetWidth += 1;
    EXPECT_FALSE(LumenHZB::validateDispatchParams(params, tampered));
    tampered.targetWidth -= 1;
    tampered.sourceIsLinearZ = true;
    EXPECT_FALSE(LumenHZB::validateDispatchParams(params, tampered));
}

} // namespace Falcor
