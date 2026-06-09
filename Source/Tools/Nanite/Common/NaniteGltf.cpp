#include "NaniteGltf.h"

#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace FalcorRendering::NaniteTool
{
namespace
{
struct Mat4
{
    std::array<float, 16> cols{};

    static Mat4 identity()
    {
        Mat4 m;
        m.cols = {
            1.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, 0.f, 0.f, 1.f,
        };
        return m;
    }
};

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 result;
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) sum += a.cols[static_cast<size_t>(k * 4 + row)] * b.cols[static_cast<size_t>(col * 4 + k)];
            result.cols[static_cast<size_t>(col * 4 + row)] = sum;
        }
    }
    return result;
}

Mat4 fromNode(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16)
    {
        Mat4 m;
        for (size_t i = 0; i < 16; ++i) m.cols[i] = static_cast<float>(node.matrix[i]);
        return m;
    }

    const float tx = static_cast<float>(node.translation.size() == 3 ? node.translation[0] : 0.0);
    const float ty = static_cast<float>(node.translation.size() == 3 ? node.translation[1] : 0.0);
    const float tz = static_cast<float>(node.translation.size() == 3 ? node.translation[2] : 0.0);

    const float qx = static_cast<float>(node.rotation.size() == 4 ? node.rotation[0] : 0.0);
    const float qy = static_cast<float>(node.rotation.size() == 4 ? node.rotation[1] : 0.0);
    const float qz = static_cast<float>(node.rotation.size() == 4 ? node.rotation[2] : 0.0);
    const float qw = static_cast<float>(node.rotation.size() == 4 ? node.rotation[3] : 1.0);

    const float sx = static_cast<float>(node.scale.size() == 3 ? node.scale[0] : 1.0);
    const float sy = static_cast<float>(node.scale.size() == 3 ? node.scale[1] : 1.0);
    const float sz = static_cast<float>(node.scale.size() == 3 ? node.scale[2] : 1.0);

    const float xx = qx * qx;
    const float yy = qy * qy;
    const float zz = qz * qz;
    const float xy = qx * qy;
    const float xz = qx * qz;
    const float yz = qy * qz;
    const float wx = qw * qx;
    const float wy = qw * qy;
    const float wz = qw * qz;

    Mat4 m = Mat4::identity();
    m.cols[0] = (1.f - 2.f * (yy + zz)) * sx;
    m.cols[1] = (2.f * (xy + wz)) * sx;
    m.cols[2] = (2.f * (xz - wy)) * sx;
    m.cols[4] = (2.f * (xy - wz)) * sy;
    m.cols[5] = (1.f - 2.f * (xx + zz)) * sy;
    m.cols[6] = (2.f * (yz + wx)) * sy;
    m.cols[8] = (2.f * (xz + wy)) * sz;
    m.cols[9] = (2.f * (yz - wx)) * sz;
    m.cols[10] = (1.f - 2.f * (xx + yy)) * sz;
    m.cols[12] = tx;
    m.cols[13] = ty;
    m.cols[14] = tz;
    return m;
}

Float3 transformPoint(const Mat4& m, Float3 p)
{
    const float x = m.cols[0] * p.x + m.cols[4] * p.y + m.cols[8] * p.z + m.cols[12];
    const float y = m.cols[1] * p.x + m.cols[5] * p.y + m.cols[9] * p.z + m.cols[13];
    const float z = m.cols[2] * p.x + m.cols[6] * p.y + m.cols[10] * p.z + m.cols[14];
    return { x, y, z };
}

Float3 transformNormal(const Mat4& m, Float3 n)
{
    const float x = m.cols[0] * n.x + m.cols[4] * n.y + m.cols[8] * n.z;
    const float y = m.cols[1] * n.x + m.cols[5] * n.y + m.cols[9] * n.z;
    const float z = m.cols[2] * n.x + m.cols[6] * n.y + m.cols[10] * n.z;
    return normalize({ x, y, z });
}

uint32_t getMaterialIndex(InputScene& scene, const tinygltf::Model& model, int materialIndex)
{
    std::string materialName = "default";
    if (materialIndex >= 0 && static_cast<size_t>(materialIndex) < model.materials.size())
    {
        const tinygltf::Material& material = model.materials[static_cast<size_t>(materialIndex)];
        materialName = material.name.empty() ? ("material_" + std::to_string(materialIndex)) : material.name;
    }

    for (uint32_t i = 0; i < scene.materialNames.size(); ++i)
    {
        if (scene.materialNames[i] == materialName) return i;
    }

    scene.materialNames.push_back(materialName);
    return static_cast<uint32_t>(scene.materialNames.size() - 1);
}

