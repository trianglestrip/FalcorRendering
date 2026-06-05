#include "FilamentIBL.h"
#include "Core/Platform/OS.h"
#include "Utils/Image/ImageIO.h"
#include "Utils/Math/FalcorMath.h"
#include "Utils/Math/ScalarMath.h"

using namespace math;

#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace Falcor
{
using namespace math;

namespace
{
    constexpr uint32_t kPlaceholderCubemapSize = 256;

    std::filesystem::path getIblDataRoot()
    {
        return getRuntimeDirectory() / "data" / "ibl";
    }

    bool fileExists(const std::filesystem::path& path)
    {
        return !path.empty() && std::filesystem::exists(path);
    }
} // namespace

ref<FilamentIBL> FilamentIBL::create(ref<Device> pDevice)
{
    return ref<FilamentIBL>(new FilamentIBL(pDevice));
}

FilamentIBL::FilamentIBL(ref<Device> pDevice) : mpDevice(pDevice)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(TextureFilteringMode::Linear, TextureFilteringMode::Linear, TextureFilteringMode::Linear);
    samplerDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
    mpLinearSampler = pDevice->createSampler(samplerDesc);
}

bool FilamentIBL::loadDefault()
{
    const auto iblRoot = getIblDataRoot();
    const auto lightroomDir = iblRoot / "lightroom_14b";

    // The generated DDS cubemap currently terminates the process inside Falcor's
    // DDS texture creation path on some devices. Keep the renderer usable and
    // preserve Filament's diffuse SH while the packed cubemap path is fixed.
    logWarning("FilamentIBL: DDS IBL loading is disabled. Using procedural specular and analytic DFG fallback.");
    createPlaceholderSpecularCubemap();
    createFallbackDfgLut();
    mUsingPlaceholder = true;

    if (!tryLoadSphericalHarmonics(lightroomDir / "sh.txt"))
    {
        mUseSH = false;
        mSH[0] = float3(kShSentinel, 0.f, 0.f);
    }

    return isLoaded();

#if 0
    const std::filesystem::path specularCandidates[] = {
        lightroomDir / "lightroom_14b_ibl.dds",
        lightroomDir / "ibl_specular.dds",
        lightroomDir / "specular.dds",
    };

    bool specularLoaded = false;
    for (const auto& path : specularCandidates)
    {
        if (tryLoadSpecularCubemap(path))
        {
            specularLoaded = true;
            break;
        }
    }

    if (!specularLoaded)
    {
        logWarning("FilamentIBL: No specular cubemap found under '{}'. Using procedural placeholder.", lightroomDir.string());
        createPlaceholderSpecularCubemap();
        mUsingPlaceholder = true;
    }

    if (!tryLoadDfgLut(iblRoot / "dfg.dds"))
    {
        logWarning("FilamentIBL: DFG LUT not found at '{}'. Using analytic fallback LUT.", (iblRoot / "dfg.dds").string());
        createFallbackDfgLut();
    }

    if (!tryLoadSphericalHarmonics(lightroomDir / "sh.txt"))
    {
        // Filament sentinel: use roughness-one cubemap mip for diffuse irradiance.
        mUseSH = false;
        mSH[0] = float3(kShSentinel, 0.f, 0.f);
    }

    return isLoaded();
#endif
}

bool FilamentIBL::tryLoadSpecularCubemap(const std::filesystem::path& path)
{
    if (!fileExists(path))
        return false;

    ref<Texture> pTex;
    try
    {
        logInfo("FilamentIBL: Loading specular cubemap '{}'.", path.string());
        pTex = ImageIO::loadTextureFromDDS(mpDevice, path, false);
    }
    catch (const std::exception& e)
    {
        logWarning("FilamentIBL: Failed to load specular cubemap '{}': {}", path.string(), e.what());
        return false;
    }
    catch (...)
    {
        logWarning("FilamentIBL: Failed to load specular cubemap '{}'.", path.string());
        return false;
    }

    if (!pTex || pTex->getType() != Texture::Type::TextureCube)
    {
        logWarning("FilamentIBL: '{}' is not a valid cubemap DDS.", path.string());
        return false;
    }

    mpSpecularCubemap = pTex;
    mRoughnessOneLevel = float(std::max(1u, mpSpecularCubemap->getMipCount()) - 1);
    logInfo("FilamentIBL: Loaded specular cubemap '{}' ({}x{}, {} mips).",
        path.string(), mpSpecularCubemap->getWidth(), mpSpecularCubemap->getHeight(), mpSpecularCubemap->getMipCount());
    return true;
}

