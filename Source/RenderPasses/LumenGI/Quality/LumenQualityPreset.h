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
#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace Falcor
{

/** LumenGI quality preset table format version (S8).

    Increment whenever serializeToString() emits a different layout or
    qualityForPreset() changes a default; parse() rejects strings carrying a
    presetVersion different from this constant, so a bumped version makes old
    serialized configurations safely unreadable instead of mis-parsed.
    LumenQualityPresetTests.cpp pins this value >= 1.
*/
constexpr uint32_t kLumenQualityPresetVersion = 1;

/** User-facing LumenGI quality tiers (S8). Ordered Low -> Reference so the
    table in qualityForPreset() is monotonically increasing in quality.
*/
enum class LumenQualityPreset : uint32_t
{
    Low = 0,
    Medium = 1,
    High = 2,
    Reference = 3,
    Count,
};

/** Per-preset LumenGI tuning parameters (pure CPU, no GPU or Device dependency).

    The four tiers trade GI resolution, probe density, per-update ray budget and
    cache memory against quality:

      - giResolutionScale           1.0/0.5/0.25 render fraction. Low DOWNGRADES
                                     resolution (quarter-res, upscaled); High and
                                     Reference run full-res (matches the frozen
                                     LumenReconstruction::qualityForPreset mapping
                                     Low->Quarter, Medium->Half, High/Reference->Full).
      - probeTileSize               8/12/16 texels per probe tile. Smaller tiles =
                                     more, denser probes for the same atlas.
      - probeDirectionsPerUpdate    8/12/16/24 directions sampled per probe update;
                                     more directions = less temporal noise.
      - traceMaxDistanceMeters      Ray length limit in meters (> 0).
      - atlasBudgetBytes            Combined GPU budget for the cache atlases (> 0).
      - cacheLightingSamplesPerTexel 1/2/4/8 NEE draws per cache texel. Mirrors the
                                     frozen LumenCacheLightingQuality mapping in
                                     LumenSurfaceCacheLightingData.slang (Low/Medium/
                                     High/Reference -> 1/2/4/8).
      - useTemporal/useSpatial/useFeedback  Reconstruction feedback toggles.
      - pagesPerFrame               Max resident pages streamed into the atlas per
                                     frame; a larger number fills a cold cache faster.

    Default table rationale (qualityForPreset):
      Low       0.25 GI res (quarter, upscaled), sparsest probes (8px tiles),
                8 dirs/update, 1 sample/texel, 256 MiB atlas, 16 pages/frame.
      Medium    0.5 GI res (half), 12px tiles, 12 dirs, 2 samples/texel,
                512 MiB atlas, 32 pages/frame.
      High      1.0 GI res (full), 16px tiles, 16 dirs, 4 samples/texel,
                1 GiB atlas, 48 pages/frame.
      Reference 1.0 GI res (full), 16px tiles, 24 dirs, 8 samples/texel,
                1.5 GiB atlas, 64 pages/frame.
*/
struct LumenQualityParams
{
    float giResolutionScale = 1.0f;                 ///< 1.0/0.5/0.25 (full/half/quarter).
    uint32_t probeTileSize = 16;                    ///< 8/12/16 texels per tile.
    uint32_t probeDirectionsPerUpdate = 16;         ///< 8/12/16/24.
    float traceMaxDistanceMeters = 200.0f;          ///< > 0.
    uint64_t atlasBudgetBytes = 1024ull * 1024ull * 1024ull; ///< > 0 (1 GiB default).
    uint32_t cacheLightingSamplesPerTexel = 4;      ///< 1/2/4/8.
    bool useTemporal = true;                        ///< Temporal accumulation.
    bool useSpatial = true;                         ///< Spatial denoising.
    bool useFeedback = true;                        ///< Multi-bounce cache feedback.
    uint32_t pagesPerFrame = 48;                    ///< > 0.

    bool operator==(const LumenQualityParams& rhs) const
    {
        return giResolutionScale == rhs.giResolutionScale
            && probeTileSize == rhs.probeTileSize
            && probeDirectionsPerUpdate == rhs.probeDirectionsPerUpdate
            && traceMaxDistanceMeters == rhs.traceMaxDistanceMeters
            && atlasBudgetBytes == rhs.atlasBudgetBytes
            && cacheLightingSamplesPerTexel == rhs.cacheLightingSamplesPerTexel
            && useTemporal == rhs.useTemporal
            && useSpatial == rhs.useSpatial
            && useFeedback == rhs.useFeedback
            && pagesPerFrame == rhs.pagesPerFrame;
    }

    bool operator!=(const LumenQualityParams& rhs) const { return !(*this == rhs); }

    /** True when every field is within its accepted value set.

        Note: the scalar checks use exact equality on 0.25f/0.5f/1.0f and the
        integer sets {8,12,16}, {8,12,16,24}, {1,2,4,8}; all of these values are
        exactly representable, so exact comparison is the intended contract.
    */
    bool isValid() const
    {
        const bool resolutionOk = giResolutionScale == 1.0f || giResolutionScale == 0.5f
            || giResolutionScale == 0.25f;
        const bool tileOk = probeTileSize == 8 || probeTileSize == 12 || probeTileSize == 16;
        const bool directionsOk = probeDirectionsPerUpdate == 8 || probeDirectionsPerUpdate == 12
            || probeDirectionsPerUpdate == 16 || probeDirectionsPerUpdate == 24;
        const bool samplesOk = cacheLightingSamplesPerTexel == 1 || cacheLightingSamplesPerTexel == 2
            || cacheLightingSamplesPerTexel == 4 || cacheLightingSamplesPerTexel == 8;
        return resolutionOk && tileOk && directionsOk && samplesOk
            && std::isfinite(traceMaxDistanceMeters) && traceMaxDistanceMeters > 0.0f
            && atlasBudgetBytes > 0 && pagesPerFrame > 0;
    }
};

/** Default per-preset parameters (frozen S8 table). Out-of-range preset values
    fall through to the Reference tier.
*/
inline LumenQualityParams qualityForPreset(LumenQualityPreset preset)
{
    LumenQualityParams params;
    switch (preset)
    {
    case LumenQualityPreset::Low:
        params.giResolutionScale = 0.25f;
        params.probeTileSize = 8;
        params.probeDirectionsPerUpdate = 8;
        params.traceMaxDistanceMeters = 100.0f;
        params.atlasBudgetBytes = 256ull * 1024ull * 1024ull;
        params.cacheLightingSamplesPerTexel = 1;
        params.useTemporal = true;
        params.useSpatial = false;
        params.useFeedback = false;
        params.pagesPerFrame = 16;
        break;
    case LumenQualityPreset::Medium:
        params.giResolutionScale = 0.5f;
        params.probeTileSize = 12;
        params.probeDirectionsPerUpdate = 12;
        params.traceMaxDistanceMeters = 150.0f;
        params.atlasBudgetBytes = 512ull * 1024ull * 1024ull;
        params.cacheLightingSamplesPerTexel = 2;
        params.useTemporal = true;
        params.useSpatial = true;
        params.useFeedback = true;
        params.pagesPerFrame = 32;
        break;
    case LumenQualityPreset::High:
        params.giResolutionScale = 1.0f;
        params.probeTileSize = 16;
        params.probeDirectionsPerUpdate = 16;
        params.traceMaxDistanceMeters = 200.0f;
        params.atlasBudgetBytes = 1024ull * 1024ull * 1024ull;
        params.cacheLightingSamplesPerTexel = 4;
        params.useTemporal = true;
        params.useSpatial = true;
        params.useFeedback = true;
        params.pagesPerFrame = 48;
        break;
    case LumenQualityPreset::Reference:
    default:
        params.giResolutionScale = 1.0f;
        params.probeTileSize = 16;
        params.probeDirectionsPerUpdate = 24;
        params.traceMaxDistanceMeters = 300.0f;
        params.atlasBudgetBytes = 1536ull * 1024ull * 1024ull;
        params.cacheLightingSamplesPerTexel = 8;
        params.useTemporal = true;
        params.useSpatial = true;
        params.useFeedback = true;
        params.pagesPerFrame = 64;
        break;
    }
    return params;
}

/** Free-function form of LumenQualityParams::isValid().
*/
inline bool isValid(const LumenQualityParams& params)
{
    return params.isValid();
}

/** Serialize a preset to a fixed-format, JSON-style object string.

    Deterministic by construction: field order and float precision are fixed
    (floats use std::fixed with 2 decimals; every preset-table value is exactly
    representable), so identical params always produce the identical string.
    The format starts with the presetVersion field and is the inverse of parse().
*/
inline std::string serializeToString(const LumenQualityParams& params)
{
    std::ostringstream os;
    os << '{'
       << "\"presetVersion\":" << kLumenQualityPresetVersion
       << ",\"giResolutionScale\":" << std::fixed << std::setprecision(2) << params.giResolutionScale
       << ",\"probeTileSize\":" << params.probeTileSize
       << ",\"probeDirectionsPerUpdate\":" << params.probeDirectionsPerUpdate
       << ",\"traceMaxDistanceMeters\":" << std::fixed << std::setprecision(2) << params.traceMaxDistanceMeters
       << ",\"atlasBudgetBytes\":" << params.atlasBudgetBytes
       << ",\"cacheLightingSamplesPerTexel\":" << params.cacheLightingSamplesPerTexel
       << ",\"useTemporal\":" << (params.useTemporal ? "true" : "false")
       << ",\"useSpatial\":" << (params.useSpatial ? "true" : "false")
       << ",\"useFeedback\":" << (params.useFeedback ? "true" : "false")
       << ",\"pagesPerFrame\":" << params.pagesPerFrame
       << '}';
    return os.str();
}

namespace LumenQualityDetail
{
inline bool extractValue(const std::string& text, const char* key, std::string& out)
{
    const std::string needle = std::string("\"") + key + "\":";
    const size_t start = text.find(needle);
    if (start == std::string::npos)
    {
        return false;
    }
    size_t end = start + needle.size();
    while (end < text.size() && text[end] != ',' && text[end] != '}')
    {
        ++end;
    }
    out = text.substr(start + needle.size(), end - (start + needle.size()));
    return !out.empty();
}

inline bool parseFloatValue(const std::string& value, float& out)
{
    std::istringstream is(value);
    is >> out;
    return !is.fail() && is.eof();
}

template<typename T>
inline bool parseUnsignedValue(const std::string& value, T& out)
{
    std::istringstream is(value);
    is >> out;
    return !is.fail() && is.eof();
}

inline bool parseBoolValue(const std::string& value, bool& out)
{
    if (value == "true")
    {
        out = true;
        return true;
    }
    if (value == "false")
    {
        out = false;
        return true;
    }
    return false;
}
} // namespace LumenQualityDetail

/** Parse the output of serializeToString().

    @return False for malformed strings (missing/empty field, bad number, bad
    bool) or for strings whose presetVersion differs from
    kLumenQualityPresetVersion. On success `out` receives the parsed params;
    validity against the accepted value sets is NOT checked here -- callers use
    isValid() for that (parse() only guarantees the string was well-formed).
*/
inline bool parse(const std::string& text, LumenQualityParams& out)
{
    std::string value;
    uint32_t version = 0;
    if (!LumenQualityDetail::extractValue(text, "presetVersion", value)
        || !LumenQualityDetail::parseUnsignedValue(value, version))
    {
        return false;
    }
    if (version != kLumenQualityPresetVersion)
    {
        return false;
    }

    LumenQualityParams params;
    if (!LumenQualityDetail::extractValue(text, "giResolutionScale", value)
        || !LumenQualityDetail::parseFloatValue(value, params.giResolutionScale))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "probeTileSize", value)
        || !LumenQualityDetail::parseUnsignedValue(value, params.probeTileSize))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "probeDirectionsPerUpdate", value)
        || !LumenQualityDetail::parseUnsignedValue(value, params.probeDirectionsPerUpdate))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "traceMaxDistanceMeters", value)
        || !LumenQualityDetail::parseFloatValue(value, params.traceMaxDistanceMeters))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "atlasBudgetBytes", value)
        || !LumenQualityDetail::parseUnsignedValue(value, params.atlasBudgetBytes))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "cacheLightingSamplesPerTexel", value)
        || !LumenQualityDetail::parseUnsignedValue(value, params.cacheLightingSamplesPerTexel))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "useTemporal", value)
        || !LumenQualityDetail::parseBoolValue(value, params.useTemporal))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "useSpatial", value)
        || !LumenQualityDetail::parseBoolValue(value, params.useSpatial))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "useFeedback", value)
        || !LumenQualityDetail::parseBoolValue(value, params.useFeedback))
    {
        return false;
    }
    if (!LumenQualityDetail::extractValue(text, "pagesPerFrame", value)
        || !LumenQualityDetail::parseUnsignedValue(value, params.pagesPerFrame))
    {
        return false;
    }

    out = params;
    return true;
}
} // namespace Falcor
