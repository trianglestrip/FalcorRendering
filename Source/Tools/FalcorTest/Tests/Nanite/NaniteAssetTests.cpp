/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
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
 **************************************************************************/
#include "Testing/UnitTest.h"

#include "NaniteBuild.h"
#include "NaniteObj.h"
#include "NaniteToolAsset.h"
#include "Nanite/NaniteCompress.h"

#include <cmath>
#include <fstream>
#include <vector>

namespace Falcor
{
namespace
{
using Falcor::Nanite::computeGpuMemoryStats;
using Falcor::Nanite::GpuMemoryStats;
using Falcor::Nanite::hasSourceGeometry;
using Falcor::Nanite::validateRuntimeTables;
using Falcor::Nanite::validateSourceGeometry;
using namespace FalcorRendering::NaniteTool;

constexpr float kBoundsEpsilon = 1e-4f;

const std::filesystem::path kCubeObjPath = getRuntimeDirectory() / "data/framework/meshes/cube.obj";

WriteOptions debugWriteOptions()
{
    WriteOptions options;
    options.compressVertices = false;
    options.debugUncompressed = true;
    return options;
}

WriteOptions compressedWriteOptions()
{
    WriteOptions options;
    options.compressVertices = true;
    options.debugUncompressed = false;
    return options;
}

Asset buildCubeAsset()
{
    InputScene scene = loadObjScene(kCubeObjPath);
    BuildOptions options;
    return buildNaniteAsset(scene, options);
}

Asset normalizedExpectedAsset(const Asset& asset, const WriteOptions& options)
{
    Asset expected = asset;
    buildMetadataTables(expected, options.groupClusters);
    expected.version = kNaniteVersion;
    return expected;
}

bool nearlyEqual(float a, float b, float epsilon = kBoundsEpsilon)
{
    return std::fabs(a - b) <= epsilon;
}

bool nearlyEqual(Float3 a, Float3 b, float epsilon = kBoundsEpsilon)
{
    return nearlyEqual(a.x, b.x, epsilon) && nearlyEqual(a.y, b.y, epsilon) && nearlyEqual(a.z, b.z, epsilon);
}

bool nearlyEqual(const Bounds& a, const Bounds& b, float epsilon = kBoundsEpsilon)
{
    return nearlyEqual(a.min, b.min, epsilon) && nearlyEqual(a.max, b.max, epsilon);
}

bool indicesEqual(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i] != b[i]) return false;
    }
    return true;
}

bool boundsContainsPoint(const Bounds& bounds, Float3 point, float epsilon = kBoundsEpsilon)
{
    return point.x >= bounds.min.x - epsilon && point.x <= bounds.max.x + epsilon && point.y >= bounds.min.y - epsilon &&
           point.y <= bounds.max.y + epsilon && point.z >= bounds.min.z - epsilon && point.z <= bounds.max.z + epsilon;
}

bool boundsContainsBounds(const Bounds& outer, const Bounds& inner, float epsilon = kBoundsEpsilon)
{
    return boundsContainsPoint(outer, inner.min, epsilon) && boundsContainsPoint(outer, inner.max, epsilon);
}

