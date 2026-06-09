#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace FalcorRendering::NaniteTool
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
    Bounds bounds;
    Float3 sphereCenter;
    float sphereRadius = 0.f;
    Float3 coneNormal;
    float coneAngle = 0.f;
    float geometricError = 0.f;
};

struct Asset
{
    std::string sourcePath;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Cluster> clusters;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Bounds bounds;
};

constexpr uint32_t kNaniteMagic = ('F') | ('N' << 8) | ('A' << 16) | ('N' << 24);
constexpr uint32_t kNaniteVersion = 1;

inline Float3 operator+(Float3 a, Float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Float3 operator-(Float3 a, Float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Float3 operator*(Float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline Float3 operator/(Float3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }

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
uint64_t triangleCount(const Asset& asset);

void writeAsset(const std::filesystem::path& path, const Asset& asset);
Asset readAsset(const std::filesystem::path& path);
std::vector<std::string> validateAsset(const Asset& asset);
}
