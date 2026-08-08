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

#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <string>

/** Resource memory owned or attributed to the LumenGI pipeline.

    Values are allocation sizes in bytes, not logical texel payload sizes. Keeping the
    categories explicit makes benchmark manifests comparable as the implementation grows.
*/
struct LumenGIResourceStats
{
    uint64_t surfaceCacheMaterialBytes = 0;
    uint64_t surfaceCacheRadianceBytes = 0;
    uint64_t surfaceCacheMetadataBytes = 0;
    uint64_t screenProbeBytes = 0;
    uint64_t historyBytes = 0;
    uint64_t accelerationStructureBytes = 0;
    uint64_t meshSdfBytes = 0;
    uint64_t radianceCacheBytes = 0;
    uint64_t transientBytes = 0;

    /** Sum resources that persist across frames. The result saturates on overflow. */
    uint64_t getPersistentBytes() const noexcept;

    /** Sum all tracked resources. The result saturates on overflow. */
    uint64_t getTotalBytes() const noexcept;
};

/** Stable, GPU-independent per-frame statistics for LumenGI.

    A zero memory budget means that no budget has been configured. In that state the
    frame is considered within budget and remaining/excess bytes are both zero.
*/

/** Per-frame lighting counters accumulated on the GPU by the tracing shader.

    The host clears the gLumenGICounters buffer before each trace dispatch and
    reads it back into these fields after the frame. The counter order follows
    LumenGICounterIndex in LumenGIData.slang. These fields are intentionally
    excluded from the serialized schema: the telemetry dictionary and the JSON
    layout are frozen at schema version 1 (LumenGIStatsTests.cpp asserts their
    exact shape). Adding them to telemetry is a schema version 2 change.
*/
struct LumenGIFrameCounters
{
    uint64_t nanInfSamples = 0;    ///< Samples whose radiance contained NaN/Inf and was zeroed.
    uint64_t fireflySamples = 0;   ///< Samples clamped by the radiance ceiling.
    uint64_t negativeSamples = 0;  ///< Samples with negative radiance components, zeroed.
    uint64_t tracedSamples = 0;    ///< Diffuse indirect rays traced (hit and miss).

    void reset() noexcept { *this = LumenGIFrameCounters{}; }
};

struct LumenGIStats
{
    using Dictionary = std::map<std::string, uint64_t>;

    static constexpr uint32_t kSchemaVersion = 1;

    uint64_t frameIndex = 0;
    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
    uint32_t internalWidth = 0;
    uint32_t internalHeight = 0;
    uint64_t memoryBudgetBytes = 0;
    LumenGIResourceStats resources;
    LumenGIFrameCounters counters;

    bool hasMemoryBudget() const noexcept { return memoryBudgetBytes != 0; }
    uint64_t getTotalMemoryBytes() const noexcept { return resources.getTotalBytes(); }
    bool isWithinMemoryBudget() const noexcept;
    uint64_t getBudgetRemainingBytes() const noexcept;
    uint64_t getBudgetExcessBytes() const noexcept;

    /** Return a flat dictionary with stable snake_case field names.

        Boolean values are encoded as zero or one so the dictionary has a single value
        type and can be forwarded to telemetry systems without type coercion.
    */
    Dictionary toDictionary() const;

    /** Serialize the same schema as compact JSON with deterministic field ordering. */
    std::string toJson() const;
};

namespace LumenGIStatsDetail
{
inline uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs) noexcept
{
    const uint64_t maxValue = std::numeric_limits<uint64_t>::max();
    return rhs > maxValue - lhs ? maxValue : lhs + rhs;
}
} // namespace LumenGIStatsDetail

inline uint64_t LumenGIResourceStats::getPersistentBytes() const noexcept
{
    uint64_t total = 0;
    total = LumenGIStatsDetail::saturatingAdd(total, surfaceCacheMaterialBytes);
    total = LumenGIStatsDetail::saturatingAdd(total, surfaceCacheRadianceBytes);
    total = LumenGIStatsDetail::saturatingAdd(total, surfaceCacheMetadataBytes);
    total = LumenGIStatsDetail::saturatingAdd(total, screenProbeBytes);
    total = LumenGIStatsDetail::saturatingAdd(total, historyBytes);
    total = LumenGIStatsDetail::saturatingAdd(total, accelerationStructureBytes);
    total = LumenGIStatsDetail::saturatingAdd(total, meshSdfBytes);
    total = LumenGIStatsDetail::saturatingAdd(total, radianceCacheBytes);
    return total;
}

