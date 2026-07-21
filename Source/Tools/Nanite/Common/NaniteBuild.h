#pragma once

#include "NaniteToolAsset.h"
#include "NaniteObj.h"

#include <cstdint>

namespace FalcorRendering::NaniteTool
{
struct BuildOptions
{
    uint32_t clusterTriangleTarget = 128;
    uint32_t maxClusterVertices = 256;
    uint32_t workerCount = 0; ///< 0 selects the hardware thread count.
    bool dedupVerts = false;  ///< Merge identical vertices within each source mesh section.
};

Asset buildNaniteAsset(const InputScene& scene, const BuildOptions& options);
void embedSourceGeometry(Asset& asset, const InputScene& scene);
InputScene inputSceneFromSource(const Asset& asset);
}
