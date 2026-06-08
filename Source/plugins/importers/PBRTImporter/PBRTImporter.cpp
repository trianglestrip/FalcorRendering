/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/

/**
 * pbrt-v4 scene importer.
 *
 * This implements a scene importer for pbrt-v4. As Falcor only supports
 * a small subset of the features available in pbrt-v4, the importer has
 * to take some approximations when importing scenes or simply ignore
 * certain objects/attributes in the scene.
 *
 * The following describes how the importer works on a high level
 * (see PBRTImporter::import() for more details):
 * - A scene file is parsed using pbrt::parseFile() or pbrt::parseString().
 * - The parser dispatches commands via the pbrt::ParserTarget interface.
 * - The pbrt::BasicSceneBuilder (implementing pbrt::ParserTarget) builds
 * a pbrt::BasicScene representing the parsed scene.
 * - The buildScene() function in this file takes a pbrt::BasicScene
 * and generates the Falcor scene using Falcor::SceneBuilder.
 *
 * The parser code and pbrt::BasicScene are derived directly from pbrt-v4
 * code. The code was simplified in a few areas but should more or less
 * reflect what is done in pbrt-v4.
 *
 * The code to convert to a Falcor scene is mostly contained in this file.
 * This is where a lot of approximations take place, e.g. for material
 * conversion. The current code is trying to emit warnings for all
 * unhandled object types and parameters. Also, each handler is annotated
 * with the parameter set that should be handled. All of this information
 * was collected from reading pbrt-v4 code, as there is no specification
 * available for the scene format. This means that while it reflects the
 * current state of pbrt-v4 (as of March 2022), things may change in the
 * future.
 */

#include "PBRTImporter.h"

#include <pbrtio/Parser.h>
#include <pbrtio/Builder.h>
#include <pbrtio/Helpers.h>
#include <pbrtio/LoopSubdivide.h>
#include <pbrtio/Scene.h>

#include "Core/Error.h"
#include "Core/API/Device.h"
#include "Utils/Settings/Settings.h"
#include "Utils/Logger.h"
#include "Utils/Timing/TimeReport.h"
#include "Utils/Math/FalcorMath.h"
#include "Utils/Math/FNVHash.h"
#include "Utils/Math/MathHelpers.h"
#include "Scene/Importer.h"
#include "Scene/Material/Material.h"
#include "Scene/Material/StandardMaterial.h"
#include "Scene/Material/RGLMaterial.h"
#include "Scene/Material/HairMaterial.h"
#include "Scene/Material/PBRT/PBRTDiffuseMaterial.h"
#include "Scene/Material/PBRT/PBRTCoatedDiffuseMaterial.h"
#include "Scene/Material/PBRT/PBRTConductorMaterial.h"
#include "Scene/Material/PBRT/PBRTCoatedConductorMaterial.h"
#include "Scene/Material/PBRT/PBRTDielectricMaterial.h"
#include "Scene/Material/PBRT/PBRTDiffuseTransmissionMaterial.h"
#include "Scene/Curves/CurveTessellation.h"

