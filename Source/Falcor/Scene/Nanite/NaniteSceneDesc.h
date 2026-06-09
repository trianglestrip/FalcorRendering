#pragma once

#include "NaniteTypes.h"
#include "Utils/Math/AABB.h"

namespace Falcor
{

struct NaniteMeshDesc
{
    uint32_t meshIndex = 0;
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    uint32_t materialIndex = 0;
    AABB bounds;
};

} // namespace Falcor
