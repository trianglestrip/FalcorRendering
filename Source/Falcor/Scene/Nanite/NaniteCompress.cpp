#include "NaniteCompress.h"

#include <algorithm>
#include <cmath>

namespace Falcor::Nanite
{
namespace
{
float signNotZero(float v)
{
    return (v >= 0.f) ? 1.f : -1.f;
}
} // namespace

uint32_t packOctahedralNormal(Float3 normal)
{
    normal = normalize(normal);
    const float sum = std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
    Float2 oct = {normal.x / sum, normal.y / sum};
    if (normal.z < 0.f)
    {
        oct = { (1.f - std::abs(oct.y)) * signNotZero(oct.x), (1.f - std::abs(oct.x)) * signNotZero(oct.y) };
    }

    const auto toU16 = [](float v)
    {
        const float clamped = std::clamp(v * 0.5f + 0.5f, 0.f, 1.f);
        return static_cast<uint16_t>(std::lround(clamped * 65535.f));
    };

    const uint16_t u = toU16(oct.x);
    const uint16_t v = toU16(oct.y);
    return static_cast<uint32_t>(u) | (static_cast<uint32_t>(v) << 16);
}

Float3 unpackOctahedralNormal(uint32_t packed)
{
    const auto fromU16 = [](uint32_t v) { return (v / 65535.f) * 2.f - 1.f; };
    Float2 oct = { fromU16(packed & 0xFFFFu), fromU16(packed >> 16) };
    Float3 normal = { oct.x, oct.y, 1.f - std::abs(oct.x) - std::abs(oct.y) };
    if (normal.z < 0.f)
    {
        normal = {
            (1.f - std::abs(normal.y)) * signNotZero(normal.x),
            (1.f - std::abs(normal.x)) * signNotZero(normal.y),
            normal.z,
        };
    }
    return normalize(normal);
}

void quantizePosition(Float3 position, const Bounds& bounds, uint16_t out[3])
{
    const Float3 extent = bounds.max - bounds.min;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float* pos = &position.x;
        const float* bmin = &bounds.min.x;
        const float* bmax = &bounds.max.x;
        const float range = bmax[axis] - bmin[axis];
        float t = 0.f;
        if (range > 0.f) t = (pos[axis] - bmin[axis]) / range;
        t = std::clamp(t, 0.f, 1.f);
        out[axis] = static_cast<uint16_t>(std::lround(t * 65535.f));
    }
}

Float3 dequantizePosition(const uint16_t in[3], const Bounds& bounds)
{
    Float3 result;
    const Float3 extent = bounds.max - bounds.min;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float t = in[axis] / 65535.f;
        const float* bmin = &bounds.min.x;
        const float* bmax = &bounds.max.x;
        (&result.x)[axis] = bmin[axis] + t * (bmax[axis] - bmin[axis]);
    }
    return result;
}

CompressedVertex compressVertex(const Vertex& vertex, const Bounds& clusterBounds)
{
    CompressedVertex out{};
    quantizePosition(vertex.position, clusterBounds, &out.posX);
    out.packedNormal = packOctahedralNormal(vertex.normal);
    out.texU = static_cast<uint16_t>(std::clamp(std::lround(vertex.texCoord.x * 65535.f), 0l, 65535l));
    out.texV = static_cast<uint16_t>(std::clamp(std::lround(vertex.texCoord.y * 65535.f), 0l, 65535l));
    return out;
}

Vertex decompressVertex(const CompressedVertex& compressed, const Bounds& clusterBounds)
{
    const uint16_t pos[3] = { compressed.posX, compressed.posY, compressed.posZ };
    Vertex out{};
    out.position = dequantizePosition(pos, clusterBounds);
    out.normal = unpackOctahedralNormal(compressed.packedNormal);
    out.texCoord = { compressed.texU / 65535.f, compressed.texV / 65535.f };
    return out;
}

std::vector<CompressedVertex> compressVertices(const Asset& asset)
{
    std::vector<CompressedVertex> compressed(asset.vertices.size());
    for (size_t clusterIndex = 0; clusterIndex < asset.clusters.size(); ++clusterIndex)
    {
        const Cluster& cluster = asset.clusters[clusterIndex];
        for (uint32_t i = 0; i < cluster.vertexCount; ++i)
        {
            const uint32_t globalIndex = cluster.vertexOffset + i;
            compressed[globalIndex] = compressVertex(asset.vertices[globalIndex], cluster.bounds);
        }
    }
    return compressed;
}

void decompressVertices(const std::vector<CompressedVertex>& compressed, const Asset& asset, std::vector<Vertex>& outVertices)
{
    outVertices.resize(compressed.size());
    for (size_t clusterIndex = 0; clusterIndex < asset.clusters.size(); ++clusterIndex)
    {
        const Cluster& cluster = asset.clusters[clusterIndex];
        for (uint32_t i = 0; i < cluster.vertexCount; ++i)
        {
            const uint32_t globalIndex = cluster.vertexOffset + i;
            outVertices[globalIndex] = decompressVertex(compressed[globalIndex], cluster.bounds);
        }
    }
}

float maxPositionError(const Vertex& a, const Vertex& b)
{
    return std::max({ std::abs(a.position.x - b.position.x),
                      std::abs(a.position.y - b.position.y),
                      std::abs(a.position.z - b.position.z) });
}

float maxNormalError(const Vertex& a, const Vertex& b)
{
    return length(a.normal - b.normal);
}

} // namespace Falcor::Nanite