void expectAssetsEqual(CPUUnitTestContext& ctx, const Asset& expected, const Asset& actual)
{
    EXPECT_EQ(actual.version, expected.version);
    EXPECT_EQ(actual.meshes.size(), expected.meshes.size());
    EXPECT_EQ(actual.materials.size(), expected.materials.size());
    EXPECT_EQ(actual.clusters.size(), expected.clusters.size());
    EXPECT_EQ(actual.clusterGroups.size(), expected.clusterGroups.size());
    EXPECT_EQ(actual.hierarchyNodes.size(), expected.hierarchyNodes.size());
    EXPECT_EQ(actual.pages.size(), expected.pages.size());
    EXPECT_EQ(actual.vertices.size(), expected.vertices.size());
    EXPECT_EQ(actual.indices.size(), expected.indices.size());
    EXPECT_TRUE(nearlyEqual(actual.bounds, expected.bounds));

    for (size_t i = 0; i < expected.meshes.size(); ++i)
    {
        EXPECT_EQ(actual.meshes[i].name, expected.meshes[i].name);
        EXPECT_EQ(actual.meshes[i].firstCluster, expected.meshes[i].firstCluster);
        EXPECT_EQ(actual.meshes[i].clusterCount, expected.meshes[i].clusterCount);
        EXPECT_EQ(actual.meshes[i].firstMaterial, expected.meshes[i].firstMaterial);
        EXPECT_EQ(actual.meshes[i].materialCount, expected.meshes[i].materialCount);
        EXPECT_TRUE(nearlyEqual(actual.meshes[i].bounds, expected.meshes[i].bounds));
    }

    for (size_t i = 0; i < expected.materials.size(); ++i)
    {
        EXPECT_EQ(actual.materials[i].name, expected.materials[i].name);
    }

    for (size_t i = 0; i < expected.clusters.size(); ++i)
    {
        const Cluster& a = actual.clusters[i];
        const Cluster& e = expected.clusters[i];
        EXPECT_EQ(a.meshIndex, e.meshIndex);
        EXPECT_EQ(a.materialIndex, e.materialIndex);
        EXPECT_EQ(a.vertexOffset, e.vertexOffset);
        EXPECT_EQ(a.vertexCount, e.vertexCount);
        EXPECT_EQ(a.indexOffset, e.indexOffset);
        EXPECT_EQ(a.indexCount, e.indexCount);
        EXPECT_EQ(a.triangleCount, e.triangleCount);
        EXPECT_EQ(a.lodLevel, e.lodLevel);
        EXPECT_EQ(a.flags, e.flags);
        EXPECT_EQ(a.groupIndex, e.groupIndex);
        EXPECT_EQ(a.pageIndex, e.pageIndex);
        EXPECT_TRUE(nearlyEqual(a.bounds, e.bounds));
        EXPECT_TRUE(nearlyEqual(a.sphereCenter, e.sphereCenter));
        EXPECT_TRUE(nearlyEqual(a.sphereRadius, e.sphereRadius));
        EXPECT_TRUE(nearlyEqual(a.coneNormal, e.coneNormal));
        EXPECT_TRUE(nearlyEqual(a.coneAngle, e.coneAngle));
        EXPECT_TRUE(nearlyEqual(a.geometricError, e.geometricError));
        EXPECT_TRUE(nearlyEqual(a.surfaceArea, e.surfaceArea));
    }

    for (size_t i = 0; i < expected.vertices.size(); ++i)
    {
        EXPECT_TRUE(nearlyEqual(actual.vertices[i].position, expected.vertices[i].position));
        EXPECT_TRUE(nearlyEqual(actual.vertices[i].normal, expected.vertices[i].normal));
        EXPECT_TRUE(nearlyEqual(actual.vertices[i].texCoord.x, expected.vertices[i].texCoord.x));
        EXPECT_TRUE(nearlyEqual(actual.vertices[i].texCoord.y, expected.vertices[i].texCoord.y));
    }

    EXPECT_TRUE(indicesEqual(actual.indices, expected.indices));
}

std::vector<std::string> validateAssetBoundsContainment(const Asset& asset)
{
    std::vector<std::string> errors;

    for (size_t clusterIndex = 0; clusterIndex < asset.clusters.size(); ++clusterIndex)
    {
        const Cluster& cluster = asset.clusters[clusterIndex];
        for (uint32_t localVertex = 0; localVertex < cluster.vertexCount; ++localVertex)
        {
            const uint32_t vertexIndex = cluster.vertexOffset + localVertex;
            if (vertexIndex >= asset.vertices.size())
            {
                errors.push_back(
                    "Cluster " + std::to_string(clusterIndex) + " references vertex index " + std::to_string(vertexIndex) +
                    " outside the vertex buffer."
                );
                break;
            }

            if (!boundsContainsPoint(cluster.bounds, asset.vertices[vertexIndex].position))
            {
                errors.push_back("Cluster " + std::to_string(clusterIndex) + " bounds do not contain vertex " + std::to_string(localVertex) + ".");
                break;
            }
        }
    }

    for (size_t clusterIndex = 0; clusterIndex < asset.clusters.size(); ++clusterIndex)
    {
        if (!boundsContainsBounds(asset.bounds, asset.clusters[clusterIndex].bounds))
        {
            errors.push_back("Asset bounds do not contain cluster " + std::to_string(clusterIndex) + " bounds.");
        }
    }

    for (size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
    {
        if (!boundsContainsBounds(asset.bounds, asset.meshes[meshIndex].bounds))
        {
            errors.push_back("Asset bounds do not contain mesh " + std::to_string(meshIndex) + " bounds.");
        }
    }

    return errors;
}

} // namespace