#include <taskflow.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <limits>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Falcor
{
namespace pbrt
{
using namespace ::pbrtio::pbrt;
namespace io = ::pbrtio::pbrt;

float2 toFalcor(const io::float2& v)
{
    return float2(v.x, v.y);
}

float3 toFalcor(const io::float3& v)
{
    return float3(v.x, v.y, v.z);
}

float4 toFalcor(const io::float4& v)
{
    return float4(v.x, v.y, v.z, v.w);
}

float4x4 toFalcor(const io::float4x4& m)
{
    return float4x4{
        m[0][0], m[1][0], m[2][0], m[3][0],
        m[0][1], m[1][1], m[2][1], m[3][1],
        m[0][2], m[1][2], m[2][2], m[3][2],
        m[0][3], m[1][3], m[2][3], m[3][3],
    };
}

io::float3 toPbrt(const float3& v)
{
    return io::float3(v.x, v.y, v.z);
}

io::Spectrum makePbrtSpectrum(const float3& v)
{
    return io::Spectrum(toPbrt(v));
}

io::Spectrum makePbrtSpectrum(float value)
{
    return io::Spectrum(io::float3(value));
}

std::vector<float2> toFalcor(const std::vector<io::float2>& values)
{
    std::vector<float2> result;
    result.reserve(values.size());
    for (const auto& value : values)
        result.push_back(toFalcor(value));
    return result;
}

std::vector<float3> toFalcor(const std::vector<io::float3>& values)
{
    std::vector<float3> result;
    result.reserve(values.size());
    for (const auto& value : values)
        result.push_back(toFalcor(value));
    return result;
}

const float4x4 kYtoZ = {
    // clang-format off
    1.f, 0.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f,
    0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 0.f, 1.f,
    // clang-format on
};

const float4x4 kInvertZ = {
    // clang-format off
    1.f, 0.f, 0.f,  0.f,
    0.f, 1.f, 0.f,  0.f,
    0.f, 0.f, -1.f, 0.f,
    0.f, 0.f, 0.f,  1.f,
    // clang-format on
};

/**
 * Holds the results from creating a camera.
 */
struct Camera
{
    Falcor::ref<Falcor::Camera> pCamera;
    float4x4 transform = float4x4::identity();
};

/**
 * Holds the results from creating a light.
 */
struct Light
{
    Falcor::ref<Falcor::Light> pLight;
    Falcor::ref<Falcor::EnvMap> pEnvMap;
};

/**
 * Represents a float texture.
 * These can be unassigned (std::monostate), a constant float or a texture.
 * Note: pbrt-v4 supports many additional texture types that we currently don't support and represent here.
 */
struct FloatTexture
{
    std::variant<std::monostate, float, Falcor::ref<Texture>> texture;
    Transform transform;

    bool isConstant() const { return std::holds_alternative<float>(texture); }
    float getConstant() const
    {
        FALCOR_ASSERT(isConstant());
        return std::get<float>(texture);
    }
};

/**
 * Represents a spectrum texture.
 * These can be unassigned (std::monostate), a constant spectrum or a texture.
 * Note: pbrt-v4 supports many additional texture types that we currently don't support and represent here.
 */
struct SpectrumTexture
{
    SpectrumType spectrumType = SpectrumType::Albedo;
    std::variant<std::monostate, Spectrum, Falcor::ref<Texture>> texture;
    Transform transform;

    bool isConstant() const { return std::holds_alternative<Spectrum>(texture); }
    Spectrum getConstant() const
    {
        FALCOR_ASSERT(isConstant());
        return std::get<Spectrum>(texture);
    }
};

struct Medium
{};

/**
 * Holds the results from creating a shape.
 */
struct Shape
{
    Falcor::ref<Falcor::TriangleMesh> pTriangleMesh;
    float4x4 transform = float4x4::identity();
    Falcor::ref<Falcor::Material> pMaterial;
};

struct PlyMeshKey
{
    size_t processJobIndex = 0;
    const Falcor::Material* pMaterial = nullptr;

    bool operator==(const PlyMeshKey& other) const
    {
        return processJobIndex == other.processJobIndex && pMaterial == other.pMaterial;
    }
};

struct PlyMeshKeyHash
{
    size_t operator()(const PlyMeshKey& key) const
    {
        size_t h = std::hash<size_t>{}(key.processJobIndex);
        h ^= std::hash<const void*>{}(key.pMaterial) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct PlyProcessKey
{
    ::pbrtio::PbrtResourceIndex resourceIndex = ::pbrtio::kInvalidResourceIndex;
    bool reverseOrientation = false;
    float4x4 textureTransform = float4x4::identity();

    bool operator==(const PlyProcessKey& other) const
    {
        return resourceIndex == other.resourceIndex &&
               reverseOrientation == other.reverseOrientation &&
               std::memcmp(&textureTransform, &other.textureTransform, sizeof(textureTransform)) == 0;
    }
};

struct PlyProcessKeyHash
{
    size_t operator()(const PlyProcessKey& key) const
    {
        size_t h = std::hash<size_t>{}(key.resourceIndex);
        h ^= std::hash<bool>{}(key.reverseOrientation) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= fnvHashArray64(&key.textureTransform, sizeof(key.textureTransform)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct PlyProcessJob
{
    ::pbrtio::PbrtResourceIndex resourceIndex = ::pbrtio::kInvalidResourceIndex;
    Falcor::ref<Falcor::Material> pMaterial;
    bool reverseOrientation = false;
    bool valid = false;
    Falcor::SceneBuilder::ProcessedMesh mesh;
};

struct PlyMeshJob
{
    std::string filename;
    size_t processJobIndex = 0;
    Falcor::ref<Falcor::Material> pMaterial;
    Falcor::MeshID meshID;
};

struct PlyGeometryJob
{
    std::filesystem::path path;
    ::pbrtio::PbrtMeshResource* pResource = nullptr;
    bool valid = false;
    bool used = false;
};

struct PlyMeshPlan
{
    std::unordered_map<const ShapeSceneEntity*, size_t> shapeToPlyJob;
    std::vector<PlyGeometryJob> geometryJobs;
    std::vector<PlyProcessJob> processJobs;
    std::vector<PlyMeshJob> plyJobs;
};

/**
 * Holds a list of aggregated curve shapes (strands).
 * PBRT's curve shape only contains a single strand.
 * We aggregate strands that have the same transform/material
 * so we can process them as a collection.
 */
struct CurveAggregate
{
    using Key = std::tuple<float4x4, const Falcor::Material*>;
    struct KeyHash
    {
        std::size_t operator()(const Key& key) const { return fnvHashArray64(&key, sizeof(key)); }
    };

    float4x4 transform = float4x4::identity();
    Falcor::ref<Falcor::Material> pMaterial;
    uint32_t splitDepth;

    std::vector<uint32_t> strands; ///< Contains the number of points in each strand.
    std::vector<float3> points;    ///< Concatenated list of points of all strands.
    std::vector<float> widths;     ///< Concatenated list of widths of all strands.
};

struct InstanceDefinition
{
    std::vector<std::pair<MeshID, float4x4>> meshes;  // List of meshID + transform
    std::vector<std::pair<CurveID, float4x4>> curves; // List of curveID + transfrom
};

struct BuilderContext
{
    BasicScene& scene;
    SceneBuilder& builder;
    ::pbrtio::PbrtLoadedScene& resources;

    std::map<std::string, FloatTexture> floatTextures;
    std::map<std::string, SpectrumTexture> spectrumTextures;

    std::map<std::string, Medium> media;

    std::map<std::string, Falcor::ref<Falcor::Material>> namedMaterials;
    std::vector<Falcor::ref<Falcor::Material>> materials;

    Falcor::ref<Falcor::Material> pDefaultMaterial;

    std::unordered_map<CurveAggregate::Key, CurveAggregate, CurveAggregate::KeyHash> curveAggregates;

    std::map<std::string, InstanceDefinition> instanceDefinitions;

    size_t curveCount = 0;

    bool usePBRTMaterials = false;
    bool rotateImageTextures90 = false;
    bool rotateImageTextures180 = false;
    bool flipTextureV = true;
    uint32_t plyIOConcurrency = 0;
    bool logPlyTiming = true;
    bool fastPlyTangents = true;
    uint32_t nonConstantRoughnessFallbackCount = 0;
    uint32_t anisotropicRoughnessFallbackCount = 0;

    Falcor::ref<Falcor::Material> getMaterial(const MaterialRef& materialRef)
    {
        Falcor::ref<Falcor::Material> pMaterial;

        if (const uint32_t* pIndex = std::get_if<uint32_t>(&materialRef))
        {
            FALCOR_ASSERT(*pIndex >= 0 && *pIndex < materials.size());
            pMaterial = materials[*pIndex];
        }
        else if (const std::string* pName = std::get_if<std::string>(&materialRef))
        {
            auto it = namedMaterials.find(*pName);
            FALCOR_ASSERT(it != namedMaterials.end());
            pMaterial = it->second;
        }

        if (!pMaterial)
        {
            if (!pDefaultMaterial)
            {
                pDefaultMaterial = Falcor::StandardMaterial::create(builder.getDevice(), "Default");
                pDefaultMaterial->setDoubleSided(true);
            }
            return pDefaultMaterial;
        }

        return pMaterial;
    }

    Resolver resolver = [this](const std::filesystem::path& path) { return scene.resolvePath(path); };
};

inline void warnUnsupportedType(const FileLoc& loc, const std::string_view category, const std::string_view name)
{
    logWarning(loc, "{} type '{}' is currently not supported and ignored.", category, name);
}

inline void warnUnsupportedParameters(const ParameterDictionary& params, std::vector<std::string> names)
{
    for (const auto& name : names)
    {
        if (params.hasParameter(name))
        {
            logWarning(params.getParameterLoc(name), "Parameter '{}' is currently not supported and ignored.", name);
        }
    }
}

float3 spectrumToRGB(const Spectrum& spectrum, SpectrumType spectrumType)
{
    // TODO: Handle spectrum type.
    if (auto pRGB = std::get_if<io::float3>(&spectrum))
    {
        return toFalcor(*pRGB);
    }
    else if (auto pPiecewiseLinearSpectrum = std::get_if<io::PiecewiseLinearSpectrum>(&spectrum))
    {
        (void)pPiecewiseLinearSpectrum;
        return toFalcor(io::spectrumToRGB(spectrum));
    }
    else if (auto pBlackbodySpectrum = std::get_if<io::BlackbodySpectrum>(&spectrum))
    {
        (void)pBlackbodySpectrum;
        return toFalcor(io::spectrumToRGB(spectrum));
    }
    else
    {
        FALCOR_THROW("Unhandled spectrum variant.");
    }
}

Transform createPbrtImageTextureTransform(const ParameterDictionary& params, const FileLoc& loc, bool rotate90, bool rotate180, bool flipTextureV)
{
    Transform transform;

    const std::string mapping = params.getString("mapping", "uv");
    if (mapping != "uv")
    {
        logWarning(loc, "Image texture mapping '{}' is currently not supported. Using 'uv' mapping instead.", mapping);
    }

    const float uscale = params.getFloat("uscale", 1.f);
    const float vscale = params.getFloat("vscale", 1.f);
    const float udelta = params.getFloat("udelta", 0.f);
    const float vdelta = params.getFloat("vdelta", 0.f);

    if (std::abs(uscale) < 1e-8f || std::abs(vscale) < 1e-8f)
    {
        logWarning(loc, "Image texture has a zero UV scale. Using default texture transform.");
        return transform;
    }

    // PBRT image textures apply UVMapping and then flip t at sample time:
    //   s = uscale * u + udelta
    //   t = 1 - (vscale * v + vdelta)
    //
    // Falcor's image upload / material sampling convention is opposite to PBRT's image-space
    // vertical origin for these scene textures. Keep pbrtio renderer-agnostic and apply that
    // backend adaptation here by optionally flipping the sampled texture V again:
    //   t = vscale * v + vdelta
    //
    // SceneBuilder stores inverse(material.textureTransform) into mesh UVs, so set
    // the material transform to the inverse of the desired PBRT sampling transform.
    if (rotate90)
    {
        if (flipTextureV)
            logWarning(loc, "PBRTImporter:flipTextureV is not applied to rotateImageTextures90 textures yet.");

        // Rotate the PBRT sampling coordinates 90 degrees around the UV center:
        //   s' = t
        //   t' = 1 - s
        // The values below are the inverse of that complete sampling transform.
        transform.setScaling(float3(-1.f / vscale, 1.f / uscale, 1.f));
        transform.setRotation(math::quatFromAngleAxis(math::radians(90.f), float3(0.f, 0.f, 1.f)));
        transform.setTranslation(float3((1.f - udelta) / uscale, (1.f - vdelta) / vscale, 0.f));
    }
    else if (rotate180)
    {
        transform.setScaling(float3(-1.f / uscale, -1.f / vscale, 1.f));
        transform.setTranslation(float3((1.f - udelta) / uscale, (1.f - vdelta) / vscale, 0.f));
    }
    else
    {
        if (flipTextureV)
        {
            transform.setScaling(float3(1.f / uscale, 1.f / vscale, 1.f));
            transform.setTranslation(float3(-udelta / uscale, -vdelta / vscale, 0.f));
        }
        else
        {
            transform.setScaling(float3(1.f / uscale, -1.f / vscale, 1.f));
            transform.setTranslation(float3(-udelta / uscale, (1.f - vdelta) / vscale, 0.f));
        }
    }
    return transform;
}

float3 getSpectrumAsRGB(
    BuilderContext& ctx,
    const ParameterDictionary& params,
    const std::string& name,
    float3 def,
    SpectrumType spectrumType
)
{
    auto spectrum = params.getSpectrum(name, makePbrtSpectrum(def), ctx.resolver);
    return spectrumToRGB(spectrum, spectrumType);
}

std::optional<SpectrumTexture> getSpectrumTextureOrNull(
    BuilderContext& ctx,
    const ParameterDictionary& params,
    const std::string& name,
    SpectrumType spectrumType
)
{
    if (params.hasTexture(name))
    {
        auto texture = params.getTexture(name);
        auto loc = params.getParameterLoc(name);
        if (texture.empty())
            throwError(loc, "No texture name provided for parameter '{}'.", name);

        auto it = ctx.spectrumTextures.find(texture);
        if (it == ctx.spectrumTextures.end())
            throwError(loc, "Cannot find spectrum texture named '{}' for parameter '{}'.", texture, name);

        SpectrumTexture spectrumTexture = it->second;
        spectrumTexture.spectrumType = spectrumType;
        return spectrumTexture;
    }
    else if (params.hasSpectrum(name))
    {
        SpectrumTexture spectrumTexture;
        spectrumTexture.texture = params.getSpectrum(name, makePbrtSpectrum(float3(0.f)), ctx.resolver);
        spectrumTexture.spectrumType = spectrumType;
        return spectrumTexture;
    }

    return {};
}

SpectrumTexture getSpectrumTexture(
    BuilderContext& ctx,
    const ParameterDictionary& params,
    const std::string& name,
    float3 def,
    SpectrumType spectrumType
)
{
    if (auto spectrumTexture = getSpectrumTextureOrNull(ctx, params, name, spectrumType))
        return *spectrumTexture;

    SpectrumTexture spectrumTexture;
    spectrumTexture.texture = makePbrtSpectrum(def);
    spectrumTexture.spectrumType = spectrumType;
    return spectrumTexture;
}

void assignSpectrumTexture(
    const SpectrumTexture& spectrumTexture,
    std::function<void(float3)> constantSetter,
    std::function<void(Falcor::ref<Texture>)> textureSetter,
    std::function<void(const Transform&)> textureTransformSetter = {}
)
{
    if (const auto* pSpectrum = std::get_if<Spectrum>(&spectrumTexture.texture))
        constantSetter(spectrumToRGB(*pSpectrum, spectrumTexture.spectrumType));
    else if (const auto* pTexture = std::get_if<Falcor::ref<Texture>>(&spectrumTexture.texture))
    {
        textureSetter(*pTexture);
        if (textureTransformSetter)
            textureTransformSetter(spectrumTexture.transform);
    }
}

std::optional<FloatTexture> getFloatTextureOrNull(BuilderContext& ctx, const ParameterDictionary& params, const std::string& name)
{
    if (params.hasTexture(name))
    {
        auto texture = params.getTexture(name);
        auto loc = params.getParameterLoc(name);
        if (texture.empty())
            throwError(loc, "No texture name provided for parameter '{}'.", name);

        auto it = ctx.floatTextures.find(texture);
        if (it == ctx.floatTextures.end())
            throwError(loc, "Cannot find float texture named '{}' for parameter '{}'.", texture, name);

        return it->second;
    }
    else if (params.hasFloat(name))
    {
        FloatTexture floatTexture;
        floatTexture.texture = params.getFloat(name, 0.f);
        return floatTexture;
    }

    return {};
}

FloatTexture getFloatTexture(BuilderContext& ctx, const ParameterDictionary& params, const std::string& name, float def)
{
    if (auto floatTexture = getFloatTextureOrNull(ctx, params, name))
        return *floatTexture;

    FloatTexture floatTexture;
    floatTexture.texture = def;
    return floatTexture;
}

float getFloatTextureConstantOnly(BuilderContext& ctx, const ParameterDictionary& params, const std::string& name, float def)
{
    if (auto floatTexture = getFloatTextureOrNull(ctx, params, name))
    {
        if (floatTexture->isConstant())
            return floatTexture->getConstant();

        logWarning(params.getParameterLoc(name), "Non-constant '{}' is currently not supported. Using constant value of '{}'.", name, def);
    }
    return def;
}

// Note: This function returns roughness as an NDF "alpha" value.
float2 getRoughness(
    BuilderContext& ctx,
    const SceneEntity& entity,
    const std::string& roughnessName = "roughness",
    const std::string& uroughnessName = "uroughness",
    const std::string& vroughnessName = "vroughness"
)
{
    const auto& params = entity.params;

    auto uroughness = getFloatTextureOrNull(ctx, params, uroughnessName);
    auto vroughness = getFloatTextureOrNull(ctx, params, vroughnessName);
    if (!uroughness)
        uroughness = getFloatTexture(ctx, params, roughnessName, 0.f);
    if (!vroughness)
        vroughness = getFloatTexture(ctx, params, roughnessName, 0.f);
    auto remaproughness = params.getBool("remaproughness", true);

    if (!uroughness->isConstant() || !vroughness->isConstant())
    {
        float2 fallback{0.5f};
        ctx.nonConstantRoughnessFallbackCount++;
        return fallback;
    }

    float2 roughness{uroughness->getConstant() + vroughness->getConstant()};

    // "remaproughness" determines if roughness represents a "linear" roughness value and should be converted to the NDF "alpha" value.
    // PBRT always uses the Trowbridge-Reitz / GGX NDF.
    if (remaproughness)
        roughness = sqrt(roughness);

    return roughness;
}

// Note: This function returns roughness as an NDF "alpha" value.
float getScalarRoughness(
    BuilderContext& ctx,
    const SceneEntity& entity,
    const std::string& roughnessName = "roughness",
    const std::string& uroughnessName = "uroughness",
    const std::string& vroughnessName = "vroughness"
)
{
    float2 roughness = getRoughness(ctx, entity, roughnessName, uroughnessName, vroughnessName);

    if (roughness.x != roughness.y)
        ctx.anisotropicRoughnessFallbackCount++;

    return 0.5f * (roughness.x + roughness.y);
}

float getScalarEta(BuilderContext& ctx, const SceneEntity& entity, const std::string& etaName = "eta")
{
    const auto& params = entity.params;

    float eta = params.getFloat(etaName, 1.5f);
    if (params.hasSpectrum(etaName))
    {
        // This is a very crude approximation to get a scalar index of refraction value from a spectrum.
        auto rgb = getSpectrumAsRGB(ctx, params, etaName, float3(1.5f), SpectrumType::Unbounded);
        eta = (rgb.r + rgb.g + rgb.b) / 3.f;
    }

    return eta;
}

float3 fresnelDieletricConductor(float3 eta, float3 k, float cosTheta)
{
    float cosTheta2 = cosTheta * cosTheta;
    float sinTheta2 = 1.f - cosTheta2;
    float3 eta2 = eta * eta;
    float3 k2 = k * k;

    float3 t0 = eta2 - k2 - sinTheta2;
    float3 a2plusb2 = sqrt(t0 * t0 + 4.f * eta2 * k2);
    float3 t1 = a2plusb2 + cosTheta2;
    float3 a = sqrt(0.5f * (a2plusb2 + t0));
    float3 t2 = 2.f * a * cosTheta;
    float3 Rs = (t1 - t2) / (t1 + t2);

    float3 t3 = cosTheta2 * a2plusb2 + sinTheta2 * sinTheta2;
    float3 t4 = t2 * sinTheta2;
    float3 Rp = Rs * (t3 - t4) / (t3 + t4);

    return 0.5f * (Rp + Rs);
}

std::pair<float3, float3> getConductorEtaK(
    BuilderContext& ctx,
    const SceneEntity& entity,
    const std::string& reflectanceName = "reflectance",
    const std::string& etaName = "eta",
    const std::string& kName = "k"
)
{
    const auto& params = entity.params;

    auto eta = getSpectrumTextureOrNull(ctx, params, etaName, SpectrumType::Unbounded);
    auto k = getSpectrumTextureOrNull(ctx, params, kName, SpectrumType::Unbounded);
    auto reflectance = getSpectrumTextureOrNull(ctx, params, reflectanceName, SpectrumType::Albedo);

    if (reflectance && (eta || k))
        throwError(entity.loc, "Both '{}' and '{}' and '{}' can't be provided.", reflectanceName, etaName, kName);

    if (reflectance)
    {
        float3 r(0.5f);
        if (reflectance->isConstant())
            r = spectrumToRGB(reflectance->getConstant(), SpectrumType::Albedo);
        else
            logWarning(
                entity.loc, "Non-constant '{}' is not currently supported. Using constant {},{},{} instead.", reflectanceName, r.x, r.y, r.z
            );

        // Avoid r == 1 NaN case.
        r = clamp(r, float3(0.f), float3(0.9999f));
        float3 etaRgb(1.f);
        float3 kRgb = 2.f * sqrt(r) / sqrt(1.f - r);
        return {etaRgb, kRgb};
    }

    Spectrum etas = makePbrtSpectrum(float3(0.27f, 0.68f, 1.32f));
    Spectrum ks = makePbrtSpectrum(float3(3.61f, 2.62f, 2.29f));

    if (eta)
    {
        if (eta->isConstant())
            etas = eta->getConstant();
        else
            logWarning(
                entity.loc, "Non-constant '{}' is not currently supported. Using constant 'metal-Cu-eta' spectrum instead.", etaName
            );
    }

    if (k)
    {
        if (k->isConstant())
            ks = k->getConstant();
        else
            logWarning(entity.loc, "Non-constant '{}' is not currently supported. Using constant 'metal-Cu-k' spectrum instead.", kName);
    }

    return {spectrumToRGB(etas, SpectrumType::Unbounded), spectrumToRGB(ks, SpectrumType::Unbounded)};
}

float3 getConductorSpecularAlbedo(
    BuilderContext& ctx,
    const SceneEntity& entity,
    const std::string& reflectanceName = "reflectance",
    const std::string& etaName = "eta",
    const std::string& kName = "k"
)
{
    auto [eta, k] = getConductorEtaK(ctx, entity, reflectanceName, etaName, kName);

    // Approximate by reflectance at incident angle.
    return fresnelDieletricConductor(eta, k, 1.f);
}

Camera createCamera(BuilderContext& ctx, const CameraSceneEntity& entity)
{
    const auto& type = entity.name;
    const auto& params = entity.params;

    Camera camera;

    if (type == "perspective")
    {
        warnUnsupportedParameters(params, {"screenwindow"});

        auto lensradius = params.getFloat("lensradius", 0.f);
        auto focaldistance = params.getFloat("focaldistance", 1e6f);
        auto fov = params.getFloat("fov", 90.f);
        const auto& film = ctx.scene.getFilm();
        const int xres = film.params.getInt("xresolution", 1280);
        const int yres = film.params.getInt("yresolution", 720);
        const float defaultAspect = yres > 0 ? float(xres) / float(yres) : 16.f / 9.f;
        const float frameAspectRatio = params.getFloat("frameaspectratio", defaultAspect);

        // PBRT defines fov along the narrower image axis. Falcor stores vertical fov via focal length.
        float fovY = fov;
        if (frameAspectRatio < 1.f)
            fovY = math::degrees(2.f * std::atan(std::tan(math::radians(fov) * 0.5f) / frameAspectRatio));

        auto pCamera = Falcor::Camera::create("Camera");
        pCamera->setApertureRadius(lensradius);
        pCamera->setFocalDistance(focaldistance);
        pCamera->setAspectRatio(frameAspectRatio);
        float focalLength = fovYToFocalLength(math::radians(fovY), 24.f);
        pCamera->setFocalLength(focalLength);

        camera.pCamera = pCamera;
        camera.transform = toFalcor(entity.transform);
    }
    else if (type == "orthographic" || type == "realistic" || type == "spherical")
    {
        warnUnsupportedType(entity.loc, "Camera", entity.name);
    }
    else
    {
        throwError(entity.loc, "Unknown camera type '{}'.", type);
    }

    return camera;
}

Light createLight(BuilderContext& ctx, const LightSceneEntity& entity)
{
    const auto& type = entity.name;
    const auto& params = entity.params;

    Light light;

    // Unsupported light types.
    if (type == "point" || type == "spot" || type == "goniometric" || type == "projection")
    {
        warnUnsupportedType(entity.loc, "Light", entity.name);
    }
    else if (type == "distant")
    {
        // Parameters:
        // Spectrum L, Float scale, Point3 from, Point3 to, Float illuminance
        auto L = params.getSpectrum("L", makePbrtSpectrum(float3(1.f)), ctx.resolver);
        auto scale = params.getFloat("scale", 1.f);
        auto from = toFalcor(params.getPoint3("from", io::float3(0.f, 0.f, 0.f)));
        auto to = toFalcor(params.getPoint3("to", io::float3(0.f, 0.f, 1.f)));
        auto illuminance = params.getFloat("illuminance", -1.f);

        // TODO: Missing spectrum normalization to 1 nit

        float3 intensity = spectrumToRGB(L, SpectrumType::Illuminant);
        intensity *= scale;
        if (illuminance > 0.f)
            intensity *= illuminance;

        float3 direction = normalize(transformVector(toFalcor(entity.transform), to - from));

        auto pDirectionalLight = Falcor::DirectionalLight::create("DirectionalLight");
        pDirectionalLight->setIntensity(intensity);
        pDirectionalLight->setWorldDirection(direction);
        light.pLight = pDirectionalLight;
    }
    else if (type == "infinite")
    {
        // Parameters:
        // Spectrum[] L, Float scale, Point3[] portal, String filename, Float illuminance
        warnUnsupportedParameters(params, {"portal", "illuminance"});

        auto L = params.getSpectrumArray("L", ctx.resolver);
        auto scale = params.getFloat("scale", 1.f);
        auto filename = params.getString("filename", "");

        if (!L.empty() && !filename.empty())
            throwError(entity.loc, "Can't specify both emission 'L' and 'filename' for infinite light.");

        if (!L.empty())
        {
            // Falcor doesn't have constant infinite emitter.
            // We create a one pixel env map for now.
            float4 data{spectrumToRGB(L[0], SpectrumType::Illuminant), 0.f};
            auto pTexture = ctx.builder.getDevice()->createTexture2D(1, 1, ResourceFormat::RGBA32Float, 1, Texture::kMaxPossible, &data);
            auto pEnvMap = EnvMap::create(ctx.builder.getDevice(), pTexture);

            light.pEnvMap = pEnvMap;
        }
        else if (!filename.empty())
        {
            auto path = ctx.resolver(filename);
            auto pTexture = Falcor::Texture::createFromFile(ctx.builder.getDevice(), path, false, false);
            auto pEnvMap = Falcor::EnvMap::create(ctx.builder.getDevice(), pTexture);
            pEnvMap->setIntensity(scale);

            float3 rotation;
            math::extractEulerAngleXYZ(toFalcor(entity.transform), rotation.x, rotation.y, rotation.z);
            pEnvMap->setRotation(math::degrees(rotation));

            light.pEnvMap = pEnvMap;
        }
    }
    else
    {
        throwError(entity.loc, "Unknown light type '{}'.", type);
    }

    return light;
}

// Shared imagemap loading: resolves filter, encoding, path, and creates the GPU texture.
inline Falcor::ref<Texture> loadImageMapTexture(
    BuilderContext& ctx, const TextureSceneEntity& entity, Transform& outTransform)
{
    const auto& params = entity.params;
    warnUnsupportedParameters(params, {"maxanisotropy", "wrap", "invert"});

    auto path = ctx.resolver(params.getString("filename", ""));

    auto filter = params.getString("filter", "bilinear");
    if (filter != "bilinear" && filter != "trilinear")
    {
        logWarning(entity.loc, "Filter '{}' is not currently supported, using 'bilinear' instead.", filter);
        filter = "bilinear";
    }
    bool generateMips = (filter == "trilinear");

    std::string defaultEncoding = hasExtension(path, "png") ? "sRGB" : "linear";
    auto encoding = params.getString("encoding", defaultEncoding);
    if (encoding != "linear" && encoding != "sRGB")
    {
        logWarning(entity.loc, "Encoding '{}' is not currently supported, using '{}' instead.", encoding, defaultEncoding);
        encoding = defaultEncoding;
    }
    bool sRGB = (encoding == "sRGB");

    outTransform = createPbrtImageTextureTransform(params, entity.loc, ctx.rotateImageTextures90, ctx.rotateImageTextures180, ctx.flipTextureV);
    return Falcor::Texture::createFromFile(ctx.builder.getDevice(), path, generateMips, sRGB);
}

// Check if a type is in the unsupported set and emit warning.
inline bool isUnsupportedTextureType(const std::string& type)
{
    static const std::unordered_set<std::string> kUnsupported = {
        "mix", "directionmix", "bilerp", "checkerboard", "dots", "ptex",
    };
    return kUnsupported.count(type) > 0;
}

FloatTexture createFloatTexture(BuilderContext& ctx, const TextureSceneEntity& entity)
{
    const auto& type = entity.name;
    const auto& params = entity.params;

    FloatTexture floatTexture;

    const float4x4 textureFromRender = toFalcor(entity.transform);
    if (textureFromRender != float4x4::identity())
        logWarning(entity.loc, "Texture transforms are currently not supported and ignored.");

    if (type == "constant")
    {
        floatTexture.texture = params.getFloat("value", 1.f);
    }
    else if (type == "scale")
    {
        const float scale = params.hasFloat("scale") ? params.getFloat("scale", 1.f) : 1.f;
        if (params.hasTexture("scale"))
            (void)params.getTexture("scale");

        bool scaleApplied = false;
        if (params.hasFloat("tex"))
        {
            floatTexture.texture = params.getFloat("tex", 1.f) * scale;
            scaleApplied = true;
        }
        else if (params.hasTexture("tex"))
        {
            const auto texName = params.getTexture("tex");
            auto it = ctx.floatTextures.find(texName);
            floatTexture = (it != ctx.floatTextures.end()) ? it->second : FloatTexture{};
            if (it == ctx.floatTextures.end())
                floatTexture.texture = scale;
        }
        else
        {
            floatTexture.texture = scale;
        }

        if (!scaleApplied && floatTexture.isConstant())
            floatTexture.texture = floatTexture.getConstant() * scale;
    }
    else if (type == "imagemap")
    {
        (void)params.getFloat("scale", 1.f);
        floatTexture.texture = loadImageMapTexture(ctx, entity, floatTexture.transform);
    }
    else if (isUnsupportedTextureType(type) || type == "fbm" || type == "wrinkled" || type == "windy")
    {
        warnUnsupportedType(entity.loc, "Float texture", entity.name);
    }
    else
    {
        throwError(entity.loc, "Unknown float texture type '{}'.", type);
    }

    return floatTexture;
}

SpectrumTexture createSpectrumTexture(BuilderContext& ctx, const TextureSceneEntity& entity)
{
    const auto& type = entity.name;
    const auto& params = entity.params;

    SpectrumTexture spectrumTexture;
    spectrumTexture.spectrumType = SpectrumType::Albedo;

    const float4x4 textureFromRender = toFalcor(entity.transform);
    if (textureFromRender != float4x4::identity())
        logWarning(entity.loc, "Texture transforms are currently not supported and ignored.");

    if (type == "constant")
    {
        spectrumTexture.texture = params.getSpectrum("value", makePbrtSpectrum(float3(1.f)), ctx.resolver);
    }
    else if (type == "scale")
    {
        const float scale = params.hasFloat("scale") ? params.getFloat("scale", 1.f) : 1.f;
        if (params.hasTexture("scale"))
            (void)params.getTexture("scale");

        bool scaleApplied = false;
        if (params.hasSpectrum("tex"))
        {
            const auto tex = params.getSpectrum("tex", makePbrtSpectrum(float3(1.f)), ctx.resolver);
            spectrumTexture.texture = makePbrtSpectrum(spectrumToRGB(tex, spectrumTexture.spectrumType) * scale);
            scaleApplied = true;
        }
        else if (params.hasTexture("tex"))
        {
            const auto texName = params.getTexture("tex");
            auto it = ctx.spectrumTextures.find(texName);
            spectrumTexture = (it != ctx.spectrumTextures.end()) ? it->second : SpectrumTexture{};
            if (it == ctx.spectrumTextures.end())
                spectrumTexture.texture = makePbrtSpectrum(float3(scale));
        }
        else
        {
            spectrumTexture.texture = makePbrtSpectrum(float3(scale));
            scaleApplied = true;
        }

        if (!scaleApplied && spectrumTexture.isConstant())
            spectrumTexture.texture = makePbrtSpectrum(spectrumToRGB(spectrumTexture.getConstant(), spectrumTexture.spectrumType) * scale);
    }
    else if (type == "imagemap")
    {
        (void)params.getFloat("scale", 1.f);
        spectrumTexture.texture = loadImageMapTexture(ctx, entity, spectrumTexture.transform);
    }
    else if (isUnsupportedTextureType(type) || type == "marble")
    {
        warnUnsupportedType(entity.loc, "Spectrum texture", entity.name);
    }
    else
    {
        throwError(entity.loc, "Unknown spectrum texture type '{}'.", type);
    }

    return spectrumTexture;
}

Falcor::ref<Falcor::Material> createMaterial(BuilderContext& ctx, const MaterialSceneEntity& entity, bool isAreaLight = false)
{
    const auto& type = entity.type;
    const auto& params = entity.params;

    auto warnUnsupported = [&]() { warnUnsupportedType(entity.loc, "Material", type); };

    Falcor::ref<Falcor::Material> pMaterial;

    if (type == "" || type == "none")
    {
        logWarning(entity.loc, "Material type '{}' is deprecated, use 'interface' instead.", type);
    }
    else if (type == "interface")
    {
        // Nothing to do.
    }
    else if (type == "diffuse")
    {
        // Parameters:
        // SpectrumTexture reflectance
        // FloatTexture displacement
        // Displacement textures are intentionally ignored by this realtime viewer.

        auto reflectance = getSpectrumTexture(ctx, params, "reflectance", float3(0.5f), SpectrumType::Albedo);

        if (ctx.usePBRTMaterials && !isAreaLight)
        {
            auto pPBRTMaterial = PBRTDiffuseMaterial::create(ctx.builder.getDevice(), entity.name);
            assignSpectrumTexture(
                reflectance,
                [&](float3 rgb) { pPBRTMaterial->setBaseColor(float4(rgb, 1.f)); },
                [&](Falcor::ref<Texture> pTexture) { pPBRTMaterial->setBaseColorTexture(pTexture); },
                [&](const Transform& transform) { pPBRTMaterial->setTextureTransform(transform); }
            );
            pPBRTMaterial->setDoubleSided(true);
            pMaterial = pPBRTMaterial;
        }
        else
        {
            auto pStandardMaterial = StandardMaterial::create(ctx.builder.getDevice(), entity.name);
            pStandardMaterial->setMetallic(0.f);
            pStandardMaterial->setRoughness(1.f);
            assignSpectrumTexture(
                reflectance,
                [&](float3 rgb) { pStandardMaterial->setBaseColor(float4(rgb, 1.f)); },
                [&](Falcor::ref<Texture> pTexture) { pStandardMaterial->setBaseColorTexture(pTexture); },
                [&](const Transform& transform) { pStandardMaterial->setTextureTransform(transform); }
            );
            pStandardMaterial->setDoubleSided(true);
            pMaterial = pStandardMaterial;
        }
    }
    else if (type == "coateddiffuse")
    {
        // Parameters:
        // SpectrumTexture reflectance
        // FloatTexture roughness, FloatTexture uroughness, FloatTexture vroughness, Bool remaproughness
        // FloatTexture thickness, Float|Spectrum eta, Int maxdepth, Int nsamples, FloatTexture g, SpectrumTexture albedo
        // FloatTexture displacement
        warnUnsupportedParameters(params, {"thickness", "eta", "maxdepth", "nsamples", "g", "albedo"});

        auto reflectance = getSpectrumTexture(ctx, params, "reflectance", float3(0.5f), SpectrumType::Albedo);

        if (ctx.usePBRTMaterials && !isAreaLight)
        {
            float2 roughness = getRoughness(ctx, entity);

            auto pPBRTMaterial = PBRTCoatedDiffuseMaterial::create(ctx.builder.getDevice(), entity.name);
            pPBRTMaterial->setRoughness(roughness);
            assignSpectrumTexture(
                reflectance,
                [&](float3 rgb) { pPBRTMaterial->setBaseColor(float4(rgb, 1.f)); },
                [&](Falcor::ref<Texture> pTexture) { pPBRTMaterial->setBaseColorTexture(pTexture); },
                [&](const Transform& transform) { pPBRTMaterial->setTextureTransform(transform); }
            );
            pPBRTMaterial->setDoubleSided(true);
            pMaterial = pPBRTMaterial;
        }
        else
        {
            float roughness = getScalarRoughness(ctx, entity);

            auto pStandardMaterial = StandardMaterial::create(ctx.builder.getDevice(), entity.name);
            pStandardMaterial->setMetallic(0.f);
            pStandardMaterial->setRoughness(std::sqrt(roughness));
            assignSpectrumTexture(
                reflectance,
                [&](float3 rgb) { pStandardMaterial->setBaseColor(float4(rgb, 1.f)); },
                [&](Falcor::ref<Texture> pTexture) { pStandardMaterial->setBaseColorTexture(pTexture); },
                [&](const Transform& transform) { pStandardMaterial->setTextureTransform(transform); }
            );
            pStandardMaterial->setDoubleSided(true);
            pMaterial = pStandardMaterial;
        }
    }
    else if (type == "conductor")
    {
        // Parameters:
        // SpectrumTexture eta, SpectrumTexture k, SpectrumTexture reflectance
        // FloatTexture roughness, FloatTexture uroughness, FloatTexture vroughness, Bool remaproughness
        // FloatTexture displacement
        // Displacement textures are intentionally ignored by this realtime viewer.

        if (ctx.usePBRTMaterials && !isAreaLight)
        {
            auto [eta, k] = getConductorEtaK(ctx, entity);
            float2 roughness = getRoughness(ctx, entity);

            auto pPBRTMaterial = PBRTConductorMaterial::create(ctx.builder.getDevice(), entity.name);
            pPBRTMaterial->setBaseColor(float4(eta, 1.f));
            pPBRTMaterial->setTransmissionColor(k);
            pPBRTMaterial->setRoughness(roughness);
            pPBRTMaterial->setDoubleSided(true);
            pMaterial = pPBRTMaterial;
        }
        else
        {
            float3 specularAlbedo = getConductorSpecularAlbedo(ctx, entity);
            float roughness = getScalarRoughness(ctx, entity);

            auto pStandardMaterial = StandardMaterial::create(ctx.builder.getDevice(), entity.name);
            pStandardMaterial->setBaseColor(float4(specularAlbedo, 1.f));
            pStandardMaterial->setMetallic(1.f);
            pStandardMaterial->setRoughness(std::sqrt(roughness));
            pStandardMaterial->setDoubleSided(true);
            pMaterial = pStandardMaterial;
        }
    }
    else if (type == "coatedconductor")
    {
        // Parameters:
        // FloatTexture interface.roughness, FloatTexture interface.uroughness, FloatTexture interface.vroughness, Spectrum interface.eta
        // FloatTexture conductor.roughness, FloatTexture conductor.uroughness, FloatTexture conductor.vroughness, Spectrum conductor.eta
        // SpectrumTexture conductor.eta, SpectrumTexture conductor.k, SpectrumTexture conductor.reflectance
        // Bool remaproughness
        // Int maxdepth, Int nsamples
        // FloatTexture g, SpectrumTexture albedo
        // FloatTexture displacement

        if (ctx.usePBRTMaterials && !isAreaLight)
        {
            warnUnsupportedParameters(params, {"maxdepth", "nsamples", "g", "albedo"});

            float2 interfaceRoughness = getRoughness(ctx, entity, "interface.roughness", "interface.uroughness", "interface.vroughness");
            float interfaceEta = getScalarEta(ctx, entity, "interface.eta");
            auto [eta, k] = getConductorEtaK(ctx, entity, "conductor.reflectance", "conductor.eta", "conductor.k");
            float2 conductorRoughness = getRoughness(ctx, entity, "conductor.roughness", "conductor.uroughness", "conductor.vroughness");

            auto pPBRTMaterial = PBRTCoatedConductorMaterial::create(ctx.builder.getDevice(), entity.name);
            pPBRTMaterial->setBaseColor(float4(eta, 1.f));
            pPBRTMaterial->setTransmissionColor(k);
            pPBRTMaterial->setSpecularParams(float4(interfaceRoughness, conductorRoughness));
            pPBRTMaterial->setIndexOfRefraction(interfaceEta);
            pPBRTMaterial->setDoubleSided(true);
            pMaterial = pPBRTMaterial;
        }
        else
        {
            warnUnsupportedParameters(
                params,
                {"interface.roughness",
                 "interface.uroughness",
                 "interface.vroughness",
                 "interface.eta",
                 "maxdepth",
                 "nsamples",
                 "g",
                 "albedo"}
            );

            float3 specularAlbedo = getConductorSpecularAlbedo(ctx, entity, "conductor.reflectance", "conductor.eta", "conductor.k");
            float roughness = getScalarRoughness(ctx, entity, "conductor.roughness", "conductor.uroughness", "conductor.vroughness");

            auto pStandardMaterial = StandardMaterial::create(ctx.builder.getDevice(), entity.name);
            pStandardMaterial->setBaseColor(float4(specularAlbedo, 1.f));
            pStandardMaterial->setMetallic(1.f);
            pStandardMaterial->setRoughness(std::sqrt(roughness));
            pStandardMaterial->setDoubleSided(true);
            pMaterial = pStandardMaterial;
        }
    }
    else if (type == "dielectric")
    {
        // Parameters:
        // Float|Spectrum eta
        // FloatTexture roughness, FloatTexture uroughness, FloatTexture vroughness, Bool remaproughness
        // FloatTexture displacement
        // Displacement textures are intentionally ignored by this realtime viewer.

        if (ctx.usePBRTMaterials && !isAreaLight)
        {
            float2 roughness = getRoughness(ctx, entity);
            float eta = getScalarEta(ctx, entity);

            auto pPBRTMaterial = PBRTDielectricMaterial::create(ctx.builder.getDevice(), entity.name);
            pPBRTMaterial->setRoughness(roughness);
            pPBRTMaterial->setIndexOfRefraction(eta);
            pMaterial = pPBRTMaterial;
        }
        else
        {
            float roughness = getScalarRoughness(ctx, entity);
            float eta = getScalarEta(ctx, entity);

            auto pStandardMaterial = StandardMaterial::create(ctx.builder.getDevice(), entity.name);
            pStandardMaterial->setMetallic(0.f);
            pStandardMaterial->setRoughness(std::sqrt(roughness));
            pStandardMaterial->setIndexOfRefraction(eta);
            pStandardMaterial->setSpecularTransmission(1.f);
            pMaterial = pStandardMaterial;
        }
    }
    else if (type == "thindielectric")
    {
        // Parameters:
        // Float|Spectrum eta
        // FloatTexture displacement
        // Displacement textures are intentionally ignored by this realtime viewer.

        float eta = getScalarEta(ctx, entity);

        auto pStandardMaterial = StandardMaterial::create(ctx.builder.getDevice(), entity.name);
        pStandardMaterial->setMetallic(0.f);
        pStandardMaterial->setRoughness(0.f);
        pStandardMaterial->setIndexOfRefraction(eta);
        pStandardMaterial->setSpecularTransmission(1.f);
        pStandardMaterial->setThinSurface(true);
        pMaterial = pStandardMaterial;
    }
    else if (type == "diffusetransmission")
    {
        // Parameters:
        // SpectrumTexture reflectance, SpectrumTexture transmittance, Float scale
        // FloatTexture displacement
        warnUnsupportedParameters(params, {"scale"});

        auto reflectance = getSpectrumTexture(ctx, params, "reflectance", float3(0.25f), SpectrumType::Albedo);
        auto transmittance = getSpectrumTexture(ctx, params, "transmittance", float3(0.25f), SpectrumType::Albedo);

        if (ctx.usePBRTMaterials && !isAreaLight)
        {
            auto pPBRTMaterial = PBRTDiffuseTransmissionMaterial::create(ctx.builder.getDevice(), entity.name);
            assignSpectrumTexture(
                reflectance,
                [&](float3 rgb) { pPBRTMaterial->setBaseColor(float4(rgb, 1.f)); },
                [&](Falcor::ref<Texture> pTexture) { pPBRTMaterial->setBaseColorTexture(pTexture); },
                [&](const Transform& transform) { pPBRTMaterial->setTextureTransform(transform); }
            );
            assignSpectrumTexture(
                transmittance,
                [&](float3 rgb) { pPBRTMaterial->setTransmissionColor(rgb); },
                [&](Falcor::ref<Texture> pTexture) { pPBRTMaterial->setTransmissionTexture(pTexture); },
                [&](const Transform& transform) { pPBRTMaterial->setTextureTransform(transform); }
            );
            pMaterial = pPBRTMaterial;
        }
        else
        {
            auto pStandardMaterial = StandardMaterial::create(ctx.builder.getDevice(), entity.name);
            pStandardMaterial->setMetallic(0.f);
            pStandardMaterial->setRoughness(1.f);
            pStandardMaterial->setDiffuseTransmission(0.5f);
            assignSpectrumTexture(
                reflectance,
                [&](float3 rgb) { pStandardMaterial->setBaseColor(float4(rgb, 1.f)); },
                [&](Falcor::ref<Texture> pTexture) { pStandardMaterial->setBaseColorTexture(pTexture); },
                [&](const Transform& transform) { pStandardMaterial->setTextureTransform(transform); }
            );
            assignSpectrumTexture(
                transmittance,
                [&](float3 rgb) { pStandardMaterial->setTransmissionColor(rgb); },
                [&](Falcor::ref<Texture> pTexture) { pStandardMaterial->setTransmissionTexture(pTexture); },
                [&](const Transform& transform) { pStandardMaterial->setTextureTransform(transform); }
            );
            pStandardMaterial->setDoubleSided(true);
            pMaterial = pStandardMaterial;
        }
    }
    else if (type == "hair")
    {
        // Parameters:
        // SpectrumTexture sigma_a, SpectrumTexture reflectance, SpectrumTexture color,
        // FloatTexture eumelanin, FloatTexture pheomelanin
        // FloatTexture eta, FloatTexture beta_m, FloatTexture beta_n, FloatTexture alpha

        auto sigma_a = getSpectrumTextureOrNull(ctx, params, "sigma_a", SpectrumType::Unbounded);
        auto reflectance = getSpectrumTextureOrNull(ctx, params, "reflectance", SpectrumType::Albedo);
        if (!reflectance)
        {
            reflectance = getSpectrumTextureOrNull(ctx, params, "color", SpectrumType::Albedo);
        }
        auto eumelanin = getFloatTextureOrNull(ctx, params, "eumelanin");
        auto pheomelanin = getFloatTextureOrNull(ctx, params, "pheomelanin");

        float eta = getFloatTextureConstantOnly(ctx, params, "eta", 1.55f);
        float beta_m = getFloatTextureConstantOnly(ctx, params, "beta_m", 0.3f);
        float beta_n = getFloatTextureConstantOnly(ctx, params, "beta_n", 0.3f);
        float alpha = getFloatTextureConstantOnly(ctx, params, "alpha", 2.f);

        ref<HairMaterial> pHairMaterial = HairMaterial::create(ctx.builder.getDevice(), entity.name);
        float3 baseColor = HairMaterial::colorFromSigmaA(HairMaterial::sigmaAFromConcentration(0.5f, 0.2f), beta_n);
        pHairMaterial->setBaseColor(float4(baseColor, 1.f));
        pHairMaterial->setSpecularParams(float4(beta_m, beta_n, alpha, 0.f));
        pHairMaterial->setIndexOfRefraction(eta);

        if (sigma_a)
        {
            if (reflectance)
                logWarning(entity.loc, "Ignoring 'reflectance' parameter since 'sigma_a' was provided.");
            if (eumelanin)
                logWarning(entity.loc, "Ignoring 'eumelanin' parameter since 'sigma_a' was provided.");
            if (pheomelanin)
                logWarning(entity.loc, "Ignoring 'pheomelanin' parameter since 'sigma_a' was provided.");

            assignSpectrumTexture(
                *sigma_a,
                [&](float3 constant) { pHairMaterial->setBaseColor(float4(HairMaterial::colorFromSigmaA(constant, beta_n), 1.f)); },
                [&](Falcor::ref<Texture> pTexture)
                { logWarning(entity.loc, "Non-constant 'sigma_a' is currently not supported. Using default color instead."); }
            );
        }
        else if (reflectance)
        {
            if (eumelanin)
                logWarning(entity.loc, "Ignoring 'eumelanin' parameter since 'reflectance' was provided.");
            if (pheomelanin)
                logWarning(entity.loc, "Ignoring 'pheomelanin' parameter since 'reflectance' was provided.");

            assignSpectrumTexture(
                *reflectance,
                [&](float3 constant) { pHairMaterial->setBaseColor(float4(constant, 1.f)); },
                [&](Falcor::ref<Texture> pTexture) { pHairMaterial->setBaseColorTexture(pTexture); },
                [&](const Transform& transform) { pHairMaterial->setTextureTransform(transform); }
            );
        }
        else if (eumelanin || pheomelanin)
        {
            float eumelaninValue = getFloatTextureConstantOnly(ctx, params, "eumelanin", 0.3f);
            float pheomelaninValue = getFloatTextureConstantOnly(ctx, params, "pheomelanin", 0.3f);
            float3 color = HairMaterial::colorFromSigmaA(HairMaterial::sigmaAFromConcentration(eumelaninValue, pheomelaninValue), beta_n);
            pHairMaterial->setBaseColor(float4(color, 1.f));
        }

        pMaterial = pHairMaterial;
    }
    else if (type == "measured")
    {
        // Parameters:
        // String filename
        if (ctx.usePBRTMaterials && !isAreaLight)
        {
            auto path = ctx.resolver(params.getString("filename", ""));
            try
            {
                auto pRGLMaterial = RGLMaterial::create(ctx.builder.getDevice(), entity.name, path);
                pMaterial = pRGLMaterial;
            }
            catch (const RuntimeError& e)
            {
                logWarning(entity.loc, "Failed to load 'measured' material: {}", e.what());
            }
        }
        else
        {
            auto pStandardMaterial = StandardMaterial::create(ctx.builder.getDevice(), entity.name);
            pStandardMaterial->setBaseColor(float4(0.6f, 0.6f, 0.6f, 1.f));
            pStandardMaterial->setMetallic(0.f);
            pStandardMaterial->setRoughness(0.35f);
            pStandardMaterial->setDoubleSided(true);
            pMaterial = pStandardMaterial;
        }
    }
    else if (type == "subsurface")
    {
        warnUnsupported();
    }
    else if (type == "mix")
    {
        warnUnsupported();
    }
    else
    {
        throwError(entity.loc, "Unknown material type '{}'.", type);
    }

    // Load normal map.
    if (pMaterial)
    {
        auto normalmap = params.getString("normalmap", "");
        if (!normalmap.empty())
        {
            auto pNormalMap = Texture::createFromFile(ctx.builder.getDevice(), ctx.resolver(normalmap), true, false);
            pMaterial->setTexture(Material::TextureSlot::Normal, pNormalMap);
        }
    }

    return pMaterial;
}

Medium createMedium(BuilderContext& ctx, const MediumSceneEntity& entity)
{
    warnUnsupportedType(entity.loc, "Medium", entity.params.getString("type", ""));
    return {};
}

void createAreaLight(BuilderContext& ctx, const SceneEntity& entity, const Falcor::ref<Falcor::Material>& pMaterial)
{
    auto warnUnsupported = [&]() { warnUnsupportedType(entity.loc, "Area light", entity.name); };

    const auto& type = entity.name;
    const auto& params = entity.params;

    if (type == "diffuse")
    {
        warnUnsupportedParameters(params, {"twosided", "power", "filename"});

        auto L = getSpectrumAsRGB(ctx, params, "L", float3(1.f), SpectrumType::Illuminant);
        auto scale = params.getFloat("scale", 1.f);
        L *= scale;

        if (auto pStandardMaterial = dynamic_ref_cast<Falcor::StandardMaterial>(pMaterial))
        {
            pStandardMaterial->setEmissiveColor(L);
            pStandardMaterial->setEmissiveFactor(1.f);
        }
        else
        {
            logWarning(entity.loc, "Area lights are only supported for shapes that have a standard material.");
        }
    }
    else
    {
        throwError(entity.loc, "Unknown area light type '{}'.", type);
    }
}

Shape createShape(BuilderContext& ctx, const ShapeSceneEntity& entity)
{
    auto warnUnsupported = [&]() { warnUnsupportedType(entity.loc, "Shape", entity.name); };

    const auto& type = entity.name;
    const auto& params = entity.params;

    warnUnsupportedParameters(params, {"alpha"});

    Shape shape;

    if (type == "sphere")
    {
        // Parameters:
        // Float radius, Float zmin, Float zmax, Float phimax
        warnUnsupportedParameters(params, {"zmin", "zmax", "phimax"});

        auto radius = params.getFloat("radius", 1.f);

        shape.pTriangleMesh = Falcor::TriangleMesh::createSphere(radius);
        shape.pTriangleMesh->setName("sphere");
        shape.transform = toFalcor(entity.transform);
    }
    else if (type == "cylinder")
    {
        // Parameters:
        // Float radius, Float zmin, Float zmax, Float phimax
        warnUnsupported();
    }
    else if (type == "disk")
    {
        // Parameters:
        // Float radius, Float height, Float innerradius, Float phimax
        warnUnsupportedParameters(params, {"innerradius", "phimax"});

        auto radius = params.getFloat("radius", 1.f);
        auto height = params.getFloat("height", 0.f);

        shape.pTriangleMesh = Falcor::TriangleMesh::createDisk(radius);
        shape.pTriangleMesh->setName("disk");
        float4x4 transform = mul(math::matrixFromTranslation(float3(0.f, 0.f, height)), kYtoZ);
        shape.pTriangleMesh->applyTransform(transform);
    }
    else if (type == "bilinearmesh")
    {
        // Parameters:
        // Int[] indices, Point3[] P, Point2[] uv, Normal3[] N, Int[] faceIndices, String emissionfilename
        warnUnsupported();
    }
    else if (type == "curve")
    {
        // Parameters:
        // Float width, Float width0, Float width1, Int degree, String basis,
        // Point3[] P, String type, Normal3[] N, Int splitdepth
        warnUnsupportedParameters(params, {"degree", "N"});

        auto splitdepth = params.getInt("splitdepth", 1);

        auto width = params.getFloat("width", 1.f);
        auto width0 = params.getFloat("width0", width);
        auto width1 = params.getFloat("width1", width);

        auto basis = params.getString("basis", "bezier");
        if (basis != "bspline")
            logWarning(entity.loc, "Basis '{}' is not supported. Using 'bspline' basis instead.", basis);

        auto curveType = params.getString("type", "flat");
        if (curveType != "cylinder")
            logWarning(entity.loc, "Curve type '{}' is not supported. Using 'cylinder' type instead.", curveType);

        auto P = toFalcor(params.getPoint3Array("P"));

        // Create or get existing curve aggregate.
        auto pMaterial = ctx.getMaterial(entity.materialRef);
        CurveAggregate::Key key{toFalcor(entity.transform), pMaterial.get()};
        auto it = ctx.curveAggregates.find(key);
        if (it == ctx.curveAggregates.end())
        {
            it = ctx.curveAggregates.emplace(key, CurveAggregate{}).first;
            it->second.transform = toFalcor(entity.transform);
            it->second.pMaterial = pMaterial;
            it->second.splitDepth = splitdepth;
        }
        CurveAggregate& aggregate = it->second;

        // Append curve to aggregate.
        size_t pointCount = P.size();
        size_t offset = aggregate.points.size();
        aggregate.strands.push_back(pointCount);
        aggregate.points.resize(aggregate.points.size() + pointCount);
        aggregate.widths.resize(aggregate.widths.size() + pointCount);
        for (size_t i = 0; i < pointCount; ++i)
        {
            float t = float(i) / pointCount;
            aggregate.points[offset + i] = P[i];
            aggregate.widths[offset + i] = math::lerp(width0, width1, t);
        }
    }
    else if (type == "trianglemesh")
    {
        // Parameters:
        // Int[] indices, Point3[] P, Point2[] uv, Vector3[] S, Normal3[] N, Int[] faceIndices
        warnUnsupportedParameters(params, {"S", "faceIndices"});

        auto indices = params.getIntArray("indices");
        auto P = toFalcor(params.getPoint3Array("P"));
        auto N = toFalcor(params.getNormalArray("N"));
        auto uv = toFalcor(params.getPoint2Array("uv"));

        if (indices.empty())
        {
            if (P.size() == 3)
            {
                indices = {0, 1, 2};
            }
            else
            {
                logWarning(entity.loc, "Vertex indices 'indices' missing. Skipping.");
                return {};
            }
        }
        if (indices.size() % 3 != 0)
        {
            logWarning(
                entity.loc, "Number of vertex indices {} is not a multiple of 3. Discarding {} indices.", indices.size(), indices.size() % 3
            );
            while (indices.size() % 3 != 0)
                indices.pop_back();
        }
        if (P.empty())
        {
            logWarning(entity.loc, "Vertex positions 'positions' missing. Skipping.");
            return {};
        }
        if (!uv.empty() && uv.size() != P.size())
        {
            logWarning(entity.loc, "Number of 'uv' elements must match number of 'P' elements. Discarding 'uv'.");
            uv = {};
        }
        if (!N.empty() && N.size() != P.size())
        {
            logWarning(entity.loc, "Number of 'N' elements must match number of 'P' elements. Discarding 'N'.");
            N = {};
        }
        for (auto i : indices)
        {
            if (i < 0 || i >= P.size())
            {
                logWarning(entity.loc, "Vertex index {} is out of bounds. Skipping.", i);
                return {};
            }
        }

        Falcor::TriangleMesh::VertexList vertexList(P.size());
        for (size_t i = 0; i < P.size(); ++i)
        {
            auto& vertex = vertexList[i];
            vertex.position = P[i];
            vertex.normal = N.empty() ? float3(0.f) : N[i];
            vertex.texCoord = uv.empty() ? float2(0.f) : uv[i];
        }

        Falcor::TriangleMesh::IndexList indexList(indices.size());
        for (size_t i = 0; i < indices.size(); ++i)
            indexList[i] = indices[i];

        shape.pTriangleMesh = Falcor::TriangleMesh::create(std::move(vertexList), std::move(indexList));
        shape.transform = toFalcor(entity.transform);
    }
    else if (type == "plymesh")
    {
        // Parameters:
        // String filename, Texture displacement, Float displacement.edgelength,
        warnUnsupportedParameters(params, {"displacement", "displacement.edgelength"});

        auto filename = params.getString("filename", "");
        auto path = ctx.resolver(filename);
        shape.transform = toFalcor(entity.transform);

        // Area-light plymeshes are emissive-only in PBRT; do not add visible geometry.
        if (entity.lightIndex == -1)
        {
            shape.pTriangleMesh = Falcor::TriangleMesh::createFromFile(path.string());
            if (shape.pTriangleMesh)
                shape.pTriangleMesh->setName(filename);
        }
    }
    else if (type == "loopsubdiv")
    {
        // Parameters:
        // Int levels, Int[] indices, Point3[] P
        // String scheme (also not supported in pbrt-v4)
        warnUnsupportedParameters(params, {"scheme"});

        auto levels = params.getInt("levels", 3);
        auto indices = params.getIntArray("indices");
        auto P = params.getPoint3Array("P");

        if (indices.empty())
            throwError(entity.loc, "Missing vertex indices in 'indices'.");
        if (P.empty())
            throwError(entity.loc, "Missing vertex positions in 'P'.");

        auto result =
            loopSubdivide(levels, P, fstd::span<const uint32_t>(reinterpret_cast<const uint32_t*>(indices.data()), indices.size()));

        Falcor::TriangleMesh::VertexList vertexList(result.positions.size());
        for (size_t i = 0; i < result.positions.size(); ++i)
        {
            auto& vertex = vertexList[i];
            vertex.position = toFalcor(result.positions[i]);
            vertex.normal = toFalcor(result.normals[i]);
            vertex.texCoord = float2(0.f);
        }

        shape.pTriangleMesh = Falcor::TriangleMesh::create(vertexList, result.indices);
        shape.pTriangleMesh->setName("loopsubdiv");
        shape.transform = toFalcor(entity.transform);
    }
    else
    {
        throwError(entity.loc, "Unknown shape type '{}'.", type);
    }

    // Reverse orientation.
    if (entity.reverseOrientation && shape.pTriangleMesh)
        shape.pTriangleMesh->setFrontFaceCW(!shape.pTriangleMesh->getFrontFaceCW());

    // Get the material.
    shape.pMaterial = ctx.getMaterial(entity.materialRef);

    // Create area light.
    if (entity.lightIndex != -1)
    {
        std::string nameSuffix = fmt::format("Emissive{}", entity.lightIndex);

        // Create a new material as we may already use it for other shapes with no area light attached to it.
        if (!std::holds_alternative<std::monostate>(entity.materialRef))
        {
            shape.pMaterial = createMaterial(ctx, ctx.scene.getMaterial(entity.materialRef), true);
            shape.pMaterial->setName(shape.pMaterial->getName() + "_" + nameSuffix);
        }
        else
        {
            auto pStandardMaterial = Falcor::StandardMaterial::create(ctx.builder.getDevice(), nameSuffix);
            pStandardMaterial->setBaseColor(float4(0.f, 0.f, 0.f, 1.f));
            pStandardMaterial->setRoughness(0.f);
            shape.pMaterial = pStandardMaterial;
        }
        const SceneEntity& areaLightEntity = ctx.scene.getAreaLight(entity.lightIndex);
        createAreaLight(ctx, areaLightEntity, shape.pMaterial);
    }

    return shape;
}

Falcor::SceneBuilder::ProcessedMesh processTriangleMesh(
    const SceneBuilder& builder,
    const Falcor::ref<Falcor::TriangleMesh>& pTriangleMesh,
    const Falcor::ref<Falcor::Material>& pMaterial,
    bool frontFaceCW
)
{
    FALCOR_CHECK(pTriangleMesh != nullptr, "'pTriangleMesh' is missing");
    FALCOR_CHECK(pMaterial != nullptr, "'pMaterial' is missing");

    Falcor::SceneBuilder::Mesh mesh;
    const auto& indices = pTriangleMesh->getIndices();
    const auto& vertices = pTriangleMesh->getVertices();

    mesh.name = pTriangleMesh->getName();
    mesh.faceCount = (uint32_t)(indices.size() / 3);
    mesh.vertexCount = (uint32_t)vertices.size();
    mesh.indexCount = (uint32_t)indices.size();
    mesh.pIndices = indices.data();
    mesh.topology = Vao::Topology::TriangleList;
    mesh.isFrontFaceCW = frontFaceCW;
    mesh.pMaterial = pMaterial;

    std::vector<float3> positions(vertices.size());
    std::vector<float3> normals(vertices.size());
    std::vector<float2> texCoords(vertices.size());
    std::transform(vertices.begin(), vertices.end(), positions.begin(), [](const auto& v) { return v.position; });
    std::transform(vertices.begin(), vertices.end(), normals.begin(), [](const auto& v) { return v.normal; });
    std::transform(vertices.begin(), vertices.end(), texCoords.begin(), [](const auto& v) { return v.texCoord; });

    mesh.positions = {positions.data(), SceneBuilder::Mesh::AttributeFrequency::Vertex};
    mesh.normals = {normals.data(), SceneBuilder::Mesh::AttributeFrequency::Vertex};
    mesh.texCrds = {texCoords.data(), SceneBuilder::Mesh::AttributeFrequency::Vertex};

    return builder.processMesh(mesh);
}

Falcor::SceneBuilder::ProcessedMesh processPbrtMeshView(
    const SceneBuilder& builder,
    const std::string& name,
    ::pbrtio::PbrtMeshView view,
    std::span<const float4> tangents,
    const Falcor::ref<Falcor::Material>& pMaterial,
    bool frontFaceCW
)
{
    static_assert(sizeof(::pbrtio::pbrt::float2) == sizeof(Falcor::float2));
    static_assert(sizeof(::pbrtio::pbrt::float3) == sizeof(Falcor::float3));

    FALCOR_CHECK(!view.positions.empty(), "'positions' is missing");
    FALCOR_CHECK(!view.indices.empty(), "'indices' is missing");
    FALCOR_CHECK(view.indices.size() % 3 == 0, "'indices' must contain complete triangles");
    FALCOR_CHECK(pMaterial != nullptr, "'pMaterial' is missing");

    Falcor::SceneBuilder::Mesh mesh;
    mesh.name = name;
    mesh.faceCount = (uint32_t)(view.indices.size() / 3);
    mesh.vertexCount = (uint32_t)view.positions.size();
    mesh.indexCount = (uint32_t)view.indices.size();
    mesh.pIndices = view.indices.data();
    mesh.topology = Vao::Topology::TriangleList;
    mesh.isFrontFaceCW = frontFaceCW;
    mesh.pMaterial = pMaterial;

    mesh.positions = {reinterpret_cast<const float3*>(view.positions.data()), SceneBuilder::Mesh::AttributeFrequency::Vertex};
    if (view.normals.size() == view.positions.size())
        mesh.normals = {reinterpret_cast<const float3*>(view.normals.data()), SceneBuilder::Mesh::AttributeFrequency::Vertex};
    if (view.texcoords.size() == view.positions.size())
        mesh.texCrds = {reinterpret_cast<const float2*>(view.texcoords.data()), SceneBuilder::Mesh::AttributeFrequency::Vertex};
    if (tangents.size() == view.positions.size())
    {
        mesh.tangents = {tangents.data(), SceneBuilder::Mesh::AttributeFrequency::Vertex};
        mesh.useOriginalTangentSpace = true;
    }

    return builder.processMesh(mesh);
}

std::string normalizedResourceKey(const std::filesystem::path& path)
{
    return path.lexically_normal().string();
}

PlyMeshPlan collectPlyMeshes(BuilderContext& ctx)
{
    PlyMeshPlan plan;

    std::unordered_map<std::string, ::pbrtio::PbrtResourceIndex> meshResourceMap;
    auto& meshResources = ctx.resources.meshResources;
    plan.geometryJobs.resize(meshResources.size());
    for (::pbrtio::PbrtResourceIndex i = 0; i < meshResources.size(); ++i)
    {
        plan.geometryJobs[i].path = meshResources[i].plyPath;
        plan.geometryJobs[i].pResource = &meshResources[i];
        meshResourceMap.emplace(normalizedResourceKey(meshResources[i].plyPath), i);
    }

    std::unordered_map<PlyProcessKey, size_t, PlyProcessKeyHash> processJobMap;
    std::unordered_map<PlyMeshKey, size_t, PlyMeshKeyHash> plyJobMap;

    auto addShape = [&](const ShapeSceneEntity& entity)
    {
        if (entity.name != "plymesh" || entity.lightIndex != -1)
            return;

        warnUnsupportedParameters(entity.params, {"displacement", "displacement.edgelength"});

        auto filename = entity.params.getString("filename", "");
        if (filename.empty())
        {
            logWarning(entity.loc, "Missing plymesh filename. Skipping.");
            return;
        }

        const auto path = ctx.resolver(filename);
        const auto geometryIt = meshResourceMap.find(normalizedResourceKey(path));
        if (geometryIt == meshResourceMap.end())
        {
            logWarning(entity.loc, "PLY resource '{}' was not collected by pbrtio. Falling back to direct loading.", filename);
            return;
        }

        const ::pbrtio::PbrtResourceIndex resourceIndex = geometryIt->second;
        plan.geometryJobs[resourceIndex].used = true;
        auto pMaterial = ctx.getMaterial(entity.materialRef);
        const bool reverseOrientation = entity.reverseOrientation;
        PlyProcessKey processKey{resourceIndex, reverseOrientation, pMaterial->getTextureTransform().getMatrix()};
        auto [processIt, processInserted] = processJobMap.emplace(processKey, plan.processJobs.size());
        const size_t processJobIndex = processIt->second;

        if (processInserted)
        {
            PlyProcessJob processJob;
            processJob.resourceIndex = resourceIndex;
            processJob.pMaterial = pMaterial;
            processJob.reverseOrientation = reverseOrientation;
            plan.processJobs.push_back(std::move(processJob));
        }

        PlyMeshKey key{processJobIndex, pMaterial.get()};
        auto [it, inserted] = plyJobMap.emplace(key, plan.plyJobs.size());
        const size_t meshJobIndex = it->second;
        plan.shapeToPlyJob.emplace(&entity, meshJobIndex);

        if (!inserted)
            return;

        PlyMeshJob job;
        job.filename = filename;
        job.processJobIndex = processJobIndex;
        job.pMaterial = pMaterial;
        plan.plyJobs.push_back(std::move(job));
    };

    for (const auto& entity : ctx.scene.getShapes())
        addShape(entity);

    std::unordered_set<std::string> usedInstanceDefinitions;
    for (const auto& instance : ctx.scene.getInstances())
        usedInstanceDefinitions.insert(instance.name);

    for (const auto& [name, instanceDefinition] : ctx.scene.getInstanceDefinitions())
    {
        if (usedInstanceDefinitions.find(name) == usedInstanceDefinitions.end())
            continue;

        for (const auto& entity : instanceDefinition.shapes)
            addShape(entity);
    }

    return plan;
}

void executePlyMeshPlan(BuilderContext& ctx, PlyMeshPlan& plan)
{
    if (plan.geometryJobs.empty() || plan.plyJobs.empty())
        return;

    tf::Taskflow taskflow;
    std::vector<tf::Task> geometryTasks(plan.geometryJobs.size());
    const uint32_t hardwareThreads = (std::max)(2u, std::thread::hardware_concurrency());
    const uint32_t defaultIOConcurrency = (std::min)(16u, (std::max)(4u, hardwareThreads / 2u));
    const uint32_t ioConcurrency = ctx.plyIOConcurrency > 0 ? ctx.plyIOConcurrency : defaultIOConcurrency;
    tf::Semaphore ioSemaphore(ioConcurrency);

    std::atomic<uint64_t> loadMicros = 0;
    std::atomic<uint64_t> processMicros = 0;
    std::atomic<uint32_t> loadedResources = 0;
    std::atomic<uint32_t> failedResources = 0;
    std::atomic<uint32_t> processedMeshes = 0;
    uint32_t usedResources = 0;

    auto elapsedMicros = [](const std::chrono::steady_clock::time_point& start) -> uint64_t {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start
        ).count();
    };

    for (size_t i = 0; i < plan.geometryJobs.size(); ++i)
    {
        if (!plan.geometryJobs[i].used)
            continue;
        usedResources++;

        geometryTasks[i] = taskflow.emplace([&, i]() {
            const auto start = std::chrono::steady_clock::now();
            auto& geometry = plan.geometryJobs[i];
            if (!geometry.pResource)
                return;
            geometry.valid = geometry.pResource->mesh.loaded || ::pbrtio::loadPbrtMeshResource(*geometry.pResource);
            loadMicros.fetch_add(elapsedMicros(start), std::memory_order_relaxed);
            if (geometry.valid)
                loadedResources.fetch_add(1, std::memory_order_relaxed);
            else
                failedResources.fetch_add(1, std::memory_order_relaxed);
        });
        geometryTasks[i].acquire(ioSemaphore).release(ioSemaphore);
    }

    std::vector<tf::Task> processTasks(plan.processJobs.size());
    for (size_t i = 0; i < plan.processJobs.size(); ++i)
    {
        auto meshTask = taskflow.emplace([&, i]() {
            const auto start = std::chrono::steady_clock::now();
            auto& job = plan.processJobs[i];
            if (job.resourceIndex >= plan.geometryJobs.size()) return;
            const auto& geometry = plan.geometryJobs[job.resourceIndex];
            if (!geometry.valid || !geometry.pResource || !geometry.pResource->mesh.loaded) return;
            const bool frontFaceCW = job.reverseOrientation;
            std::vector<float4> fastTangents;
            const auto meshView = geometry.pResource->mesh.view();
            if (ctx.fastPlyTangents && meshView.normals.size() == meshView.positions.size())
            {
                fastTangents.resize(meshView.normals.size());
                for (size_t normalIndex = 0; normalIndex < meshView.normals.size(); ++normalIndex)
                {
                    const auto& normal = meshView.normals[normalIndex];
                    fastTangents[normalIndex] = float4(perp_stark(float3(normal.x, normal.y, normal.z)), 1.f);
                }
            }
            job.mesh = processPbrtMeshView(ctx.builder, geometry.pResource->mesh.name, meshView, fastTangents, job.pMaterial, frontFaceCW);
            job.valid = true;
            processMicros.fetch_add(elapsedMicros(start), std::memory_order_relaxed);
            processedMeshes.fetch_add(1, std::memory_order_relaxed);
        });
        processTasks[i] = meshTask;
        geometryTasks[plan.processJobs[i].resourceIndex].precede(meshTask);
    }

    tf::Executor executor(hardwareThreads);
    executor.run(taskflow).get();

    const auto addStart = std::chrono::steady_clock::now();
    uint32_t addedMeshes = 0;
    for (auto& job : plan.plyJobs)
    {
        if (job.processJobIndex < plan.processJobs.size() && plan.processJobs[job.processJobIndex].valid)
        {
            auto mesh = plan.processJobs[job.processJobIndex].mesh;
            mesh.pMaterial = job.pMaterial;
            job.meshID = ctx.builder.addProcessedMesh(mesh);
            addedMeshes++;
        }
    }
    const uint64_t addMicros = elapsedMicros(addStart);

    if (ctx.logPlyTiming)
    {
        logInfo(
            "PBRTImporter: PLY DAG resources={} processJobs={} meshJobs={} ioConcurrency={} loaded={} failed={} processed={} added={} loadSum={:.3f}ms processSum={:.3f}ms add={:.3f}ms",
            usedResources,
            plan.processJobs.size(),
            plan.plyJobs.size(),
            ioConcurrency,
            loadedResources.load(std::memory_order_relaxed),
            failedResources.load(std::memory_order_relaxed),
            processedMeshes.load(std::memory_order_relaxed),
            addedMeshes,
            double(loadMicros.load(std::memory_order_relaxed)) / 1000.0,
            double(processMicros.load(std::memory_order_relaxed)) / 1000.0,
            double(addMicros) / 1000.0
        );
    }
}

bool instantiatePlannedPlyMesh(BuilderContext& ctx, const PlyMeshPlan& plan, const ShapeSceneEntity& entity)
{
    const auto it = plan.shapeToPlyJob.find(&entity);
    if (it == plan.shapeToPlyJob.end())
        return false;

    const size_t jobIndex = it->second;
    const auto& job = plan.plyJobs[jobIndex];
    if (!job.meshID.isValid())
        return false;

    auto nodeID = ctx.builder.addNode({job.filename, toFalcor(entity.transform)});
    ctx.builder.addMeshInstance(nodeID, job.meshID);
    return true;
}

bool instantiatePlannedPlyMesh(
    const PlyMeshPlan& plan,
    const ShapeSceneEntity& entity,
    InstanceDefinition& instanceDefinition
)
{
    const auto it = plan.shapeToPlyJob.find(&entity);
    if (it == plan.shapeToPlyJob.end())
        return false;

    const auto& job = plan.plyJobs[it->second];
    if (!job.meshID.isValid())
        return false;

    instanceDefinition.meshes.emplace_back(job.meshID, toFalcor(entity.transform));

    return true;
}

/**
 * Create curve geometry from a curve aggregate.
 * This can either result in mesh or curve geometry depending on the tesselation mode.
 */
std::variant<Falcor::MeshID, Falcor::CurveID> createCurveGeometry(BuilderContext& ctx, const CurveAggregate& curveAggregate)
{
    CurveTessellationMode mode = CurveTessellationMode::LinearSweptSphere;

    if (is_set(ctx.builder.getFlags(), SceneBuilder::Flags::TessellateCurvesIntoPolyTubes))
    {
        mode = CurveTessellationMode::PolyTube;
    }

    uint32_t subdivPerSegment = 1u << curveAggregate.splitDepth;

    if (mode == CurveTessellationMode::LinearSweptSphere)
    {
        auto result = CurveTessellation::convertToLinearSweptSphere(
            curveAggregate.strands.size(),
            curveAggregate.strands.data(),
            curveAggregate.points.data(),
            curveAggregate.widths.data(),
            nullptr,
            1,
            subdivPerSegment,
            1,
            1,
            1.f,
            float4x4::identity()
        );

        Falcor::SceneBuilder::Curve curve;
        curve.degree = result.degree;
        curve.vertexCount = result.points.size();
        curve.indexCount = result.indices.size();
        curve.pIndices = result.indices.data();
        curve.pMaterial = curveAggregate.pMaterial;
        curve.positions.pData = result.points.data();
        curve.radius.pData = result.radius.data();

        return ctx.builder.addCurve(curve);
    }
    else
    {
        Falcor::CurveTessellation::MeshResult result;
        if (mode == CurveTessellationMode::PolyTube)
        {
            result = CurveTessellation::convertToPolytube(
                curveAggregate.strands.size(),
                curveAggregate.strands.data(),
                curveAggregate.points.data(),
                curveAggregate.widths.data(),
                nullptr,
                subdivPerSegment,
                1,
                1,
                1.f,
                4
            );
        }
        else
        {
            FALCOR_UNREACHABLE();
        }

        Falcor::SceneBuilder::Mesh mesh;
        mesh.faceCount = result.faceVertexIndices.size() / 3;
        mesh.vertexCount = result.vertices.size();
        mesh.indexCount = result.faceVertexIndices.size();
        mesh.pIndices = result.faceVertexIndices.data();
        mesh.topology = Vao::Topology::TriangleList;
        mesh.pMaterial = curveAggregate.pMaterial;
        mesh.positions.pData = result.vertices.data();
        mesh.positions.frequency = Falcor::SceneBuilder::Mesh::AttributeFrequency::Vertex;
        mesh.normals.pData = result.normals.data();
        mesh.normals.frequency = Falcor::SceneBuilder::Mesh::AttributeFrequency::Vertex;
        mesh.tangents.pData = result.tangents.data();
        mesh.tangents.frequency = Falcor::SceneBuilder::Mesh::AttributeFrequency::Vertex;
        mesh.texCrds.pData = result.texCrds.data();
        mesh.texCrds.frequency = Falcor::SceneBuilder::Mesh::AttributeFrequency::Vertex;
        mesh.curveRadii.pData = result.radii.data();
        mesh.curveRadii.frequency = Falcor::SceneBuilder::Mesh::AttributeFrequency::Vertex;

        return ctx.builder.addMesh(mesh);
    }
}

InstanceDefinition createInstanceDefinition(BuilderContext& ctx, const InstanceDefinitionSceneEntity& entity, const PlyMeshPlan& plyPlan)
{
    InstanceDefinition instanceDefinition;

    for (const auto& shapeEntity : entity.shapes)
    {
        if (instantiatePlannedPlyMesh(plyPlan, shapeEntity, instanceDefinition))
            continue;

        // Process shapes and create meshes.
        auto shape = createShape(ctx, shapeEntity);
        if (shape.pTriangleMesh)
        {
            auto meshID = ctx.builder.addTriangleMesh(shape.pTriangleMesh, shape.pMaterial);
            instanceDefinition.meshes.emplace_back(meshID, shape.transform);
        }

        // Create curves from curve aggregates assembled during the processing step above.
        for (const auto& [_, curveAggregate] : ctx.curveAggregates)
        {
            auto meshOrCurveID = createCurveGeometry(ctx, curveAggregate);
            if (auto meshID = std::get_if<Falcor::MeshID>(&meshOrCurveID))
            {
                instanceDefinition.meshes.emplace_back(*meshID, curveAggregate.transform);
            }
            else if (auto curveID = std::get_if<Falcor::CurveID>(&meshOrCurveID))
            {
                instanceDefinition.curves.emplace_back(*curveID, curveAggregate.transform);
            }
            else
            {
                FALCOR_UNREACHABLE();
            }
        }
        ctx.curveAggregates.clear();
    }

    return instanceDefinition;
}

void buildScene(BuilderContext& ctx)
{
    // Load textures before materials because PBRT material parameters can reference named textures.
    for (const auto& [name, entity] : ctx.scene.getFloatTextures())
        ctx.floatTextures.emplace(name, createFloatTexture(ctx, entity));
    for (const auto& [name, entity] : ctx.scene.getSpectrumTextures())
        ctx.spectrumTextures.emplace(name, createSpectrumTexture(ctx, entity));

    // Create media.
    for (const auto& entity : ctx.scene.getMedia())
        ctx.media.emplace(entity.name, createMedium(ctx, entity));

    // Create materials before geometry because mesh processing needs material texture transforms.
    for (const auto& [name, entity] : ctx.scene.getNamedMaterials())
        ctx.namedMaterials.emplace(name, createMaterial(ctx, entity));
    for (const auto& entity : ctx.scene.getMaterials())
        ctx.materials.push_back(createMaterial(ctx, entity));

    if (ctx.nonConstantRoughnessFallbackCount > 0)
        logInfo(
            "PBRTImporter: {} material roughness values are texture/non-constant and are currently approximated as constant roughness 0.5.",
            ctx.nonConstantRoughnessFallbackCount
        );
    if (ctx.anisotropicRoughnessFallbackCount > 0)
        logInfo(
            "PBRTImporter: {} anisotropic roughness values are currently approximated with the average of u/v roughness.",
            ctx.anisotropicRoughnessFallbackCount
        );

    // Create camera.
    auto camera = createCamera(ctx, ctx.scene.getCamera());
    if (camera.pCamera)
    {
        auto nodeID = ctx.builder.addNode({"camera", mul(camera.transform, kInvertZ)});
        camera.pCamera->setNodeID(nodeID);
        ctx.builder.addCamera(camera.pCamera);
    }

    // Create lights.
    for (const auto& entity : ctx.scene.getLights())
    {
        auto light = createLight(ctx, entity);
        if (light.pLight)
        {
            ctx.builder.addLight(light.pLight);
        }
        if (light.pEnvMap)
        {
            if (ctx.builder.getEnvMap() == nullptr)
            {
                ctx.builder.setEnvMap(light.pEnvMap);
            }
            else
            {
                logWarning(entity.loc, "No support for multiple infinite light. Discarding this light.");
            }
        }
    }

    // Create shared PLY geometry for top-level and instanced shapes.
    auto plan = collectPlyMeshes(ctx);
    executePlyMeshPlan(ctx, plan);

    const auto& shapes = ctx.scene.getShapes();
    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex)
    {
        const auto& entity = shapes[shapeIndex];
        if (instantiatePlannedPlyMesh(ctx, plan, entity))
            continue;

        auto shape = createShape(ctx, entity);
        if (shape.pTriangleMesh)
        {
            auto nodeID = ctx.builder.addNode({entity.name, shape.transform});
            auto meshID = ctx.builder.addTriangleMesh(shape.pTriangleMesh, shape.pMaterial);
            ctx.builder.addMeshInstance(nodeID, meshID);
        }
    }

    // Create curves from curve aggregates assembled during shape processing.
    for (const auto& [_, curveAggregate] : ctx.curveAggregates)
    {
        auto nodeID = ctx.builder.addNode({"curves", curveAggregate.transform});
        auto meshOrCurveID = createCurveGeometry(ctx, curveAggregate);
        if (auto meshID = std::get_if<Falcor::MeshID>(&meshOrCurveID))
        {
            ctx.builder.addMeshInstance(nodeID, *meshID);
        }
        else if (auto curveID = std::get_if<Falcor::CurveID>(&meshOrCurveID))
        {
            ctx.builder.addCurveInstance(nodeID, *curveID);
        }
        else
        {
            FALCOR_UNREACHABLE();
        }
    }
    ctx.curveAggregates.clear();

    auto getInstanceDefinition = [&ctx, &plan](const InstanceSceneEntity& entity)
    {
        auto it = ctx.instanceDefinitions.find(entity.name);
        if (it == ctx.instanceDefinitions.end())
        {
            auto it2 = ctx.scene.getInstanceDefinitions().find(entity.name);
            if (it2 == ctx.scene.getInstanceDefinitions().end())
            {
                throwError(entity.loc, "Object instance '{}' not defined.", entity.name);
            }
            it = ctx.instanceDefinitions.emplace(entity.name, createInstanceDefinition(ctx, it2->second, plan)).first;
        }
        return it->second;
    };

    // Create instanced shapes.
    for (const auto& entity : ctx.scene.getInstances())
    {
        const auto& instanceDefinition = getInstanceDefinition(entity);
        auto instanceTransform = toFalcor(entity.transform);

        for (const auto& [meshID, transform] : instanceDefinition.meshes)
        {
            auto nodeID = ctx.builder.addNode({"instance", mul(instanceTransform, transform)});
            ctx.builder.addMeshInstance(nodeID, meshID);
        }

        for (const auto& [curveID, transform] : instanceDefinition.curves)
        {
            auto nodeID = ctx.builder.addNode({"instance_curve", mul(instanceTransform, transform)});
            ctx.builder.addCurveInstance(nodeID, curveID);
        }
    }
}

} // namespace pbrt

std::unique_ptr<Importer> PBRTImporter::create()
{
    return std::make_unique<PBRTImporter>();
}

void PBRTImporter::importScene(
    const std::filesystem::path& path,
    SceneBuilder& builder,
    const std::map<std::string, std::string>& materialToShortName
)
{
    if (!path.is_absolute())
        throw ImporterError(path, "Expected absolute path.");

    try
    {
        TimeReport timeReport;
        pbrt::BasicScene pbrtScene(path.parent_path());
        pbrt::BasicSceneBuilder pbrtBuilder(pbrtScene);
        pbrt::parseFile(pbrtBuilder, path);
        timeReport.measure("Parsing pbrt scene");

        ::pbrtio::PbrtLoadedScene pbrtResources;
        ::pbrtio::collectPbrtScene(pbrtScene, pbrtResources);

        pbrt::BuilderContext ctx{pbrtScene, builder, pbrtResources};
        ctx.usePBRTMaterials = builder.getSettings().getOption("PBRTImporter:usePBRTMaterials", false);
        ctx.rotateImageTextures90 = builder.getSettings().getOption("PBRTImporter:rotateImageTextures90", false);
        ctx.rotateImageTextures180 = builder.getSettings().getOption("PBRTImporter:rotateImageTextures180", false);
        ctx.flipTextureV = builder.getSettings().getOption("PBRTImporter:flipTextureV", true);
        ctx.plyIOConcurrency = builder.getSettings().getOption("PBRTImporter:plyIOConcurrency", 0);
        ctx.logPlyTiming = builder.getSettings().getOption("PBRTImporter:logPlyTiming", true);
        ctx.fastPlyTangents = builder.getSettings().getOption("PBRTImporter:fastPlyTangents", true);
        pbrt::buildScene(ctx);
        timeReport.measure("Building pbrt scene");
        timeReport.printToLog();
    }
    catch (const RuntimeError& e)
    {
        throw ImporterError(path, e.what());
    }
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<Importer, PBRTImporter>();
}

} // namespace Falcor
