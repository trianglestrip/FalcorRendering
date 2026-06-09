#include "NaniteBuild.h"

#include <taskflow.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace FalcorRendering::NaniteTool
{
namespace
{
struct ClusterWork
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<uint32_t, uint32_t> remap;
    Bounds bounds = emptyBounds();
};

struct MeshBuildResult
{
    Mesh mesh;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Cluster> clusters;
};

uint32_t countNewVertices(const ClusterWork& work, const uint32_t sourceIndices[3])
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < 3; ++i)
    {
        if (!work.remap.contains(sourceIndices[i])) ++count;
    }
    return count;
}

Float3 triangleNormal(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, size_t triangleIndex)
{
    const Vertex& a = vertices[indices[triangleIndex * 3 + 0]];
    const Vertex& b = vertices[indices[triangleIndex * 3 + 1]];
    const Vertex& c = vertices[indices[triangleIndex * 3 + 2]];
    return normalize(cross(b.position - a.position, c.position - a.position));
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

    work.vertices.clear();
    work.indices.clear();
    work.remap.clear();
    work.bounds = emptyBounds();
}

void appendTriangle(ClusterWork& work, const InputMesh& mesh, const uint32_t sourceIndices[3])
{
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t sourceIndex = sourceIndices[i];
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

    ClusterWork work;
    const uint32_t triangleCount = static_cast<uint32_t>(inputMesh.indices.size() / 3);
    for (uint32_t tri = 0; tri < triangleCount; ++tri)
    {
        const uint32_t sourceIndices[3] = {
            inputMesh.indices[tri * 3 + 0],
            inputMesh.indices[tri * 3 + 1],
            inputMesh.indices[tri * 3 + 2],
        };

        for (uint32_t sourceIndex : sourceIndices)
        {
            if (sourceIndex >= inputMesh.vertices.size()) throw std::runtime_error("Input mesh index is out of range.");
        }

        const uint32_t currentTriangles = static_cast<uint32_t>(work.indices.size() / 3);
        const uint32_t newVertices = countNewVertices(work, sourceIndices);
        const bool fullByTriangles = currentTriangles >= options.clusterTriangleTarget;
        const bool fullByVertices = !work.indices.empty() && work.vertices.size() + newVertices > options.maxClusterVertices;
        if (fullByTriangles || fullByVertices)
        {
            finalizeCluster(result, meshIndex, materialIndex, work);
        }

        appendTriangle(work, inputMesh, sourceIndices);
    }

    finalizeCluster(result, meshIndex, materialIndex, work);
    result.mesh.clusterCount = static_cast<uint32_t>(result.clusters.size());
    return result;
}
}

Asset buildNaniteAsset(const InputScene& scene, const BuildOptions& options)
{
    if (options.clusterTriangleTarget == 0) throw std::runtime_error("clusterTriangleTarget must be greater than zero.");
    if (options.maxClusterVertices < 3) throw std::runtime_error("maxClusterVertices must be at least 3.");

    Asset asset;
    asset.sourcePath = scene.sourcePath.string();
    asset.bounds = emptyBounds();

    asset.materials.reserve(scene.materialNames.size());
    for (const std::string& name : scene.materialNames) asset.materials.push_back({ name });
    if (asset.materials.empty()) asset.materials.push_back({ "default" });

    std::vector<size_t> meshIndices;
    meshIndices.reserve(scene.meshes.size());
    for (size_t i = 0; i < scene.meshes.size(); ++i)
    {
        if (!scene.meshes[i].indices.empty()) meshIndices.push_back(i);
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
            const InputMesh& inputMesh = scene.meshes[inputMeshIndex];
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

        include(asset.bounds, result.mesh.bounds);
        asset.meshes.push_back(result.mesh);
        asset.vertices.insert(asset.vertices.end(), result.vertices.begin(), result.vertices.end());
        asset.indices.insert(asset.indices.end(), result.indices.begin(), result.indices.end());
        asset.clusters.insert(asset.clusters.end(), result.clusters.begin(), result.clusters.end());
    }

    if (asset.meshes.empty()) throw std::runtime_error("No meshes were converted into Nanite clusters.");
    return asset;
}
}