CPU_TEST(NaniteAsset_ReadWriteRoundtripV2, TAGS("Nanite"))
{
    ASSERT_TRUE(std::filesystem::exists(kCubeObjPath)) << "Missing test mesh: " << kCubeObjPath.string();

    const WriteOptions options = debugWriteOptions();
    Asset asset = buildCubeAsset();
    asset.sourcePath = kCubeObjPath.string();
    ASSERT_FALSE(asset.clusters.empty());
    ASSERT_FALSE(asset.vertices.empty());
    ASSERT_FALSE(asset.indices.empty());

    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_cube_v2.fnanite");
    writeAsset(tempPath, asset, options);

    Asset loaded = readAsset(tempPath);
    Asset expected = normalizedExpectedAsset(asset, options);
    expected.sourcePath = kCubeObjPath.string();
    expectAssetsEqual(ctx, expected, loaded);
    EXPECT_EQ(loaded.sourcePath, kCubeObjPath.string());
    EXPECT_EQ(loaded.version, kNaniteVersion);

    std::filesystem::remove(tempPath);
}

CPU_TEST(NaniteAsset_ReadWriteRoundtripV1, TAGS("Nanite"))
{
    ASSERT_TRUE(std::filesystem::exists(kCubeObjPath)) << "Missing test mesh: " << kCubeObjPath.string();

    Asset asset = buildCubeAsset();
    asset.sourcePath = kCubeObjPath.string();

    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_cube_v1.fnanite");
    writeAssetV1(tempPath, asset);

    Asset loaded = readAsset(tempPath);
    EXPECT_EQ(loaded.version, kNaniteVersionV1);
    EXPECT_EQ(loaded.meshes.size(), asset.meshes.size());
    EXPECT_EQ(loaded.clusters.size(), asset.clusters.size());
    EXPECT_EQ(loaded.vertices.size(), asset.vertices.size());
    EXPECT_TRUE(indicesEqual(loaded.indices, asset.indices));
    EXPECT_FALSE(loaded.clusterGroups.empty());
    EXPECT_FALSE(loaded.hierarchyNodes.empty());
    EXPECT_FALSE(loaded.pages.empty());

    const std::vector<std::string> errors = validateAsset(loaded);
    EXPECT_TRUE(errors.empty()) << fmt::format("Unexpected validation errors: {}", fmt::join(errors, "; "));

    std::filesystem::remove(tempPath);
}

