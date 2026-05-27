#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"

using namespace Falcor;

class FilamentPostProcess : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(FilamentPostProcess, "FilamentPostProcess", "Filament Post Processing Pass");

    static ref<FilamentPostProcess> create(ref<Device> pDevice, const Properties& props) { return make_ref<FilamentPostProcess>(pDevice, props); }

    FilamentPostProcess(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;

    // Filament Strict UI Settings
    struct FilamentSettings {
        // View
        bool postProcessingEnabled = true;
        int antiAliasing = 1; // 0: None, 1: FXAA, 2: TAA
        bool dithering = true;

        // Light & IBL
        float iblIntensity = 30000.0f;
        float iblRotation = 0.0f;
        float sunIntensity = 100000.0f;
        float3 sunColor = float3(1.0f, 1.0f, 1.0f);
        float3 sunDirection = float3(0.0f, -1.0f, 0.0f);

        // Bloom
        bool enableBloom = true;
        float bloomStrength = 0.1f;
        int bloomLevels = 6;
        int bloomBlendMode = 0; // 0: Add, 1: Screen
        float bloomThreshold = 1.0f;

        // SSAO (Filament SAO parameters)
        bool enableSSAO = true;
        float ssaoRadius = 0.3f;          // Filament default radius
        float ssaoBias = 0.001f;          // Filament default bias
        float ssaoPower = 1.0f;           // power curve (doubled internally)
        float ssaoIntensity = 1.0f;       // Filament default intensity
        int   ssaoSampleCount = 11;       // Filament MEDIUM quality: 11
        int   ssaoSpiralTurns = 6;        // Filament MEDIUM: 6
        float ssaoMinHorizonAngleSineSquared = 0.0f;
        float ssaoPeak2 = 0.0001f;
        float ssaoProjectionScale = 1.0f;

        // Shadow (safe dummy shadow map + identity matrices, toggle won't crash)
        bool enableShadows = true;
        int shadowType = 1; // 0: PCF Hard, 1: PCF Low (3x3), 2: VSM
        int shadowCascades = 4;
        float shadowBias = 0.001f;
        float4 cascadeSplits = float4(5.0f, 15.0f, 40.0f, 100.0f);
        uint32_t shadowMapSize = 2048;
        float4x4 shadowLightViewProj = float4x4(); // Set by renderShadowMap

        // Depth of Field
        bool enableDoF = false;
        float dofFocalDistance = 10.0f;
        float dofAperture = 2.8f;
        float dofMaxCoC = 5.0f;

        // Vignette
        bool enableVignette = true;
        float vignetteMidpoint = 0.5f;
        float vignetteRoundness = 0.5f;
        float vignetteFeather = 0.5f;
        float3 vignetteColor = float3(0.0f, 0.0f, 0.0f);

        // Color Grading
        int toneMapping = 0; // 0: ACES, 1: Filmic, 2: Linear, 3: Display
        float exposure = 0.0f; // EV
        float contrast = 1.0f;
        float vibrance = 1.0f;
        float saturation = 1.0f;
    };

    // Custom execution for PBRTOfflineRenderer
    void executeCustom(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings, const ref<Texture>& pShadowMap = nullptr);

private:
    // Post-processing passes
    ref<ComputePass> mpColorGradingPass;
    ref<ComputePass> mpBloomDownsamplePass;
    ref<ComputePass> mpBloomUpsamplePass;
    ref<ComputePass> mpFXAAPass;

    // SSAO passes
    ref<ComputePass> mpSSAOPass;
    ref<ComputePass> mpSSAOBlurPass;

    // Shadow map pass
    ref<ComputePass> mpShadowMapPass;

    // Intermediate textures
    static const uint32_t kMaxBloomLevels = 7;
    ref<Texture> mpBloomMips[kMaxBloomLevels];
    ref<Texture> mpColorGradingTarget;
    ref<Texture> mpAOBuffer;
    ref<Texture> mpAOBlurTarget;
    ref<Texture> mpShadowVisibility;
    ref<Texture> mpSSAONoiseTexture;

    // Samplers
    ref<Sampler> mpLinearSampler;
    ref<Sampler> mpPointSampler;

    // Fallback white texture for AO/Shadow when disabled
    ref<Texture> mpWhiteTexture;
    ref<Texture> mpDummyShadowMap;     // 1x1 shadow map to prevent null binding

    // Helper functions
    void updateBloomTextures(ref<Device> pDevice, uint32_t width, uint32_t height, uint32_t levels);
    void updateAOTextures(ref<Device> pDevice, uint32_t width, uint32_t height);
    void createNoiseTexture(ref<Device> pDevice);
    void executeSSAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings);
    void executeShadowMap(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings, const ref<Texture>& pShadowMapDepth = nullptr);
};