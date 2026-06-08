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
        bool enableMSAA = false;
        bool msaaCustomResolve = false;
        bool screenSpaceGuardBand = false;

        // Light & IBL
        float iblIntensity = 1.0f;
        float iblRotation = 0.0f;
        bool enableSunlight = false;
        float sunIntensity = 0.0f;
        float3 sunColor = float3(1.0f, 1.0f, 1.0f);
        float3 sunDirection = float3(0.0f, -1.0f, 0.0f);
        float sunAngularRadiusDeg = 0.545f;
        float sunHaloSize = 10.0f;
        float sunHaloFalloff = 80.0f;
        float ambientIntensity = 0.0f;

        // Bloom
        bool enableBloom = false;
        float bloomStrength = 0.25f;
        int bloomLevels = 6;
        int bloomBlendMode = 0; // 0: Add, 1: Screen
        float bloomThreshold = 0.0f;
        bool bloomThresholdEnabled = false;
        int bloomQuality = 2;
        bool bloomLensFlare = false;

        // SSAO (Filament SAO parameters)
        bool enableSSAO = true;
        bool forwardSSAO = true; // When true, SSAO is applied in forward pass (not colorGradingPrep)
        float ssaoResolution = 0.5f;      // Filament AmbientOcclusionOptions.resolution (0.5 = half-res, 1.0 = full-res)
        float ssaoRadius = 1.0f;          // PBRT viewer default; Filament engine default is 0.3m.
        float ssaoBias = 0.001f;          // Filament default bias
        float ssaoPower = 1.0f;           // power curve (doubled internally)
        float ssaoIntensity = 1.0f;       // Filament default intensity.
        int   ssaoSampleCount = 11;       // Filament MEDIUM quality.
        int   ssaoSpiralTurns = 6;        // Filament MEDIUM quality.
        float ssaoMinHorizonAngleSineSquared = 0.0f;
        float ssaoMinHorizonAngleRad = 0.0f;
        float ssaoPeak2 = 0.0001f;
        float ssaoProjectionScale = 1.0f;
        int ssaoMode = 0; // 0: SAO, 1: GTAO horizon-based
        int ssaoQuality = 1;
        int ssaoLowPassFilter = 1;
        bool ssaoBentNormals = false;
        bool ssaoHighQualityUpsampling = true;
        int gtaoSlices = 4;
        int gtaoSteps = 3;
        float gtaoRadius = 1.0f;
        float gtaoThicknessHeuristic = 0.004f;
        bool gtaoUseVisibilityBitmasks = false;
        float gtaoConstThickness = 0.5f;
        bool gtaoLinearThickness = false;
        bool ssctEnabled = false;
        float ssctLightConeRad = 0.0f;
        float ssctShadowDistance = 0.3f;
        float ssctContactDistanceMax = 1.0f;
        float ssctIntensity = 0.8f;
        float ssctDepthBias = 0.01f;
        float ssctDepthSlopeBias = 0.01f;
        int ssctSampleCount = 4;
        float3 ssctLightDirection = float3(0.0f, -1.0f, 0.0f);

        // FSR / sharpening (RCAS post-pass; full EASU upsample when DSR < 1 is future work)
        bool enableFSR = false;
        float fsrSharpness = 0.5f;
        bool dynamicResolutionEnabled = false;
        bool dynamicResolutionHomogeneous = true;
        float dynamicResolutionMinScale = 1.0f;
        float dynamicResolutionMaxScale = 1.0f;
        int dynamicResolutionQuality = 2;
        float dynamicResolutionScale = 1.0f; // placeholder for DSR integration

        // Shadow (CSM atlas rendered in forward pass; post-process shadow is optional debug)
        bool enableShadows = true;
        bool postProcessShadow = false;
        int shadowTypeFilament = 0; // 0: PCF, 1: VSM, 2: DPCF, 3: PCSS, 4: PCFd
        int shadowType = 1; // 0: PCF Hard, 1: PCF Low (3x3), 2: VSM
        int shadowCascades = 4;
        float shadowBias = 0.001f;
        float4 cascadeSplits = float4(5.0f, 15.0f, 40.0f, 100.0f);
        uint32_t shadowMapSize = 2048;
        float vsmExponent = 5.2f;
        float vsmMaxMoment = 65504.f;
        float vsmLightBleedReduction = 0.15f;
        float vsmBlurWidth = 3.0f;
        float shadowFar = 100.0f;
        bool shadowStable = false;
        bool shadowLiSPSM = true;
        bool debugCascades = false;
        bool enableContactShadows = false;
        float3 cascadeSplitPositions = float3(0.125f, 0.25f, 0.5f);
        bool vsmHighPrecision = false;
        bool vsmElvsm = false;
        int vsmMsaaSamplesLog2 = 0;
        int vsmAnisotropy = 0;
        bool vsmMipmapping = false;
        float softShadowPenumbraScale = 1.0f;
        float softShadowPenumbraRatioScale = 1.0f;
        float4x4 shadowLightViewProj[4] = {};
        float4 cascadeAtlasRect[4] = {};

        // Depth of Field
        bool enableDoF = false;
        float dofFocalDistance = 10.0f;
        float dofAperture = 2.8f;
        float dofMaxCoC = 5.0f;
        float dofCocScale = 1.0f;
        float dofCocAspectRatio = 1.0f;
        int dofRingCount = 5;
        bool dofNativeResolution = false;
        bool dofMedianFilter = false;

        // Fog (exponential distance + height, HDR pre-tone-map)
        bool enableFog = false;
        float fogDensity = 0.02f;
        float fogStart = 0.0f;
        float3 fogColor = float3(0.7f, 0.75f, 0.8f);
        float fogHeight = 0.0f;
        float fogHeightFalloff = 1.0f;
        float fogInScatteringStart = 0.0f;
        float fogInScatteringSize = 1.0f;
        bool fogExcludeSkybox = false;
        int fogColorSource = 0; // 0: Constant, 1: IBL, 2: Skybox

        // SSR (stub: structure depth only until reflection pass is implemented)
        bool enableSSR = false;
        float ssrThickness = 0.1f;
        float ssrBias = 0.01f;
        float ssrMaxDistance = 3.0f;
        float ssrStride = 2.0f;

        // Vignette
        bool enableVignette = false;
        float vignetteMidpoint = 0.5f;
        float vignetteRoundness = 0.5f;
        float vignetteFeather = 0.5f;
        float3 vignetteColor = float3(0.0f, 0.0f, 0.0f);

        // Color Grading
        int toneMappingFilament = 0; // 0: LINEAR, 1: ACES_LEGACY, 2: ACES, 3: FILMIC, 4: AGX, 5: GENERIC, 6: PBR_NEUTRAL, 7: GT7, 8: DISPLAY_RANGE
        int toneMapping = 2; // Filament enum: 0 LINEAR, 1 ACES_LEGACY, 2 ACES, 3 FILMIC, 4 AGX, 5 GENERIC, 6 PBR_NEUTRAL, 7 GT7, 8 DISPLAY_RANGE
        bool colorGradingEnabled = true;
        bool colorGradingLinkedCurves = false;
        bool colorGradingLuminanceScaling = false;
        bool colorGradingGamutMapping = false;
        int colorGradingQuality = 1;
        int colorGradingCustomLut = 0;
        int colorGradingColorSpace = 0;
        float exposure = 0.0f; // EV
        float nightAdaptation = 0.0f;
        float temperature = 0.0f;
        float tint = 0.0f;
        float contrast = 1.0f;
        float vibrance = 1.0f;
        float saturation = 1.0f;
        bool enableColorGradingLUT = false;
        int lutSize = 32; // 16 or 32

        // TAA
        float taaFeedback = 0.9f;
        float taaUpscaling = 1.0f;
        bool taaHistoryReprojection = true;
        bool taaFilterHistory = true;
        bool taaFilterInput = true;
        float taaLodBias = -1.0f;
        bool taaHDR = true;
        bool taaUseYCoCg = false;
        bool taaPreventFlickering = true;
        int taaJitterPattern = 4;
        int taaBoxClipping = 0;
        int taaBoxType = 0;
        float taaVarianceGamma = 1.0f;
        float taaSharpness = 0.0f;
        float2 cameraJitter = float2(0.f, 0.f); // Subpixel jitter (normalized), set when antiAliasing==2

        // Scene / Camera UI mirrors Filament viewer. Most fields are UI presets/placeholders.
        bool sceneAutoScaleEnabled = false;
        bool sceneAutoInstancingEnabled = false;
        bool sceneSkyboxEnabled = true;
        float3 sceneBackgroundColor = float3(0.0f, 0.0f, 0.0f);
        bool sceneGroundPlaneEnabled = false;
        float sceneGroundShadowStrength = 0.75f;
        float cameraFocalLength = 28.0f;
        float cameraAperture = 16.0f;
        float cameraShutterSpeed = 125.0f;
        float cameraSensitivity = 100.0f;
        float cameraFocusDistance = 10.0f;

        // Camera (set each frame by PBRTOfflineRenderer)
        float4x4 invViewProj = float4x4();
        float4x4 invView = float4x4();
        float4x4 invProj = float4x4();
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        float ssaoBilateralThreshold = 0.05f;
        float2 positionParams = float2(1.155f, 0.649f); // invProj scale * 2 for x,y
        float3 cameraPos = float3(0.f);
    };

    // Custom execution for PBRTOfflineRenderer
    void executeCustom(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings, const ref<Texture>& pShadowMap = nullptr, const ref<Texture>& pMotionVec = nullptr, const ref<Texture>& pShadowMoments = nullptr);

    // EVSM separable Gaussian blur on RG32F shadow moments atlas (per-cascade tiles)
    void blurShadowMoments(RenderContext* pRenderContext, const ref<Texture>& pMoments, const ref<Texture>& pTemp, const FilamentSettings& settings);

    // Pre-pass SSAO (depth prepass -> structure -> SSAO), consumed by forward shader via getAOTexture().
    void executePrePassSSAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings);
    // Deferred MRT SSAO (depth + GBuffer normal -> structure -> SAO), consumed by fullscreen lighting via getAOTexture().
    void executeDeferredSSAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const ref<Texture>& pNormalW, const FilamentSettings& settings);
    ref<Texture> getAOTexture(const FilamentSettings& settings) const;
    ref<Sampler> getLinearSampler() const { return mpLinearSampler; }
    ref<Sampler> getPointSampler() const { return mpPointSampler; }
    uint2 getAOBufferSize() const { return uint2(mAOBufferWidth, mAOBufferHeight); }
    void bindAOShaderVars(const ShaderVar& var, const FilamentSettings& settings, const ref<Texture>& pDepthPrepass) const;

