#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace Falcor::Nanite
{

struct Float2
{
    float x = 0.f;
    float y = 0.f;
};

struct Float3
{
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct Bounds
{
    Float3 min;
    Float3 max;
};

struct Vertex
{
    Float3 position;
    Float3 normal;
    Float2 texCoord;
};

struct Material
{
    std::string name;
};

struct Mesh
{
    std::string name;
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    uint32_t firstMaterial = 0;
    uint32_t materialCount = 0;
    Bounds bounds;
};

struct Cluster
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
    Bounds bounds;
    Float3 sphereCenter;
    float sphereRadius = 0.f;
    Float3 coneNormal;
    float coneAngle = 0.f;
    float geometricError = 0.f;
    float surfaceArea = 0.f;
};

struct ClusterGroup
{
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    uint32_t parentGroup = UINT32_MAX;
    uint32_t lodLevel = 0;
    Bounds bounds;
    float geometricError = 0.f;
};

struct HierarchyNode
{
    uint32_t childNodeOffset = 0;
    uint32_t childNodeCount = 0;
    uint32_t clusterGroupIndex = UINT32_MAX;
    uint32_t clusterOffset = 0;
    uint32_t clusterCount = 0;
    Float3 sphereCenter;
    float sphereRadius = 0.f;
    float minError = 0.f;
    float maxError = 0.f;
};

struct PageDesc
{
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    uint32_t flags = 0;
    uint32_t byteSize = 0;
};

struct ClusterDebugInfo
{
    uint32_t sourceMeshIndex = 0;
    uint32_t sourceMaterialIndex = 0;
    uint32_t boundaryVertexCount = 0;
    std::vector<uint32_t> sourceTriangleIndices;
};

struct PartitionStats
{
    uint64_t totalLocalVertices = 0;
    uint64_t boundarySourceVertices = 0;
    uint64_t interClusterEdges = 0;
};

/// Pre-cluster source geometry embedded for offline re-bake (UE-style static mesh source).
struct SourceMeshSection
{
    std::string name;
    std::string materialName;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Bounds bounds;
};

constexpr uint32_t kNaniteMagic = ('F') | ('N' << 8) | ('A' << 16) | ('N' << 24);
constexpr uint32_t kNaniteVersionV1 = 1;
constexpr uint32_t kNaniteVersion = 2;

constexpr uint32_t kFlagCompressedVertices = 1u << 0;
constexpr uint32_t kFlagDebugUncompressed = 1u << 1;
constexpr uint32_t kFlagHasSourceGeometry = 1u << 2;

constexpr uint32_t kPageFlagResident = 1u << 0;

enum class ChunkType : uint32_t
{
    Mesh = 0,
    Material = 1,
    Cluster = 2,
    ClusterGroup = 3,
    Hierarchy = 4,
    Page = 5,
    Vertex = 6,
    Index = 7,
    StringTable = 8,
    CompressedVertex = 9,
    SourceMesh = 10,
    SourceVertex = 11,
    SourceIndex = 12,
};

inline Float3 operator+(Float3 a, Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Float3 operator-(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Float3 operator*(Float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Float3 operator/(Float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }

float dot(Float3 a, Float3 b);
Float3 cross(Float3 a, Float3 b);
float length(Float3 v);
Float3 normalize(Float3 v);
Bounds emptyBounds();
bool isEmpty(const Bounds& bounds);
void include(Bounds& bounds, Float3 p);
void include(Bounds& bounds, const Bounds& other);
Float3 center(const Bounds& bounds);
float radius(const Bounds& bounds);

} // namespace Falcor::Nanite
