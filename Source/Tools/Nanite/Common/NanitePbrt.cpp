#include "NanitePbrt.h"

#include <pbrtio/Scene.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <stdexcept>
#include <string>

namespace FalcorRendering::NaniteTool
{
namespace
{
Float3 toFloat3(const pbrtio::pbrt::float3& v)
{
    return { v.x, v.y, v.z };
}

Float2 toFloat2(const pbrtio::pbrt::float2& v)
{
    return { v.x, v.y };
}

uint32_t getMaterialIndex(InputScene& scene, const std::string& name)
{
    const std::string materialName = name.empty() ? "default" : name;
    for (uint32_t i = 0; i < scene.materialNames.size(); ++i)
    {
        if (scene.materialNames[i] == materialName) return i;
    }

    scene.materialNames.push_back(materialName);
    return static_cast<uint32_t>(scene.materialNames.size() - 1);
}

Float3 transformPoint(const pbrtio::pbrt::mat4f& transform, Float3 point)
{
    const pbrtio::pbrt::float4 transformed = transform * pbrtio::pbrt::float4(point.x, point.y, point.z, 1.f);
    return { transformed.x, transformed.y, transformed.z };
}

Float3 transformNormal(const pbrtio::pbrt::mat4f& transform, Float3 normal)
{
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
    const glm::vec3 transformed = glm::normalize(normalMatrix * glm::vec3(normal.x, normal.y, normal.z));
    return { transformed.x, transformed.y, transformed.z };
}

void appendMeshInstance(
    InputScene& scene,
    const pbrtio::PbrtMeshInstance& instance,
    const pbrtio::PbrtMeshData& meshData,
    uint32_t instanceIndex
)
{
    if (meshData.positions.empty() || meshData.indices.empty()) return;

    const uint32_t materialIndex = getMaterialIndex(scene, instance.materialName);
    const std::string baseName = instance.plyPath.stem().string();
    const std::string objectName = instanceIndex > 0 ? baseName + "_" + std::to_string(instanceIndex) : baseName;

    InputMesh mesh;
    mesh.name = objectName + ":" + scene.materialNames[materialIndex];
    mesh.materialIndex = materialIndex;
    mesh.bounds = emptyBounds();

    mesh.vertices.reserve(meshData.positions.size());
    for (size_t i = 0; i < meshData.positions.size(); ++i)
    {
        Vertex vertex;
        vertex.position = transformPoint(instance.transform, toFloat3(meshData.positions[i]));
        vertex.normal = i < meshData.normals.size()
            ? transformNormal(instance.transform, toFloat3(meshData.normals[i]))
            : Float3{ 0.f, 1.f, 0.f };
        vertex.texCoord = i < meshData.texcoords.size() ? toFloat2(meshData.texcoords[i]) : Float2{};
        mesh.vertices.push_back(vertex);
        include(mesh.bounds, vertex.position);
        include(scene.bounds, vertex.position);
    }

    mesh.indices = meshData.indices;
    scene.meshes.push_back(std::move(mesh));
}
}

InputScene loadPbrtScene(const std::filesystem::path& path, uint32_t workerCount)
{
    pbrtio::PbrtLoadOptions options;
    options.loadMeshes = true;
    options.workerCount = workerCount;

    pbrtio::PbrtLoadedScene loaded;
    if (!pbrtio::loadPbrtScene(path, loaded, options))
    {
        throw std::runtime_error("Failed to load PBRT scene: " + path.string());
    }

    InputScene scene;
    scene.sourcePath = path;
    scene.bounds = emptyBounds();
    scene.materialNames.push_back("default");

    uint32_t instanceIndex = 0;
    for (const pbrtio::PbrtMeshInstance& instance : loaded.meshes)
    {
        const pbrtio::PbrtMeshResource* resource = loaded.getMeshResource(instance.meshResourceIndex);
        if (!resource || !resource->mesh.loaded) continue;
        appendMeshInstance(scene, instance, resource->mesh, instanceIndex++);
    }

    std::erase_if(scene.meshes, [](const InputMesh& mesh) { return mesh.indices.empty(); });
    if (scene.meshes.empty())
    {
        throw std::runtime_error("PBRT scene contains no triangle meshes: " + path.string());
    }

    return scene;
}
}
