#include "NaniteGpuTypes.h"

#include <stdexcept>

namespace Falcor::Nanite
{
GpuMemoryStats computeGpuMemoryStats(const Asset& asset)
{
    GpuMemoryStats stats{};
    stats.clusterCount = static_cast<uint32_t>(asset.clusters.size());
    stats.pageCount = static_cast<uint32_t>(asset.pages.size());
    stats.materialCount = static_cast<uint32_t>(asset.materials.size());
    stats.clusterBytes = asset.clusters.size() * sizeof(NaniteGpuCluster);
    stats.hierarchyBytes = asset.hierarchyNodes.size() * sizeof(NaniteGpuHierarchyNode);
    stats.pageBytes = asset.pages.size() * sizeof(NaniteGpuPageDesc);
    stats.vertexBytes = asset.vertices.size() * sizeof(NaniteGpuVertex);
    stats.indexBytes = asset.indices.size() * sizeof(uint32_t);
    stats.materialBytes = asset.materials.size() * sizeof(NaniteGpuMaterial);
    stats.residencyBytes = asset.pages.size() * sizeof(uint32_t);
    stats.totalGpuBytes = stats.clusterBytes + stats.hierarchyBytes + stats.pageBytes + stats.vertexBytes +
                          stats.indexBytes + stats.materialBytes + stats.residencyBytes;
    return stats;
}

void validateRuntimeTables(const Asset& asset)
{
    if (asset.version != kNaniteVersion && asset.version != kNaniteVersionV1)
    {
        throw std::runtime_error("Unsupported Nanite asset version.");
    }

    for (size_t groupIndex = 0; groupIndex < asset.clusterGroups.size(); ++groupIndex)
    {
        const ClusterGroup& group = asset.clusterGroups[groupIndex];
        if (group.firstCluster + group.clusterCount > asset.clusters.size())
        {
            throw std::runtime_error("ClusterGroup " + std::to_string(groupIndex) + " references out-of-range clusters.");
        }
    }

    for (size_t nodeIndex = 0; nodeIndex < asset.hierarchyNodes.size(); ++nodeIndex)
    {
        const HierarchyNode& node = asset.hierarchyNodes[nodeIndex];
        if (node.childNodeCount > 0 &&
            node.childNodeOffset + node.childNodeCount > asset.hierarchyNodes.size())
        {
            throw std::runtime_error("Hierarchy node " + std::to_string(nodeIndex) + " references out-of-range child nodes.");
        }
        if (node.clusterOffset + node.clusterCount > asset.clusters.size())
        {
            throw std::runtime_error("Hierarchy node " + std::to_string(nodeIndex) + " references out-of-range clusters.");
        }
        if (node.clusterGroupIndex != UINT32_MAX && node.clusterGroupIndex >= asset.clusterGroups.size())
        {
            throw std::runtime_error("Hierarchy node " + std::to_string(nodeIndex) + " references an invalid cluster group.");
        }
    }

    for (size_t pageIndex = 0; pageIndex < asset.pages.size(); ++pageIndex)
    {
        const PageDesc& page = asset.pages[pageIndex];
        if (page.firstCluster + page.clusterCount > asset.clusters.size())
        {
            throw std::runtime_error("Page " + std::to_string(pageIndex) + " references out-of-range clusters.");
        }
    }

    for (size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
    {
        const Mesh& mesh = asset.meshes[meshIndex];
        if (mesh.firstCluster + mesh.clusterCount > asset.clusters.size())
        {
            throw std::runtime_error("Mesh " + std::to_string(meshIndex) + " references out-of-range clusters.");
        }
        if (mesh.firstMaterial + mesh.materialCount > asset.materials.size())
        {
            throw std::runtime_error("Mesh " + std::to_string(meshIndex) + " references out-of-range materials.");
        }
    }
}

} // namespace Falcor::Nanite
