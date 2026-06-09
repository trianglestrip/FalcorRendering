#include "NaniteSceneGpu.h"

namespace Falcor
{

void createNaniteSceneGpuBuffers(
    ref<Device> pDevice,
    const std::vector<NaniteMeshDesc>& meshDesc,
    const std::vector<GeometryInstanceData>& instanceData,
    ref<Buffer>& outMeshDescBuffer,
    ref<Buffer>& outInstanceBuffer)
{
    outMeshDescBuffer = nullptr;
    outInstanceBuffer = nullptr;

    if (!meshDesc.empty())
    {
        std::vector<Nanite::NaniteGpuMeshDesc> gpuMeshDesc(meshDesc.size());
        for (size_t i = 0; i < meshDesc.size(); ++i)
        {
            const NaniteMeshDesc& src = meshDesc[i];
            Nanite::NaniteGpuMeshDesc& dst = gpuMeshDesc[i];
            dst.meshIndex = src.meshIndex;
            dst.firstCluster = src.firstCluster;
            dst.clusterCount = src.clusterCount;
            dst.materialIndex = src.materialIndex;
            dst.boundsMin[0] = src.bounds.minPoint.x;
            dst.boundsMin[1] = src.bounds.minPoint.y;
            dst.boundsMin[2] = src.bounds.minPoint.z;
            dst.boundsMax[0] = src.bounds.maxPoint.x;
            dst.boundsMax[1] = src.bounds.maxPoint.y;
            dst.boundsMax[2] = src.bounds.maxPoint.z;
        }

        outMeshDescBuffer = pDevice->createBuffer(
            gpuMeshDesc.size() * sizeof(Nanite::NaniteGpuMeshDesc),
            ResourceBindFlags::ShaderResource,
            MemoryType::DeviceLocal,
            gpuMeshDesc.data());
    }

    if (!instanceData.empty())
    {
        outInstanceBuffer = pDevice->createBuffer(
            instanceData.size() * sizeof(GeometryInstanceData),
            ResourceBindFlags::ShaderResource,
            MemoryType::DeviceLocal,
            instanceData.data());
    }
}

} // namespace Falcor
