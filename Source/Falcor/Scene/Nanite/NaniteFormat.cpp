#include "NaniteFormat.h"
#include "NaniteCompress.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace Falcor::Nanite
{
namespace
{
struct ChunkDesc
{
    uint32_t type = 0;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct DiskHeaderV2
{
    uint32_t magic = kNaniteMagic;
    uint32_t version = kNaniteVersion;
    uint32_t headerSize = sizeof(DiskHeaderV2);
    uint32_t flags = 0;
    uint32_t meshCount = 0;
    uint32_t materialCount = 0;
    uint32_t clusterCount = 0;
    uint32_t clusterGroupCount = 0;
    uint32_t hierarchyNodeCount = 0;
    uint32_t pageCount = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t chunkCount = 0;
    uint32_t stringTableSize = 0;
    uint64_t chunkTableOffset = 0;
    uint64_t stringTableOffset = 0;
    Bounds bounds{};
};

struct DiskHeaderV1
{
    uint32_t magic = kNaniteMagic;
    uint32_t version = kNaniteVersionV1;
    uint32_t headerSize = sizeof(DiskHeaderV1);
    uint32_t flags = 0;
    uint32_t meshCount = 0;
    uint32_t materialCount = 0;
    uint32_t clusterCount = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t stringTableSize = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    uint64_t meshOffset = 0;
    uint64_t materialOffset = 0;
    uint64_t clusterOffset = 0;
    uint64_t vertexOffset = 0;
    uint64_t indexOffset = 0;
    uint64_t stringTableOffset = 0;
    Bounds bounds{};
};

struct DiskMesh
{
    uint32_t nameOffset = 0;
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    uint32_t firstMaterial = 0;
    uint32_t materialCount = 0;
    uint32_t reserved = 0;
    Bounds bounds{};
};

struct DiskMaterial
{
    uint32_t nameOffset = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    uint32_t reserved2 = 0;
};

struct DiskClusterV1
{
    uint32_t meshIndex = 0;
    uint32_t materialIndex = 0;
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t lodLevel = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
    Bounds bounds{};
    Float3 sphereCenter{};
    float sphereRadius = 0.f;
    Float3 coneNormal{};
    float coneAngle = 0.f;
    float geometricError = 0.f;
    float surfaceArea = 0.f;
    float reservedError[2] = {};
};

struct DiskCluster
{
    uint32_t meshIndex = 0;
    uint32_t materialIndex = 0;
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t lodLevel = 0;
    uint32_t flags = 0;
    uint32_t groupIndex = 0;
    uint32_t pageIndex = 0;
    Bounds bounds{};
    Float3 sphereCenter{};
    float sphereRadius = 0.f;
    Float3 coneNormal{};
    float coneAngle = 0.f;
    float geometricError = 0.f;
    float surfaceArea = 0.f;
};

struct DiskClusterGroup
{
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    uint32_t parentGroup = UINT32_MAX;
    uint32_t lodLevel = 0;
    Bounds bounds{};
    float geometricError = 0.f;
    float reserved = 0.f;
};

struct DiskHierarchyNode
{
    uint32_t childNodeOffset = 0;
    uint32_t childNodeCount = 0;
    uint32_t clusterGroupIndex = UINT32_MAX;
    uint32_t clusterOffset = 0;
    uint32_t clusterCount = 0;
    Float3 sphereCenter{};
    float sphereRadius = 0.f;
    float minError = 0.f;
    float maxError = 0.f;
};

struct DiskPageDesc
{
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    uint32_t flags = 0;
    uint32_t byteSize = 0;
};

static_assert(sizeof(Vertex) == 32);

template<typename T>
void writeRaw(std::ofstream& stream, const T& value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template<typename T>
void writeArray(std::ofstream& stream, const std::vector<T>& values)
{
    if (!values.empty())
    {
        stream.write(reinterpret_cast<const char*>(values.data()), sizeof(T) * values.size());
    }
}

template<typename T>
std::vector<T> readArray(std::ifstream& stream, uint64_t offset, uint32_t count, uint64_t fileSize, const char* label)
{
    if (count == 0) return {};

    const uint64_t byteCount = sizeof(T) * static_cast<uint64_t>(count);
    if (offset > fileSize || byteCount > fileSize - offset)
    {
        throw std::runtime_error(std::string("Invalid ") + label + " range in .fnanite file.");
    }

    std::vector<T> values(count);
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(byteCount));
    if (!stream) throw std::runtime_error(std::string("Failed to read ") + label + " block.");
    return values;
}

const ChunkDesc* findChunk(const std::vector<ChunkDesc>& chunks, ChunkType type)
{
    for (const ChunkDesc& chunk : chunks)
    {
        if (chunk.type == static_cast<uint32_t>(type)) return &chunk;
    }
    return nullptr;
}

void validateChunkTable(const std::vector<ChunkDesc>& chunks, uint64_t fileSize)
{
    for (const ChunkDesc& chunk : chunks)
    {
        if (chunk.size == 0) continue;
        if (chunk.offset > fileSize || chunk.size > fileSize - chunk.offset)
        {
            throw std::runtime_error("Invalid chunk data range in .fnanite file.");
        }
    }
}

void validateHeaderCounts(
    const DiskHeaderV2& header,
    uint32_t meshCount,
    uint32_t materialCount,
    uint32_t clusterCount,
    uint32_t groupCount,
    uint32_t hierarchyCount,
    uint32_t pageCount
)
{
    if (header.meshCount != meshCount)
    {
        throw std::runtime_error("Mesh count mismatch between header and chunk table.");
    }
    if (header.materialCount != materialCount)
    {
        throw std::runtime_error("Material count mismatch between header and chunk table.");
    }
    if (header.clusterCount != clusterCount)
    {
        throw std::runtime_error("Cluster count mismatch between header and chunk table.");
    }
    if (header.clusterGroupCount != groupCount)
    {
        throw std::runtime_error("Cluster group count mismatch between header and chunk table.");
    }
    if (header.hierarchyNodeCount != hierarchyCount)
    {
        throw std::runtime_error("Hierarchy node count mismatch between header and chunk table.");
    }
    if (header.pageCount != pageCount)
    {
        throw std::runtime_error("Page count mismatch between header and chunk table.");
    }
}

uint32_t addString(std::vector<char>& table, const std::string& value)
{
    if (table.size() > std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error("Nanite string table is too large.");
    }

    const uint32_t offset = static_cast<uint32_t>(table.size());
    table.insert(table.end(), value.begin(), value.end());
    table.push_back('\0');
    return offset;
}

std::string readString(const std::vector<char>& table, uint32_t offset)
{
    if (offset >= table.size()) throw std::runtime_error("Invalid string offset in .fnanite file.");

    const char* begin = table.data() + offset;
    const char* end = table.data() + table.size();
    const char* terminator = std::find(begin, end, '\0');
    if (terminator == end) throw std::runtime_error("Unterminated string in .fnanite file.");

    return std::string(begin, terminator);
}
} // namespace

float dot(Float3 a, Float3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Float3 cross(Float3 a, Float3 b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float length(Float3 v)
{
    return std::sqrt(dot(v, v));
}

Float3 normalize(Float3 v)
{
    const float len = length(v);
    if (len <= 0.f) return { 0.f, 1.f, 0.f };
    return v / len;
}

Bounds emptyBounds()
{
    const float inf = std::numeric_limits<float>::infinity();
    return { { inf, inf, inf }, { -inf, -inf, -inf } };
}

bool isEmpty(const Bounds& bounds)
{
    return bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y || bounds.min.z > bounds.max.z;
}

void include(Bounds& bounds, Float3 p)
{
    bounds.min.x = std::min(bounds.min.x, p.x);
    bounds.min.y = std::min(bounds.min.y, p.y);
    bounds.min.z = std::min(bounds.min.z, p.z);
    bounds.max.x = std::max(bounds.max.x, p.x);
    bounds.max.y = std::max(bounds.max.y, p.y);
    bounds.max.z = std::max(bounds.max.z, p.z);
}

void include(Bounds& bounds, const Bounds& other)
{
    if (isEmpty(other)) return;
    include(bounds, other.min);
    include(bounds, other.max);
}

Float3 center(const Bounds& bounds)
{
    if (isEmpty(bounds)) return {};
    return (bounds.min + bounds.max) * 0.5f;
}

float radius(const Bounds& bounds)
{
    if (isEmpty(bounds)) return 0.f;
    return length(bounds.max - center(bounds));
}

uint64_t triangleCount(const Asset& asset)
{
    uint64_t count = 0;
    for (const Cluster& cluster : asset.clusters) count += cluster.triangleCount;
    return count;
}

void buildMetadataTables(Asset& asset, uint32_t groupClusters)
{
    if (groupClusters == 0) groupClusters = 32;

    asset.clusterGroups.clear();
    asset.hierarchyNodes.clear();
    asset.pages.clear();

    if (asset.clusters.empty()) return;

    PageDesc page{};
    page.firstCluster = 0;
    page.clusterCount = static_cast<uint32_t>(asset.clusters.size());
    page.flags = kPageFlagResident;
    page.byteSize = static_cast<uint32_t>(
        asset.vertices.size() * sizeof(Vertex) + asset.indices.size() * sizeof(uint32_t));
    asset.pages.push_back(page);

    for (uint32_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
    {
        const Mesh& mesh = asset.meshes[meshIndex];
        const uint32_t groupCount = (mesh.clusterCount + groupClusters - 1) / groupClusters;
        const uint32_t firstGroupIndex = static_cast<uint32_t>(asset.clusterGroups.size());

        for (uint32_t g = 0; g < groupCount; ++g)
        {
            const uint32_t firstCluster = mesh.firstCluster + g * groupClusters;
            const uint32_t count = std::min(groupClusters, mesh.clusterCount - g * groupClusters);

            ClusterGroup group{};
            group.firstCluster = firstCluster;
            group.clusterCount = count;
            group.parentGroup = UINT32_MAX;
            group.lodLevel = 0;
            group.bounds = emptyBounds();
            group.geometricError = 0.f;

            for (uint32_t c = 0; c < count; ++c)
            {
                Cluster& cluster = asset.clusters[firstCluster + c];
                cluster.groupIndex = static_cast<uint32_t>(asset.clusterGroups.size());
                cluster.pageIndex = 0;
                include(group.bounds, cluster.bounds);
                group.geometricError = std::max(group.geometricError, cluster.geometricError);
                if (cluster.geometricError <= 0.f)
                {
                    cluster.geometricError = cluster.sphereRadius * 0.01f + 0.001f;
                }
            }

            if (group.geometricError <= 0.f)
            {
                group.geometricError = radius(group.bounds) * 0.05f + 0.01f;
            }

            asset.clusterGroups.push_back(group);
        }

        HierarchyNode root{};
        root.childNodeOffset = static_cast<uint32_t>(asset.hierarchyNodes.size() + 1);
        root.childNodeCount = groupCount;
        root.clusterGroupIndex = UINT32_MAX;
        root.clusterOffset = mesh.firstCluster;
        root.clusterCount = mesh.clusterCount;
        root.sphereCenter = center(mesh.bounds);
        root.sphereRadius = radius(mesh.bounds);
        root.minError = 0.f;
        root.maxError = 0.f;
        for (uint32_t g = 0; g < groupCount; ++g)
        {
            root.maxError = std::max(root.maxError, asset.clusterGroups[firstGroupIndex + g].geometricError);
        }

        const uint32_t rootIndex = static_cast<uint32_t>(asset.hierarchyNodes.size());
        asset.hierarchyNodes.push_back(root);

        for (uint32_t g = 0; g < groupCount; ++g)
        {
            const ClusterGroup& group = asset.clusterGroups[firstGroupIndex + g];

            HierarchyNode child{};
            child.childNodeOffset = 0;
            child.childNodeCount = 0;
            child.clusterGroupIndex = firstGroupIndex + g;
            child.clusterOffset = group.firstCluster;
            child.clusterCount = group.clusterCount;
            child.sphereCenter = center(group.bounds);
            child.sphereRadius = radius(group.bounds);
            child.minError = 0.f;
            child.maxError = group.geometricError;
            asset.hierarchyNodes.push_back(child);
        }

        (void)rootIndex;
    }
}

void writeAssetV1(const std::filesystem::path& path, const Asset& asset)
{
    if (asset.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        asset.indices.size() > std::numeric_limits<uint32_t>::max() ||
        asset.clusters.size() > std::numeric_limits<uint32_t>::max() ||
        asset.meshes.size() > std::numeric_limits<uint32_t>::max() ||
        asset.materials.size() > std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error("Nanite asset exceeds V1 32-bit table limits.");
    }

    std::vector<char> strings;
    addString(strings, asset.sourcePath);

    std::vector<DiskMesh> meshes;
    meshes.reserve(asset.meshes.size());
    for (const Mesh& mesh : asset.meshes)
    {
        DiskMesh disk{};
        disk.nameOffset = addString(strings, mesh.name);
        disk.firstCluster = mesh.firstCluster;
        disk.clusterCount = mesh.clusterCount;
        disk.firstMaterial = mesh.firstMaterial;
        disk.materialCount = mesh.materialCount;
        disk.bounds = mesh.bounds;
        meshes.push_back(disk);
    }

    std::vector<DiskMaterial> materials;
    materials.reserve(asset.materials.size());
    for (const Material& material : asset.materials)
    {
        DiskMaterial disk{};
        disk.nameOffset = addString(strings, material.name);
        materials.push_back(disk);
    }

    std::vector<DiskClusterV1> clusters;
    clusters.reserve(asset.clusters.size());
    for (const Cluster& cluster : asset.clusters)
    {
        DiskClusterV1 disk{};
        disk.meshIndex = cluster.meshIndex;
        disk.materialIndex = cluster.materialIndex;
        disk.vertexOffset = cluster.vertexOffset;
        disk.vertexCount = cluster.vertexCount;
        disk.indexOffset = cluster.indexOffset;
        disk.indexCount = cluster.indexCount;
        disk.triangleCount = cluster.triangleCount;
        disk.lodLevel = cluster.lodLevel;
        disk.flags = cluster.flags;
        disk.bounds = cluster.bounds;
        disk.sphereCenter = cluster.sphereCenter;
        disk.sphereRadius = cluster.sphereRadius;
        disk.coneNormal = cluster.coneNormal;
        disk.coneAngle = cluster.coneAngle;
        disk.geometricError = cluster.geometricError;
        disk.surfaceArea = cluster.surfaceArea;
        clusters.push_back(disk);
    }

    DiskHeaderV1 header{};
    header.meshCount = static_cast<uint32_t>(meshes.size());
    header.materialCount = static_cast<uint32_t>(materials.size());
    header.clusterCount = static_cast<uint32_t>(clusters.size());
    header.vertexCount = static_cast<uint32_t>(asset.vertices.size());
    header.indexCount = static_cast<uint32_t>(asset.indices.size());
    header.stringTableSize = static_cast<uint32_t>(strings.size());
    header.bounds = asset.bounds;

    uint64_t offset = sizeof(DiskHeaderV1);
    header.meshOffset = offset;
    offset += sizeof(DiskMesh) * meshes.size();
    header.materialOffset = offset;
    offset += sizeof(DiskMaterial) * materials.size();
    header.clusterOffset = offset;
    offset += sizeof(DiskClusterV1) * clusters.size();
    header.vertexOffset = offset;
    offset += sizeof(Vertex) * asset.vertices.size();
    header.indexOffset = offset;
    offset += sizeof(uint32_t) * asset.indices.size();
    header.stringTableOffset = offset;

    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Failed to open output file: " + path.string());

    writeRaw(stream, header);
    writeArray(stream, meshes);
    writeArray(stream, materials);
    writeArray(stream, clusters);
    writeArray(stream, asset.vertices);
    writeArray(stream, asset.indices);
    if (!strings.empty()) stream.write(strings.data(), static_cast<std::streamsize>(strings.size()));
    if (!stream) throw std::runtime_error("Failed to write .fnanite file: " + path.string());
}

void writeAsset(const std::filesystem::path& path, const Asset& asset, const WriteOptions& options)
{
    Asset assetCopy = asset;
    buildMetadataTables(assetCopy, options.groupClusters);

    if (assetCopy.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        assetCopy.indices.size() > std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error("Nanite asset exceeds 32-bit table limits.");
    }

    const bool useCompression = options.compressVertices && !options.debugUncompressed;
    uint32_t fileFlags = 0;
    if (useCompression) fileFlags |= kFlagCompressedVertices;
    if (options.debugUncompressed) fileFlags |= kFlagDebugUncompressed;

    std::vector<char> strings;
    addString(strings, assetCopy.sourcePath);

    std::vector<DiskMesh> meshes;
    meshes.reserve(assetCopy.meshes.size());
    for (const Mesh& mesh : assetCopy.meshes)
    {
        DiskMesh disk{};
        disk.nameOffset = addString(strings, mesh.name);
        disk.firstCluster = mesh.firstCluster;
        disk.clusterCount = mesh.clusterCount;
        disk.firstMaterial = mesh.firstMaterial;
        disk.materialCount = mesh.materialCount;
        disk.bounds = mesh.bounds;
        meshes.push_back(disk);
    }

    std::vector<DiskMaterial> materials;
    materials.reserve(assetCopy.materials.size());
    for (const Material& material : assetCopy.materials)
    {
        DiskMaterial disk{};
        disk.nameOffset = addString(strings, material.name);
        materials.push_back(disk);
    }

    std::vector<DiskCluster> clusters;
    clusters.reserve(assetCopy.clusters.size());
    for (const Cluster& cluster : assetCopy.clusters)
    {
        DiskCluster disk{};
        disk.meshIndex = cluster.meshIndex;
        disk.materialIndex = cluster.materialIndex;
        disk.vertexOffset = cluster.vertexOffset;
        disk.vertexCount = cluster.vertexCount;
        disk.indexOffset = cluster.indexOffset;
        disk.indexCount = cluster.indexCount;
        disk.triangleCount = cluster.triangleCount;
        disk.lodLevel = cluster.lodLevel;
        disk.flags = cluster.flags;
        disk.groupIndex = cluster.groupIndex;
        disk.pageIndex = cluster.pageIndex;
        disk.bounds = cluster.bounds;
        disk.sphereCenter = cluster.sphereCenter;
        disk.sphereRadius = cluster.sphereRadius;
        disk.coneNormal = cluster.coneNormal;
        disk.coneAngle = cluster.coneAngle;
        disk.geometricError = cluster.geometricError;
        disk.surfaceArea = cluster.surfaceArea;
        clusters.push_back(disk);
    }

    std::vector<DiskClusterGroup> groups;
    groups.reserve(assetCopy.clusterGroups.size());
    for (const ClusterGroup& group : assetCopy.clusterGroups)
    {
        DiskClusterGroup disk{};
        disk.firstCluster = group.firstCluster;
        disk.clusterCount = group.clusterCount;
        disk.parentGroup = group.parentGroup;
        disk.lodLevel = group.lodLevel;
        disk.bounds = group.bounds;
        disk.geometricError = group.geometricError;
        groups.push_back(disk);
    }

    std::vector<DiskHierarchyNode> hierarchy;
    hierarchy.reserve(assetCopy.hierarchyNodes.size());
    for (const HierarchyNode& node : assetCopy.hierarchyNodes)
    {
        DiskHierarchyNode disk{};
        disk.childNodeOffset = node.childNodeOffset;
        disk.childNodeCount = node.childNodeCount;
        disk.clusterGroupIndex = node.clusterGroupIndex;
        disk.clusterOffset = node.clusterOffset;
        disk.clusterCount = node.clusterCount;
        disk.sphereCenter = node.sphereCenter;
        disk.sphereRadius = node.sphereRadius;
        disk.minError = node.minError;
        disk.maxError = node.maxError;
        hierarchy.push_back(disk);
    }

    std::vector<DiskPageDesc> pages;
    pages.reserve(assetCopy.pages.size());
    for (const PageDesc& page : assetCopy.pages)
    {
        DiskPageDesc disk{};
        disk.firstCluster = page.firstCluster;
        disk.clusterCount = page.clusterCount;
        disk.flags = page.flags;
        disk.byteSize = page.byteSize;
        pages.push_back(disk);
    }

    std::vector<CompressedVertex> compressedVertices;
    if (useCompression) compressedVertices = compressVertices(assetCopy);

    std::vector<ChunkDesc> chunks;
    auto addChunk = [&](ChunkType type, const auto& data)
    {
        ChunkDesc chunk{};
        chunk.type = static_cast<uint32_t>(type);
        chunk.offset = 0;
        chunk.size = sizeof(typename std::decay_t<decltype(data)>::value_type) * data.size();
        chunks.push_back(chunk);
    };

    addChunk(ChunkType::Mesh, meshes);
    addChunk(ChunkType::Material, materials);
    addChunk(ChunkType::Cluster, clusters);
    addChunk(ChunkType::ClusterGroup, groups);
    addChunk(ChunkType::Hierarchy, hierarchy);
    addChunk(ChunkType::Page, pages);
    if (useCompression)
        addChunk(ChunkType::CompressedVertex, compressedVertices);
    else
        addChunk(ChunkType::Vertex, assetCopy.vertices);
    addChunk(ChunkType::Index, assetCopy.indices);
    if (!strings.empty()) addChunk(ChunkType::StringTable, strings);

    DiskHeaderV2 header{};
    header.flags = fileFlags;
    header.meshCount = static_cast<uint32_t>(meshes.size());
    header.materialCount = static_cast<uint32_t>(materials.size());
    header.clusterCount = static_cast<uint32_t>(clusters.size());
    header.clusterGroupCount = static_cast<uint32_t>(groups.size());
    header.hierarchyNodeCount = static_cast<uint32_t>(hierarchy.size());
    header.pageCount = static_cast<uint32_t>(pages.size());
    header.vertexCount = static_cast<uint32_t>(assetCopy.vertices.size());
    header.indexCount = static_cast<uint32_t>(assetCopy.indices.size());
    header.chunkCount = static_cast<uint32_t>(chunks.size());
    header.stringTableSize = static_cast<uint32_t>(strings.size());
    header.bounds = assetCopy.bounds;

    uint64_t offset = sizeof(DiskHeaderV2);
    header.chunkTableOffset = offset;
    offset += sizeof(ChunkDesc) * chunks.size();

    for (size_t i = 0; i < chunks.size(); ++i)
    {
        chunks[i].offset = offset;
        offset += chunks[i].size;
    }

    const ChunkDesc* stringChunk = findChunk(chunks, ChunkType::StringTable);
    header.stringTableOffset = stringChunk ? stringChunk->offset : 0;

    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Failed to open output file: " + path.string());

    writeRaw(stream, header);
    writeArray(stream, chunks);
    writeArray(stream, meshes);
    writeArray(stream, materials);
    writeArray(stream, clusters);
    writeArray(stream, groups);
    writeArray(stream, hierarchy);
    writeArray(stream, pages);
    if (useCompression)
        writeArray(stream, compressedVertices);
    else
        writeArray(stream, assetCopy.vertices);
    writeArray(stream, assetCopy.indices);
    if (!strings.empty()) stream.write(strings.data(), static_cast<std::streamsize>(strings.size()));
    if (!stream) throw std::runtime_error("Failed to write .fnanite file: " + path.string());
}

Asset readAssetV1(std::ifstream& stream, const DiskHeaderV1& header, uint64_t fileSize)
{
    const auto diskMeshes = readArray<DiskMesh>(stream, header.meshOffset, header.meshCount, fileSize, "mesh");
    const auto diskMaterials = readArray<DiskMaterial>(stream, header.materialOffset, header.materialCount, fileSize, "material");
    const auto diskClusters = readArray<DiskClusterV1>(stream, header.clusterOffset, header.clusterCount, fileSize, "cluster");
    auto vertices = readArray<Vertex>(stream, header.vertexOffset, header.vertexCount, fileSize, "vertex");
    auto indices = readArray<uint32_t>(stream, header.indexOffset, header.indexCount, fileSize, "index");

    std::vector<char> strings;
    if (header.stringTableSize > 0)
    {
        if (header.stringTableOffset > fileSize || header.stringTableSize > fileSize - header.stringTableOffset)
        {
            throw std::runtime_error("Invalid string table range in .fnanite file.");
        }

        strings.resize(header.stringTableSize);
        stream.seekg(static_cast<std::streamoff>(header.stringTableOffset), std::ios::beg);
        stream.read(strings.data(), static_cast<std::streamsize>(strings.size()));
        if (!stream) throw std::runtime_error("Failed to read string table.");
    }

    Asset asset;
    asset.version = kNaniteVersionV1;
    asset.flags = header.flags;
    asset.sourcePath = strings.empty() ? std::string{} : readString(strings, 0);
    asset.bounds = header.bounds;
    asset.vertices = std::move(vertices);
    asset.indices = std::move(indices);

    asset.meshes.reserve(diskMeshes.size());
    for (const DiskMesh& disk : diskMeshes)
    {
        Mesh mesh{};
        mesh.name = readString(strings, disk.nameOffset);
        mesh.firstCluster = disk.firstCluster;
        mesh.clusterCount = disk.clusterCount;
        mesh.firstMaterial = disk.firstMaterial;
        mesh.materialCount = disk.materialCount;
        mesh.bounds = disk.bounds;
        asset.meshes.push_back(std::move(mesh));
    }

    asset.materials.reserve(diskMaterials.size());
    for (const DiskMaterial& disk : diskMaterials)
    {
        Material material{};
        material.name = readString(strings, disk.nameOffset);
        asset.materials.push_back(std::move(material));
    }

    asset.clusters.reserve(diskClusters.size());
    for (const DiskClusterV1& disk : diskClusters)
    {
        Cluster cluster{};
        cluster.meshIndex = disk.meshIndex;
        cluster.materialIndex = disk.materialIndex;
        cluster.vertexOffset = disk.vertexOffset;
        cluster.vertexCount = disk.vertexCount;
        cluster.indexOffset = disk.indexOffset;
        cluster.indexCount = disk.indexCount;
        cluster.triangleCount = disk.triangleCount;
        cluster.lodLevel = disk.lodLevel;
        cluster.flags = disk.flags;
        cluster.bounds = disk.bounds;
        cluster.sphereCenter = disk.sphereCenter;
        cluster.sphereRadius = disk.sphereRadius;
        cluster.coneNormal = disk.coneNormal;
        cluster.coneAngle = disk.coneAngle;
        cluster.geometricError = disk.geometricError;
        cluster.surfaceArea = disk.surfaceArea;
        asset.clusters.push_back(cluster);
    }

    buildMetadataTables(asset, 32);
    return asset;
}

Asset readAssetV2(std::ifstream& stream, const DiskHeaderV2& header, uint64_t fileSize)
{
    if (header.chunkTableOffset > fileSize ||
        sizeof(ChunkDesc) * static_cast<uint64_t>(header.chunkCount) > fileSize - header.chunkTableOffset)
    {
        throw std::runtime_error("Invalid chunk table range in .fnanite file.");
    }

    auto chunks = readArray<ChunkDesc>(stream, header.chunkTableOffset, header.chunkCount, fileSize, "chunk table");
    validateChunkTable(chunks, fileSize);

    const ChunkDesc* meshChunk = findChunk(chunks, ChunkType::Mesh);
    const ChunkDesc* materialChunk = findChunk(chunks, ChunkType::Material);
    const ChunkDesc* clusterChunk = findChunk(chunks, ChunkType::Cluster);
    const ChunkDesc* groupChunk = findChunk(chunks, ChunkType::ClusterGroup);
    const ChunkDesc* hierarchyChunk = findChunk(chunks, ChunkType::Hierarchy);
    const ChunkDesc* pageChunk = findChunk(chunks, ChunkType::Page);
    const ChunkDesc* vertexChunk = findChunk(chunks, ChunkType::Vertex);
    const ChunkDesc* compressedChunk = findChunk(chunks, ChunkType::CompressedVertex);
    const ChunkDesc* indexChunk = findChunk(chunks, ChunkType::Index);
    const ChunkDesc* stringChunk = findChunk(chunks, ChunkType::StringTable);

    if (!meshChunk || !materialChunk || !clusterChunk || !indexChunk)
    {
        throw std::runtime_error("Missing required chunk in V2 .fnanite file.");
    }

    const uint32_t meshCount = static_cast<uint32_t>(meshChunk->size / sizeof(DiskMesh));
    const uint32_t materialCount = static_cast<uint32_t>(materialChunk->size / sizeof(DiskMaterial));
    const uint32_t clusterCount = static_cast<uint32_t>(clusterChunk->size / sizeof(DiskCluster));
    const uint32_t groupCount = groupChunk ? static_cast<uint32_t>(groupChunk->size / sizeof(DiskClusterGroup)) : 0;
    const uint32_t hierarchyCount = hierarchyChunk ? static_cast<uint32_t>(hierarchyChunk->size / sizeof(DiskHierarchyNode)) : 0;
    const uint32_t pageCount = pageChunk ? static_cast<uint32_t>(pageChunk->size / sizeof(DiskPageDesc)) : 0;

    validateHeaderCounts(header, meshCount, materialCount, clusterCount, groupCount, hierarchyCount, pageCount);

    const auto diskMeshes = readArray<DiskMesh>(stream, meshChunk->offset, meshCount, fileSize, "mesh");
    const auto diskMaterials = readArray<DiskMaterial>(stream, materialChunk->offset, materialCount, fileSize, "material");
    const auto diskClusters = readArray<DiskCluster>(stream, clusterChunk->offset, clusterCount, fileSize, "cluster");

    std::vector<DiskClusterGroup> diskGroups;
    if (groupChunk)
    {
        const uint32_t groupCount = static_cast<uint32_t>(groupChunk->size / sizeof(DiskClusterGroup));
        diskGroups = readArray<DiskClusterGroup>(stream, groupChunk->offset, groupCount, fileSize, "cluster group");
    }

    std::vector<DiskHierarchyNode> diskHierarchy;
    if (hierarchyChunk)
    {
        const uint32_t nodeCount = static_cast<uint32_t>(hierarchyChunk->size / sizeof(DiskHierarchyNode));
        diskHierarchy = readArray<DiskHierarchyNode>(stream, hierarchyChunk->offset, nodeCount, fileSize, "hierarchy");
    }

    std::vector<DiskPageDesc> diskPages;
    if (pageChunk)
    {
        const uint32_t pageCount = static_cast<uint32_t>(pageChunk->size / sizeof(DiskPageDesc));
        diskPages = readArray<DiskPageDesc>(stream, pageChunk->offset, pageCount, fileSize, "page");
    }

    std::vector<char> strings;
    if (stringChunk && stringChunk->size > 0)
    {
        strings = readArray<char>(stream, stringChunk->offset, static_cast<uint32_t>(stringChunk->size), fileSize, "string table");
    }

    Asset asset;
    asset.version = kNaniteVersion;
    asset.flags = header.flags;
    asset.sourcePath = strings.empty() ? std::string{} : readString(strings, 0);
    asset.bounds = header.bounds;

    asset.meshes.reserve(diskMeshes.size());
    for (const DiskMesh& disk : diskMeshes)
    {
        Mesh mesh{};
        mesh.name = readString(strings, disk.nameOffset);
        mesh.firstCluster = disk.firstCluster;
        mesh.clusterCount = disk.clusterCount;
        mesh.firstMaterial = disk.firstMaterial;
        mesh.materialCount = disk.materialCount;
        mesh.bounds = disk.bounds;
        asset.meshes.push_back(std::move(mesh));
    }

    asset.materials.reserve(diskMaterials.size());
    for (const DiskMaterial& disk : diskMaterials)
    {
        Material material{};
        material.name = readString(strings, disk.nameOffset);
        asset.materials.push_back(std::move(material));
    }

    asset.clusters.reserve(diskClusters.size());
    for (const DiskCluster& disk : diskClusters)
    {
        Cluster cluster{};
        cluster.meshIndex = disk.meshIndex;
        cluster.materialIndex = disk.materialIndex;
        cluster.vertexOffset = disk.vertexOffset;
        cluster.vertexCount = disk.vertexCount;
        cluster.indexOffset = disk.indexOffset;
        cluster.indexCount = disk.indexCount;
        cluster.triangleCount = disk.triangleCount;
        cluster.lodLevel = disk.lodLevel;
        cluster.flags = disk.flags;
        cluster.groupIndex = disk.groupIndex;
        cluster.pageIndex = disk.pageIndex;
        cluster.bounds = disk.bounds;
        cluster.sphereCenter = disk.sphereCenter;
        cluster.sphereRadius = disk.sphereRadius;
        cluster.coneNormal = disk.coneNormal;
        cluster.coneAngle = disk.coneAngle;
        cluster.geometricError = disk.geometricError;
        cluster.surfaceArea = disk.surfaceArea;
        asset.clusters.push_back(cluster);
    }

    asset.clusterGroups.reserve(diskGroups.size());
    for (const DiskClusterGroup& disk : diskGroups)
    {
        ClusterGroup group{};
        group.firstCluster = disk.firstCluster;
        group.clusterCount = disk.clusterCount;
        group.parentGroup = disk.parentGroup;
        group.lodLevel = disk.lodLevel;
        group.bounds = disk.bounds;
        group.geometricError = disk.geometricError;
        asset.clusterGroups.push_back(group);
    }

    asset.hierarchyNodes.reserve(diskHierarchy.size());
    for (const DiskHierarchyNode& disk : diskHierarchy)
    {
        HierarchyNode node{};
        node.childNodeOffset = disk.childNodeOffset;
        node.childNodeCount = disk.childNodeCount;
        node.clusterGroupIndex = disk.clusterGroupIndex;
        node.clusterOffset = disk.clusterOffset;
        node.clusterCount = disk.clusterCount;
        node.sphereCenter = disk.sphereCenter;
        node.sphereRadius = disk.sphereRadius;
        node.minError = disk.minError;
        node.maxError = disk.maxError;
        asset.hierarchyNodes.push_back(node);
    }

    asset.pages.reserve(diskPages.size());
    for (const DiskPageDesc& disk : diskPages)
    {
        PageDesc page{};
        page.firstCluster = disk.firstCluster;
        page.clusterCount = disk.clusterCount;
        page.flags = disk.flags;
        page.byteSize = disk.byteSize;
        asset.pages.push_back(page);
    }

    if (compressedChunk)
    {
        const uint32_t compressedCount = static_cast<uint32_t>(compressedChunk->size / sizeof(CompressedVertex));
        const auto compressed = readArray<CompressedVertex>(
            stream, compressedChunk->offset, compressedCount, fileSize, "compressed vertex");
        decompressVertices(compressed, asset, asset.vertices);
    }
    else if (vertexChunk)
    {
        const uint32_t vertexCount = static_cast<uint32_t>(vertexChunk->size / sizeof(Vertex));
        asset.vertices = readArray<Vertex>(stream, vertexChunk->offset, vertexCount, fileSize, "vertex");
    }
    else
    {
        throw std::runtime_error("V2 .fnanite file has no vertex data chunk.");
    }

    const uint32_t indexCount = static_cast<uint32_t>(indexChunk->size / sizeof(uint32_t));
    asset.indices = readArray<uint32_t>(stream, indexChunk->offset, indexCount, fileSize, "index");

    if (asset.clusterGroups.empty() || asset.hierarchyNodes.empty() || asset.pages.empty())
    {
        buildMetadataTables(asset, 32);
    }

    return asset;
}

Asset readAsset(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Failed to open .fnanite file: " + path.string());

    const uint64_t fileSize = static_cast<uint64_t>(std::filesystem::file_size(path));
    if (fileSize < sizeof(DiskHeaderV1)) throw std::runtime_error("File is too small to be a .fnanite asset.");

    uint32_t magic = 0;
    stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != kNaniteMagic) throw std::runtime_error("Invalid .fnanite magic.");
    stream.seekg(0, std::ios::beg);

    uint32_t version = 0;
    stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    stream.read(reinterpret_cast<char*>(&version), sizeof(version));
    stream.seekg(0, std::ios::beg);

    if (version == kNaniteVersionV1)
    {
        DiskHeaderV1 header;
        stream.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!stream) throw std::runtime_error("Failed to read .fnanite header.");
        if (header.headerSize != sizeof(DiskHeaderV1)) throw std::runtime_error("Unexpected .fnanite V1 header size.");
        return readAssetV1(stream, header, fileSize);
    }

    if (version == kNaniteVersion)
    {
        DiskHeaderV2 header;
        stream.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!stream) throw std::runtime_error("Failed to read .fnanite header.");
        if (header.headerSize != sizeof(DiskHeaderV2)) throw std::runtime_error("Unexpected .fnanite V2 header size.");
        return readAssetV2(stream, header, fileSize);
    }

    throw std::runtime_error("Unsupported .fnanite version " + std::to_string(version) + ". Supported: V1 and V2.");
}

std::vector<std::string> validateAsset(const Asset& asset)
{
    std::vector<std::string> errors;

    if (asset.materials.empty()) errors.push_back("Asset has no materials.");
    if (asset.meshes.empty()) errors.push_back("Asset has no meshes.");

    for (size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
    {
        const Mesh& mesh = asset.meshes[meshIndex];
        if (mesh.firstCluster > asset.clusters.size() || mesh.clusterCount > asset.clusters.size() - mesh.firstCluster)
        {
            errors.push_back("Mesh " + std::to_string(meshIndex) + " has an invalid cluster range.");
        }
        if (mesh.firstMaterial > asset.materials.size() || mesh.materialCount > asset.materials.size() - mesh.firstMaterial)
        {
            errors.push_back("Mesh " + std::to_string(meshIndex) + " has an invalid material range.");
        }
    }

    for (size_t clusterIndex = 0; clusterIndex < asset.clusters.size(); ++clusterIndex)
    {
        const Cluster& cluster = asset.clusters[clusterIndex];
        if (cluster.meshIndex >= asset.meshes.size())
        {
            errors.push_back("Cluster " + std::to_string(clusterIndex) + " references an invalid mesh.");
        }
        if (cluster.materialIndex >= asset.materials.size())
        {
            errors.push_back("Cluster " + std::to_string(clusterIndex) + " references an invalid material.");
        }
        if (cluster.vertexOffset > asset.vertices.size() || cluster.vertexCount > asset.vertices.size() - cluster.vertexOffset)
        {
            errors.push_back("Cluster " + std::to_string(clusterIndex) + " has an invalid vertex range.");
        }
        if (cluster.indexOffset > asset.indices.size() || cluster.indexCount > asset.indices.size() - cluster.indexOffset)
        {
            errors.push_back("Cluster " + std::to_string(clusterIndex) + " has an invalid index range.");
        }
        if (cluster.indexCount != cluster.triangleCount * 3)
        {
            errors.push_back("Cluster " + std::to_string(clusterIndex) + " index count does not match triangle count.");
        }

        for (uint32_t i = 0; i < cluster.indexCount && cluster.indexOffset + i < asset.indices.size(); ++i)
        {
            if (asset.indices[cluster.indexOffset + i] >= cluster.vertexCount)
            {
                errors.push_back("Cluster " + std::to_string(clusterIndex) + " has a local index out of range.");
                break;
            }
        }

        for (uint32_t i = 0; i < cluster.vertexCount; ++i)
        {
            const Vertex& v = asset.vertices[cluster.vertexOffset + i];
            if (v.position.x < cluster.bounds.min.x - 1e-3f || v.position.x > cluster.bounds.max.x + 1e-3f ||
                v.position.y < cluster.bounds.min.y - 1e-3f || v.position.y > cluster.bounds.max.y + 1e-3f ||
                v.position.z < cluster.bounds.min.z - 1e-3f || v.position.z > cluster.bounds.max.z + 1e-3f)
            {
                errors.push_back("Cluster " + std::to_string(clusterIndex) + " vertex outside cluster bounds.");
                break;
            }
        }
    }

    return errors;
}

} // namespace Falcor::Nanite
