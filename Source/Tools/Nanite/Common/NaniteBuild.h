#pragma once

#include "NaniteAsset.h"
#include "NaniteObj.h"

#include <cstdint>

namespace FalcorRendering::NaniteTool
{
struct BuildOptions
{
    uint32_t clusterTriangleTarget = 128;
    uint32_t maxClusterVertices = 256;
    uint32_t workerCount = 0; ///< 0 selects the hardware thread count.
};

Asset buildNaniteAsset(const InputScene& scene, const BuildOptions& options);
}
