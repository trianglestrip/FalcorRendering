#include "NaniteBuild.h"

#include <taskflow.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FalcorRendering::NaniteTool
{
namespace
{
struct ClusterWork
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<uint32_t, uint32_t> remap;
    std::vector<uint32_t> sourceTriangleIndices;
    Bounds bounds = emptyBounds();
    float surfaceArea = 0.f;
};

struct MeshBuildResult
{
    Mesh mesh;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Cluster> clusters;
    std::vector<ClusterDebugInfo> clusterDebugInfo;
    PartitionStats partitionStats;
    uint64_t sourceTriangleCount = 0;
    uint64_t degenerateTriangleCount = 0;
};

struct SourceTriangle
{
    uint32_t sourceTriangleIndex = 0;
    std::array<uint32_t, 3> sourceIndices{};
    Float3 centroid;
    Float3 normal;
    float surfaceArea = 0.f;
    uint32_t mortonCode = 0;
};

constexpr float kDegenerateAreaEpsilon = 1e-20f;

struct PositionEdgeKey
{
    std::array<uint32_t, 6> data{};

    bool operator==(const PositionEdgeKey& other) const { return data == other.data; }
};

struct PositionEdgeKeyHash
{
    size_t operator()(const PositionEdgeKey& key) const
    {
        size_t hash = key.data[0];
        for (size_t i = 1; i < key.data.size(); ++i)
        {
            hash ^= key.data[i] + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

PositionEdgeKey makePositionEdgeKey(const Float3& a, const Float3& b)
{
    auto encode = [](const Float3& position)
    {
        return std::array<uint32_t, 3>{
            std::bit_cast<uint32_t>(position.x),
            std::bit_cast<uint32_t>(position.y),
            std::bit_cast<uint32_t>(position.z),
        };
    };

    std::array<uint32_t, 3> left = encode(a);
    std::array<uint32_t, 3> right = encode(b);
    if (left > right) std::swap(left, right);
    return PositionEdgeKey{ { left[0], left[1], left[2], right[0], right[1], right[2] } };
}

PositionEdgeKey triangleEdgeKey(const InputMesh& mesh, const SourceTriangle& triangle, size_t edgeIndex)
{
    const Float3& a = mesh.vertices[triangle.sourceIndices[edgeIndex]].position;
    const Float3& b = mesh.vertices[triangle.sourceIndices[(edgeIndex + 1) % 3]].position;
    return makePositionEdgeKey(a, b);
}

std::vector<std::vector<uint32_t>> buildTriangleAdjacency(
    const std::vector<SourceTriangle>& triangles,
    const InputMesh& mesh)
{
    std::unordered_map<PositionEdgeKey, std::vector<uint32_t>, PositionEdgeKeyHash> edgeToTriangles;
    edgeToTriangles.reserve(triangles.size() * 3);

    for (uint32_t triIndex = 0; triIndex < static_cast<uint32_t>(triangles.size()); ++triIndex)
    {
        const SourceTriangle& triangle = triangles[triIndex];
        for (size_t edgeIndex = 0; edgeIndex < 3; ++edgeIndex)
        {
            edgeToTriangles[triangleEdgeKey(mesh, triangle, edgeIndex)].push_back(triIndex);
        }
    }

    std::vector<std::vector<uint32_t>> adjacency(triangles.size());
    for (const auto& entry : edgeToTriangles)
    {
        const std::vector<uint32_t>& triList = entry.second;
        for (size_t i = 0; i < triList.size(); ++i)
        {
            for (size_t j = i + 1; j < triList.size(); ++j)
            {
                adjacency[triList[i]].push_back(triList[j]);
                adjacency[triList[j]].push_back(triList[i]);
            }
        }
    }
    return adjacency;
}

struct ClusterGrowState
{
    std::unordered_map<uint32_t, uint32_t> remap;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;

    bool canAdd(const SourceTriangle& triangle, uint32_t maxVertices, uint32_t maxTriangles) const
    {
        if (triangleCount >= maxTriangles) return false;
        uint32_t newVertices = 0;
        for (uint32_t sourceIndex : triangle.sourceIndices)
        {
            if (!remap.contains(sourceIndex)) ++newVertices;
        }
        return vertexCount + newVertices <= maxVertices;
    }

    void add(const SourceTriangle& triangle)
    {
        for (uint32_t sourceIndex : triangle.sourceIndices)
        {
            if (!remap.contains(sourceIndex))
            {
                remap.emplace(sourceIndex, vertexCount++);
            }
        }
        ++triangleCount;
    }
};

bool compareTriangleOrder(uint32_t lhs, uint32_t rhs, const std::vector<SourceTriangle>& triangles)
{
    const SourceTriangle& a = triangles[lhs];
    const SourceTriangle& b = triangles[rhs];
    if (a.mortonCode != b.mortonCode) return a.mortonCode < b.mortonCode;
    return a.sourceTriangleIndex < b.sourceTriangleIndex;
}

void removeFrontierTriangle(std::vector<uint32_t>& frontier, std::vector<uint8_t>& inFrontier, uint32_t triIndex)
{
    inFrontier[triIndex] = 0;
    auto it = std::find(frontier.begin(), frontier.end(), triIndex);
    if (it != frontier.end()) frontier.erase(it);
}

void pushFrontierTriangle(
    std::vector<uint32_t>& frontier,
    std::vector<uint8_t>& inFrontier,
    uint32_t triIndex,
    const std::vector<uint8_t>& assigned)
{
    if (assigned[triIndex] || inFrontier[triIndex]) return;
    inFrontier[triIndex] = 1;
    frontier.push_back(triIndex);
}

void computePartitionStats(
    MeshBuildResult& result,
    const std::vector<SourceTriangle>& triangles,
    const std::vector<std::vector<uint32_t>>& adjacency,
    const InputMesh& mesh)
{
    std::unordered_map<uint32_t, uint32_t> sourceTriangleToLocal;
    sourceTriangleToLocal.reserve(triangles.size());
    for (uint32_t localIndex = 0; localIndex < static_cast<uint32_t>(triangles.size()); ++localIndex)
    {
        sourceTriangleToLocal.emplace(triangles[localIndex].sourceTriangleIndex, localIndex);
    }

    std::unordered_map<uint32_t, uint32_t> sourceTriangleToCluster;
    sourceTriangleToCluster.reserve(triangles.size());
    for (size_t clusterIndex = 0; clusterIndex < result.clusterDebugInfo.size(); ++clusterIndex)
    {
        for (uint32_t sourceTriangleIndex : result.clusterDebugInfo[clusterIndex].sourceTriangleIndices)
        {
            sourceTriangleToCluster.emplace(sourceTriangleIndex, static_cast<uint32_t>(clusterIndex));
        }
    }

    std::unordered_map<uint32_t, uint32_t> sourceVertexClusterRefs;
    std::unordered_set<PositionEdgeKey, PositionEdgeKeyHash> interClusterEdgeKeys;

    for (size_t clusterIndex = 0; clusterIndex < result.clusters.size(); ++clusterIndex)
    {
        const Cluster& cluster = result.clusters[clusterIndex];
        result.partitionStats.totalLocalVertices += cluster.vertexCount;

        std::unordered_map<uint32_t, bool> boundaryVertices;
        for (uint32_t tri = 0; tri < cluster.triangleCount; ++tri)
        {
            const uint32_t sourceTriangleIndex = result.clusterDebugInfo[clusterIndex].sourceTriangleIndices[tri];
            const uint32_t localTriIndex = sourceTriangleToLocal.at(sourceTriangleIndex);
            const SourceTriangle& sourceTriangle = triangles[localTriIndex];
            const std::array<uint32_t, 3> localIndices = {
                result.indices[cluster.indexOffset + tri * 3 + 0],
                result.indices[cluster.indexOffset + tri * 3 + 1],
                result.indices[cluster.indexOffset + tri * 3 + 2],
            };

            for (size_t edgeIndex = 0; edgeIndex < 3; ++edgeIndex)
            {
                const PositionEdgeKey edgeKey = triangleEdgeKey(mesh, sourceTriangle, edgeIndex);

                bool sharesClusterEdge = false;
                for (uint32_t neighborLocal : adjacency[localTriIndex])
                {
                    const uint32_t neighborSource = triangles[neighborLocal].sourceTriangleIndex;
                    const auto neighborClusterIt = sourceTriangleToCluster.find(neighborSource);
                    if (neighborClusterIt != sourceTriangleToCluster.end()
                        && neighborClusterIt->second == static_cast<uint32_t>(clusterIndex))
                    {
                        sharesClusterEdge = true;
                        break;
                    }
                }

                if (!sharesClusterEdge)
                {
                    boundaryVertices[localIndices[edgeIndex]] = true;
                    boundaryVertices[localIndices[(edgeIndex + 1) % 3]] = true;

                    for (uint32_t neighborLocal : adjacency[localTriIndex])
                    {
                        const uint32_t neighborSource = triangles[neighborLocal].sourceTriangleIndex;
                        const auto neighborClusterIt = sourceTriangleToCluster.find(neighborSource);
                        if (neighborClusterIt != sourceTriangleToCluster.end()
                            && neighborClusterIt->second != static_cast<uint32_t>(clusterIndex))
                        {
                            interClusterEdgeKeys.insert(edgeKey);
                        }
                    }
                }
            }

            for (uint32_t sourceIndex : sourceTriangle.sourceIndices)
            {
                ++sourceVertexClusterRefs[sourceIndex];
            }
        }

        result.clusterDebugInfo[clusterIndex].boundaryVertexCount = static_cast<uint32_t>(boundaryVertices.size());
    }

    result.partitionStats.interClusterEdges = interClusterEdgeKeys.size();
    for (const auto& entry : sourceVertexClusterRefs)
    {
        if (entry.second > 1) ++result.partitionStats.boundarySourceVertices;
    }
}

Float3 triangleNormal(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, size_t triangleIndex)
{
    const Vertex& a = vertices[indices[triangleIndex * 3 + 0]];
    const Vertex& b = vertices[indices[triangleIndex * 3 + 1]];
    const Vertex& c = vertices[indices[triangleIndex * 3 + 2]];
    return normalize(cross(b.position - a.position, c.position - a.position));
}

uint32_t expandMortonBits(uint32_t value)
{
    value = (value * 0x00010001u) & 0xFF0000FFu;
    value = (value * 0x00000101u) & 0x0F00F00Fu;
    value = (value * 0x00000011u) & 0xC30C30C3u;
    value = (value * 0x00000005u) & 0x49249249u;
    return value;
}

uint32_t quantizeMortonAxis(float value, float minValue, float maxValue)
{
    const float extent = maxValue - minValue;
    float normalized = extent > 0.f ? (value - minValue) / extent : 0.5f;
    if (!std::isfinite(normalized)) normalized = 0.5f;
    normalized = std::clamp(normalized, 0.f, 1.f);
    return static_cast<uint32_t>(normalized * 1023.f + 0.5f);
}

uint32_t calculateMortonCode(Float3 centroid, const Bounds& bounds)
{
    const uint32_t x = quantizeMortonAxis(centroid.x, bounds.min.x, bounds.max.x);
    const uint32_t y = quantizeMortonAxis(centroid.y, bounds.min.y, bounds.max.y);
    const uint32_t z = quantizeMortonAxis(centroid.z, bounds.min.z, bounds.max.z);
    return (expandMortonBits(x) << 2) | (expandMortonBits(y) << 1) | expandMortonBits(z);
}

SourceTriangle makeSourceTriangle(const InputMesh& inputMesh, uint32_t triangleIndex, const Bounds& bounds)
{
    SourceTriangle triangle;
    triangle.sourceTriangleIndex = triangleIndex;
    triangle.sourceIndices = {
        inputMesh.indices[triangleIndex * 3 + 0],
        inputMesh.indices[triangleIndex * 3 + 1],
        inputMesh.indices[triangleIndex * 3 + 2],
    };

    for (uint32_t sourceIndex : triangle.sourceIndices)
    {
        if (sourceIndex >= inputMesh.vertices.size()) throw std::runtime_error("Input mesh index is out of range.");
    }

    const Float3 p0 = inputMesh.vertices[triangle.sourceIndices[0]].position;
    const Float3 p1 = inputMesh.vertices[triangle.sourceIndices[1]].position;
    const Float3 p2 = inputMesh.vertices[triangle.sourceIndices[2]].position;
    const Float3 normalArea = cross(p1 - p0, p2 - p0);
    const float doubleArea = length(normalArea);

    triangle.surfaceArea = doubleArea * 0.5f;
    triangle.normal = normalize(normalArea);
    triangle.centroid = (p0 + p1 + p2) / 3.f;
    triangle.mortonCode = calculateMortonCode(triangle.centroid, bounds);
    return triangle;
}

bool isDegenerate(const SourceTriangle& triangle)
{
    return !std::isfinite(triangle.surfaceArea) || triangle.surfaceArea <= kDegenerateAreaEpsilon;
}

void finalizeCluster(MeshBuildResult& result, uint32_t meshIndex, uint32_t materialIndex, ClusterWork& work)
{
    if (work.indices.empty()) return;

    Cluster cluster;
    cluster.meshIndex = meshIndex;
    cluster.materialIndex = materialIndex;
    cluster.vertexOffset = static_cast<uint32_t>(result.vertices.size());
    cluster.vertexCount = static_cast<uint32_t>(work.vertices.size());
    cluster.indexOffset = static_cast<uint32_t>(result.indices.size());
    cluster.indexCount = static_cast<uint32_t>(work.indices.size());
    cluster.triangleCount = cluster.indexCount / 3;
    cluster.lodLevel = 0;
    cluster.bounds = work.bounds;
    cluster.sphereCenter = center(work.bounds);
    cluster.sphereRadius = radius(work.bounds);
    cluster.geometricError = cluster.sphereRadius * 0.001f;
    cluster.surfaceArea = work.surfaceArea;

    Float3 normalSum{};
    for (uint32_t tri = 0; tri < cluster.triangleCount; ++tri)
    {
        normalSum = normalSum + triangleNormal(work.vertices, work.indices, tri);
    }
    cluster.coneNormal = normalize(normalSum);

    float minDot = 1.f;
    for (uint32_t tri = 0; tri < cluster.triangleCount; ++tri)
    {
        minDot = std::min(minDot, dot(cluster.coneNormal, triangleNormal(work.vertices, work.indices, tri)));
    }
    minDot = std::clamp(minDot, -1.f, 1.f);
    cluster.coneAngle = std::acos(minDot);

    result.vertices.insert(result.vertices.end(), work.vertices.begin(), work.vertices.end());
    result.indices.insert(result.indices.end(), work.indices.begin(), work.indices.end());
    result.clusters.push_back(cluster);

    ClusterDebugInfo debugInfo;
    debugInfo.sourceMaterialIndex = materialIndex;
    debugInfo.sourceTriangleIndices = work.sourceTriangleIndices;
    result.clusterDebugInfo.push_back(std::move(debugInfo));

    work.vertices.clear();
    work.indices.clear();
    work.remap.clear();
    work.sourceTriangleIndices.clear();
    work.bounds = emptyBounds();
    work.surfaceArea = 0.f;
}

void appendTriangle(ClusterWork& work, const InputMesh& mesh, const SourceTriangle& triangle)
{
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t sourceIndex = triangle.sourceIndices[i];
        auto it = work.remap.find(sourceIndex);
        if (it == work.remap.end())
        {
            const uint32_t localIndex = static_cast<uint32_t>(work.vertices.size());
            work.remap.emplace(sourceIndex, localIndex);
            work.vertices.push_back(mesh.vertices[sourceIndex]);
            include(work.bounds, mesh.vertices[sourceIndex].position);
            work.indices.push_back(localIndex);
        }
        else
        {
            work.indices.push_back(it->second);
        }
    }

    work.sourceTriangleIndices.push_back(triangle.sourceTriangleIndex);
    work.surfaceArea += triangle.surfaceArea;
}

MeshBuildResult buildMesh(const InputMesh& inputMesh, uint32_t meshIndex, uint32_t materialIndex, const BuildOptions& options)
{
    if (inputMesh.indices.empty()) return {};
    if (inputMesh.indices.size() % 3 != 0) throw std::runtime_error("Input mesh has a non-triangle index count.");

    MeshBuildResult result;
    result.mesh.name = inputMesh.name;
    result.mesh.firstCluster = 0;
    result.mesh.firstMaterial = materialIndex;
    result.mesh.materialCount = 1;
    result.mesh.bounds = inputMesh.bounds;

    std::vector<SourceTriangle> triangles;
    triangles.reserve(inputMesh.indices.size() / 3);

    const uint32_t originalTriangleCount = static_cast<uint32_t>(inputMesh.indices.size() / 3);
    result.sourceTriangleCount = originalTriangleCount;
    for (uint32_t tri = 0; tri < originalTriangleCount; ++tri)
    {
        SourceTriangle triangle = makeSourceTriangle(inputMesh, tri, inputMesh.bounds);
        if (isDegenerate(triangle))
        {
            ++result.degenerateTriangleCount;
            continue;
        }

        triangles.push_back(triangle);
    }

    std::stable_sort(triangles.begin(), triangles.end(), [](const SourceTriangle& a, const SourceTriangle& b)
    {
        if (a.mortonCode != b.mortonCode) return a.mortonCode < b.mortonCode;
        return a.sourceTriangleIndex < b.sourceTriangleIndex;
    });

    const std::vector<std::vector<uint32_t>> adjacency = buildTriangleAdjacency(triangles, inputMesh);
    std::vector<uint32_t> seedOrder(triangles.size());
    std::iota(seedOrder.begin(), seedOrder.end(), 0);
    std::stable_sort(seedOrder.begin(), seedOrder.end(), [&](uint32_t lhs, uint32_t rhs)
    {
        return compareTriangleOrder(lhs, rhs, triangles);
    });

    std::vector<uint8_t> assigned(triangles.size(), 0);
    std::vector<uint8_t> inFrontier(triangles.size(), 0);
    ClusterWork work;

    for (uint32_t seedLocalIndex : seedOrder)
    {
        if (assigned[seedLocalIndex]) continue;

        ClusterGrowState growState;
        growState.add(triangles[seedLocalIndex]);
        assigned[seedLocalIndex] = 1;

        std::vector<uint32_t> clusterTriangles;
        clusterTriangles.push_back(seedLocalIndex);

        std::vector<uint32_t> frontier;
        for (uint32_t neighbor : adjacency[seedLocalIndex])
        {
            pushFrontierTriangle(frontier, inFrontier, neighbor, assigned);
        }

        while (growState.triangleCount < options.clusterTriangleTarget)
        {
            uint32_t selectedLocalIndex = std::numeric_limits<uint32_t>::max();

            if (!frontier.empty())
            {
                std::stable_sort(frontier.begin(), frontier.end(), [&](uint32_t lhs, uint32_t rhs)
                {
                    return compareTriangleOrder(lhs, rhs, triangles);
                });

                for (uint32_t candidate : frontier)
                {
                    if (growState.canAdd(triangles[candidate], options.maxClusterVertices, options.clusterTriangleTarget))
                    {
                        selectedLocalIndex = candidate;
                        break;
                    }
                }
            }

            if (selectedLocalIndex == std::numeric_limits<uint32_t>::max())
            {
                for (uint32_t candidate : seedOrder)
                {
                    if (assigned[candidate]) continue;
                    if (growState.canAdd(triangles[candidate], options.maxClusterVertices, options.clusterTriangleTarget))
                    {
                        selectedLocalIndex = candidate;
                        break;
                    }
                }
            }

            if (selectedLocalIndex == std::numeric_limits<uint32_t>::max()) break;

            growState.add(triangles[selectedLocalIndex]);
            assigned[selectedLocalIndex] = 1;
            clusterTriangles.push_back(selectedLocalIndex);
            removeFrontierTriangle(frontier, inFrontier, selectedLocalIndex);

            for (uint32_t neighbor : adjacency[selectedLocalIndex])
            {
                pushFrontierTriangle(frontier, inFrontier, neighbor, assigned);
            }
        }

        for (uint32_t localIndex : clusterTriangles)
        {
            appendTriangle(work, inputMesh, triangles[localIndex]);
        }
        finalizeCluster(result, meshIndex, materialIndex, work);
    }

    computePartitionStats(result, triangles, adjacency, inputMesh);
    result.mesh.clusterCount = static_cast<uint32_t>(result.clusters.size());
    return result;
}
}

Asset buildNaniteAsset(const InputScene& scene, const BuildOptions& options)
{
    if (options.clusterTriangleTarget == 0) throw std::runtime_error("clusterTriangleTarget must be greater than zero.");
    if (options.maxClusterVertices < 3) throw std::runtime_error("maxClusterVertices must be at least 3.");

    InputScene buildScene = scene;
    if (options.dedupVerts)
    {
        for (InputMesh& mesh : buildScene.meshes) deduplicateMeshVertices(mesh);
    }

    Asset asset;
    asset.sourcePath = buildScene.sourcePath.string();
    asset.bounds = emptyBounds();

    asset.materials.reserve(buildScene.materialNames.size());
    for (const std::string& name : buildScene.materialNames) asset.materials.push_back({ name });
    if (asset.materials.empty()) asset.materials.push_back({ "default" });

    std::vector<size_t> meshIndices;
    meshIndices.reserve(buildScene.meshes.size());
    for (size_t i = 0; i < buildScene.meshes.size(); ++i)
    {
        if (!buildScene.meshes[i].indices.empty()) meshIndices.push_back(i);
    }
    if (meshIndices.empty()) throw std::runtime_error("No meshes were converted into Nanite clusters.");

    std::vector<MeshBuildResult> results(meshIndices.size());

    const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t requestedWorkers = options.workerCount == 0 ? hardwareThreads : options.workerCount;
    const uint32_t workerCount = std::max(1u, requestedWorkers);

    tf::Taskflow taskflow("NaniteBuildAsset");
    tf::Task start = taskflow.emplace([]() {}).name("start");
    tf::Task finish = taskflow.emplace([]() {}).name("finish");

    for (size_t taskIndex = 0; taskIndex < meshIndices.size(); ++taskIndex)
    {
        const size_t inputMeshIndex = meshIndices[taskIndex];
        tf::Task task = taskflow.emplace([&, taskIndex, inputMeshIndex]()
        {
            const InputMesh& inputMesh = buildScene.meshes[inputMeshIndex];
            const uint32_t meshIndex = static_cast<uint32_t>(taskIndex);
            const uint32_t materialIndex = std::min(inputMesh.materialIndex, static_cast<uint32_t>(asset.materials.size() - 1));
            results[taskIndex] = buildMesh(inputMesh, meshIndex, materialIndex, options);
        }).name("build mesh");
        start.precede(task);
        task.precede(finish);
    }

    tf::Executor executor(workerCount);
    executor.run(taskflow).get();

    asset.meshes.reserve(results.size());
    for (MeshBuildResult& result : results)
    {
        asset.sourceTriangleCount += result.sourceTriangleCount;
        asset.degenerateTriangleCount += result.degenerateTriangleCount;
        if (result.clusters.empty()) continue;

        const uint32_t meshIndex = static_cast<uint32_t>(asset.meshes.size());
        const uint32_t clusterBase = static_cast<uint32_t>(asset.clusters.size());
        const uint32_t vertexBase = static_cast<uint32_t>(asset.vertices.size());
        const uint32_t indexBase = static_cast<uint32_t>(asset.indices.size());

        result.mesh.firstCluster = clusterBase;
        result.mesh.clusterCount = static_cast<uint32_t>(result.clusters.size());

        for (Cluster& cluster : result.clusters)
        {
            cluster.meshIndex = meshIndex;
            cluster.vertexOffset += vertexBase;
            cluster.indexOffset += indexBase;
        }
        for (ClusterDebugInfo& debugInfo : result.clusterDebugInfo)
        {
            debugInfo.sourceMeshIndex = meshIndex;
        }

        include(asset.bounds, result.mesh.bounds);
        asset.partitionStats.totalLocalVertices += result.partitionStats.totalLocalVertices;
        asset.partitionStats.boundarySourceVertices += result.partitionStats.boundarySourceVertices;
        asset.partitionStats.interClusterEdges += result.partitionStats.interClusterEdges;
        asset.meshes.push_back(result.mesh);
        asset.vertices.insert(asset.vertices.end(), result.vertices.begin(), result.vertices.end());
        asset.indices.insert(asset.indices.end(), result.indices.begin(), result.indices.end());
        asset.clusters.insert(asset.clusters.end(), result.clusters.begin(), result.clusters.end());
        asset.clusterDebugInfo.insert(asset.clusterDebugInfo.end(), result.clusterDebugInfo.begin(), result.clusterDebugInfo.end());
    }

    if (asset.meshes.empty()) throw std::runtime_error("No meshes were converted into Nanite clusters.");
    return asset;
}
}
