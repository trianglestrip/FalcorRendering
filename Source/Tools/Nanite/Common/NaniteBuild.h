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
};

Asset buildNaniteAsset(const InputScene& scene, const BuildOptions& options);
}
