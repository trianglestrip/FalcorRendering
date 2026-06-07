/*
 * PBRT scene data extraction and resource indexing.
 */

#include <pbrtio/Scene.h>

#include <pbrtio/Builder.h>
#include <pbrtio/Parser.h>
#include <pbrtio/PbrtSpectrum.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace pbrtio {

using namespace pbrtio::pbrt;

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

    const float radius = std::max(sceneRadius, 0.1f);
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
    const size_t vertexCount = std::min(worldPoints.size(), size_t(4));
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
        bmin = glm::min(bmin, p);
        bmax = glm::max(bmax, p);
    }
    if (out.meshes.empty()) {
        out.sceneCenter = float3(0.f);
        out.sceneRadius = 1.f;
    } else {
        out.sceneCenter = (bmin + bmax) * 0.5f;
        out.sceneRadius = std::max(length(bmax - bmin) * 0.5f, 0.5f);
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

bool loadPbrtScene(const std::filesystem::path& pbrtPath, PbrtLoadedScene& out) {
    if (!std::filesystem::exists(pbrtPath)) {
        return false;
    }

    BasicScene scene(pbrtPath.parent_path());
    BasicSceneBuilder builder(scene);
    parseFile(builder, pbrtPath);

    collectPbrtScene(scene, out);
    return !out.meshes.empty();
}

} // namespace pbrtio
