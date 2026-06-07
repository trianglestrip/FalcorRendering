/*
 * PBRT scene data extraction and resource indexing.
 */

#include <pbrtio/Scene.h>

#include <pbrtio/Builder.h>
#include <pbrtio/Parser.h>
#include <pbrtio/PbrtSpectrum.h>

#include <mio.hpp>
#include <taskflow.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace pbrtio {

using namespace pbrtio::pbrt;

namespace {

constexpr float kMaxImportedPositionMagnitude = 1e4f;

struct PlyProperty {
    std::string type;
    std::string name;
};

bool isFinite(const float2& v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

bool isFinite(const float3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool isUsablePosition(const float3& p) {
    return isFinite(p) && glm::all(glm::lessThan(glm::abs(p), float3(kMaxImportedPositionMagnitude)));
}

float3 safeNormalize(const float3& v, const float3& fallback) {
    if (!isFinite(v)) return fallback;
    const float len = length(v);
    return len > 1e-12f && std::isfinite(len) ? v / len : fallback;
}

size_t getPlyScalarSize(const std::string& type) {
    if (type == "float" || type == "float32" || type == "int" || type == "int32" || type == "uint" || type == "uint32")
        return 4;
    if (type == "double" || type == "float64")
        return 8;
    if (type == "char" || type == "int8" || type == "uchar" || type == "uint8")
        return 1;
    if (type == "short" || type == "int16" || type == "ushort" || type == "uint16")
        return 2;
    return 0;
}

template<typename T>
bool readScalar(const char*& cursor, const char* end, T& value) {
    if (static_cast<size_t>(end - cursor) < sizeof(T)) return false;
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

bool skipScalar(const char*& cursor, const char* end, const std::string& type) {
    const size_t size = getPlyScalarSize(type);
    if (size == 0 || static_cast<size_t>(end - cursor) < size) return false;
    cursor += size;
    return true;
}

bool readPlyFloat(const char*& cursor, const char* end, const std::string& type, float& value) {
    if (type == "float" || type == "float32")
        return readScalar(cursor, end, value);
    if (type == "double" || type == "float64") {
        double v = 0.0;
        if (!readScalar(cursor, end, v)) return false;
        value = static_cast<float>(v);
        return true;
    }

    value = 0.f;
    return skipScalar(cursor, end, type);
}

bool readPlyListCount(const char*& cursor, const char* end, const std::string& type, uint32_t& value) {
    if (type == "uchar" || type == "uint8") {
        uint8_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        value = v;
        return true;
    }
    if (type == "char" || type == "int8") {
        int8_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        if (v < 0) return false;
        value = static_cast<uint32_t>(v);
        return true;
    }
    if (type == "ushort" || type == "uint16") {
        uint16_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        value = v;
        return true;
    }
    if (type == "short" || type == "int16") {
        int16_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        if (v < 0) return false;
        value = static_cast<uint32_t>(v);
        return true;
    }
    if (type == "uint" || type == "uint32") {
        uint32_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        value = v;
        return true;
    }
    if (type == "int" || type == "int32") {
        int32_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        if (v < 0) return false;
        value = static_cast<uint32_t>(v);
        return true;
    }
    return false;
}

bool readPlyIndex(const char*& cursor, const char* end, const std::string& type, int32_t& value) {
    if (type == "int" || type == "int32")
        return readScalar(cursor, end, value);
    if (type == "uint" || type == "uint32") {
        uint32_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        if (v > static_cast<uint32_t>((std::numeric_limits<int32_t>::max)())) return false;
        value = static_cast<int32_t>(v);
        return true;
    }
    if (type == "short" || type == "int16") {
        int16_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        value = v;
        return true;
    }
    if (type == "ushort" || type == "uint16") {
        uint16_t v = 0;
        if (!readScalar(cursor, end, v)) return false;
        value = v;
        return true;
    }
    return false;
}

std::string_view trimLine(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}

bool loadBinaryLittleEndianPly(const std::filesystem::path& path, PbrtMeshData& mesh) {
    std::error_code error;
#ifdef _WIN32
    auto mapping = mio::make_mmap_source(path.wstring(), error);
#else
    auto mapping = mio::make_mmap_source(path.string(), error);
#endif
    if (error || !mapping.is_open() || mapping.empty()) return false;

    const char* data = mapping.data();
    const char* end = data + mapping.size();
    const char* cursor = data;

    auto nextLine = [&]() -> std::string_view {
        if (cursor >= end) return {};
        const char* begin = cursor;
        while (cursor < end && *cursor != '\n') ++cursor;
        const char* lineEnd = cursor;
        if (cursor < end && *cursor == '\n') ++cursor;
        return trimLine(std::string_view(begin, static_cast<size_t>(lineEnd - begin)));
    };

    if (nextLine() != "ply") return false;

    bool binaryLittleEndian = false;
    bool inVertex = false;
    bool hasFaceList = false;
    size_t vertexCount = 0;
    size_t faceCount = 0;
    std::string faceCountType;
    std::string faceIndexType;
    std::vector<PlyProperty> vertexProperties;

    while (cursor < end) {
        const std::string line(nextLine());
        if (line == "end_header") break;

        std::istringstream ls(line);
        std::string token;
        ls >> token;
        if (token == "format") {
            std::string format;
            ls >> format;
            binaryLittleEndian = format == "binary_little_endian";
        } else if (token == "element") {
            std::string element;
            size_t count = 0;
            ls >> element >> count;
            inVertex = element == "vertex";
            if (element == "vertex") vertexCount = count;
            else if (element == "face") faceCount = count;
        } else if (token == "property") {
            std::string type;
            ls >> type;
            if (inVertex) {
                std::string name;
                ls >> name;
                if (getPlyScalarSize(type) == 0) return false;
                vertexProperties.push_back({type, name});
            } else if (type == "list") {
                std::string name;
                ls >> faceCountType >> faceIndexType >> name;
                hasFaceList = name == "vertex_indices";
            }
        }
    }

    if (!binaryLittleEndian || vertexCount == 0 || faceCount == 0 || vertexProperties.empty() || !hasFaceList)
        return false;

    PbrtMeshData result;
    result.name = path.filename().string();
    result.positions.reserve(vertexCount);
    result.normals.reserve(vertexCount);
    result.texcoords.reserve(vertexCount);

    for (size_t i = 0; i < vertexCount; ++i) {
        float3 position(0.f);
        float3 normal(0.f, 1.f, 0.f);
        float2 texCoord(0.f);

        for (const auto& property : vertexProperties) {
            float value = 0.f;
            if (!readPlyFloat(cursor, end, property.type, value)) return false;

            if (property.name == "x") position.x = value;
            else if (property.name == "y") position.y = value;
            else if (property.name == "z") position.z = value;
            else if (property.name == "nx") normal.x = value;
            else if (property.name == "ny") normal.y = value;
            else if (property.name == "nz") normal.z = value;
            else if (property.name == "u" || property.name == "s") texCoord.x = value;
            else if (property.name == "v" || property.name == "t") texCoord.y = value;
        }

        result.positions.push_back(position);
        result.normals.push_back(safeNormalize(normal, float3(0.f, 1.f, 0.f)));
        result.texcoords.push_back(isFinite(texCoord) ? texCoord : float2(0.f));
    }

    result.indices.reserve(faceCount * 3);
    for (size_t faceIdx = 0; faceIdx < faceCount; ++faceIdx) {
        uint32_t count = 0;
        if (!readPlyListCount(cursor, end, faceCountType, count)) return false;

        std::array<int32_t, 16> localFaceIndices{};
        std::vector<int32_t> largeFaceIndices;
        int32_t* faceIndices = localFaceIndices.data();
        if (count > localFaceIndices.size()) {
            largeFaceIndices.resize(count);
            faceIndices = largeFaceIndices.data();
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (!readPlyIndex(cursor, end, faceIndexType, faceIndices[i])) return false;
        }
        if (count < 3) continue;

        bool validFace = true;
        for (uint32_t i = 0; i < count; ++i) {
            const int32_t index = faceIndices[i];
            if (index < 0 || static_cast<size_t>(index) >= result.positions.size() ||
                    !isUsablePosition(result.positions[index])) {
                validFace = false;
                break;
            }
        }
        if (!validFace) continue;

        for (uint32_t i = 1; i + 1 < count; ++i) {
            result.indices.push_back(static_cast<uint32_t>(faceIndices[0]));
            result.indices.push_back(static_cast<uint32_t>(faceIndices[i]));
            result.indices.push_back(static_cast<uint32_t>(faceIndices[i + 1]));
        }
    }

    if (result.positions.empty() || result.indices.empty()) return false;

    result.loaded = true;
    mesh = std::move(result);
    return true;
}

} // namespace

static float3 spectrumToColor(const Spectrum& spectrum) {
    return spectrumToRGB(spectrum);
}

static mat4f pbrtWorldTransform(const mat4f& pbrtTransform) {
    return pbrtTransform;
}

static float3 transformPoint(const mat4f& m, const float3& p) {
    return float3(m * float4(p, 1.f));
}

static float3 transformVector(const mat4f& m, const float3& v) {
    return float3(m * float4(v, 0.f));
}

static void extractCamera(const BasicScene& scene, float sceneRadius, PbrtCameraSettings& camera) {
    const auto& cam = scene.getCamera();
    const mat4f worldFromCamera = pbrtWorldTransform(cam.transform);

    camera.eye = transformPoint(worldFromCamera, float3(0.f));
    // PBRT camera looks along +Z in its local camera space.
    camera.target = transformPoint(worldFromCamera, float3(0.f, 0.f, 1.f));
    camera.up = normalize(transformVector(worldFromCamera, float3(0.f, 1.f, 0.f)));
    camera.verticalFovDegrees = cam.params.getFloat("fov", 45.f);

    const auto& film = scene.getFilm();
    const int xres = film.params.getInt("xresolution", 1280);
    const int yres = film.params.getInt("yresolution", 720);
    camera.aspectRatio = yres > 0 ? static_cast<float>(xres) / static_cast<float>(yres) : (16.f / 9.f);

    const float radius = (std::max)(sceneRadius, 0.1f);
    camera.nearPlane = radius * 0.01f;
    camera.farPlane = radius * 20.f;
}

static std::filesystem::path resolveSpectrumImagePath(const BasicScene& scene,
        const std::string& textureName, bool& outSRGB, float4* outUvTransform = nullptr) {
    if (outUvTransform) {
        *outUvTransform = float4{ 1.f, 1.f, 0.f, 0.f };
    }
    const auto& textures = scene.getSpectrumTextures();
    const auto it = textures.find(textureName);
    if (it == textures.end() || it->second.name != "imagemap") {
        return {};
    }
    const auto& entity = it->second;
    const std::string filename = entity.params.getString("filename", "");
    if (filename.empty()) {
        return {};
    }
    const std::string encoding = entity.params.getString("encoding", "");
    if (encoding == "linear") {
        outSRGB = false;
    } else if (encoding == "sRGB") {
        outSRGB = true;
    } else {
        outSRGB = true;
    }
    if (outUvTransform) {
        const std::string mapping = entity.params.getString("mapping", "uv");
        if (mapping == "uv") {
            *outUvTransform = float4{
                    entity.params.getFloat("uscale", 1.f),
                    entity.params.getFloat("vscale", 1.f),
                    entity.params.getFloat("udelta", 0.f),
                    entity.params.getFloat("vdelta", 0.f),
            };
        }
    }
    return scene.resolvePath(filename);
}

static void addDistantLight(const BasicScene& scene, const LightSceneEntity& entity,
        std::vector<PbrtLightInstance>& lights) {
    if (entity.name != "distant") {
        return;
    }
    const auto& params = entity.params;
    const mat4f xf = pbrtWorldTransform(entity.transform);

    PbrtLightInstance light;
    light.type = PbrtLightType::Directional;
    light.color = spectrumToColor(params.getSpectrum("L", Spectrum(float3(1.f)),
            [&](const std::filesystem::path& p) { return scene.resolvePath(p); }));
    const float scale = params.getFloat("scale", 1.f);
    const float illuminance = params.getFloat("illuminance", -1.f);
    light.intensity = scale * (illuminance > 0.f ? illuminance : 110000.f);

    const float3 from = params.getPoint3("from", float3(0.f));
    const float3 to = params.getPoint3("to", float3(0.f, 0.f, 1.f));
    light.direction = normalize(transformVector(xf, to - from));
    light.castShadows = true;
    lights.push_back(light);
}

static bool tryAddRectAreaLight(const BasicScene& scene, const ShapeSceneEntity& shape,
        std::vector<PbrtRectAreaLight>& areaLights) {
    if (shape.lightIndex < 0) {
        return false;
    }
    if (shape.name != "trianglemesh" && shape.name != "bilinearmesh") {
        return false;
    }

    const SceneEntity& area = scene.getAreaLight(shape.lightIndex);
    if (area.name != "diffuse") {
        return false;
    }

    const auto& params = shape.params;
    const std::vector<float3> points = params.getPoint3Array("P");
    if (points.size() < 3) {
        return false;
    }

    const mat4f xf = pbrtWorldTransform(shape.transform);
    std::vector<float3> worldPoints;
    worldPoints.reserve(points.size());
    for (const float3& p : points) {
        worldPoints.push_back(transformPoint(xf, p));
    }

    PbrtRectAreaLight areaMesh;
    const size_t vertexCount = (std::min)(worldPoints.size(), size_t(4));
    for (size_t i = 0; i < vertexCount; ++i) {
        areaMesh.positions[i] = worldPoints[i];
    }
    if (vertexCount == 3) {
        areaMesh.positions[3] = worldPoints[2];
        areaMesh.indices[0] = 0;
        areaMesh.indices[1] = 1;
        areaMesh.indices[2] = 2;
        areaMesh.indices[3] = 0;
        areaMesh.indices[4] = 2;
        areaMesh.indices[5] = 0;
    } else if (params.hasInt("indices")) {
        const auto& idx = params.getIntArray("indices");
        if (idx.size() >= 6) {
            for (int i = 0; i < 6; ++i) {
                areaMesh.indices[i] = static_cast<uint16_t>(idx[i]);
            }
        }
    }

    const float3 p0 = areaMesh.positions[0];
    const float3 p1 = areaMesh.positions[1];
    const float3 p2 = areaMesh.positions[2];
    const float3 p3 = areaMesh.positions[3];
    const float3 e1 = p1 - p0;
    const float3 e2 = p3 - p0;
    areaMesh.normal = normalize(cross(e1, e2));
    areaMesh.width = length(e1);
    areaMesh.height = length(e2);
    areaMesh.center = (p0 + p1 + p2 + p3) * 0.25f;

    areaMesh.radiance = spectrumToColor(area.params.getSpectrum("L", Spectrum(float3(1.f)),
            [&](const std::filesystem::path& p) { return scene.resolvePath(p); }));
    areaMesh.scale = area.params.getFloat("scale", 1.f);
    areaLights.push_back(areaMesh);
    return true;
}

static void addInfiniteLight(const BasicScene& scene, const LightSceneEntity& entity,
        PbrtEnvironmentLight& environment) {
    if (entity.name != "infinite") {
        return;
    }
    const auto& params = entity.params;
    bool sRGB = true;
    if (params.hasTexture("L")) {
        environment.mapPath = resolveSpectrumImagePath(scene, params.getTexture("L"), sRGB);
        environment.scale = params.getFloat("scale", 1.f);
        environment.valid = !environment.mapPath.empty();
    }
}

static void collectLights(const BasicScene& scene, const std::vector<ShapeSceneEntity>& shapes,
        PbrtLoadedScene& out) {
    out.lights.clear();
    out.areaLights.clear();
    out.environment = {};
    for (const auto& entity : scene.getLights()) {
        addDistantLight(scene, entity, out.lights);
        addInfiniteLight(scene, entity, out.environment);
    }
    for (const auto& shape : shapes) {
        tryAddRectAreaLight(scene, shape, out.areaLights);
    }
}

static void collectAllShapes(const BasicScene& scene, std::vector<ShapeSceneEntity>& out) {
    out = scene.getShapes();
    for (const auto& instance : scene.getInstances()) {
        auto it = scene.getInstanceDefinitions().find(instance.name);
        if (it == scene.getInstanceDefinitions().end()) {
            continue;
        }
        for (const auto& shape : it->second.shapes) {
            ShapeSceneEntity instShape = shape;
            instShape.transform = instance.transform * shape.transform;
            out.push_back(instShape);
        }
    }
}

static void resolveMaterial(const BasicScene& scene, const MaterialRef& ref,
        PbrtMeshInstance& inst) {
    if (std::holds_alternative<std::monostate>(ref)) {
        return;
    }
    const MaterialSceneEntity& mat = scene.getMaterial(ref);
    const auto& params = mat.params;
    if (mat.type == "diffuse" || mat.type == "coateddiffuse") {
        if (params.hasTexture("reflectance")) {
            inst.baseColorTexturePath = resolveSpectrumImagePath(scene,
                    params.getTexture("reflectance"), inst.baseColorTextureSRGB,
                    &inst.baseColorUvTransform);
        } else if (params.hasSpectrum("reflectance")) {
            inst.baseColor = spectrumToColor(params.getSpectrum("reflectance", Spectrum(float3(0.8f)),
                    [&](const std::filesystem::path& p) { return scene.resolvePath(p); }));
        }
        if (mat.type == "coateddiffuse") {
            const float ur = params.getFloat("uroughness", inst.roughness);
            const float vr = params.getFloat("vroughness", inst.roughness);
            inst.roughness = (ur + vr) * 0.5f;
        } else if (params.hasFloat("roughness")) {
            inst.roughness = params.getFloat("roughness", inst.roughness);
        }
    } else if (mat.type == "conductor" || mat.type == "coatedconductor") {
        inst.metallic = 1.f;
        const float ur = params.getFloat("uroughness", 0.15f);
        const float vr = params.getFloat("vroughness", inst.roughness);
        inst.roughness = (ur + vr) * 0.5f;
        inst.baseColor = float3(0.9f, 0.9f, 0.92f);
    } else if (mat.type == "dielectric" || mat.type == "thindielectric") {
        inst.roughness = params.getFloat("roughness", 0.05f);
        inst.baseColor = float3(0.95f);
    }
}

static void addShape(const BasicScene& scene, const ShapeSceneEntity& shape,
        std::vector<PbrtMeshInstance>& meshes) {
    if (shape.name != "plymesh") {
        return;
    }
    // Area-light carrier plymeshes are lights in PBRT, not renderable scene geometry.
    if (shape.lightIndex >= 0) {
        return;
    }
    const auto filename = shape.params.getString("filename", "");
    if (filename.empty()) {
        return;
    }

    PbrtMeshInstance inst;
    inst.plyPath = scene.resolvePath(filename);
    inst.transform = pbrtWorldTransform(shape.transform);

    if (!std::holds_alternative<std::monostate>(shape.materialRef)) {
        if (const auto* name = std::get_if<std::string>(&shape.materialRef)) {
            inst.materialName = *name;
        }
        resolveMaterial(scene, shape.materialRef, inst);
    }

    meshes.push_back(std::move(inst));
}

static std::string resourceKey(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

static std::string imageResourceKey(const std::filesystem::path& path, bool sRGB) {
    return resourceKey(path) + (sRGB ? "|srgb" : "|linear");
}

static size_t addImageResource(PbrtLoadedScene& scene,
        std::unordered_map<std::string, size_t>& imageMap,
        const std::filesystem::path& path, bool sRGB) {
    if (path.empty()) {
        return kInvalidResourceIndex;
    }

    const std::string key = imageResourceKey(path, sRGB);
    auto [it, inserted] = imageMap.emplace(key, scene.imageResources.size());
    if (inserted) {
        PbrtImageResource resource;
        resource.path = path;
        resource.sRGB = sRGB;
        scene.imageResources.push_back(std::move(resource));
    }
    return it->second;
}

static void buildResourceReferences(PbrtLoadedScene& scene) {
    scene.meshResources.clear();
    scene.imageResources.clear();

    std::unordered_map<std::string, size_t> meshMap;
    std::unordered_map<std::string, size_t> imageMap;

    for (auto& mesh : scene.meshes) {
        const std::string meshKey = resourceKey(mesh.plyPath);
        auto [it, inserted] = meshMap.emplace(meshKey, scene.meshResources.size());
        if (inserted) {
            PbrtMeshResource resource;
            resource.plyPath = mesh.plyPath;
            scene.meshResources.push_back(std::move(resource));
        }
        mesh.meshResourceIndex = it->second;

        mesh.baseColorTextureResourceIndex = addImageResource(scene, imageMap,
                mesh.baseColorTexturePath, mesh.baseColorTextureSRGB);
    }

    if (scene.environment.valid) {
        scene.environment.imageResourceIndex = addImageResource(scene, imageMap,
                scene.environment.mapPath, false);
    }
}

void collectPbrtScene(const BasicScene& scene, PbrtLoadedScene& out) {
    out.searchPath = scene.resolvePath(".");
    out.meshes.clear();

    for (const auto& shape : scene.getShapes()) {
        addShape(scene, shape, out.meshes);
    }

    for (const auto& instance : scene.getInstances()) {
        auto it = scene.getInstanceDefinitions().find(instance.name);
        if (it == scene.getInstanceDefinitions().end()) {
            continue;
        }
        for (const auto& shape : it->second.shapes) {
            ShapeSceneEntity instShape = shape;
            instShape.transform = instance.transform * shape.transform;
            addShape(scene, instShape, out.meshes);
        }
    }

    std::vector<ShapeSceneEntity> allShapes;
    collectAllShapes(scene, allShapes);
    collectLights(scene, allShapes, out);
    buildResourceReferences(out);

    float3 bmin(1e30f), bmax(-1e30f);
    for (const auto& mesh : out.meshes) {
        const float4 t = mesh.transform[3];
        const float3 p(t.x, t.y, t.z);
        bmin = (glm::min)(bmin, p);
        bmax = (glm::max)(bmax, p);
    }
    if (out.meshes.empty()) {
        out.sceneCenter = float3(0.f);
        out.sceneRadius = 1.f;
    } else {
        out.sceneCenter = (bmin + bmax) * 0.5f;
        out.sceneRadius = (std::max)(length(bmax - bmin) * 0.5f, 0.5f);
    }

    extractCamera(scene, out.sceneRadius, out.camera);

    // Fallback camera when PBRT camera transform is degenerate.
    if (!std::isfinite(length(out.camera.target - out.camera.eye)) ||
            length(out.camera.target - out.camera.eye) < 1e-4f) {
        const float dist = out.sceneRadius * 2.5f;
        out.camera.eye = out.sceneCenter + float3(0.f, dist * 0.35f, dist);
        out.camera.target = out.sceneCenter;
        out.camera.up = float3(0.f, 1.f, 0.f);
    }
}

bool loadPbrtMeshResource(PbrtMeshResource& resource) {
    resource.mesh = {};
    if (resource.plyPath.empty()) return false;
    return loadBinaryLittleEndianPly(resource.plyPath, resource.mesh);
}

bool loadPbrtSceneResources(PbrtLoadedScene& scene, const PbrtLoadOptions& options) {
    if (!options.loadMeshes || scene.meshResources.empty()) {
        return true;
    }

    tf::Taskflow taskflow;
    std::atomic_bool ok = true;
    for (auto& resource : scene.meshResources) {
        taskflow.emplace([&resource, &ok]() {
            if (!loadPbrtMeshResource(resource)) {
                ok = false;
            }
        });
    }

    const uint32_t hardwareWorkers = (std::max)(1u, std::thread::hardware_concurrency());
    const uint32_t workerCount = options.workerCount > 0 ? options.workerCount : hardwareWorkers;
    tf::Executor executor((std::max)(1u, workerCount));
    executor.run(taskflow).get();
    return ok.load();
}

bool loadPbrtScene(const std::filesystem::path& pbrtPath, PbrtLoadedScene& out, const PbrtLoadOptions& options) {
    if (!std::filesystem::exists(pbrtPath)) {
        return false;
    }

    BasicScene scene(pbrtPath.parent_path());
    BasicSceneBuilder builder(scene);
    parseFile(builder, pbrtPath);

    collectPbrtScene(scene, out);
    if (!loadPbrtSceneResources(out, options)) {
        return false;
    }
    return !out.meshes.empty();
}

} // namespace pbrtio
