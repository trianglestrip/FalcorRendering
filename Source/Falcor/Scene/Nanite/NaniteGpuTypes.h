#pragma once

#include "NaniteAssetData.h"

#include <cstdint>

namespace Falcor::Nanite
{

struct NaniteGpuCluster
{
    uint32_t meshIndex;
    uint32_t materialIndex;
    uint32_t vertexOffset;
    uint32_t vertexCount;
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t triangleCount;
    uint32_t lodLevel;
    uint32_t flags;
    uint32_t groupIndex;
    uint32_t pageIndex;
    float boundsMin[3];
    float boundsMax[3];
    float sphereCenter[3];
    float sphereRadius;
    float coneNormal[3];
    float coneAngle;
    float geometricError;
    float surfaceArea;
};

struct NaniteGpuHierarchyNode
{
    uint32_t childNodeOffset;
    uint32_t childNodeCount;
    uint32_t clusterGroupIndex;
    uint32_t clusterOffset;
    uint32_t clusterCount;
    float sphereCenter[3];
    float sphereRadius;
    float minError;
    float maxError;
};

struct NaniteGpuPageDesc
{
    uint32_t firstCluster;
    uint32_t clusterCount;
    uint32_t flags;
    uint32_t byteSize;
};

struct NaniteGpuVertex
{
    float position[3];
    float normal[3];
    float texCoord[2];
};

struct NaniteGpuMaterial
{
    uint32_t index;
    uint32_t nameHash;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct NaniteGpuMeshDesc
{
    uint32_t meshIndex;
    uint32_t firstCluster;
    uint32_t clusterCount;
    uint32_t materialIndex;
    float boundsMin[3];
    float boundsMax[3];
};

struct GpuMemoryStats
{
    uint64_t clusterBytes = 0;
    uint64_t hierarchyBytes = 0;
    uint64_t pageBytes = 0;
    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;
    uint64_t materialBytes = 0;
    uint64_t residencyBytes = 0;
    uint64_t totalGpuBytes = 0;
    uint32_t clusterCount = 0;
    uint32_t pageCount = 0;
    uint32_t materialCount = 0;
};

GpuMemoryStats computeGpuMemoryStats(const Asset& asset);
void validateRuntimeTables(const Asset& asset);

} // namespace Falcor::Nanite