bool FilamentIBL::tryLoadDfgLut(const std::filesystem::path& path)
{
    if (!fileExists(path))
        return false;

    ref<Texture> pTex;
    try
    {
        logInfo("FilamentIBL: Loading DFG LUT '{}'.", path.string());
        pTex = ImageIO::loadTextureFromDDS(mpDevice, path, false);
    }
    catch (const std::exception& e)
    {
        logWarning("FilamentIBL: Failed to load DFG LUT '{}': {}", path.string(), e.what());
        return false;
    }
    catch (...)
    {
        logWarning("FilamentIBL: Failed to load DFG LUT '{}'.", path.string());
        return false;
    }

    if (!pTex || pTex->getType() != Texture::Type::Texture2D)
    {
        logWarning("FilamentIBL: '{}' is not a valid 2D DFG LUT DDS.", path.string());
        return false;
    }

    mpDfgLut = pTex;
    logInfo("FilamentIBL: Loaded DFG LUT '{}' ({}x{}).", path.string(), mpDfgLut->getWidth(), mpDfgLut->getHeight());
    return true;
}

bool FilamentIBL::tryLoadSphericalHarmonics(const std::filesystem::path& path)
{
    if (!fileExists(path))
        return false;

    std::ifstream in(path);
    if (!in)
        return false;

    for (uint32_t i = 0; i < 9; ++i)
    {
        std::string line;
        if (!std::getline(in, line))
            return false;

        float r = 0.f, g = 0.f, b = 0.f;
        if (std::sscanf(line.c_str(), "(%f,%f,%f)", &r, &g, &b) != 3)
            return false;
        mSH[i] = float3(r, g, b);
    }

    mUseSH = true;
    logInfo("FilamentIBL: Loaded spherical harmonics from '{}'.", path.string());
    return true;
}

float3 FilamentIBL::cubemapDirection(uint32_t face, float u, float v)
{
    // Map [0,1] to [-1,1]
    const float uc = 2.f * u - 1.f;
    const float vc = 2.f * v - 1.f;

    switch (face)
    {
    case 0:
        return normalize(float3(1.f, -vc, -uc)); // +X
    case 1:
        return normalize(float3(-1.f, -vc, uc)); // -X
    case 2:
        return normalize(float3(uc, 1.f, vc)); // +Y
    case 3:
        return normalize(float3(uc, -1.f, -vc)); // -Y
    case 4:
        return normalize(float3(uc, -vc, 1.f)); // +Z
    default:
        return normalize(float3(-uc, -vc, -1.f)); // -Z
    }
}

float3 FilamentIBL::sampleProceduralEnvironment(const float3& dir)
{
    // Indoor studio-like gradient inspired by lightroom: warm ceiling, neutral walls, darker floor.
    const float3 skyColor = float3(0.85f, 0.82f, 0.78f);
    const float3 horizonColor = float3(0.55f, 0.52f, 0.48f);
    const float3 groundColor = float3(0.12f, 0.10f, 0.08f);

    const float t = saturate(dir.y * 0.5f + 0.5f);
    float3 color = lerp(groundColor, horizonColor, smoothstep(0.f, 0.35f, t));
    color = lerp(color, skyColor, smoothstep(0.35f, 1.f, t));

    // Soft window-like highlight.
    const float3 windowDir = normalize(float3(0.35f, 0.15f, 0.92f));
    const float window = pow(saturate(dot(dir, windowDir)), 32.f);
    color += float3(1.2f, 1.15f, 1.0f) * window * 0.35f;

    return color;
}