CPU_TEST(NaniteAsset_UnsupportedVersion, TAGS("Nanite"))
{
    const WriteOptions options = debugWriteOptions();
    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_unsupported.fnanite");
    writeAsset(tempPath, buildCubeAsset(), options);

    {
        std::fstream stream(tempPath, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(4, std::ios::beg);
        const uint32_t version = 99;
        stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
    }

    try
    {
        readAsset(tempPath);
        EXPECT(false) << "Expected unsupported version error.";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_NE(std::string(e.what()).find("Unsupported .fnanite version"), std::string::npos);
    }

    std::filesystem::remove(tempPath);
}

CPU_TEST(NaniteAsset_CorruptChunkTable, TAGS("Nanite"))
{
    const WriteOptions options = debugWriteOptions();
    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_corrupt_chunk.fnanite");
    writeAsset(tempPath, buildCubeAsset(), options);

    {
        std::fstream stream(tempPath, std::ios::in | std::ios::out | std::ios::binary);
        // DiskHeaderV2::chunkTableOffset begins after 14 uint32 fields (56 bytes).
        stream.seekp(56, std::ios::beg);
        const uint64_t badOffset = 0xFFFFFFFFFFFFFFFFull;
        stream.write(reinterpret_cast<const char*>(&badOffset), sizeof(badOffset));
    }

    try
    {
        readAsset(tempPath);
        EXPECT(false) << "Expected corrupt chunk table error.";
    }
    catch (const std::runtime_error&)
    {
        EXPECT(true);
    }

    std::filesystem::remove(tempPath);
}

CPU_TEST(NaniteAsset_InvalidFile, TAGS("Nanite"))
{
    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_invalid.fnanite");

    {
        std::ofstream stream(tempPath, std::ios::binary);
        stream.write("bad", 3);
    }
    EXPECT_THROW(readAsset(tempPath));

    const WriteOptions options = debugWriteOptions();
    writeAsset(tempPath, buildCubeAsset(), options);

    {
        std::fstream stream(tempPath, std::ios::in | std::ios::out | std::ios::binary);
        stream.put('X');
    }
    EXPECT_THROW(readAsset(tempPath));

    std::filesystem::remove(tempPath);
}

CPU_TEST(NaniteAsset_ClusterRangeValid, TAGS("Nanite"))
{
    Asset asset = buildCubeAsset();
    const std::vector<std::string> errors = validateAsset(asset);
    EXPECT_TRUE(errors.empty()) << fmt::format("Unexpected validation errors: {}", fmt::join(errors, "; "));
}

CPU_TEST(NaniteAsset_ClusterRangeInvalid, TAGS("Nanite"))
{
    Asset asset = buildCubeAsset();
    ASSERT_FALSE(asset.clusters.empty());

    {
        Asset invalid = asset;
        invalid.clusters[0].vertexCount = static_cast<uint32_t>(invalid.vertices.size()) + 1;
        const std::vector<std::string> errors = validateAsset(invalid);
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const std::string& error)
                                { return error.find("invalid vertex range") != std::string::npos; }));
    }

    {
        Asset invalid = asset;
        invalid.clusters[0].indexCount = invalid.clusters[0].triangleCount * 3 + 3;
        const std::vector<std::string> errors = validateAsset(invalid);
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const std::string& error)
                                { return error.find("invalid index range") != std::string::npos; }));
    }

    {
        Asset invalid = asset;
        invalid.clusters[0].indexCount = invalid.clusters[0].triangleCount * 3 - 3;
        const std::vector<std::string> errors = validateAsset(invalid);
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const std::string& error)
                                { return error.find("index count does not match triangle count") != std::string::npos; }));
    }

    {
        Asset invalid = asset;
        invalid.clusters[0].meshIndex = static_cast<uint32_t>(invalid.meshes.size());
        const std::vector<std::string> errors = validateAsset(invalid);
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const std::string& error)
                                { return error.find("invalid mesh") != std::string::npos; }));
    }

    if (!asset.meshes.empty())
    {
        Asset invalid = asset;
        invalid.meshes[0].clusterCount = static_cast<uint32_t>(invalid.clusters.size()) + 1;
        const std::vector<std::string> errors = validateAsset(invalid);
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const std::string& error)
                                { return error.find("invalid cluster range") != std::string::npos; }));
    }
}

CPU_TEST(NaniteAsset_CompressedReadWriteRoundtrip, TAGS("Nanite"))
{
    ASSERT_TRUE(std::filesystem::exists(kCubeObjPath)) << "Missing test mesh: " << kCubeObjPath.string();

    const WriteOptions options = compressedWriteOptions();
    Asset asset = buildCubeAsset();
    asset.sourcePath = kCubeObjPath.string();

    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_cube_compressed.fnanite");
    writeAsset(tempPath, asset, options);

    Asset loaded = readAsset(tempPath);
    EXPECT_EQ(loaded.version, kNaniteVersion);
    EXPECT_TRUE((loaded.flags & kFlagCompressedVertices) != 0);
    EXPECT_FALSE((loaded.flags & kFlagDebugUncompressed) != 0);

    const std::vector<std::string> errors = validateAsset(loaded);
    EXPECT_TRUE(errors.empty()) << fmt::format("Unexpected validation errors: {}", fmt::join(errors, "; "));

    EXPECT_EQ(loaded.indices.size(), asset.indices.size());
    EXPECT_TRUE(indicesEqual(loaded.indices, asset.indices));

    std::filesystem::remove(tempPath);
}

