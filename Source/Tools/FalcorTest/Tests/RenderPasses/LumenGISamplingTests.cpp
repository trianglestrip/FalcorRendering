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
 # PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/

/** CPU tests for LumenGI sampling math, PRNG reproducibility, MIS helpers,
    radiance-clamp / firefly helpers, and the C++/Slang shared layout.

    The sampling helpers implemented below are deliberate CPU mirrors of the
    Slang functions used by the LumenGI tracing path:

    - sample_disk_concentric / sample_cosine_hemisphere_concentric mirror
      `Utils/Math/MathHelpers.slang` (used by
      `RenderPasses/LumenGI/Tracing/LumenHardwareTrace.rt.slang`).
    - The PRNG mirrors `Utils/Sampling/Pseudorandom/LCG.slang`,
      `Utils/Math/HashUtils.slang` (blockCipherTEA) and
      `Utils/Math/BitTricks.slang` (interleave_32bit) through
      `Utils/Sampling/TinyUniformSampleGenerator.slang`, which is what the
      shader instantiates with `SampleGenerator(pixel, frameIndex)`.

    The MIS / radiance-clamp / firefly helpers define the *reference*
    conventions that the Agent B shader implementation is expected to match.
    There is no MIS code in the LumenGI shaders at this baseline, so these
    assertions are pure math (standard balance/power heuristics) and must be
    re-checked against the shader formulas when Agent B lands them (see
    Agent C report for the exact expected formulas).

    The shared-layout tests assert that the C++ DebugMode enum values match
    the kLumenDebug* constants in `LumenGIData.slang`. Only the constants that
    exist today are asserted (subset assertions); new constants added by Agent
    B do not break this file.

    This file is self-contained: it depends only on Falcor public headers and
    the LumenGI pass header. It is registered in the FalcorTest target by root
    (Source/Tools/FalcorTest/CMakeLists.txt is root-owned).
 */

#include "Testing/UnitTest.h"
#include "Core/Enum.h"
#include "../../../../RenderPasses/LumenGI/LumenGI.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace Falcor
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr float kInvPiFloat = 1.0f / static_cast<float>(kPi);

// ---------------------------------------------------------------------------
// CPU mirrors of the Slang sampling functions.
// ---------------------------------------------------------------------------

/** Mirror of sample_disk_concentric() in Utils/Math/MathHelpers.slang. */
void sampleDiskConcentric(float ux, float uy, float& ox, float& oy)
{
    const float sx = 2.f * ux - 1.f;
    const float sy = 2.f * uy - 1.f;
    if (sx == 0.f && sy == 0.f)
    {
        ox = 0.f;
        oy = 0.f;
        return;
    }
    float r, phi;
    if (std::abs(sx) > std::abs(sy))
    {
        r = sx;
        phi = (sy / sx) * static_cast<float>(kPi) * 0.25f;
    }
    else
    {
        r = sy;
        phi = static_cast<float>(kPi) * 0.5f - (sx / sy) * static_cast<float>(kPi) * 0.25f;
    }
    ox = r * std::cos(phi);
    oy = r * std::sin(phi);
}

/** Mirror of sample_cosine_hemisphere_concentric() in Utils/Math/MathHelpers.slang.
    Returns the local-frame direction (z up) and its pdf (= z/pi). */
void sampleCosineHemisphereConcentric(float ux, float uy, float& dx, float& dy, float& dz, float& pdf)
{
    float sx, sy;
    sampleDiskConcentric(ux, uy, sx, sy);
    dz = std::sqrt(std::max(0.f, 1.f - sx * sx - sy * sy));
    pdf = dz * kInvPiFloat;
    dx = sx;
    dy = sy;
}

