#pragma once

#include "NaniteTypes.h"

#include <filesystem>
#include <vector>

namespace Falcor::Nanite
{

struct Asset
{
    std::string sourcePath;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Cluster> clusters;
    std::vector<ClusterGroup> clusterGroups;
    std::vector<HierarchyNode> hierarchyNodes;
    std::vector<PageDesc> pages;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Bounds bounds;
    uint32_t version = kNaniteVersion;
    uint32_t flags = 0;
    uint64_t sourceTriangleCount = 0;
    uint64_t degenerateTriangleCount = 0;
    PartitionStats partitionStats;
    std::vector<ClusterDebugInfo> clusterDebugInfo;
};

struct WriteOptions
{
    bool compressVertices = true;
    bool debugUncompressed = false;
    uint32_t groupClusters = 32;
};

uint64_t triangleCount(const Asset& asset);

void buildMetadataTables(Asset& asset, uint32_t groupClusters);
void writeAssetV1(const std::filesystem::path& path, const Asset& asset);
void writeAsset(const std::filesystem::path& path, const Asset& asset, const WriteOptions& options = {});
Asset readAsset(const std::filesystem::path& path);
std::vector<std::string> validateAsset(const Asset& asset);

} // namespace Falcor::Nanite
