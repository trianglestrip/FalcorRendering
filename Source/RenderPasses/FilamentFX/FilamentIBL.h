#pragma once
#include "Falcor.h"
#include "FilamentPostProcess.h"

namespace Falcor
{

/**
 * Independent Filament-style IBL resource manager.
 * Loads prefiltered specular cubemap + DFG LUT from data/ibl/ without using Scene::EnvMap.
 */
class FilamentIBL : public Object
{
public:
    static constexpr uint32_t kDfgLutSize = 128;
    static constexpr float kShSentinel = 65504.0f;

    static ref<FilamentIBL> create(ref<Device> pDevice);

    FilamentIBL(ref<Device> pDevice);

    /** Load default lightroom_14b assets from data/ibl/. Falls back to procedural placeholders on failure. */
    bool loadDefault();

    void bindShaderVars(const ShaderVar& rootVar, const FilamentPostProcess::FilamentSettings& settings) const;

    bool isLoaded() const { return mpSpecularCubemap != nullptr && mpDfgLut != nullptr; }
    bool usingPlaceholder() const { return mUsingPlaceholder; }

    float getRoughnessOneLevel() const { return mRoughnessOneLevel; }

private:
    bool tryLoadSpecularCubemap(const std::filesystem::path& path);
    bool tryLoadDfgLut(const std::filesystem::path& path);
    bool tryLoadSphericalHarmonics(const std::filesystem::path& path);

    void createPlaceholderSpecularCubemap();
    void createFallbackDfgLut();

    static float3 cubemapDirection(uint32_t face, float u, float v);
    static float3 sampleProceduralEnvironment(const float3& dir);

    ref<Device> mpDevice;
    ref<Texture> mpSpecularCubemap;
    ref<Texture> mpDfgLut;
    ref<Sampler> mpLinearSampler;

    float3 mSH[9] = {};
    bool mUseSH = false;
    bool mUsingPlaceholder = false;
    float mRoughnessOneLevel = 4.0f;
};

} // namespace Falcor
