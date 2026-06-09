#include "NaniteAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace FalcorRendering::NaniteTool
{
namespace
{
struct DiskHeader
{
    uint32_t magic = kNaniteMagic;
    uint32_t version = kNaniteVersion;
    uint32_t headerSize = sizeof(DiskHeader);
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
}

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

void writeAsset(const std::filesystem::path& path, const Asset& asset)
{
    if (asset.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        asset.indices.size() > std::numeric_limits<uint32_t>::max() ||
        asset.clusters.size() > std::numeric_limits<uint32_t>::max() ||
        asset.meshes.size() > std::numeric_limits<uint32_t>::max() ||
        asset.materials.size() > std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error("Nanite asset exceeds v1 32-bit table limits.");
    }

    std::vector<char> strings;
    addString(strings, asset.sourcePath);

    std::vector<DiskMesh> meshes;
    meshes.reserve(asset.meshes.size());
    for (const Mesh& mesh : asset.meshes)
    {
        DiskMesh disk;
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
        DiskMaterial disk;
        disk.nameOffset = addString(strings, material.name);
        materials.push_back(disk);
    }

    std::vector<DiskCluster> clusters;
    clusters.reserve(asset.clusters.size());
    for (const Cluster& cluster : asset.clusters)
    {
        DiskCluster disk;
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

    DiskHeader header;
    header.meshCount = static_cast<uint32_t>(meshes.size());
    header.materialCount = static_cast<uint32_t>(materials.size());
    header.clusterCount = static_cast<uint32_t>(clusters.size());
    header.vertexCount = static_cast<uint32_t>(asset.vertices.size());
    header.indexCount = static_cast<uint32_t>(asset.indices.size());
    header.stringTableSize = static_cast<uint32_t>(strings.size());
    header.bounds = asset.bounds;

    uint64_t offset = sizeof(DiskHeader);
    header.meshOffset = offset;
    offset += sizeof(DiskMesh) * meshes.size();
    header.materialOffset = offset;
    offset += sizeof(DiskMaterial) * materials.size();
    header.clusterOffset = offset;
    offset += sizeof(DiskCluster) * clusters.size();
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

Asset readAsset(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Failed to open .fnanite file: " + path.string());

    const uint64_t fileSize = static_cast<uint64_t>(std::filesystem::file_size(path));
    if (fileSize < sizeof(DiskHeader)) throw std::runtime_error("File is too small to be a .fnanite asset.");

    DiskHeader header;
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream) throw std::runtime_error("Failed to read .fnanite header.");
    if (header.magic != kNaniteMagic) throw std::runtime_error("Invalid .fnanite magic.");
    if (header.version != kNaniteVersion) throw std::runtime_error("Unsupported .fnanite version.");
    if (header.headerSize != sizeof(DiskHeader)) throw std::runtime_error("Unexpected .fnanite header size.");

    const auto diskMeshes = readArray<DiskMesh>(stream, header.meshOffset, header.meshCount, fileSize, "mesh");
    const auto diskMaterials = readArray<DiskMaterial>(stream, header.materialOffset, header.materialCount, fileSize, "material");
    const auto diskClusters = readArray<DiskCluster>(stream, header.clusterOffset, header.clusterCount, fileSize, "cluster");
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
    asset.sourcePath = strings.empty() ? std::string{} : readString(strings, 0);
    asset.bounds = header.bounds;
    asset.vertices = std::move(vertices);
    asset.indices = std::move(indices);

    asset.meshes.reserve(diskMeshes.size());
    for (const DiskMesh& disk : diskMeshes)
    {
        Mesh mesh;
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
        Material material;
        material.name = readString(strings, disk.nameOffset);
        asset.materials.push_back(std::move(material));
    }

    asset.clusters.reserve(diskClusters.size());
    for (const DiskCluster& disk : diskClusters)
    {
        Cluster cluster;
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

    return asset;
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
    }

    return errors;
}
}