InputMesh& getMesh(InputScene& scene, std::unordered_map<std::string, size_t>& meshMap, const std::string& meshKey, uint32_t materialIndex)
{
    const std::string key = meshKey + "\n" + std::to_string(materialIndex);
    auto it = meshMap.find(key);
    if (it != meshMap.end()) return scene.meshes[it->second];

    InputMesh mesh;
    mesh.name = meshKey + ":" + scene.materialNames[materialIndex];
    mesh.materialIndex = materialIndex;
    mesh.bounds = emptyBounds();

    const size_t index = scene.meshes.size();
    scene.meshes.push_back(std::move(mesh));
    meshMap.emplace(key, index);
    return scene.meshes.back();
}

void appendTriangle(InputMesh& mesh, InputScene& scene, const Vertex& a, const Vertex& b, const Vertex& c)
{
    const uint32_t baseIndex = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(a);
    mesh.vertices.push_back(b);
    mesh.vertices.push_back(c);
    mesh.indices.push_back(baseIndex + 0);
    mesh.indices.push_back(baseIndex + 1);
    mesh.indices.push_back(baseIndex + 2);

    include(mesh.bounds, a.position);
    include(mesh.bounds, b.position);
    include(mesh.bounds, c.position);
    include(scene.bounds, a.position);
    include(scene.bounds, b.position);
    include(scene.bounds, c.position);
}

const unsigned char* accessorData(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
{
    if (accessor.bufferView < 0) return nullptr;
    const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
    if (view.buffer < 0) return nullptr;
    const tinygltf::Buffer& buffer = model.buffers[static_cast<size_t>(view.buffer)];
    return buffer.data.data() + view.byteOffset + accessor.byteOffset;
}

size_t accessorStride(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
{
    if (accessor.bufferView < 0) return 0;
    const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
    if (view.byteStride != 0) return view.byteStride;

    size_t componentSize = 0;
    switch (accessor.componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: componentSize = 1; break;
    case TINYGLTF_COMPONENT_TYPE_SHORT:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: componentSize = 2; break;
    case TINYGLTF_COMPONENT_TYPE_INT:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    case TINYGLTF_COMPONENT_TYPE_FLOAT: componentSize = 4; break;
    default: return 0;
    }

    size_t componentCount = 1;
    switch (accessor.type)
    {
    case TINYGLTF_TYPE_SCALAR: componentCount = 1; break;
    case TINYGLTF_TYPE_VEC2: componentCount = 2; break;
    case TINYGLTF_TYPE_VEC3: componentCount = 3; break;
    case TINYGLTF_TYPE_VEC4: componentCount = 4; break;
    default: return 0;
    }

    return componentSize * componentCount;
}

Float3 readFloat3(const tinygltf::Model& model, const tinygltf::Accessor& accessor, const unsigned char* base, size_t index)
{
    const size_t stride = accessorStride(model, accessor);
    const unsigned char* ptr = base + index * stride;
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && accessor.type == TINYGLTF_TYPE_VEC3)
    {
        const float* values = reinterpret_cast<const float*>(ptr);
        return { values[0], values[1], values[2] };
    }

    throw std::runtime_error("Unsupported POSITION accessor component/type combination.");
}

Float2 readFloat2(const tinygltf::Accessor& accessor, const unsigned char* base, size_t index, size_t stride)
{
    const unsigned char* ptr = base + index * stride;
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && accessor.type == TINYGLTF_TYPE_VEC2)
    {
        const float* values = reinterpret_cast<const float*>(ptr);
        return { values[0], values[1] };
    }

    return {};
}

uint32_t readIndex(const tinygltf::Accessor& accessor, const unsigned char* base, size_t index, size_t stride)
{
    const unsigned char* ptr = base + index * stride;
    switch (accessor.componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return ptr[0];
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const uint16_t*>(ptr);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return *reinterpret_cast<const uint32_t*>(ptr);
    default: throw std::runtime_error("Unsupported index accessor component type.");
    }
}

