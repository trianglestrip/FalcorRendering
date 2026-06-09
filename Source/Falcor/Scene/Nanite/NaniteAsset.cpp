#include "NaniteAsset.h"

#include <cstring>
#include <functional>
#include <stdexcept>

namespace Falcor
{
namespace
{
void copyFloat3(float dst[3], Nanite::Float3 v)
{
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
}

void copyBounds(float bmin[3], float bmax[3], const Nanite::Bounds& bounds)
{
    copyFloat3(bmin, bounds.min);
    copyFloat3(bmax, bounds.max);
}

uint32_t hashMaterialName(const std::string& name)
{
    return static_cast<uint32_t>(std::hash<std::string>{}(name));
}
} // namespace

ref<NaniteAsset> NaniteAsset::create(ref<Device> pDevice, const std::filesystem::path& path)
{
    Nanite::Asset asset = Nanite::readAsset(path);
    const std::vector<std::string> errors = Nanite::validateAsset(asset);
    if (!errors.empty())
    {
        std::string message = "Nanite asset validation failed:";
        for (const std::string& error : errors) message += "\n  " + error;
        throw std::runtime_error(message);
    }
    Nanite::validateRuntimeTables(asset);
    return make_ref<NaniteAsset>(pDevice, std::move(asset));
}

NaniteAsset::NaniteAsset(ref<Device> pDevice, Nanite::Asset asset)
    : mpDevice(pDevice)
    , mAsset(std::move(asset))
{
    buildGpuData();
}

void NaniteAsset::buildGpuData()
{
    mGpuClusters.resize(mAsset.clusters.size());
    for (size_t i = 0; i < mAsset.clusters.size(); ++i)
    {
        const Nanite::Cluster& cluster = mAsset.clusters[i];
        Nanite::NaniteGpuCluster& gpu = mGpuClusters[i];
        gpu.meshIndex = cluster.meshIndex;
        gpu.materialIndex = cluster.materialIndex;
        gpu.vertexOffset = cluster.vertexOffset;
        gpu.vertexCount = cluster.vertexCount;
        gpu.indexOffset = cluster.indexOffset;
        gpu.indexCount = cluster.indexCount;
        gpu.triangleCount = cluster.triangleCount;
        gpu.lodLevel = cluster.lodLevel;
        gpu.flags = cluster.flags;
        gpu.groupIndex = cluster.groupIndex;
        gpu.pageIndex = cluster.pageIndex;
        copyBounds(gpu.boundsMin, gpu.boundsMax, cluster.bounds);
        copyFloat3(gpu.sphereCenter, cluster.sphereCenter);
        gpu.sphereRadius = cluster.sphereRadius;
        copyFloat3(gpu.coneNormal, cluster.coneNormal);
        gpu.coneAngle = cluster.coneAngle;
        gpu.geometricError = cluster.geometricError;
        gpu.surfaceArea = cluster.surfaceArea;
    }

    mGpuHierarchy.resize(mAsset.hierarchyNodes.size());
    for (size_t i = 0; i < mAsset.hierarchyNodes.size(); ++i)
    {
        const Nanite::HierarchyNode& node = mAsset.hierarchyNodes[i];
        Nanite::NaniteGpuHierarchyNode& gpu = mGpuHierarchy[i];
        gpu.childNodeOffset = node.childNodeOffset;
        gpu.childNodeCount = node.childNodeCount;
        gpu.clusterGroupIndex = node.clusterGroupIndex;
        gpu.clusterOffset = node.clusterOffset;
        gpu.clusterCount = node.clusterCount;
        copyFloat3(gpu.sphereCenter, node.sphereCenter);
        gpu.sphereRadius = node.sphereRadius;
        gpu.minError = node.minError;
        gpu.maxError = node.maxError;
    }

    mGpuPages.resize(mAsset.pages.size());
    for (size_t i = 0; i < mAsset.pages.size(); ++i)
    {
        const Nanite::PageDesc& page = mAsset.pages[i];
        Nanite::NaniteGpuPageDesc& gpu = mGpuPages[i];
        gpu.firstCluster = page.firstCluster;
        gpu.clusterCount = page.clusterCount;
        gpu.flags = page.flags;
        gpu.byteSize = page.byteSize;
    }

    mGpuVertices.resize(mAsset.vertices.size());
    for (size_t i = 0; i < mAsset.vertices.size(); ++i)
    {
        const Nanite::Vertex& vertex = mAsset.vertices[i];
        Nanite::NaniteGpuVertex& gpu = mGpuVertices[i];
        copyFloat3(gpu.position, vertex.position);
        copyFloat3(gpu.normal, vertex.normal);
        gpu.texCoord[0] = vertex.texCoord.x;
        gpu.texCoord[1] = vertex.texCoord.y;
    }

    mGpuMaterials.resize(mAsset.materials.size());
    for (size_t i = 0; i < mAsset.materials.size(); ++i)
    {
        Nanite::NaniteGpuMaterial& gpu = mGpuMaterials[i];
        gpu.index = static_cast<uint32_t>(i);
        gpu.nameHash = hashMaterialName(mAsset.materials[i].name);
        gpu.reserved0 = 0;
        gpu.reserved1 = 0;
    }

    updateMemoryStats();
}

void NaniteAsset::updateMemoryStats()
{
    mMemoryStats = Nanite::computeGpuMemoryStats(mAsset);
}

void NaniteAsset::upload(RenderContext* pRenderContext)
{
    if (mUploaded) return;

    const ResourceBindFlags kBufferBind = ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;

    if (!mGpuClusters.empty())
    {
        mpClusterBuffer = mpDevice->createBuffer(
            mGpuClusters.size() * sizeof(Nanite::NaniteGpuCluster), kBufferBind, MemoryType::DeviceLocal, mGpuClusters.data());
    }

    if (!mGpuHierarchy.empty())
    {
        mpHierarchyBuffer = mpDevice->createBuffer(
            mGpuHierarchy.size() * sizeof(Nanite::NaniteGpuHierarchyNode), kBufferBind, MemoryType::DeviceLocal, mGpuHierarchy.data());
    }

    if (!mGpuPages.empty())
    {
        mpPageBuffer = mpDevice->createBuffer(
            mGpuPages.size() * sizeof(Nanite::NaniteGpuPageDesc), kBufferBind, MemoryType::DeviceLocal, mGpuPages.data());

        std::vector<uint32_t> residency(mGpuPages.size(), 1u);
        mpPageResidencyBuffer = mpDevice->createBuffer(
            residency.size() * sizeof(uint32_t), kBufferBind, MemoryType::DeviceLocal, residency.data());
    }

    if (!mGpuVertices.empty())
    {
        mpVertexBuffer = mpDevice->createBuffer(
            mGpuVertices.size() * sizeof(Nanite::NaniteGpuVertex), kBufferBind, MemoryType::DeviceLocal, mGpuVertices.data());
    }

    if (!mAsset.indices.empty())
    {
        mpIndexBuffer = mpDevice->createBuffer(
            mAsset.indices.size() * sizeof(uint32_t), kBufferBind, MemoryType::DeviceLocal, mAsset.indices.data());
    }

    if (!mGpuMaterials.empty())
    {
        mpMaterialBuffer = mpDevice->createBuffer(
            mGpuMaterials.size() * sizeof(Nanite::NaniteGpuMaterial), kBufferBind, MemoryType::DeviceLocal, mGpuMaterials.data());
    }

    mUploaded = true;
    (void)pRenderContext;
}

void NaniteAsset::uploadPageResidency(RenderContext* pRenderContext, const std::vector<uint32_t>& residency)
{
    if (!mpPageResidencyBuffer || residency.empty()) return;
    mpPageResidencyBuffer->setBlob(residency.data(), 0, residency.size() * sizeof(uint32_t));
    (void)pRenderContext;
}

void NaniteAsset::uploadPageCpuData(RenderContext* pRenderContext, uint32_t pageIndex)
{
    if (pageIndex >= mAsset.pages.size()) return;

    if (!mUploaded)
    {
        upload(pRenderContext);
    }

    const Nanite::PageDesc& page = mAsset.pages[pageIndex];
    (void)page;
    // Geometry for all pages currently lives in the fully uploaded vertex/index buffers.
    // This hook marks the CPU-side residency transition for future partial page uploads.
}

} // namespace Falcor