private:
    // Post-processing passes
    ref<ComputePass> mpColorGradingPass;
    ref<ComputePass> mpColorGradingPrepPass;
    ref<ComputePass> mpBloomDownsamplePass;
    ref<ComputePass> mpBloomUpsamplePass;
    ref<ComputePass> mpFXAAPass;
    ref<ComputePass> mpDoFPass;
    ref<ComputePass> mpFogPass;
    ref<ComputePass> mpFSRPass;
    ref<ComputePass> mpGTAOPass;
    ref<ComputePass> mpDeferredSSAOPass;
    ref<ComputePass> mpTAAPass;

    // Structure depth pyramid (SSAO LOD)
    ref<ComputePass> mpStructureCopyPass;
    ref<ComputePass> mpStructureMipmapPass;
    ref<Texture> mpStructureDepth;
    uint32_t mStructureLevelCount = 1;

    // SSAO passes
    ref<ComputePass> mpSSAOPass;
    ref<ComputePass> mpSSAOBlurPass;

    // Shadow map + EVSM blur passes
    ref<ComputePass> mpShadowMapPass;
    ref<ComputePass> mpShadowEVSMBlurPass;

    // Intermediate textures
    static const uint32_t kMaxBloomLevels = 7;
    static const uint32_t kMaxStructureLevels = 8;
    ref<Texture> mpBloomMips[kMaxBloomLevels];
    ref<Texture> mpColorGradingTarget;
    ref<Texture> mpPrepTarget;
    ref<Texture> mpTAATarget;
    ref<Texture> mpDoFTarget;
    ref<Texture> mpFogTarget;
    ref<Texture> mpFSRTarget;
    ref<Texture> mpZeroMotionTexture;
    ref<Texture> mpHistoryColor;
    ref<Texture> mpHistoryDepth;
    ref<Texture> mpAOBuffer;
    ref<Texture> mpAOBlurTarget;
    ref<Texture> mpAOBlurTemp;
    ref<Texture> mpShadowVisibility;
    ref<Texture> mpSSAONoiseTexture;

    // Samplers
    ref<Sampler> mpLinearSampler;
    ref<Sampler> mpPointSampler;

    // Fallback white texture for AO/Shadow when disabled
    ref<Texture> mpWhiteTexture;
    ref<Texture> mpDummyShadowMap;     // 1x1 shadow map to prevent null binding
    ref<Texture> mpDummyShadowMoments; // 1x1 EVSM moments fallback
    ref<Texture> mpColorLUT;
    ref<Texture> mpIdentityLUT;        // 1x1x1 fallback when LUT path disabled

    // Helper functions
    void updateColorGradingLUT(ref<Device> pDevice, const FilamentSettings& settings);
    void updateBloomTextures(ref<Device> pDevice, uint32_t width, uint32_t height, uint32_t levels);
    void updateStructureTextures(ref<Device> pDevice, uint32_t width, uint32_t height);
    void executeStructure(RenderContext* pRenderContext, const ref<Texture>& pDepth);
    void updateAOTextures(ref<Device> pDevice, uint32_t width, uint32_t height, float resolutionScale);
    void createNoiseTexture(ref<Device> pDevice);
    void executeSSAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings);
    void executeDeferredSSAOInternal(RenderContext* pRenderContext, const ref<Texture>& pDepth, const ref<Texture>& pNormalW, const FilamentSettings& settings);
    void executeGTAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings);
    void executeFSR(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDst, const FilamentSettings& settings);
    void executeShadowMap(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings, const ref<Texture>& pShadowMapDepth = nullptr, const ref<Texture>& pShadowMoments = nullptr);
    void executeDoF(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings);
    void executeFog(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings);
    void executeSSR(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings);
    void executeTAA(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings, const ref<Texture>& pMotionVec);
    void executeColorGradingPrep(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDst, const FilamentSettings& settings, const ref<Texture>& pDepth);
    void executeColorGradingComposite(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDst, const FilamentSettings& settings);
    void updateHistory(RenderContext* pRenderContext, const ref<Texture>& pColor, const ref<Texture>& pDepth, uint32_t width, uint32_t height);
    uint32_t mHistoryWidth = 0;
    uint32_t mHistoryHeight = 0;
    uint32_t mAOBufferWidth = 0;
    uint32_t mAOBufferHeight = 0;
    uint32_t mLUTSize = 0;
    float mLUTExposure = 0.0f;
    float mLUTContrast = 1.0f;
    float mLUTVibrance = 1.0f;
    float mLUTSaturation = 1.0f;

    FilamentSettings mSettings;
    void parseProperties(const Properties& props);
};