void triangulate(const std::vector<uint32_t>& polygon, int mode, std::vector<std::array<uint32_t, 3>>& triangles)
{
    if (polygon.size() < 3) return;

    switch (mode)
    {
    case TINYGLTF_MODE_TRIANGLES:
        for (size_t i = 0; i + 2 < polygon.size(); i += 3)
        {
            triangles.push_back({ polygon[i], polygon[i + 1], polygon[i + 2] });
        }
        break;
    case TINYGLTF_MODE_TRIANGLE_STRIP:
        for (size_t i = 0; i + 2 < polygon.size(); ++i)
        {
            if (i % 2 == 0) triangles.push_back({ polygon[i], polygon[i + 1], polygon[i + 2] });
            else triangles.push_back({ polygon[i + 1], polygon[i], polygon[i + 2] });
        }
        break;
    case TINYGLTF_MODE_TRIANGLE_FAN:
        for (size_t i = 1; i + 1 < polygon.size(); ++i)
        {
            triangles.push_back({ polygon[0], polygon[i], polygon[i + 1] });
        }
        break;
    default:
        break;
    }
}

void processPrimitive(
    InputScene& scene,
    std::unordered_map<std::string, size_t>& meshMap,
    const tinygltf::Model& model,
    const tinygltf::Mesh& mesh,
    const tinygltf::Primitive& primitive,
    const std::string& meshKey,
    const Mat4& transform)
{
    if (primitive.mode != TINYGLTF_MODE_TRIANGLES &&
        primitive.mode != TINYGLTF_MODE_TRIANGLE_STRIP &&
        primitive.mode != TINYGLTF_MODE_TRIANGLE_FAN)
    {
        return;
    }

    auto positionIt = primitive.attributes.find("POSITION");
    if (positionIt == primitive.attributes.end()) return;

    const tinygltf::Accessor& positionAccessor = model.accessors[static_cast<size_t>(positionIt->second)];
    const unsigned char* positionData = accessorData(model, positionAccessor);
    if (!positionData) return;

    const size_t positionStride = accessorStride(model, positionAccessor);

    const tinygltf::Accessor* normalAccessor = nullptr;
    const unsigned char* normalData = nullptr;
    size_t normalStride = 0;
    auto normalIt = primitive.attributes.find("NORMAL");
    if (normalIt != primitive.attributes.end())
    {
        normalAccessor = &model.accessors[static_cast<size_t>(normalIt->second)];
        normalData = accessorData(model, *normalAccessor);
        normalStride = accessorStride(model, *normalAccessor);
    }

    const tinygltf::Accessor* texCoordAccessor = nullptr;
    const unsigned char* texCoordData = nullptr;
    size_t texCoordStride = 0;
    auto texCoordIt = primitive.attributes.find("TEXCOORD_0");
    if (texCoordIt != primitive.attributes.end())
    {
        texCoordAccessor = &model.accessors[static_cast<size_t>(texCoordIt->second)];
        texCoordData = accessorData(model, *texCoordAccessor);
        texCoordStride = accessorStride(model, *texCoordAccessor);
    }

    std::vector<uint32_t> indices;
    if (primitive.indices >= 0)
    {
        const tinygltf::Accessor& indexAccessor = model.accessors[static_cast<size_t>(primitive.indices)];
        const unsigned char* indexData = accessorData(model, indexAccessor);
        if (!indexData) return;

        const size_t indexStride = accessorStride(model, indexAccessor);
        indices.resize(static_cast<size_t>(indexAccessor.count));
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = readIndex(indexAccessor, indexData, i, indexStride);
    }
    else
    {
        indices.resize(static_cast<size_t>(positionAccessor.count));
        for (size_t i = 0; i < indices.size(); ++i) indices[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
    }

    std::vector<std::array<uint32_t, 3>> triangles;
    triangulate(indices, primitive.mode, triangles);
    if (triangles.empty()) return;

    const uint32_t materialIndex = getMaterialIndex(scene, model, primitive.material);
    InputMesh& inputMesh = getMesh(scene, meshMap, meshKey, materialIndex);

    for (const std::array<uint32_t, 3>& tri : triangles)
    {
        Vertex vertices[3];
        Float3 positions[3];
        for (size_t v = 0; v < 3; ++v)
        {
            const uint32_t sourceIndex = tri[v];
            if (sourceIndex >= static_cast<uint32_t>(positionAccessor.count))
            {
                throw std::runtime_error("glTF primitive references an out-of-range vertex index.");
            }

            positions[v] = readFloat3(model, positionAccessor, positionData, sourceIndex);
            positions[v] = transformPoint(transform, positions[v]);

            if (normalAccessor && normalData)
            {
                Float3 normal = readFloat3(model, *normalAccessor, normalData, sourceIndex);
                vertices[v].normal = transformNormal(transform, normal);
            }
            else
            {
                vertices[v].normal = { 0.f, 1.f, 0.f };
            }

            if (texCoordAccessor && texCoordData)
            {
                vertices[v].texCoord = readFloat2(*texCoordAccessor, texCoordData, sourceIndex, texCoordStride);
            }
            else
            {
                vertices[v].texCoord = {};
            }

            vertices[v].position = positions[v];
        }

        if (!normalAccessor)
        {
            const Float3 faceNormal = normalize(cross(positions[1] - positions[0], positions[2] - positions[0]));
            for (Vertex& vertex : vertices) vertex.normal = faceNormal;
        }

        appendTriangle(inputMesh, scene, vertices[0], vertices[1], vertices[2]);
    }
}

void processMesh(
    InputScene& scene,
    std::unordered_map<std::string, size_t>& meshMap,
    const tinygltf::Model& model,
    int meshIndex,
    const std::string& nodeName,
    const Mat4& transform)
{
    if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= model.meshes.size()) return;

    const tinygltf::Mesh& mesh = model.meshes[static_cast<size_t>(meshIndex)];
    const std::string meshName = mesh.name.empty() ? nodeName : mesh.name;
    const std::string meshKey = nodeName.empty() ? meshName : nodeName + "/" + meshName;

    for (const tinygltf::Primitive& primitive : mesh.primitives)
    {
        processPrimitive(scene, meshMap, model, mesh, primitive, meshKey, transform);
    }
}