/** Build an orthonormal frame whose Z axis is 'normal'. Columns are (u, v, n). */
void buildFrameFromNormal(float nx, float ny, float nz, float frame[3][3])
{
    float helperX = std::abs(nz) < 0.999f ? 0.f : 1.f;
    float helperY = helperX == 0.f ? 1.f : 0.f;
    float helperZ = 0.f;
    // u = normalize(cross(helper, n))
    float ux = helperY * nz - helperZ * ny;
    float uy = helperZ * nx - helperX * nz;
    float uz = helperX * ny - helperY * nx;
    const float uLen = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (uLen > 1e-12f)
    {
        ux /= uLen;
        uy /= uLen;
        uz /= uLen;
    }
    // v = cross(n, u)
    const float vx = ny * uz - nz * uy;
    const float vy = nz * ux - nx * uz;
    const float vz = nx * uy - ny * ux;
    frame[0][0] = ux;
    frame[0][1] = uy;
    frame[0][2] = uz;
    frame[1][0] = vx;
    frame[1][1] = vy;
    frame[1][2] = vz;
    frame[2][0] = nx;
    frame[2][1] = ny;
    frame[2][2] = nz;
}

// ---------------------------------------------------------------------------
// CPU mirror of TinyUniformSampleGenerator (LCG + TEA seeding).
// ---------------------------------------------------------------------------

/** Mirror of interleave_32bit() in Utils/Math/BitTricks.slang. */
uint32_t interleave32Bit(uint32_t x, uint32_t y)
{
    uint32_t vx = x & 0x0000ffffu;
    vx = (vx | (vx << 8)) & 0x00FF00FFu;
    vx = (vx | (vx << 4)) & 0x0F0F0F0Fu;
    vx = (vx | (vx << 2)) & 0x33333333u;
    vx = (vx | (vx << 1)) & 0x55555555u;

    uint32_t vy = y & 0x0000ffffu;
    vy = (vy | (vy << 8)) & 0x00FF00FFu;
    vy = (vy | (vy << 4)) & 0x0F0F0F0Fu;
    vy = (vy | (vy << 2)) & 0x33333333u;
    vy = (vy | (vy << 1)) & 0x55555555u;

    return vx | (vy << 1);
}

/** Mirror of blockCipherTEA() in Utils/Math/HashUtils.slang. */
uint32_t blockCipherTEA(uint32_t v0, uint32_t v1)
{
    uint32_t sum = 0;
    const uint32_t delta = 0x9e3779b9u;
    const uint32_t k[4] = {0xa341316cu, 0xc8013ea4u, 0xad90777du, 0x7e95761eu};
    for (uint32_t i = 0; i < 16; i++)
    {
        sum += delta;
        v0 += ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
        v1 += ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
    }
    return v0;
}

/** Seed derivation of TinyUniformSampleGenerator(pixel, sampleNumber). */
uint32_t tinyUniformSeed(uint32_t pixelX, uint32_t pixelY, uint32_t sampleNumber)
{
    return blockCipherTEA(interleave32Bit(pixelX, pixelY), sampleNumber);
}

/** Mirror of LCG nextRandom() in Utils/Sampling/Pseudorandom/LCG.slang. */
uint32_t lcgNext(uint32_t& state)
{
    const uint32_t a = 1664525u;
    const uint32_t c = 1013904223u;
    state = a * state + c;
    return state;
}

/** Mirror of sampleNext1D() in Utils/Sampling/SampleGeneratorInterface.slang. */
float lcgSample1D(uint32_t& state)
{
    const uint32_t bits = lcgNext(state);
    return static_cast<float>(bits >> 8) * 0x1p-24f;
}

// ---------------------------------------------------------------------------
// MIS and radiance helpers (reference conventions, see file comment).
// ---------------------------------------------------------------------------

/** Balance heuristic MIS weight. Convention: zero if no strategy has mass. */
double misBalanceWeight(double pdfTarget, double sumAllPdfs)
{
    return sumAllPdfs > 0.0 ? pdfTarget / sumAllPdfs : 0.0;
}

/** Power heuristic MIS weight (default exponent 2, Veach-style). */
double misPowerWeight(double pdfTarget, double sumAllPowers, double exponent = 2.0)
{
    return sumAllPowers > 0.0 ? std::pow(pdfTarget, exponent) / sumAllPowers : 0.0;
}

/** Clamp radiance to [0, maxRadiance]; negative radiance is clamped to zero. */
double clampRadiance(double radiance, double maxRadiance)
{
    return std::min(std::max(radiance, 0.0), maxRadiance);
}

