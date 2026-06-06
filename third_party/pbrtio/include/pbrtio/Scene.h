/*
 * Parse PBRT v4 into renderer-neutral scene data and resource reference tables.
 */
#pragma once

#include <pbrtio/PbrtMathTypes.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace pbrtio {

namespace pbrt {
class BasicScene;
}

using PbrtResourceIndex = size_t;
inline constexpr PbrtResourceIndex kInvalidResourceIndex = std::numeric_limits<PbrtResourceIndex>::max();

struct PbrtMeshInstance {
    std::filesystem::path plyPath;
    PbrtResourceIndex meshResourceIndex = kInvalidResourceIndex;
    pbrt::mat4f transform;
    pbrt::float3 baseColor{ 0.75f, 0.75f, 0.75f };
    float roughness = 0.45f;
    float metallic = 0.f;
    std::string materialName;
    /** Resolved imagemap path for spectrum reflectance textures (empty = solid color only). */
    std::filesystem::path baseColorTexturePath;
    PbrtResourceIndex baseColorTextureResourceIndex = kInvalidResourceIndex;
    bool baseColorTextureSRGB = true;
    /** PBRT UVMapping as (uscale, vscale, udelta, vdelta). Image textures flip v at sample time. */
    pbrt::float4 baseColorUvTransform{ 1.f, 1.f, 0.f, 0.f };
};

struct PbrtMeshResource {
    std::filesystem::path plyPath;
};

struct PbrtImageResource {
    std::filesystem::path path;
    bool sRGB = true;
};

enum class PbrtLightType {
    Directional,
    Point,
};

struct PbrtLightInstance {
    PbrtLightType type = PbrtLightType::Directional;
    pbrt::float3 color{ 1.f };
    float intensity = 1.f;
    pbrt::float3 position{ 0.f };
    pbrt::float3 direction{ 0.f, -1.f, 0.f };
    bool castShadows = true;
};

/**
 * PBRT rect area light (AreaLightSource "diffuse" on trianglemesh/bilinearmesh).
 * radiance is spectrum radiance L (W/(m^2*sr)); geometry is a world-space quad.
 */
struct PbrtRectAreaLight {
    pbrt::float3 positions[4]{};
    uint16_t indices[6]{ 0, 1, 2, 0, 2, 3 };
    pbrt::float3 normal{ 0.f, 1.f, 0.f };
    pbrt::float3 center{ 0.f };
    float width = 1.f;
    float height = 1.f;
    pbrt::float3 radiance{ 1.f };
    float scale = 1.f;
};

using PbrtAreaLightMesh = PbrtRectAreaLight;

/** PBRT Light "infinite" environment map (equirectangular HDR/EXR/etc.). */
struct PbrtEnvironmentLight {
    std::filesystem::path mapPath;
    PbrtResourceIndex imageResourceIndex = kInvalidResourceIndex;
    float scale = 1.f;
    bool valid = false;
};

struct PbrtCameraSettings {
    pbrt::float3 eye{ 0.f, 0.f, 5.f };
    pbrt::float3 target{ 0.f, 0.f, 0.f };
    pbrt::float3 up{ 0.f, 1.f, 0.f };
    float verticalFovDegrees = 45.f;
    float aspectRatio = 16.f / 9.f;
    float nearPlane = 0.01f;
    float farPlane = 100.f;
};

struct PbrtLoadedScene {
    std::filesystem::path searchPath;
    std::vector<PbrtMeshResource> meshResources;
    std::vector<PbrtImageResource> imageResources;
    std::vector<PbrtMeshInstance> meshes;
    std::vector<PbrtLightInstance> lights;
    std::vector<PbrtRectAreaLight> areaLights;
    PbrtEnvironmentLight environment;
    PbrtCameraSettings camera;
    pbrt::float3 sceneCenter{ 0.f };
    float sceneRadius = 1.f;

    const PbrtMeshResource* getMeshResource(PbrtResourceIndex index) const {
        return index < meshResources.size() ? &meshResources[index] : nullptr;
    }

    const PbrtImageResource* getImageResource(PbrtResourceIndex index) const {
        return index < imageResources.size() ? &imageResources[index] : nullptr;
    }

    std::span<const PbrtMeshResource> getMeshResources() const { return meshResources; }
    std::span<const PbrtImageResource> getImageResources() const { return imageResources; }
    std::span<const PbrtMeshInstance> getMeshes() const { return meshes; }
};

/** Collect renderer-neutral scene/resource references from an already parsed PBRT scene. */
void collectPbrtScene(const pbrt::BasicScene& scene, PbrtLoadedScene& out);

/** Parse PBRT file into scene description (meshes, lights, camera). */
bool loadPbrtScene(const std::filesystem::path& pbrtPath, PbrtLoadedScene& out);

} // namespace pbrtio