CPU_TEST(NaniteAsset_CompressionErrorWithinThreshold, TAGS("Nanite"))
{
    using Falcor::Nanite::maxNormalError;
    using Falcor::Nanite::maxPositionError;

    ASSERT_TRUE(std::filesystem::exists(kCubeObjPath)) << "Missing test mesh: " << kCubeObjPath.string();

    Asset asset = buildCubeAsset();
    asset.sourcePath = kCubeObjPath.string();

    const std::filesystem::path debugPath = std::filesystem::absolute("test_nanite_cube_debug_ref.fnanite");
    const std::filesystem::path compressedPath = std::filesystem::absolute("test_nanite_cube_compressed_err.fnanite");
    writeAsset(debugPath, asset, debugWriteOptions());
    writeAsset(compressedPath, asset, compressedWriteOptions());

    const Asset reference = readAsset(debugPath);
    const Asset compressed = readAsset(compressedPath);

    ASSERT_EQ(reference.vertices.size(), compressed.vertices.size());

    float maxPosError = 0.f;
    float maxNormError = 0.f;
    for (size_t i = 0; i < reference.vertices.size(); ++i)
    {
        maxPosError = std::max(maxPosError, maxPositionError(reference.vertices[i], compressed.vertices[i]));
        maxNormError = std::max(maxNormError, maxNormalError(reference.vertices[i], compressed.vertices[i]));
    }

    // 16-bit cluster-local quantization: worst case is roughly half a texel of cluster extent.
    EXPECT_LT(maxPosError, 0.05f) << "max position error " << maxPosError;
    EXPECT_LT(maxNormError, 0.02f) << "max normal error " << maxNormError;

    std::filesystem::remove(debugPath);
    std::filesystem::remove(compressedPath);
}

CPU_TEST(NaniteAsset_OctahedralNormalRoundtrip, TAGS("Nanite"))
{
    using Falcor::Nanite::length;
    using Falcor::Nanite::packOctahedralNormal;
    using Falcor::Nanite::unpackOctahedralNormal;

    const Float3 normals[] = {
        {0.f, 1.f, 0.f},
        {1.f, 0.f, 0.f},
        {0.f, 0.f, 1.f},
        {0.577f, 0.577f, 0.577f},
        {-0.3f, 0.7f, -0.2f},
    };

    for (const Float3& normal : normals)
    {
        const Float3 restored = unpackOctahedralNormal(packOctahedralNormal(normal));
        EXPECT_LT(length(normal - restored), 0.01f);
    }
}

CPU_TEST(NaniteAsset_BoundsContainment, TAGS("Nanite"))
{
    const WriteOptions options = debugWriteOptions();
    Asset asset = buildCubeAsset();
    writeAsset(std::filesystem::absolute("test_nanite_bounds.fnanite"), asset, options);
    asset = readAsset(std::filesystem::absolute("test_nanite_bounds.fnanite"));
    std::filesystem::remove(std::filesystem::absolute("test_nanite_bounds.fnanite"));

    const std::vector<std::string> validationErrors = validateAsset(asset);
    EXPECT_TRUE(validationErrors.empty()) << fmt::format("validateAsset errors: {}", fmt::join(validationErrors, "; "));

    const std::vector<std::string> containmentErrors = validateAssetBoundsContainment(asset);
    EXPECT_TRUE(containmentErrors.empty()) << fmt::format("Bounds containment errors: {}", fmt::join(containmentErrors, "; "));

    for (const Cluster& cluster : asset.clusters)
    {
        EXPECT_GT(cluster.vertexCount, 0u);
        EXPECT_GT(cluster.triangleCount, 0u);
        EXPECT_FALSE(isEmpty(cluster.bounds));
    }

    EXPECT_FALSE(isEmpty(asset.bounds));
}

CPU_TEST(NaniteAsset_RuntimeTableValidation, TAGS("Nanite"))
{
    Asset asset = buildCubeAsset();
    buildMetadataTables(asset, 32);
    ASSERT_FALSE(asset.clusterGroups.empty());
    ASSERT_FALSE(asset.hierarchyNodes.empty());
    ASSERT_FALSE(asset.pages.empty());
    validateRuntimeTables(asset);

    {
        Asset invalid = asset;
        invalid.clusterGroups[0].clusterCount = static_cast<uint32_t>(invalid.clusters.size()) + 1;
        try
        {
            validateRuntimeTables(invalid);
            EXPECT(false) << "Expected cluster group range validation to throw.";
        }
        catch (const std::runtime_error&)
        {
            EXPECT(true);
        }
    }

    {
        Asset invalid = asset;
        invalid.hierarchyNodes[0].clusterCount = static_cast<uint32_t>(invalid.clusters.size()) + 1;
        try
        {
            validateRuntimeTables(invalid);
            EXPECT(false) << "Expected hierarchy cluster range validation to throw.";
        }
        catch (const std::runtime_error&)
        {
            EXPECT(true);
        }
    }

    {
        Asset invalid = asset;
        invalid.pages[0].clusterCount = static_cast<uint32_t>(invalid.clusters.size()) + 1;
        try
        {
            validateRuntimeTables(invalid);
            EXPECT(false) << "Expected page cluster range validation to throw.";
        }
        catch (const std::runtime_error&)
        {
            EXPECT(true);
        }
    }
}