/** Firefly test: radiance strictly above the threshold. */
bool isFirefly(double radiance, double threshold)
{
    return radiance > threshold;
}

/** True when any component is NaN or infinity. */
bool hasNonFinite(const double rgb[3])
{
    return !std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2]);
}

/** Replace NaN/Inf with zero, keep finite values (positive or zero-clamped by caller). */
double sanitizeRadiance(double radiance)
{
    return std::isfinite(radiance) ? radiance : 0.0;
}

void expectNear(CPUUnitTestContext& ctx, double value, double expected, double tolerance, const char* label)
{
    const double error = std::abs(value - expected);
    EXPECT_TRUE_MSG(
        error <= tolerance,
        fmt::format("{}: |{} - {}| = {} exceeds tolerance {}", label, value, expected, error, tolerance)
    );
}

} // namespace

// ---------------------------------------------------------------------------
// Cosine-weighted hemisphere sampling math.
// ---------------------------------------------------------------------------

CPU_TEST(LumenGISampling_ConcentricDisk_Mapping)
{
    // Center maps to the origin (explicit branch in the shader).
    float ox, oy;
    sampleDiskConcentric(0.5f, 0.5f, ox, oy);
    EXPECT_EQ(ox, 0.f);
    EXPECT_EQ(oy, 0.f);

    // Deterministic mapping, samples stay inside the unit disk, and the
    // expected second radial moment of the uniform disk is E[r^2] = 1/2.
    uint32_t state = tinyUniformSeed(3u, 7u, 0u);
    constexpr int kNumSamples = 1 << 20;
    double sumR2 = 0.0;
    for (int i = 0; i < kNumSamples; i++)
    {
        const float ux = lcgSample1D(state);
        const float uy = lcgSample1D(state);
        sampleDiskConcentric(ux, uy, ox, oy);
        const double r2 = static_cast<double>(ox) * ox + static_cast<double>(oy) * oy;
        EXPECT_LE(r2, 1.0 + 1e-6);
        sumR2 += r2;
    }
    expectNear(ctx, sumR2 / kNumSamples, 0.5, 0.005, "mean disk r^2");
}

CPU_TEST(LumenGISampling_CosineHemisphere_PdfAndDirection)
{
    // Deterministic edge case: u = (0.5, 0.5) maps to disk center, z = 1,
    // pdf = 1/pi.
    float dx, dy, dz, pdf;
    sampleCosineHemisphereConcentric(0.5f, 0.5f, dx, dy, dz, pdf);
    EXPECT_EQ(dx, 0.f);
    EXPECT_EQ(dy, 0.f);
    EXPECT_EQ(dz, 1.f);
    expectNear(ctx, pdf, 1.0 / kPi, 1e-6, "center pdf");

    uint32_t state = tinyUniformSeed(1u, 2u, 3u);
    constexpr int kNumSamples = 1 << 20;
    constexpr int kNumBins = 16;
    double sumZ = 0.0;
    std::vector<double> binCounts(kNumBins, 0.0);
    for (int i = 0; i < kNumSamples; i++)
    {
        const float ux = lcgSample1D(state);
        const float uy = lcgSample1D(state);
        sampleCosineHemisphereConcentric(ux, uy, dx, dy, dz, pdf);

        // Direction is inside the +z hemisphere and has unit length.
        EXPECT_GE(dz, -1e-6f);
        const double lengthSq = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy + static_cast<double>(dz) * dz;
        expectNear(ctx, lengthSq, 1.0, 1e-4, "direction length^2");

        // pdf matches the formula pdf = z / pi.
        expectNear(ctx, pdf, static_cast<double>(dz) / kPi, 1e-6, "pdf = z/pi");

        sumZ += dz;
        const int bin = std::min(kNumBins - 1, static_cast<int>(dz * kNumBins));
        binCounts[bin] += 1.0;
    }

    // E[z] over a cosine-weighted hemisphere is 2/3.
    expectNear(ctx, sumZ / kNumSamples, 2.0 / 3.0, 0.005, "mean cos(theta)");

    // Verify the sampled z distribution matches pdf(z) = z/pi. For a uniform
    // bin over z = cos(theta), the expected sample fraction in [z0, z1] is
    // z1^2 - z0^2. This is a robust (finite-variance) normalization check,
    // equivalent to the pdf integrating to one over the hemisphere.
    double chiSquare = 0.0;
    for (int bin = 0; bin < kNumBins; bin++)
    {
        const double z0 = static_cast<double>(bin) / kNumBins;
        const double z1 = static_cast<double>(bin + 1) / kNumBins;
        const double expected = (z1 * z1 - z0 * z0) * kNumSamples;
        const double diff = binCounts[bin] - expected;
        chiSquare += diff * diff / expected;
    }
    // 15 degrees of freedom; 40 is far beyond the 99.9% critical value (~37.7),
    // so this only trips on real distribution mismatches, not noise.
    EXPECT_LE(chiSquare, 40.0);
}