void FilamentIBL::createPlaceholderSpecularCubemap()
{
    std::vector<float> faceData;
    faceData.resize(size_t(kPlaceholderCubemapSize) * kPlaceholderCubemapSize * 6 * 4);

    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t y = 0; y < kPlaceholderCubemapSize; ++y)
        {
            for (uint32_t x = 0; x < kPlaceholderCubemapSize; ++x)
            {
                const float u = (float(x) + 0.5f) / float(kPlaceholderCubemapSize);
                const float v = (float(y) + 0.5f) / float(kPlaceholderCubemapSize);
                const float3 dir = cubemapDirection(face, u, v);
                const float3 color = sampleProceduralEnvironment(dir);

                const size_t idx = (size_t(face) * kPlaceholderCubemapSize * kPlaceholderCubemapSize + size_t(y) * kPlaceholderCubemapSize + x) * 4;
                faceData[idx + 0] = color.x;
                faceData[idx + 1] = color.y;
                faceData[idx + 2] = color.z;
                faceData[idx + 3] = 1.f;
            }
        }
    }

    mpSpecularCubemap = mpDevice->createTextureCube(
        kPlaceholderCubemapSize,
        kPlaceholderCubemapSize,
        ResourceFormat::RGBA32Float,
        1,
        1,
        faceData.data(),
        ResourceBindFlags::ShaderResource
    );

    mRoughnessOneLevel = 0.f;
}

void FilamentIBL::createFallbackDfgLut()
{
    std::vector<float> lutData;
    lutData.resize(size_t(kDfgLutSize) * kDfgLutSize * 4);

    for (uint32_t y = 0; y < kDfgLutSize; ++y)
    {
        const float perceptualRoughness = float(y) / float(kDfgLutSize - 1);
        for (uint32_t x = 0; x < kDfgLutSize; ++x)
        {
            const float NoV = std::max(float(x) / float(kDfgLutSize - 1), 1e-4f);
            const float e = std::exp2f((-5.55473f * NoV - 6.98316f) * NoV);

            // Approximate split-sum LUT channels (x=F0=0, y=F0=1, z=cloth).
            const float dfgX = e;
            const float dfgY = e + (1.f - e) * (1.f - perceptualRoughness);
            const float dfgZ = e;

            const size_t idx = (size_t(y) * kDfgLutSize + x) * 4;
            lutData[idx + 0] = dfgX;
            lutData[idx + 1] = dfgY;
            lutData[idx + 2] = dfgZ;
            lutData[idx + 3] = 1.f;
        }
    }

    mpDfgLut = mpDevice->createTexture2D(
        kDfgLutSize,
        kDfgLutSize,
        ResourceFormat::RGBA32Float,
        1,
        1,
        lutData.data(),
        ResourceBindFlags::ShaderResource
    );
}

void FilamentIBL::bindShaderVars(const ShaderVar& rootVar, const FilamentPostProcess::FilamentSettings& settings) const
{
    FALCOR_ASSERT(mpSpecularCubemap && mpDfgLut);

    auto cb = rootVar["FilamentIBLCB"];
    cb["gIblLuminance"] = settings.iblIntensity;
    cb["gIblRoughnessOneLevel"] = mRoughnessOneLevel;
    cb["gIblRotation"] = settings.iblRotation;
    cb["gUseSH"] = mUseSH ? 1u : 0u;

    for (uint32_t i = 0; i < 9; ++i)
        cb["gIblSH"][i] = mSH[i];

    rootVar["gIblSpecular"] = mpSpecularCubemap;
    rootVar["gIblDFG"] = mpDfgLut;
    rootVar["gIblLinearSampler"] = mpLinearSampler;
}

} // namespace Falcor
