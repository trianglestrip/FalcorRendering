#pragma once

#include "NaniteAssetData.h"
#include "NaniteGpuTypes.h"

#include "Core/Object.h"
#include "Core/API/Buffer.h"
#include "Core/API/Device.h"

namespace Falcor
{
using NaniteMemoryStats = Nanite::GpuMemoryStats;

class NaniteAsset : public Object
{
    FALCOR_OBJECT(NaniteAsset)
public:
    static ref<NaniteAsset> create(ref<Device> pDevice, const std::filesystem::path& path);
    static ref<NaniteAsset> create(ref<Device> pDevice, Nanite::Asset asset);

    NaniteAsset(ref<Device> pDevice, Nanite::Asset asset);

    void upload(RenderContext* pRenderContext);

    const Nanite::Asset& getCpuAsset() const { return mAsset; }
    NaniteMemoryStats getMemoryStats() const { return mMemoryStats; }

    const ref<Buffer>& getClusterBuffer() const { return mpClusterBuffer; }
    const ref<Buffer>& getHierarchyBuffer() const { return mpHierarchyBuffer; }
    const ref<Buffer>& getPageBuffer() const { return mpPageBuffer; }
    const ref<Buffer>& getVertexBuffer() const { return mpVertexBuffer; }
    const ref<Buffer>& getIndexBuffer() const { return mpIndexBuffer; }
    const ref<Buffer>& getMaterialBuffer() const { return mpMaterialBuffer; }
    const ref<Buffer>& getPageResidencyBuffer() const { return mpPageResidencyBuffer; }

    void uploadPageResidency(RenderContext* pRenderContext, const std::vector<uint32_t>& residency);
    void uploadPageCpuData(RenderContext* pRenderContext, uint32_t pageIndex);

    ref<Device> getDevice() const { return mpDevice; }

private:
    void buildGpuData();
    void updateMemoryStats();

    ref<Device> mpDevice;
    Nanite::Asset mAsset;
    std::vector<Nanite::NaniteGpuCluster> mGpuClusters;
    std::vector<Nanite::NaniteGpuHierarchyNode> mGpuHierarchy;
    std::vector<Nanite::NaniteGpuPageDesc> mGpuPages;
    std::vector<Nanite::NaniteGpuVertex> mGpuVertices;
    std::vector<Nanite::NaniteGpuMaterial> mGpuMaterials;

    ref<Buffer> mpClusterBuffer;
    ref<Buffer> mpHierarchyBuffer;
    ref<Buffer> mpPageBuffer;
    ref<Buffer> mpVertexBuffer;
    ref<Buffer> mpIndexBuffer;
    ref<Buffer> mpMaterialBuffer;
    ref<Buffer> mpPageResidencyBuffer;

    NaniteMemoryStats mMemoryStats{};
    bool mUploaded = false;
};

} // namespace Falcor
