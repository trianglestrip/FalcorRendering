#include "NaniteBuild.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
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

void finalizeCluster(Asset& asset, uint32_t meshIndex, uint32_t materialIndex, ClusterWork& work)
{
    if (work.indices.empty()) return;

    Cluster cluster;
    cluster.meshIndex = meshIndex;
    cluster.materialIndex = materialIndex;
    cluster.vertexOffset = static_cast<uint32_t>(asset.vertices.size());
    cluster.vertexCount = static_cast<uint32_t>(work.vertices.size());
    cluster.indexOffset = static_cast<uint32_t>(asset.indices.size());
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

    asset.vertices.insert(asset.vertices.end(), work.vertices.begin(), work.vertices.end());
    asset.indices.insert(asset.indices.end(), work.indices.begin(), work.indices.end());
    asset.clusters.push_back(cluster);

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

    asset.meshes.reserve(scene.meshes.size());
    for (const InputMesh& inputMesh : scene.meshes)
    {
        if (inputMesh.indices.empty()) continue;
        if (inputMesh.indices.size() % 3 != 0) throw std::runtime_error("Input mesh has a non-triangle index count.");

        const uint32_t meshIndex = static_cast<uint32_t>(asset.meshes.size());
        Mesh mesh;
        mesh.name = inputMesh.name;
        mesh.firstCluster = static_cast<uint32_t>(asset.clusters.size());
        mesh.firstMaterial = std::min(inputMesh.materialIndex, static_cast<uint32_t>(asset.materials.size() - 1));
        mesh.materialCount = 1;
        mesh.bounds = inputMesh.bounds;

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
                finalizeCluster(asset, meshIndex, mesh.firstMaterial, work);
            }

            appendTriangle(work, inputMesh, sourceIndices);
        }

        finalizeCluster(asset, meshIndex, mesh.firstMaterial, work);
        mesh.clusterCount = static_cast<uint32_t>(asset.clusters.size()) - mesh.firstCluster;
        include(asset.bounds, mesh.bounds);
        asset.meshes.push_back(mesh);
    }

    if (asset.meshes.empty()) throw std::runtime_error("No meshes were converted into Nanite clusters.");
    return asset;
}
}