CPU_TEST(NaniteAsset_GpuMemoryStats, TAGS("Nanite"))
{
    Asset asset = buildCubeAsset();
    const GpuMemoryStats stats = computeGpuMemoryStats(asset);

    EXPECT_GT(stats.clusterCount, 0u);
    EXPECT_GT(stats.totalGpuBytes, 0u);
    EXPECT_EQ(stats.clusterBytes, stats.clusterCount * sizeof(Falcor::Nanite::NaniteGpuCluster));
    EXPECT_EQ(stats.materialBytes, asset.materials.size() * sizeof(Falcor::Nanite::NaniteGpuMaterial));
    EXPECT_EQ(
        stats.totalGpuBytes,
        stats.clusterBytes + stats.hierarchyBytes + stats.pageBytes + stats.vertexBytes + stats.indexBytes +
            stats.materialBytes + stats.residencyBytes
    );
}

CPU_TEST(NaniteAsset_SourceGeometryRoundtrip, TAGS("Nanite"))
{
    ASSERT_TRUE(std::filesystem::exists(kCubeObjPath)) << "Missing test mesh: " << kCubeObjPath.string();

    InputScene scene = loadObjScene(kCubeObjPath);
    BuildOptions options;
    Asset asset = buildNaniteAsset(scene, options);
    embedSourceGeometry(asset, scene);
    ASSERT_TRUE(hasSourceGeometry(asset));

    const WriteOptions writeOptions = debugWriteOptions();
    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_cube_source.fnanite");
    writeAsset(tempPath, asset, writeOptions);

    Asset loaded = readAsset(tempPath);
    EXPECT_TRUE(hasSourceGeometry(loaded));
    EXPECT_EQ(loaded.sourceMeshes.size(), asset.sourceMeshes.size());
    ASSERT_FALSE(loaded.sourceMeshes.empty());
    EXPECT_EQ(loaded.sourceMeshes[0].vertices.size(), asset.sourceMeshes[0].vertices.size());
    EXPECT_TRUE(indicesEqual(loaded.sourceMeshes[0].indices, asset.sourceMeshes[0].indices));

    const std::vector<std::string> sourceErrors = validateSourceGeometry(loaded);
    EXPECT_TRUE(sourceErrors.empty()) << fmt::format("Unexpected source validation errors: {}", fmt::join(sourceErrors, "; "));

    std::filesystem::remove(tempPath);
}

CPU_TEST(NaniteAsset_RebuildFromSource, TAGS("Nanite"))
{
    ASSERT_TRUE(std::filesystem::exists(kCubeObjPath)) << "Missing test mesh: " << kCubeObjPath.string();

    InputScene scene = loadObjScene(kCubeObjPath);
    BuildOptions coarseOptions;
    coarseOptions.clusterTriangleTarget = 64;
    Asset coarseAsset = buildNaniteAsset(scene, coarseOptions);
    embedSourceGeometry(coarseAsset, scene);

    const WriteOptions writeOptions = debugWriteOptions();
    const std::filesystem::path tempPath = std::filesystem::absolute("test_nanite_cube_rebuild_source.fnanite");
    writeAsset(tempPath, coarseAsset, writeOptions);

    InputScene rebuiltScene = inputSceneFromSource(readAsset(tempPath));
    BuildOptions fineOptions;
    fineOptions.clusterTriangleTarget = 8;
    Asset rebuiltAsset = buildNaniteAsset(rebuiltScene, fineOptions);
    embedSourceGeometry(rebuiltAsset, rebuiltScene);

    EXPECT_GE(rebuiltAsset.clusters.size(), coarseAsset.clusters.size());
    EXPECT_TRUE(hasSourceGeometry(rebuiltAsset));
    EXPECT_TRUE(validateAsset(rebuiltAsset).empty());

    std::filesystem::remove(tempPath);
}

} // namespace Falcor
