#pragma once

#include "NaniteAssetData.h"

namespace Falcor::Nanite
{

#pragma pack(push, 1)
struct CompressedVertex
{
    uint16_t posX = 0;
    uint16_t posY = 0;
    uint16_t posZ = 0;
    uint32_t packedNormal = 0;
    uint16_t texU = 0;
    uint16_t texV = 0;
    uint16_t padding = 0;
};
static_assert(sizeof(CompressedVertex) == 16, "CompressedVertex must remain 16 bytes on disk.");
#pragma pack(pop)

uint32_t packOctahedralNormal(Float3 normal);
Float3 unpackOctahedralNormal(uint32_t packed);

void quantizePosition(Float3 position, const Bounds& bounds, uint16_t out[3]);
Float3 dequantizePosition(const uint16_t in[3], const Bounds& bounds);

CompressedVertex compressVertex(const Vertex& vertex, const Bounds& clusterBounds);
Vertex decompressVertex(const CompressedVertex& compressed, const Bounds& clusterBounds);

std::vector<CompressedVertex> compressVertices(const Asset& asset);
void decompressVertices(const std::vector<CompressedVertex>& compressed, const Asset& asset, std::vector<Vertex>& outVertices);

float maxPositionError(const Vertex& a, const Vertex& b);
float maxNormalError(const Vertex& a, const Vertex& b);

} // namespace Falcor::Nanite