CPU_TEST(LumenGISampling_CosineHemisphere_InNormalFrame)
{
    // Sampling must stay in the hemisphere around an arbitrary normal when
    // the local direction is transformed into world space.
    const float nx = 0.2673f;
    const float ny = -0.5345f;
    const float nz = 0.8018f;
    float frame[3][3];
    buildFrameFromNormal(nx, ny, nz, frame);

    uint32_t state = tinyUniformSeed(9u, 4u, 2u);
    constexpr int kNumSamples = 1 << 16;
    for (int i = 0; i < kNumSamples; i++)
    {
        const float ux = lcgSample1D(state);
        const float uy = lcgSample1D(state);
        float dx, dy, dz, pdf;
        sampleCosineHemisphereConcentric(ux, uy, dx, dy, dz, pdf);

        const float wx = frame[0][0] * dx + frame[1][0] * dy + frame[2][0] * dz;
        const float wy = frame[0][1] * dx + frame[1][1] * dy + frame[2][1] * dz;
        const float wz = frame[0][2] * dx + frame[1][2] * dy + frame[2][2] * dz;
        const double dotNormal = static_cast<double>(wx) * nx + static_cast<double>(wy) * ny + static_cast<double>(wz) * nz;
        EXPECT_GE(dotNormal, -1e-6);
    }
}

// ---------------------------------------------------------------------------
// Fixed-seed reproducibility of the shader PRNG convention.
// ---------------------------------------------------------------------------

CPU_TEST(LumenGISampling_SameSeedReproducible)
{
    // Two independent generators with the same pixel/sample seed must produce
    // bit-identical sequences (this is what makes frames reproducible when
    // LumenHardwareTrace.rt.slang constructs SampleGenerator(pixel, frame)).
    uint32_t stateA = tinyUniformSeed(42u, 13u, 7u);
    uint32_t stateB = tinyUniformSeed(42u, 13u, 7u);
    std::vector<float> streamA(256);
    std::vector<float> streamB(256);
    for (size_t i = 0; i < streamA.size(); i++)
    {
        streamA[i] = lcgSample1D(stateA);
        streamB[i] = lcgSample1D(stateB);
    }
    for (size_t i = 0; i < streamA.size(); i++)
        EXPECT_EQ(streamA[i], streamB[i]);
}

CPU_TEST(LumenGISampling_DifferentSeedDiverges)
{
    // Different pixel or frame seeds must diverge quickly.
    uint32_t stateA = tinyUniformSeed(42u, 13u, 7u);
    uint32_t stateB = tinyUniformSeed(43u, 13u, 7u);
    uint32_t stateC = tinyUniformSeed(42u, 13u, 8u);

    int divergedB = -1;
    int divergedC = -1;
    for (int i = 0; i < 64; i++)
    {
        const float a = lcgSample1D(stateA);
        const float b = lcgSample1D(stateB);
        const float c = lcgSample1D(stateC);
        if (divergedB < 0 && a != b)
            divergedB = i;
        if (divergedC < 0 && a != c)
            divergedC = i;
    }
    EXPECT_GE(divergedB, 0);
    EXPECT_GE(divergedC, 0);
    EXPECT_LE(divergedB, 2); // differ within the first samples
    EXPECT_LE(divergedC, 2);
}