void processNode(
    InputScene& scene,
    std::unordered_map<std::string, size_t>& meshMap,
    const tinygltf::Model& model,
    int nodeIndex,
    const Mat4& parentTransform)
{
    if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= model.nodes.size()) return;

    const tinygltf::Node& node = model.nodes[static_cast<size_t>(nodeIndex)];
    const Mat4 localTransform = fromNode(node);
    const Mat4 worldTransform = multiply(parentTransform, localTransform);
    const std::string nodeName = node.name.empty() ? ("node_" + std::to_string(nodeIndex)) : node.name;

    if (node.mesh >= 0) processMesh(scene, meshMap, model, node.mesh, nodeName, worldTransform);

    for (int childIndex : node.children) processNode(scene, meshMap, model, childIndex, worldTransform);
}

void loadSceneGraph(InputScene& scene, std::unordered_map<std::string, size_t>& meshMap, const tinygltf::Model& model)
{
    if (!model.scenes.empty())
    {
        const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
        const tinygltf::Scene& gltfScene = model.scenes[static_cast<size_t>(sceneIndex)];
        for (int nodeIndex : gltfScene.nodes) processNode(scene, meshMap, model, nodeIndex, Mat4::identity());
    }
    else if (!model.nodes.empty())
    {
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
        {
            if (model.nodes[nodeIndex].mesh >= 0)
            {
                processNode(scene, meshMap, model, static_cast<int>(nodeIndex), Mat4::identity());
            }
        }
    }
    else
    {
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
        {
            processMesh(scene, meshMap, model, static_cast<int>(meshIndex), model.meshes[meshIndex].name, Mat4::identity());
        }
    }
}

std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
}

InputScene loadGltfScene(const std::filesystem::path& path)
{
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string error;
    std::string warning;

    const std::string extension = toLower(path.extension().string());
    const bool loaded = extension == ".glb"
        ? loader.LoadBinaryFromFile(&model, &error, &warning, path.string())
        : loader.LoadASCIIFromFile(&model, &error, &warning, path.string());

    if (!warning.empty()) { /* warnings are non-fatal */ }
    if (!loaded) throw std::runtime_error("Failed to load glTF file: " + path.string() + (error.empty() ? "" : (" (" + error + ")")));

    InputScene scene;
    scene.sourcePath = path;
    scene.bounds = emptyBounds();
    scene.materialNames.push_back("default");

    std::unordered_map<std::string, size_t> meshMap;
    loadSceneGraph(scene, meshMap, model);

    std::erase_if(scene.meshes, [](const InputMesh& mesh) { return mesh.indices.empty(); });
    if (scene.meshes.empty()) throw std::runtime_error("glTF file contains no triangle meshes: " + path.string());

    return scene;
}
}
