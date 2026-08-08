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
#include "../../../../RenderPasses/LumenGI/LumenGIStats.h"

#include <limits>

namespace Falcor
{

CPU_TEST(LumenGIStats_Defaults)
{
    LumenGIStats stats;
    EXPECT_EQ(stats.getTotalMemoryBytes(), 0);
    EXPECT_FALSE(stats.hasMemoryBudget());
    EXPECT_TRUE(stats.isWithinMemoryBudget());
    EXPECT_EQ(stats.getBudgetRemainingBytes(), 0);
    EXPECT_EQ(stats.getBudgetExcessBytes(), 0);
}

CPU_TEST(LumenGIStats_ResourceTotals)
{
    LumenGIResourceStats resources;
    resources.surfaceCacheMaterialBytes = 1;
    resources.surfaceCacheRadianceBytes = 2;
    resources.surfaceCacheMetadataBytes = 4;
    resources.screenProbeBytes = 8;
    resources.historyBytes = 16;
    resources.accelerationStructureBytes = 32;
    resources.meshSdfBytes = 64;
    resources.radianceCacheBytes = 128;
    resources.transientBytes = 256;

    EXPECT_EQ(resources.getPersistentBytes(), 255);
    EXPECT_EQ(resources.getTotalBytes(), 511);
}

CPU_TEST(LumenGIStats_BudgetBoundary)
{
    LumenGIStats stats;
    stats.resources.surfaceCacheMaterialBytes = 384;
    stats.resources.transientBytes = 128;

    stats.memoryBudgetBytes = 512;
    EXPECT_TRUE(stats.isWithinMemoryBudget());
    EXPECT_EQ(stats.getBudgetRemainingBytes(), 0);
    EXPECT_EQ(stats.getBudgetExcessBytes(), 0);

    stats.memoryBudgetBytes = 640;
    EXPECT_TRUE(stats.isWithinMemoryBudget());
    EXPECT_EQ(stats.getBudgetRemainingBytes(), 128);
    EXPECT_EQ(stats.getBudgetExcessBytes(), 0);

    stats.memoryBudgetBytes = 500;
    EXPECT_FALSE(stats.isWithinMemoryBudget());
    EXPECT_EQ(stats.getBudgetRemainingBytes(), 0);
    EXPECT_EQ(stats.getBudgetExcessBytes(), 12);
}

CPU_TEST(LumenGIStats_SaturatesOnOverflow)
{
    LumenGIResourceStats resources;
    resources.surfaceCacheMaterialBytes = std::numeric_limits<uint64_t>::max() - 8;
    resources.surfaceCacheRadianceBytes = 16;
    resources.transientBytes = 1;

    EXPECT_EQ(resources.getPersistentBytes(), std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(resources.getTotalBytes(), std::numeric_limits<uint64_t>::max());
}

CPU_TEST(LumenGIStats_DictionarySchema)
{
    LumenGIStats stats;
    stats.frameIndex = 17;
    stats.frameWidth = 1280;
    stats.frameHeight = 720;
    stats.internalWidth = 640;
    stats.internalHeight = 360;
    stats.memoryBudgetBytes = 1024;
    stats.resources.historyBytes = 256;

    const auto dictionary = stats.toDictionary();
    EXPECT_EQ(dictionary.size(), 22);
    EXPECT_EQ(dictionary.at("schema_version"), LumenGIStats::kSchemaVersion);
    EXPECT_EQ(dictionary.at("frame_index"), 17);
    EXPECT_EQ(dictionary.at("frame_width"), 1280);
    EXPECT_EQ(dictionary.at("internal_height"), 360);
    EXPECT_EQ(dictionary.at("history_bytes"), 256);
    EXPECT_EQ(dictionary.at("total_memory_bytes"), 256);
    EXPECT_EQ(dictionary.at("has_memory_budget"), 1);
    EXPECT_EQ(dictionary.at("within_memory_budget"), 1);
    EXPECT_EQ(dictionary.at("budget_remaining_bytes"), 768);
}

CPU_TEST(LumenGIStats_JsonIsStable)
{
    LumenGIStats stats;
    stats.frameIndex = 3;
    stats.frameWidth = 2;
    stats.frameHeight = 1;
    stats.internalWidth = 1;
    stats.internalHeight = 1;
    stats.memoryBudgetBytes = 5;
    stats.resources.surfaceCacheMaterialBytes = 4;
    stats.resources.transientBytes = 2;

    const std::string expected =
        "{\"schema_version\":1,\"frame_index\":3,\"frame_width\":2,\"frame_height\":1,\"internal_width\":1,"
        "\"internal_height\":1,\"resources\":{\"surface_cache_material_bytes\":4,\"surface_cache_radiance_bytes\":0,"
        "\"surface_cache_metadata_bytes\":0,\"screen_probe_bytes\":0,\"history_bytes\":0,"
        "\"acceleration_structure_bytes\":0,\"mesh_sdf_bytes\":0,\"radiance_cache_bytes\":0,\"transient_bytes\":2,"
        "\"persistent_memory_bytes\":4,\"total_memory_bytes\":6},\"budget\":{\"memory_budget_bytes\":5,"
        "\"has_memory_budget\":true,\"within_memory_budget\":false,\"budget_remaining_bytes\":0,"
        "\"budget_excess_bytes\":1}}";
    EXPECT_EQ(stats.toJson(), expected);
}

} // namespace Falcor