CPU_TEST(LumenGISampling_SequenceInUnitRange)
{
    uint32_t state = tinyUniformSeed(0u, 0u, 0u);
    constexpr int kNumSamples = 1 << 16;
    double mean = 0.0;
    for (int i = 0; i < kNumSamples; i++)
    {
        const float u = lcgSample1D(state);
        EXPECT_GE(u, 0.f);
        EXPECT_LT(u, 1.f);
        mean += u;
    }
    // LCG sequences are deterministic; mean must be near 0.5 (uniform [0,1)).
    expectNear(ctx, mean / kNumSamples, 0.5, 0.01, "sample mean");
}

// ---------------------------------------------------------------------------
// MIS weights.
// ---------------------------------------------------------------------------

CPU_TEST(LumenGISampling_MisBalanceHeuristic)
{
    // Three strategies with known pdfs.
    const double pdfs[3] = {0.1, 0.3, 0.6};
    double sum = pdfs[0] + pdfs[1] + pdfs[2];
    double weights[3];
    for (int i = 0; i < 3; i++)
        weights[i] = misBalanceWeight(pdfs[i], sum);

    double weightSum = 0.0;
    for (int i = 0; i < 3; i++)
    {
        EXPECT_GE(weights[i], 0.0);
        EXPECT_LE(weights[i], 1.0);
        weightSum += weights[i];
    }
    expectNear(ctx, weightSum, 1.0, 1e-12, "balance weight sum");
    expectNear(ctx, weights[2], pdfs[2] / sum, 1e-12, "dominant weight");
    expectNear(ctx, weights[0], 0.1, 1e-12, "minority weight");

    // Equal pdfs give equal weights: two strategies with pdf 0.5 each.
    const double equalWeight = misBalanceWeight(0.5, 1.0);
    expectNear(ctx, equalWeight, 0.5, 1e-12, "equal-pdf weight");
}

CPU_TEST(LumenGISampling_MisPowerHeuristic)
{
    // Power heuristic with exponent 2 (Veach). Sum of weights is one.
    const double pdfs[2] = {0.2, 0.8};
    const double powers = pdfs[0] * pdfs[0] + pdfs[1] * pdfs[1];
    const double w0 = misPowerWeight(pdfs[0], powers, 2.0);
    const double w1 = misPowerWeight(pdfs[1], powers, 2.0);
    expectNear(ctx, w0 + w1, 1.0, 1e-9, "power weight sum");
    expectNear(ctx, w0, 0.04 / 0.68, 1e-9, "power weight minority");

    // Single strategy: weight is exactly one.
    expectNear(ctx, misPowerWeight(0.25, 0.0625, 2.0), 1.0, 1e-9, "single-strategy weight");

    // Degenerate case: all pdfs zero -> weight zero (no NaN).
    EXPECT_EQ(misPowerWeight(0.0, 0.0, 2.0), 0.0);
    EXPECT_EQ(misBalanceWeight(0.0, 0.0), 0.0);
}

// ---------------------------------------------------------------------------
// Radiance clamp / firefly / NaN-Inf helpers.
// ---------------------------------------------------------------------------

CPU_TEST(LumenGISampling_RadianceClamp)
{
    EXPECT_EQ(clampRadiance(-5.0, 10.0), 0.0);
    EXPECT_EQ(clampRadiance(0.0, 10.0), 0.0);
    EXPECT_EQ(clampRadiance(0.5, 10.0), 0.5);
    EXPECT_EQ(clampRadiance(10.0, 10.0), 10.0);
    EXPECT_EQ(clampRadiance(100.0, 10.0), 10.0);
    EXPECT_EQ(clampRadiance(0.0, 0.0), 0.0);
    EXPECT_EQ(clampRadiance(-1e6, 1e-3), 0.0);
}

