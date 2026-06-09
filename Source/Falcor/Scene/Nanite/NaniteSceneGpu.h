#pragma once

#include "NaniteGpuTypes.h"
#include "NaniteSceneDesc.h"
#include "../SceneTypes.slang"

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"

namespace Falcor
{

/** Upload Nanite mesh descriptors and instance data to GPU buffers. */
void createNaniteSceneGpuBuffers(
    ref<Device> pDevice,
    const std::vector<NaniteMeshDesc>& meshDesc,
    const std::vector<GeometryInstanceData>& instanceData,
    ref<Buffer>& outMeshDescBuffer,
    ref<Buffer>& outInstanceBuffer);

} // namespace Falcor