inline uint64_t LumenGIResourceStats::getTotalBytes() const noexcept
{
    return LumenGIStatsDetail::saturatingAdd(getPersistentBytes(), transientBytes);
}

inline bool LumenGIStats::isWithinMemoryBudget() const noexcept
{
    return !hasMemoryBudget() || getTotalMemoryBytes() <= memoryBudgetBytes;
}

inline uint64_t LumenGIStats::getBudgetRemainingBytes() const noexcept
{
    if (!hasMemoryBudget()) return 0;
    const uint64_t total = getTotalMemoryBytes();
    return total < memoryBudgetBytes ? memoryBudgetBytes - total : 0;
}

inline uint64_t LumenGIStats::getBudgetExcessBytes() const noexcept
{
    if (!hasMemoryBudget()) return 0;
    const uint64_t total = getTotalMemoryBytes();
    return total > memoryBudgetBytes ? total - memoryBudgetBytes : 0;
}

inline LumenGIStats::Dictionary LumenGIStats::toDictionary() const
{
    return {
        {"schema_version", kSchemaVersion},
        {"frame_index", frameIndex},
        {"frame_width", frameWidth},
        {"frame_height", frameHeight},
        {"internal_width", internalWidth},
        {"internal_height", internalHeight},
        {"surface_cache_material_bytes", resources.surfaceCacheMaterialBytes},
        {"surface_cache_radiance_bytes", resources.surfaceCacheRadianceBytes},
        {"surface_cache_metadata_bytes", resources.surfaceCacheMetadataBytes},
        {"screen_probe_bytes", resources.screenProbeBytes},
        {"history_bytes", resources.historyBytes},
        {"acceleration_structure_bytes", resources.accelerationStructureBytes},
        {"mesh_sdf_bytes", resources.meshSdfBytes},
        {"radiance_cache_bytes", resources.radianceCacheBytes},
        {"transient_bytes", resources.transientBytes},
        {"persistent_memory_bytes", resources.getPersistentBytes()},
        {"total_memory_bytes", getTotalMemoryBytes()},
        {"memory_budget_bytes", memoryBudgetBytes},
        {"has_memory_budget", hasMemoryBudget() ? 1u : 0u},
        {"within_memory_budget", isWithinMemoryBudget() ? 1u : 0u},
        {"budget_remaining_bytes", getBudgetRemainingBytes()},
        {"budget_excess_bytes", getBudgetExcessBytes()},
    };
}

inline std::string LumenGIStats::toJson() const
{
    std::ostringstream os;
    os << '{'
       << "\"schema_version\":" << kSchemaVersion
       << ",\"frame_index\":" << frameIndex
       << ",\"frame_width\":" << frameWidth
       << ",\"frame_height\":" << frameHeight
       << ",\"internal_width\":" << internalWidth
       << ",\"internal_height\":" << internalHeight
       << ",\"resources\":{"
       << "\"surface_cache_material_bytes\":" << resources.surfaceCacheMaterialBytes
       << ",\"surface_cache_radiance_bytes\":" << resources.surfaceCacheRadianceBytes
       << ",\"surface_cache_metadata_bytes\":" << resources.surfaceCacheMetadataBytes
       << ",\"screen_probe_bytes\":" << resources.screenProbeBytes
       << ",\"history_bytes\":" << resources.historyBytes
       << ",\"acceleration_structure_bytes\":" << resources.accelerationStructureBytes
       << ",\"mesh_sdf_bytes\":" << resources.meshSdfBytes
       << ",\"radiance_cache_bytes\":" << resources.radianceCacheBytes
       << ",\"transient_bytes\":" << resources.transientBytes
       << ",\"persistent_memory_bytes\":" << resources.getPersistentBytes()
       << ",\"total_memory_bytes\":" << getTotalMemoryBytes()
       << '}'
       << ",\"budget\":{"
       << "\"memory_budget_bytes\":" << memoryBudgetBytes
       << ",\"has_memory_budget\":" << (hasMemoryBudget() ? "true" : "false")
       << ",\"within_memory_budget\":" << (isWithinMemoryBudget() ? "true" : "false")
       << ",\"budget_remaining_bytes\":" << getBudgetRemainingBytes()
       << ",\"budget_excess_bytes\":" << getBudgetExcessBytes()
       << "}}";
    return os.str();
}