CPU_TEST(LumenGISampling_FireflyDetection)
{
    EXPECT_TRUE(isFirefly(1e6, 100.0));
    EXPECT_TRUE(isFirefly(101.0, 100.0));
    EXPECT_FALSE(isFirefly(100.0, 100.0)); // strict inequality
    EXPECT_FALSE(isFirefly(50.0, 100.0));
    EXPECT_FALSE(isFirefly(0.0, 100.0));
    EXPECT_FALSE(isFirefly(-1.0, 100.0)); // negative radiance is not a firefly
}

CPU_TEST(LumenGISampling_NonFiniteDetection)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    const double finite[3] = {1.0, 2.0, 3.0};
    const double hasNan[3] = {1.0, nan, 3.0};
    const double hasInf[3] = {1.0, inf, 3.0};
    const double allNan[3] = {nan, nan, nan};

    EXPECT_FALSE(hasNonFinite(finite));
    EXPECT_TRUE(hasNonFinite(hasNan));
    EXPECT_TRUE(hasNonFinite(hasInf));
    EXPECT_TRUE(hasNonFinite(allNan));

    // Sanitization replaces non-finite values with zero and keeps finite ones.
    EXPECT_EQ(sanitizeRadiance(nan), 0.0);
    EXPECT_EQ(sanitizeRadiance(inf), 0.0);
    EXPECT_EQ(sanitizeRadiance(-inf), 0.0);
    EXPECT_EQ(sanitizeRadiance(0.25), 0.25);
    EXPECT_EQ(sanitizeRadiance(0.0), 0.0);
}

// ---------------------------------------------------------------------------
// C++/Slang shared layout.
// ---------------------------------------------------------------------------

namespace
{
// Mirrors of the kLumenDebug* constants in LumenGIData.slang, captured at the
// baseline commit eb540f67. Only these values are asserted; new constants
// added later by Agent B do not affect this test.
constexpr uint32_t kLumenDebugNoneMirror = 0;
constexpr uint32_t kLumenDebugNormalMirror = 1;
constexpr uint32_t kLumenDebugLinearDepthMirror = 2;
constexpr uint32_t kLumenDebugMotionMirror = 3;
constexpr uint32_t kLumenDebugMaterialIDMirror = 4;
constexpr uint32_t kLumenDebugConfidenceMirror = 5;
} // namespace

CPU_TEST(LumenGISampling_SharedLayout_DebugModeValues)
{
    // LumenGIData.slang constants must match the C++ DebugMode enum values
    // (the host casts mDebugMode into the shader's CB as a uint).
    EXPECT_EQ(static_cast<uint32_t>(LumenGIPass::DebugMode::None), kLumenDebugNoneMirror);
    EXPECT_EQ(static_cast<uint32_t>(LumenGIPass::DebugMode::Normal), kLumenDebugNormalMirror);
    EXPECT_EQ(static_cast<uint32_t>(LumenGIPass::DebugMode::LinearDepth), kLumenDebugLinearDepthMirror);
    EXPECT_EQ(static_cast<uint32_t>(LumenGIPass::DebugMode::Motion), kLumenDebugMotionMirror);
    EXPECT_EQ(static_cast<uint32_t>(LumenGIPass::DebugMode::MaterialID), kLumenDebugMaterialIDMirror);
    EXPECT_EQ(static_cast<uint32_t>(LumenGIPass::DebugMode::Confidence), kLumenDebugConfidenceMirror);
}

CPU_TEST(LumenGISampling_SharedLayout_DebugModeNames)
{
    // String mirrors used by Properties serialization must stay stable.
    EXPECT_EQ(enumToString(LumenGIPass::DebugMode::None), "None");
    EXPECT_EQ(enumToString(LumenGIPass::DebugMode::Normal), "Normal");
    EXPECT_EQ(enumToString(LumenGIPass::DebugMode::LinearDepth), "LinearDepth");
    EXPECT_EQ(enumToString(LumenGIPass::DebugMode::Motion), "Motion");
    EXPECT_EQ(enumToString(LumenGIPass::DebugMode::MaterialID), "MaterialID");
    EXPECT_EQ(enumToString(LumenGIPass::DebugMode::Confidence), "Confidence");
}

} // namespace Falcor
