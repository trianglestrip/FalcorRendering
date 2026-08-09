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
#include "Testing/UnitTest.h"
#include "../../../../RenderPasses/LumenGI/Quality/LumenQualityPreset.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace Falcor
{
namespace
{
constexpr uint32_t kPresetCount = static_cast<uint32_t>(LumenQualityPreset::Count);

std::array<LumenQualityPreset, kPresetCount> kPresetOrder()
{
    return {
        LumenQualityPreset::Low,
        LumenQualityPreset::Medium,
        LumenQualityPreset::High,
        LumenQualityPreset::Reference,
    };
}

// LumenQualityParams has no fmt formatter, so compare field-by-field.
bool paramsEqual(const LumenQualityParams& lhs, const LumenQualityParams& rhs)
{
    return lhs.giResolutionScale == rhs.giResolutionScale && lhs.probeTileSize == rhs.probeTileSize &&
        lhs.probeDirectionsPerUpdate == rhs.probeDirectionsPerUpdate && lhs.traceMaxDistanceMeters == rhs.traceMaxDistanceMeters &&
        lhs.atlasBudgetBytes == rhs.atlasBudgetBytes && lhs.cacheLightingSamplesPerTexel == rhs.cacheLightingSamplesPerTexel &&
        lhs.useTemporal == rhs.useTemporal && lhs.useSpatial == rhs.useSpatial && lhs.useFeedback == rhs.useFeedback &&
        lhs.pagesPerFrame == rhs.pagesPerFrame;
}
} // namespace

CPU_TEST(LumenQualityPreset_DefaultsAreValid)
{
    // Every tier from the frozen table is itself a valid, ready-to-use config.
    for (LumenQualityPreset preset : kPresetOrder())
    {
        const LumenQualityParams params = qualityForPreset(preset);
        EXPECT_TRUE(params.isValid());
        EXPECT_TRUE(isValid(params));
    }
}

CPU_TEST(LumenQualityPreset_MonotonicLowToReference)
{
    // Low -> Reference must never drop quality. probeTileSize / probeDirections /
    // samplesPerTexel / budget / pages / trace distance are non-decreasing and the
    // GI resolution scale is non-decreasing (Low=0.25 quarter-res "resolution downgrade",
    // Medium=0.5 half-res, High/Reference=1.0 full-res).
    const std::array<LumenQualityPreset, kPresetCount> order = kPresetOrder();
    std::array<LumenQualityParams, kPresetCount> params;
    for (uint32_t i = 0; i < kPresetCount; ++i)
    {
        params[i] = qualityForPreset(order[i]);
    }

    for (uint32_t i = 0; i + 1 < kPresetCount; ++i)
    {
        EXPECT_LE(params[i].probeTileSize, params[i + 1].probeTileSize);
        EXPECT_LE(params[i].probeDirectionsPerUpdate, params[i + 1].probeDirectionsPerUpdate);
        EXPECT_LE(params[i].cacheLightingSamplesPerTexel, params[i + 1].cacheLightingSamplesPerTexel);
        EXPECT_LE(params[i].atlasBudgetBytes, params[i + 1].atlasBudgetBytes);
        EXPECT_LE(params[i].pagesPerFrame, params[i + 1].pagesPerFrame);
        EXPECT_LE(params[i].traceMaxDistanceMeters, params[i + 1].traceMaxDistanceMeters);
        EXPECT_LE(params[i].giResolutionScale, params[i + 1].giResolutionScale);
    }

    // Low is the cheapest tier (quarter-res, sparsest, fewest samples) and
    // Reference the most expensive; at least one knob strictly improves end-to-end.
    EXPECT_EQ(params[0].giResolutionScale, 0.25f);
    EXPECT_EQ(params[0].probeTileSize, 8u);
    EXPECT_EQ(params[0].probeDirectionsPerUpdate, 8u);
    EXPECT_EQ(params[0].cacheLightingSamplesPerTexel, 1u);
    EXPECT_EQ(params[3].giResolutionScale, 1.0f);
    EXPECT_EQ(params[3].probeTileSize, 16u);
    EXPECT_EQ(params[3].cacheLightingSamplesPerTexel, 8u);
    EXPECT_LT(params[0].giResolutionScale, params[3].giResolutionScale);
    EXPECT_LT(params[0].probeTileSize, params[3].probeTileSize);
    EXPECT_LT(params[0].probeDirectionsPerUpdate, params[3].probeDirectionsPerUpdate);
    EXPECT_LT(params[0].cacheLightingSamplesPerTexel, params[3].cacheLightingSamplesPerTexel);
}

CPU_TEST(LumenQualityPreset_SerializeRoundTrip)
{
    // serializeToString -> parse must reproduce the exact params for every tier.
    for (LumenQualityPreset preset : kPresetOrder())
    {
        const LumenQualityParams original = qualityForPreset(preset);
        const std::string text = serializeToString(original);
        EXPECT_TRUE(!text.empty());

        LumenQualityParams parsed;
        EXPECT_TRUE(parse(text, parsed));
        EXPECT_TRUE(paramsEqual(parsed, original));
        EXPECT_TRUE(parsed.isValid());
    }
}

CPU_TEST(LumenQualityPreset_SerializeDeterministic)
{
    // Identical params -> identical bytes; distinct presets -> distinct strings.
    const LumenQualityParams params = qualityForPreset(LumenQualityPreset::High);
    const std::string first = serializeToString(params);
    const std::string second = serializeToString(params);
    EXPECT_EQ(first, second);

    std::string previous;
    for (LumenQualityPreset preset : kPresetOrder())
    {
        const std::string text = serializeToString(qualityForPreset(preset));
        EXPECT_NE(text, previous); //< No two tiers serialize identically.
        previous = text;
    }
}

CPU_TEST(LumenQualityPreset_InvalidParamsRejected)
{
    // Every knob, taken out of its accepted set, must be rejected by isValid().
    LumenQualityParams params = qualityForPreset(LumenQualityPreset::High);
    EXPECT_TRUE(params.isValid());

    params.giResolutionScale = 0.75f;
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.probeTileSize = 10;
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.probeDirectionsPerUpdate = 10;
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.cacheLightingSamplesPerTexel = 3;
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.atlasBudgetBytes = 0;
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.pagesPerFrame = 0;
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.traceMaxDistanceMeters = 0.0f;
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.traceMaxDistanceMeters = -1.0f;
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.traceMaxDistanceMeters = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(params.isValid());
    params = qualityForPreset(LumenQualityPreset::High);

    params.traceMaxDistanceMeters = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(params.isValid());
}

CPU_TEST(LumenQualityPreset_ParseRejectsBadInput)
{
    LumenQualityParams params;
    EXPECT_FALSE(parse("", params));
    EXPECT_FALSE(parse("not json at all", params));

    // A string missing the presetVersion field is malformed.
    std::string text = serializeToString(qualityForPreset(LumenQualityPreset::Medium));
    EXPECT_TRUE(parse(text, params));
    EXPECT_TRUE(paramsEqual(params, qualityForPreset(LumenQualityPreset::Medium)));

    // A string carrying a different presetVersion is rejected.
    std::string tampered = text;
    const std::string needle = std::string("\"presetVersion\":") + std::to_string(kLumenQualityPresetVersion);
    const std::string replacement = std::string("\"presetVersion\":") + std::to_string(kLumenQualityPresetVersion + 1);
    const size_t versionPos = tampered.find(needle);
    EXPECT_TRUE(versionPos != std::string::npos);
    tampered.replace(versionPos, needle.size(), replacement);
    EXPECT_FALSE(parse(tampered, params));

    // Truncated strings (a required field dropped) are malformed.
    const size_t atlasField = text.find("\"atlasBudgetBytes\"");
    EXPECT_TRUE(atlasField != std::string::npos);
    EXPECT_FALSE(parse(text.substr(0, atlasField), params));
}

CPU_TEST(LumenQualityPreset_PresetVersion)
{
    // The version is bumped on format changes and always present in serialized text.
    EXPECT_GE(kLumenQualityPresetVersion, 1u);
    const std::string text = serializeToString(qualityForPreset(LumenQualityPreset::Reference));
    EXPECT_TRUE(text.find("\"presetVersion\":") != std::string::npos);
}
} // namespace Falcor
