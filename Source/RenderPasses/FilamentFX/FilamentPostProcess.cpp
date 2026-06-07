#include "FilamentPostProcess.h"
#include <cmath>
#include <random>
#include <vector>

namespace
{
    const char kSrc[] = "src";
    const char kDst[] = "dst";
    const char kDepth[] = "depth";
    const char kMotionVec[] = "motionVec";
    const char kShadowMap[] = "shadowMap";

    const char kEnableFog[] = "enableFog";
    const char kFogDensity[] = "fogDensity";
    const char kFogStart[] = "fogStart";
    const char kFogColor[] = "fogColor";
    const char kEnableSSR[] = "enableSSR";
    const char kEnableBloom[] = "enableBloom";
    const char kEnableSSAO[] = "enableSSAO";
    const char kEnableDoF[] = "enableDoF";
    const char kAntiAliasing[] = "antiAliasing";
    const char kPostProcessingEnabled[] = "postProcessingEnabled";

    const char kColorGradingShader[] = "RenderPasses/FilamentFX/ColorGrading.cs.slang";
    const char kBloomShader[] = "RenderPasses/FilamentFX/Bloom.cs.slang";
    const char kFXAAShader[] = "RenderPasses/FilamentFX/FXAA.cs.slang";
    const char kStructureShader[] = "RenderPasses/FilamentFX/StructurePass.cs.slang";
    const char kSSAOShader[] = "RenderPasses/FilamentFX/SSAO.cs.slang";
    const char kShadowMapShader[] = "RenderPasses/FilamentFX/ShadowMap.cs.slang";
    const char kShadowEVSMShader[] = "RenderPasses/FilamentFX/ShadowEVSM.cs.slang";
    const char kDoFShader[] = "RenderPasses/FilamentFX/DoF.cs.slang";
    const char kFogShader[] = "RenderPasses/FilamentFX/Fog.cs.slang";
    const char kFSRShader[] = "RenderPasses/FilamentFX/FSR.cs.slang";
    const char kGTAOShader[] = "RenderPasses/FilamentFX/GTAO.cs.slang";
    const char kTAAShader[] = "RenderPasses/FilamentFX/TAA.cs.slang";

    const uint32_t kNoiseTextureSize = 4; // 4x4 noise texture for SSAO
    const uint32_t kMaxStructureLevels = 8;

    struct AOQualityParams
    {
        float sampleCount = 7.0f;
        float spiralTurns = 3.0f;
        float standardDeviation = 8.0f;
        uint32_t kernelSize = 11;
        float blurScale = 2.0f;
    };

    AOQualityParams getAOQualityParams(const FilamentPostProcess::FilamentSettings& settings)
    {
        AOQualityParams params;
        const int sampleCount = std::clamp(settings.ssaoSampleCount, 4, 64);

        if (sampleCount <= 7)
        {
            params.sampleCount = 7.0f;
            params.spiralTurns = 3.0f;
            params.standardDeviation = 8.0f;
        }
        else if (sampleCount <= 11)
        {
            params.sampleCount = 11.0f;
            params.spiralTurns = 6.0f;
            params.standardDeviation = 8.0f;
        }
        else if (sampleCount <= 16)
        {
            params.sampleCount = 16.0f;
            params.spiralTurns = 7.0f;
            params.standardDeviation = 6.0f;
        }
        else
        {
            params.sampleCount = 32.0f;
            params.spiralTurns = 14.0f;
            params.standardDeviation = 4.0f;
        }

        params.kernelSize = 11;
        params.standardDeviation *= 0.5f;
        params.blurScale = 2.0f;
        return params;
    }

    float getProjectionScale(float2 positionParams, uint2 resolution)
    {
        const float proj00 = 2.0f / std::max(positionParams.x, 1e-6f);
        const float proj11 = 2.0f / std::max(positionParams.y, 1e-6f);
        return std::min(0.5f * proj00 * float(resolution.x), 0.5f * proj11 * float(resolution.y));
    }

    uint32_t makeGaussianKernel(float* outKernel, uint32_t kernelSize, float standardDeviation)
    {
        constexpr uint32_t kKernelArraySize = 16;
        const uint32_t sampleCount = std::min(kKernelArraySize, (kernelSize + 1u) / 2u);
        for (uint32_t i = 0; i < sampleCount; ++i)
        {
            const float x = float(i);
            outKernel[i] = std::exp(-(x * x) / (2.0f * standardDeviation * standardDeviation));
        }
        return sampleCount;
    }

    float3 colorGradeCPU(float3 color, float exposure, float contrast, float saturation, float vibrance)
    {
        color *= std::exp2(exposure);
        color = saturate(color);
        color = pow(color, float3(contrast));
        const float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
        color = lerp(float3(luma), color, saturation);
        const float maxColor = std::max(color.x, std::max(color.y, color.z));
        const float minColor = std::min(color.x, std::min(color.y, color.z));
        const float sat = maxColor - minColor;
        const float sign = maxColor > 0.0f ? 1.0f : (maxColor < 0.0f ? -1.0f : 0.0f);
        color = lerp(float3(luma), color, 1.0f + (vibrance * (1.0f - (sign * sat))));
        return color;
    }
}

void FilamentPostProcess::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kPostProcessingEnabled)
            mSettings.postProcessingEnabled = value;
        else if (key == kAntiAliasing)
            mSettings.antiAliasing = value;
        else if (key == kEnableBloom)
            mSettings.enableBloom = value;
        else if (key == kEnableSSAO)
            mSettings.enableSSAO = value;
        else if (key == kEnableDoF)
            mSettings.enableDoF = value;
        else if (key == kEnableFog)
            mSettings.enableFog = value;
        else if (key == kFogDensity)
            mSettings.fogDensity = value;
        else if (key == kFogStart)
            mSettings.fogStart = value;
        else if (key == kFogColor)
            mSettings.fogColor = value;
        else if (key == kEnableSSR)
            mSettings.enableSSR = value;
        else
            logWarning("Unknown property '{}' in FilamentPostProcess properties.", key);
    }
}

FilamentPostProcess::FilamentPostProcess(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    parseProperties(props);

    DefineList defines;
    
    // Color Grading prep (AO/shadow before TAA) + composite (bloom/grading after DoF)
    ProgramDesc cgPrepDesc;
    cgPrepDesc.addShaderLibrary(kColorGradingShader).csEntry("colorGradingPrep");
    cgPrepDesc.addCompilerArguments({"-Wno-30081"});
    mpColorGradingPrepPass = ComputePass::create(mpDevice, cgPrepDesc, defines);

    ProgramDesc cgDesc;
    cgDesc.addShaderLibrary(kColorGradingShader).csEntry("colorGradingMain");
    cgDesc.addCompilerArguments({"-Wno-30081"});
    mpColorGradingPass = ComputePass::create(mpDevice, cgDesc, defines);

    // Bloom
    ProgramDesc bloomDownDesc;
    bloomDownDesc.addShaderLibrary(kBloomShader).csEntry("bloomDownsample");
    bloomDownDesc.addCompilerArguments({"-Wno-30081"});
    mpBloomDownsamplePass = ComputePass::create(mpDevice, bloomDownDesc, defines);

    ProgramDesc bloomUpDesc;
    bloomUpDesc.addShaderLibrary(kBloomShader).csEntry("bloomUpsample");
    bloomUpDesc.addCompilerArguments({"-Wno-30081"});
    mpBloomUpsamplePass = ComputePass::create(mpDevice, bloomUpDesc, defines);

    // FXAA
    ProgramDesc fxaaDesc;
    fxaaDesc.addShaderLibrary(kFXAAShader).csEntry("fxaaMain");
    fxaaDesc.addCompilerArguments({"-Wno-30081"});
    mpFXAAPass = ComputePass::create(mpDevice, fxaaDesc, defines);

    // Structure depth pyramid
    ProgramDesc structureCopyDesc;
    structureCopyDesc.addShaderLibrary(kStructureShader).csEntry("structureCopy");
    structureCopyDesc.addCompilerArguments({"-Wno-30081"});
    mpStructureCopyPass = ComputePass::create(mpDevice, structureCopyDesc, defines);

    ProgramDesc structureMipmapDesc;
    structureMipmapDesc.addShaderLibrary(kStructureShader).csEntry("structureMipmap");
    structureMipmapDesc.addCompilerArguments({"-Wno-30081"});
    mpStructureMipmapPass = ComputePass::create(mpDevice, structureMipmapDesc, defines);

    // SSAO
    ProgramDesc ssaoDesc;
    ssaoDesc.addShaderLibrary(kSSAOShader).csEntry("ssaoMain");
    ssaoDesc.addCompilerArguments({"-Wno-30081"});
    mpSSAOPass = ComputePass::create(mpDevice, ssaoDesc, defines);

    ProgramDesc ssaoBlurDesc;
    ssaoBlurDesc.addShaderLibrary(kSSAOShader).csEntry("ssaoBlur");
    ssaoBlurDesc.addCompilerArguments({"-Wno-30081"});
    mpSSAOBlurPass = ComputePass::create(mpDevice, ssaoBlurDesc, defines);

    // Shadow Map
    ProgramDesc shadowDesc;
    shadowDesc.addShaderLibrary(kShadowMapShader).csEntry("shadowMapMain");
    shadowDesc.addCompilerArguments({"-Wno-30081"});
    mpShadowMapPass = ComputePass::create(mpDevice, shadowDesc, defines);

    ProgramDesc evsmBlurDesc;
    evsmBlurDesc.addShaderLibrary(kShadowEVSMShader).csEntry("evsmBlur");
    evsmBlurDesc.addCompilerArguments({"-Wno-30081"});
    mpShadowEVSMBlurPass = ComputePass::create(mpDevice, evsmBlurDesc, defines);

    // Depth of Field
    ProgramDesc dofDesc;
    dofDesc.addShaderLibrary(kDoFShader).csEntry("dofMain");
    dofDesc.addCompilerArguments({"-Wno-30081"});
    mpDoFPass = ComputePass::create(mpDevice, dofDesc, defines);

    // Fog
    ProgramDesc fogDesc;
    fogDesc.addShaderLibrary(kFogShader).csEntry("fogMain");
    fogDesc.addCompilerArguments({"-Wno-30081"});
    mpFogPass = ComputePass::create(mpDevice, fogDesc, defines);

    ProgramDesc fsrDesc;
    fsrDesc.addShaderLibrary(kFSRShader).csEntry("fsrRcasMain");
    fsrDesc.addCompilerArguments({"-Wno-30081"});
    mpFSRPass = ComputePass::create(mpDevice, fsrDesc, defines);

    ProgramDesc gtaoDesc;
    gtaoDesc.addShaderLibrary(kGTAOShader).csEntry("gtaoMain");
    gtaoDesc.addCompilerArguments({"-Wno-30081"});
    mpGTAOPass = ComputePass::create(mpDevice, gtaoDesc, defines);

    // TAA
    ProgramDesc taaDesc;
    taaDesc.addShaderLibrary(kTAAShader).csEntry("taaMain");
    taaDesc.addCompilerArguments({"-Wno-30081"});
    mpTAAPass = ComputePass::create(mpDevice, taaDesc, defines);

    // Samplers
    Sampler::Desc linearSamplerDesc;
    linearSamplerDesc.setFilterMode(TextureFilteringMode::Linear, TextureFilteringMode::Linear, TextureFilteringMode::Linear);
    linearSamplerDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
    mpLinearSampler = pDevice->createSampler(linearSamplerDesc);

    Sampler::Desc pointSamplerDesc;
    pointSamplerDesc.setFilterMode(TextureFilteringMode::Point, TextureFilteringMode::Point, TextureFilteringMode::Point);
    pointSamplerDesc.setAddressingMode(TextureAddressingMode::Clamp, TextureAddressingMode::Clamp, TextureAddressingMode::Clamp);
    mpPointSampler = pDevice->createSampler(pointSamplerDesc);

    // Create SSAO noise texture
    createNoiseTexture(pDevice);

    // Create 1x1 white fallback texture for AO/Shadow when disabled
    float whitePixel[4] = {1.f, 1.f, 1.f, 1.f};
    mpWhiteTexture = pDevice->createTexture2D(1, 1, ResourceFormat::RGBA32Float, 1, 1, whitePixel,
        ResourceBindFlags::ShaderResource);

    // Create dummy shadow map (1x1, value 1.0 = no geometry in reversed-Z)
    float onePixel = 1.f;
    mpDummyShadowMap = pDevice->createTexture2D(1, 1, ResourceFormat::R32Float, 1, 1, &onePixel,
        ResourceBindFlags::ShaderResource);

    // Dummy EVSM moments (fully lit)
    float litMoments[2] = {65504.f, 65504.f};
    mpDummyShadowMoments = pDevice->createTexture2D(1, 1, ResourceFormat::RG32Float, 1, 1, litMoments,
        ResourceBindFlags::ShaderResource);

    // Zero motion vector fallback (RG32Float)
    float zeroMotion[2] = {0.f, 0.f};
    mpZeroMotionTexture = pDevice->createTexture2D(1, 1, ResourceFormat::RG32Float, 1, 1, zeroMotion,
        ResourceBindFlags::ShaderResource);

    // 1x1x1 fallback when LUT path is disabled
    float identityLUT[4] = {1.f, 1.f, 1.f, 1.f};
    mpIdentityLUT = pDevice->createTexture3D(1, 1, 1, ResourceFormat::RGBA16Float, 1, identityLUT,
        ResourceBindFlags::ShaderResource);

    // Procedural 3D color grading LUT (exposure/contrast/saturation/vibrance baked on change)
    FilamentSettings defaultLUTSettings;
    updateColorGradingLUT(pDevice, defaultLUTSettings);
}

void FilamentPostProcess::updateColorGradingLUT(ref<Device> pDevice, const FilamentSettings& settings)
{
    const uint32_t size = (uint32_t)(settings.lutSize == 16 ? 16 : 32);
    const bool needsRebuild = !mpColorLUT || mLUTSize != size
        || mLUTExposure != settings.exposure || mLUTContrast != settings.contrast
        || mLUTVibrance != settings.vibrance || mLUTSaturation != settings.saturation;
    if (!needsRebuild)
        return;

    const size_t voxelCount = size_t(size) * size * size;
    std::vector<float> lutData(voxelCount * 4);
    const float denom = (size > 1) ? float(size - 1) : 1.0f;

    for (uint32_t z = 0; z < size; ++z)
    {
        for (uint32_t y = 0; y < size; ++y)
        {
            for (uint32_t x = 0; x < size; ++x)
            {
                const float3 input(float(x) / denom, float(y) / denom, float(z) / denom);
                const float3 graded = colorGradeCPU(input, settings.exposure, settings.contrast, settings.saturation, settings.vibrance);
                const size_t idx = (size_t(z) * size * size + size_t(y) * size + x) * 4;
                lutData[idx + 0] = graded.x;
                lutData[idx + 1] = graded.y;
                lutData[idx + 2] = graded.z;
                lutData[idx + 3] = 1.0f;
            }
        }
    }

    mpColorLUT = pDevice->createTexture3D(size, size, size, ResourceFormat::RGBA16Float, 1, lutData.data(),
        ResourceBindFlags::ShaderResource);

    mLUTSize = size;
    mLUTExposure = settings.exposure;
    mLUTContrast = settings.contrast;
    mLUTVibrance = settings.vibrance;
    mLUTSaturation = settings.saturation;
}

void FilamentPostProcess::createNoiseTexture(ref<Device> pDevice)
{
    // Generate 4x4 random normal vectors for SSAO kernel rotation
    std::default_random_engine rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    
    std::vector<float> noiseData(kNoiseTextureSize * kNoiseTextureSize * 2);
    for (uint32_t i = 0; i < kNoiseTextureSize * kNoiseTextureSize; ++i)
    {
        float x = dist(rng);
        float y = dist(rng);
        float len = std::sqrt(x * x + y * y);
        noiseData[i * 2 + 0] = x / len;
        noiseData[i * 2 + 1] = y / len;
    }
    
    mpSSAONoiseTexture = pDevice->createTexture2D(
        kNoiseTextureSize, kNoiseTextureSize, ResourceFormat::RG32Float, 1, 1,
        noiseData.data(),
        ResourceBindFlags::ShaderResource);
}

void FilamentPostProcess::updateBloomTextures(ref<Device> pDevice, uint32_t width, uint32_t height, uint32_t levels)
{
    uint32_t mipWidth = width / 2;
    uint32_t mipHeight = height / 2;
    for (uint32_t i = 0; i < kMaxBloomLevels; ++i)
    {
        if (i < levels && mipWidth > 0 && mipHeight > 0)
        {
            if (!mpBloomMips[i] || mpBloomMips[i]->getWidth() != mipWidth || mpBloomMips[i]->getHeight() != mipHeight)
            {
                mpBloomMips[i] = pDevice->createTexture2D(mipWidth, mipHeight, ResourceFormat::RGBA16Float, 1, 1, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget);
            }
            mipWidth /= 2;
            mipHeight /= 2;
        }
        else
        {
            mpBloomMips[i] = nullptr;
        }
    }
}

uint32_t calcStructureLevelCount(uint32_t width, uint32_t height)
{
    // Filament: min(8, FTexture::maxLevelCount(w,h) - 5), lowest mip >= 32px
    const uint32_t maxDim = std::max(width, height);
    const uint32_t maxLevelCount = std::max(1u, (uint32_t)std::floor(std::log2((float)maxDim)) + 1u);
    const int32_t reduced = (int32_t)maxLevelCount - 5;
    return std::min(kMaxStructureLevels, (uint32_t)std::max(1, reduced));
}

void FilamentPostProcess::updateStructureTextures(ref<Device> pDevice, uint32_t width, uint32_t height)
{
    mStructureLevelCount = calcStructureLevelCount(width, height);

    if (!mpStructureDepth || mpStructureDepth->getWidth() != width || mpStructureDepth->getHeight() != height ||
        mpStructureDepth->getMipCount() != mStructureLevelCount)
    {
        mpStructureDepth = pDevice->createTexture2D(
            width, height, ResourceFormat::R32Float, 1, mStructureLevelCount, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }
}

void FilamentPostProcess::executeStructure(RenderContext* pRenderContext, const ref<Texture>& pDepth)
{
    if (!pDepth || !mpStructureCopyPass || !mpStructureMipmapPass) return;

    const uint32_t width = pDepth->getWidth();
    const uint32_t height = pDepth->getHeight();
    updateStructureTextures(mpDevice, width, height);

    if (!mpStructureDepth) return;

    // Copy main depth into structure mip 0
    {
        auto var = mpStructureCopyPass->getRootVar();
        auto cb = var["PerFrameCB"];
        if (cb.isValid())
            cb["gDstRes"] = uint2(width, height);
        var["gSrcDepth"] = pDepth;
        var["gDst"].setUav(mpStructureDepth->getUAV(0));
        mpStructureCopyPass->execute(pRenderContext, uint3(width, height, 1));
    }

    // Min-depth mip chain (mipmapDepth.mat equivalent)
    for (uint32_t level = 0; level + 1 < mStructureLevelCount; ++level)
    {
        const uint32_t srcW = mpStructureDepth->getWidth(level);
        const uint32_t srcH = mpStructureDepth->getHeight(level);
        const uint32_t dstW = mpStructureDepth->getWidth(level + 1);
        const uint32_t dstH = mpStructureDepth->getHeight(level + 1);

        auto var = mpStructureMipmapPass->getRootVar();
        auto cb = var["PerFrameCB"];
        if (cb.isValid())
        {
            cb["gSrcRes"] = uint2(srcW, srcH);
            cb["gDstRes"] = uint2(dstW, dstH);
            cb["gSrcMip"] = level;
        }
        var["gStructureDepth"] = mpStructureDepth;
        var["gDst"].setUav(mpStructureDepth->getUAV(level + 1));
        var["gPointSampler"] = mpPointSampler;

        mpStructureMipmapPass->execute(pRenderContext, uint3(dstW, dstH, 1));
    }
}

void FilamentPostProcess::updateAOTextures(ref<Device> pDevice, uint32_t width, uint32_t height, float resolutionScale)
{
    const float scale = std::clamp(resolutionScale, 0.25f, 1.0f);
    const uint32_t aoW = std::max(1u, uint32_t(std::round(float(width) * scale)));
    const uint32_t aoH = std::max(1u, uint32_t(std::round(float(height) * scale)));
    mAOBufferWidth = aoW;
    mAOBufferHeight = aoH;

    if (!mpAOBuffer || mpAOBuffer->getWidth() != aoW || mpAOBuffer->getHeight() != aoH)
    {
        mpAOBuffer = pDevice->createTexture2D(aoW, aoH, ResourceFormat::RG32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }
    if (!mpAOBlurTarget || mpAOBlurTarget->getWidth() != aoW || mpAOBlurTarget->getHeight() != aoH)
    {
        mpAOBlurTarget = pDevice->createTexture2D(aoW, aoH, ResourceFormat::RG32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }
    if (!mpAOBlurTemp || mpAOBlurTemp->getWidth() != aoW || mpAOBlurTemp->getHeight() != aoH)
    {
        mpAOBlurTemp = pDevice->createTexture2D(aoW, aoH, ResourceFormat::RG32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }
}

void FilamentPostProcess::bindAOShaderVars(const ShaderVar& var, const FilamentSettings& settings, const ref<Texture>& pDepthPrepass) const
{
    if (!var.isValid()) return;

    const bool halfRes = settings.enableSSAO && settings.ssaoResolution < 0.999f;
    auto cb = var["AODataCB"];
    if (cb.isValid())
    {
        cb["gSSAOHalfResEnabled"] = halfRes ? 1u : 0u;
        cb["gSSAOBufferSize"] = float2(float(mAOBufferWidth), float(mAOBufferHeight));
        cb["gAOBilateralEdgeDist"] = settings.ssaoBilateralEdgeDistance;
        cb["gInvFarPlane"] = 1.0f / std::max(settings.farPlane, 1.0f);
        cb["gInvProj"] = settings.invProj;
        cb["gPositionParams"] = settings.positionParams;
    }

    if (var["gAODepth"].isValid())
        var["gAODepth"] = pDepthPrepass ? pDepthPrepass : mpWhiteTexture;
    if (var["gAODepthPointSampler"].isValid())
        var["gAODepthPointSampler"] = mpPointSampler;
}

void FilamentPostProcess::executeSSAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings)
{
    if (!mpSSAOPass || !mpSSAOBlurPass || !pDepth || !mpStructureDepth) return;

    const uint2 fullResolution = uint2(pDepth->getWidth(), pDepth->getHeight());
    updateAOTextures(mpDevice, fullResolution.x, fullResolution.y, settings.ssaoResolution);
    const uint2 resolution = uint2(mAOBufferWidth, mAOBufferHeight);

    // Filament SAO parameters (matching PostProcessManager::screenSpaceAmbientOcclusion)
    const AOQualityParams quality = getAOQualityParams(settings);
    float sampleCount = quality.sampleCount;
    float radius = settings.ssaoRadius;
    float invRadiusSquared = 1.0f / (radius * radius);
    float angleInc = (2.0f * 3.14159265f) / (sampleCount - 0.5f) * quality.spiralTurns;
    
    float2 posParams = settings.positionParams;
    float projectionScale = getProjectionScale(posParams, resolution);
    float projectionScaleRadius = projectionScale * radius;

    // peak: from Filament = 0.1 * radius
    float peak = 0.1f * radius;
    float peak2 = peak * peak;

    // intensity: from Filament = (2*PI * peak * userIntensity) / sampleCount
    float intensity = (6.283185307f * peak * settings.ssaoIntensity) / sampleCount;

    // power: from Filament = userPower * 2.0 (always square for better look)
    float power = settings.ssaoPower * 2.0f;

    // SSAO main pass (Filament SAO)
    {
        auto var = mpSSAOPass->getRootVar();
        auto cb = var["PerFrameCB"];
        if (cb.isValid())
        {
            cb["gResolution"] = float4((float)resolution.x, (float)resolution.y, 1.f/resolution.x, 1.f/resolution.y);
            // positionParams: invProjection[0][0]*2, invProjection[1][1]*2
            // 60deg FOV, 16:9 aspect: invProj00=0.577, invProj11=0.324 → *2 = 1.155, 0.649
            cb["gPositionParams"]  = posParams;
            cb["gInvRadiusSquared"] = invRadiusSquared;
            cb["gMinHorizonAngleSineSquared"] = settings.ssaoMinHorizonAngleSineSquared;
            cb["gBias"]      = settings.ssaoBias;
            cb["gPeak2"]     = peak2;
            cb["gProjectionScaleRadius"] = projectionScaleRadius;
            cb["gIntensity"] = intensity;
            cb["gPower"]     = power;
            cb["gSpiralTurns"] = quality.spiralTurns;
            cb["gSampleCount"]  = float2(sampleCount, 1.0f / (sampleCount - 0.5f));
            cb["gAngleIncCosSin"] = float2(cos(angleInc), sin(angleInc));
            // Filament: invFarPlane = 1 / -zf (view-space far is negative in their convention)
            cb["gInvFarPlane"] = 1.0f / std::max(settings.farPlane, 1.0f);
            cb["gMaxLevel"]   = (int)std::max(0u, mStructureLevelCount - 1);
        }
        auto camCB = var["CameraCB"];
        if (camCB.isValid())
        {
            camCB["gNearPlane"] = settings.nearPlane;
            camCB["gFarPlane"] = settings.farPlane;
            camCB["gInvProj"] = settings.invProj;
        }
        var["gDepth"] = pDepth;
        var["gStructureDepth"] = mpStructureDepth;
        var["gDst"].setUav(mpAOBuffer->getUAV(0));
        var["gPointSampler"] = mpPointSampler;

        mpSSAOPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
    }

    float kGaussianSamples[16] = {};
    const uint32_t gaussianSampleCount = makeGaussianKernel(kGaussianSamples, quality.kernelSize, quality.standardDeviation);

    const float farPlaneOverEdgeDistance = settings.farPlane / std::max(settings.ssaoBilateralThreshold, 1e-4f);
    const float2 axes[2] = {
        float2(quality.blurScale / resolution.x, 0.f),
        float2(0.f, quality.blurScale / resolution.y)
    };
    ref<Texture> blurInput = mpAOBuffer;
    ref<Texture> blurOutputs[2] = { mpAOBlurTemp, mpAOBlurTarget };

    for (uint32_t pass = 0; pass < 2; ++pass)
    {
        auto var = mpSSAOBlurPass->getRootVar();
        auto cb = var["PerFrameCB"];
        if (cb.isValid())
        {
            cb["gResolution"] = float4((float)resolution.x, (float)resolution.y, 1.f / resolution.x, 1.f / resolution.y);
            cb["gBlurAxis"] = axes[pass];
            cb["gFarPlaneOverEdgeDistance"] = farPlaneOverEdgeDistance;
            cb["gBlurSampleCount"] = (int)gaussianSampleCount;
            for (uint32_t i = 0; i < gaussianSampleCount; ++i)
                cb["gBlurKernel"][i] = kGaussianSamples[i];
        }
        var["gAOBuffer"] = blurInput;
        var["gBlurredAO"].setUav(blurOutputs[pass]->getUAV(0));
        var["gPointSampler"] = mpPointSampler;

        mpSSAOBlurPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
        blurInput = blurOutputs[pass];
    }
}

void FilamentPostProcess::blurShadowMoments(RenderContext* pRenderContext, const ref<Texture>& pMoments, const ref<Texture>& pTemp, const FilamentSettings& settings)
{
    if (!mpShadowEVSMBlurPass || !pMoments || !pTemp || settings.vsmBlurWidth <= 0.f)
        return;

    const uint32_t atlasSize = settings.shadowMapSize;
    const uint2 resolution = uint2(atlasSize, atlasSize);
    const float blurWidth = settings.vsmBlurWidth;
    const float sigma = blurWidth / 2.5f;
    const int radius = std::min(int(std::ceil(blurWidth)), 31);
    const int sampleCount = radius + 1;

    float kernel[32] = {};
    float sum = 0.f;
    for (int i = 0; i <= radius; ++i)
    {
        kernel[i] = std::exp(-(float(i * i) / (2.f * sigma * sigma)));
        sum += (i == 0) ? kernel[i] : (2.f * kernel[i]);
    }
    const float invSum = 1.f / sum;
    for (int i = 0; i <= radius; ++i)
        kernel[i] *= invSum;

    const int2 dirs[2] = {int2(1, 0), int2(0, 1)};
    ref<Texture> blurInput = pMoments;
    ref<Texture> blurOutput = pTemp;
    const uint32_t cascadeCount = (uint32_t)std::clamp(settings.shadowCascades, 1, 4);

    for (uint32_t pass = 0; pass < 2; ++pass)
    {
        for (uint32_t c = 0; c < cascadeCount; ++c)
        {
            const float4& rect = settings.cascadeAtlasRect[c];
            const int2 boundsMin = int2(
                int(std::floor(rect.x * float(atlasSize))),
                int(std::floor(rect.y * float(atlasSize))));
            const int2 boundsMax = int2(
                int(std::ceil((rect.x + rect.z) * float(atlasSize))) - 1,
                int(std::ceil((rect.y + rect.w) * float(atlasSize))) - 1);

            auto var = mpShadowEVSMBlurPass->getRootVar();
            auto cb = var["PerFrameCB"];
            if (cb.isValid())
            {
                cb["gResolution"] = resolution;
                cb["gBlurDir"] = dirs[pass];
                cb["gBlurRadius"] = radius;
                cb["gBlurSampleCount"] = sampleCount;
                cb["gBoundsMin"] = boundsMin;
                cb["gBoundsMax"] = boundsMax;
                for (int i = 0; i < sampleCount; ++i)
                    cb["gBlurKernel"][i] = kernel[i];
            }
            var["gSrcMoments"] = blurInput;
            var["gDstMoments"].setUav(blurOutput->getUAV(0));
            var["gPointSampler"] = mpPointSampler;
            mpShadowEVSMBlurPass->execute(pRenderContext, uint3(atlasSize, atlasSize, 1));
        }
        std::swap(blurInput, blurOutput);
    }
}

void FilamentPostProcess::executeShadowMap(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings, const ref<Texture>& pShadowMapDepth, const ref<Texture>& pShadowMoments)
{
    if (!mpShadowMapPass || !pDepth) return;

    const uint2 resolution = uint2(pDepth->getWidth(), pDepth->getHeight());
    
    if (!mpShadowVisibility || mpShadowVisibility->getWidth() != resolution.x || mpShadowVisibility->getHeight() != resolution.y)
    {
        mpShadowVisibility = mpDevice->createTexture2D(resolution.x, resolution.y, ResourceFormat::R32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }

    auto var = mpShadowMapPass->getRootVar();
    auto cb = var["PerFrameCB"];
    if (cb.isValid())
    {
        cb["gResolution"] = resolution;
        cb["gShadowType"] = (uint32_t)settings.shadowType;
        cb["gCascadeCount"] = (uint32_t)settings.shadowCascades;
        cb["gCascadeSplits"] = settings.cascadeSplits;
        cb["gShadowBias"] = settings.shadowBias;
        cb["gShadowAtlasSize"] = uint2(settings.shadowMapSize, settings.shadowMapSize);
        cb["gVsmExponent"] = settings.vsmExponent;
        cb["gVsmLightBleedReduction"] = settings.vsmLightBleedReduction;
    }

    auto camCB = var["CameraCB"];
    if (camCB.isValid())
    {
        camCB["gInvViewProj"] = settings.invViewProj;
        camCB["gCameraPos"] = settings.cameraPos;
    }

    var["gDepth"] = pDepth;
    var["gShadowMap"] = pShadowMapDepth ? pShadowMapDepth : mpDummyShadowMap;
    var["gShadowMoments"] = pShadowMoments ? pShadowMoments : mpDummyShadowMoments;
    var["gDst"] = mpShadowVisibility;
    var["gPointSampler"] = mpPointSampler;

    auto shadowCB = var["ShadowCameraCB"];
    if (shadowCB.isValid())
    {
        for (int i = 0; i < 4; i++)
        {
            shadowCB["gLightViewProj"][i] = pShadowMapDepth ? settings.shadowLightViewProj[i] : float4x4::identity();
            shadowCB["gCascadeAtlasRect"][i] = pShadowMapDepth ? settings.cascadeAtlasRect[i] : float4(0, 0, 1, 1);
        }
    }

    mpShadowMapPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
}

void FilamentPostProcess::executeDoF(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings)
{
    if (!mpDoFPass || !pSrc || !pDepth || !pDst) return;
    const uint2 resolution = uint2(pSrc->getWidth(), pSrc->getHeight());

    auto var = mpDoFPass->getRootVar();
    auto cb = var["PerFrameCB"];
    if (cb.isValid())
    {
        cb["gResolution"] = resolution;
        cb["gFocalDistance"] = settings.dofFocalDistance;
        cb["gAperture"] = settings.dofAperture;
        cb["gMaxCoC"] = settings.dofMaxCoC;
        cb["gDoFEnabled"] = settings.enableDoF ? 1.0f : 0.0f;
    }
    var["gSrc"] = pSrc;
    var["gDepth"] = pDepth;
    var["gDst"] = pDst;
    var["gLinearSampler"] = mpLinearSampler;
    mpDoFPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
}

void FilamentPostProcess::executeFog(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings)
{
    if (!mpFogPass || !pSrc || !pDepth || !pDst || !settings.enableFog) return;
    const uint2 resolution = uint2(pSrc->getWidth(), pSrc->getHeight());

    auto var = mpFogPass->getRootVar();
    auto cb = var["PerFrameCB"];
    if (cb.isValid())
    {
        cb["gResolution"] = resolution;
        cb["gFogEnabled"] = 1.0f;
        cb["gFogDensity"] = settings.fogDensity;
        cb["gFogStart"] = settings.fogStart;
        cb["gFogColor"] = settings.fogColor;
    }
    auto camCB = var["CameraCB"];
    if (camCB.isValid())
    {
        camCB["gInvProj"] = settings.invProj;
        camCB["gInvViewProj"] = settings.invViewProj;
        camCB["gNearPlane"] = settings.nearPlane;
        camCB["gFarPlane"] = settings.farPlane;
        camCB["gCameraPos"] = settings.cameraPos;
    }
    var["gSrc"] = pSrc;
    var["gDepth"] = pDepth;
    var["gDst"] = pDst;
    var["gLinearSampler"] = mpLinearSampler;
    mpFogPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
}

void FilamentPostProcess::executeSSR(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings)
{
    if (!settings.enableSSR || !pDepth) return;
    // Stub: build structure depth pyramid for a future SSR trace pass.
    executeStructure(pRenderContext, pDepth);
}

void FilamentPostProcess::updateHistory(RenderContext* pRenderContext, const ref<Texture>& pColor, const ref<Texture>& pDepth, uint32_t width, uint32_t height)
{
    if (!pColor || !pDepth) return;
    if (!mpHistoryColor || mHistoryWidth != width || mHistoryHeight != height)
    {
        mpHistoryColor = mpDevice->createTexture2D(width, height, ResourceFormat::RGBA32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget);
        mpHistoryDepth = mpDevice->createTexture2D(width, height, ResourceFormat::R32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget);
        mHistoryWidth = width;
        mHistoryHeight = height;
    }
    pRenderContext->blit(pColor->getSRV(), mpHistoryColor->getRTV());
    pRenderContext->blit(pDepth->getSRV(), mpHistoryDepth->getRTV());
}

void FilamentPostProcess::executeTAA(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings, const ref<Texture>& pMotionVec)
{
    if (!mpTAAPass || !pSrc || !pDepth || !pDst) return;
    const uint2 resolution = uint2(pSrc->getWidth(), pSrc->getHeight());

    if (!mpHistoryColor || mHistoryWidth != resolution.x || mHistoryHeight != resolution.y)
    {
        updateHistory(pRenderContext, pSrc, pDepth, resolution.x, resolution.y);
        pRenderContext->blit(pSrc->getSRV(), pDst->getRTV());
        return;
    }

    const bool useMotionVec = (pMotionVec != nullptr);
    auto var = mpTAAPass->getRootVar();
    auto cb = var["PerFrameCB"];
    if (cb.isValid())
    {
        cb["gResolution"] = resolution;
        cb["gFeedback"] = settings.taaFeedback;
        cb["gEnabled"] = 1.0f;
        cb["gJitter"] = settings.cameraJitter;
        cb["gUseMotionVec"] = useMotionVec ? 1.0f : 0.0f;
    }
    var["gSrc"] = pSrc;
    var["gHistory"] = mpHistoryColor;
    var["gDepth"] = pDepth;
    var["gHistoryDepth"] = mpHistoryDepth;
    var["gMotionVec"] = useMotionVec ? pMotionVec : mpZeroMotionTexture;
    var["gDst"] = pDst;
    var["gLinearSampler"] = mpLinearSampler;
    mpTAAPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
}

void FilamentPostProcess::executeGTAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings)
{
    if (!mpGTAOPass || !mpSSAOBlurPass || !pDepth || !mpStructureDepth) return;

    const uint2 fullResolution = uint2(pDepth->getWidth(), pDepth->getHeight());
    updateAOTextures(mpDevice, fullResolution.x, fullResolution.y, settings.ssaoResolution);
    const uint2 resolution = uint2(mAOBufferWidth, mAOBufferHeight);

    const AOQualityParams quality = getAOQualityParams(settings);
    const float radius = settings.gtaoRadius;
    const float invRadiusSquared = 1.0f / std::max(radius * radius, 1e-6f);
    const float projectionScale = getProjectionScale(settings.positionParams, resolution);
    const float projectionScaleRadius = projectionScale * radius;
    const float sliceCount = (float)std::clamp(settings.gtaoSlices, 1, 16);
    const float stepsPerSlice = (float)std::clamp(settings.gtaoSteps, 1, 16);
    const float power = settings.ssaoPower;

    {
        auto var = mpGTAOPass->getRootVar();
        auto cb = var["PerFrameCB"];
        if (cb.isValid())
        {
            cb["gResolution"] = float4((float)resolution.x, (float)resolution.y, 1.f / resolution.x, 1.f / resolution.y);
            cb["gPositionParams"] = settings.positionParams;
            cb["gInvFarPlane"] = 1.0f / std::max(settings.farPlane, 1.0f);
            cb["gMaxLevel"] = (int)std::max(0u, mStructureLevelCount - 1);
            cb["gProjectionScaleRadius"] = projectionScaleRadius;
            cb["gIntensity"] = settings.ssaoIntensity;
            cb["gSliceCount"] = float2(sliceCount, 1.0f / sliceCount);
            cb["gStepsPerSlice"] = stepsPerSlice;
            cb["gRadius"] = radius;
            cb["gInvRadiusSquared"] = invRadiusSquared;
            cb["gPower"] = power;
            cb["gThicknessHeuristic"] = settings.gtaoThicknessHeuristic;
        }
        auto camCB = var["CameraCB"];
        if (camCB.isValid())
            camCB["gInvProj"] = settings.invProj;
        var["gStructureDepth"] = mpStructureDepth;
        var["gDst"].setUav(mpAOBuffer->getUAV(0));
        var["gPointSampler"] = mpPointSampler;
        mpGTAOPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
    }

    const float farPlaneOverEdgeDistance = settings.farPlane / std::max(settings.ssaoBilateralThreshold, 1e-4f);
    const float2 axes[2] = {
        float2(quality.blurScale / resolution.x, 0.f),
        float2(0.f, quality.blurScale / resolution.y)
    };
    float kGaussianSamples[16] = {};
    const uint32_t gaussianSampleCount = makeGaussianKernel(kGaussianSamples, quality.kernelSize, quality.standardDeviation);

    ref<Texture> blurInput = mpAOBuffer;
    ref<Texture> blurOutputs[2] = { mpAOBlurTemp, mpAOBlurTarget };
    for (uint32_t pass = 0; pass < 2; ++pass)
    {
        auto var = mpSSAOBlurPass->getRootVar();
        auto cb = var["PerFrameCB"];
        if (cb.isValid())
        {
            cb["gResolution"] = float4((float)resolution.x, (float)resolution.y, 1.f / resolution.x, 1.f / resolution.y);
            cb["gBlurAxis"] = axes[pass];
            cb["gFarPlaneOverEdgeDistance"] = farPlaneOverEdgeDistance;
            cb["gBlurSampleCount"] = (int)gaussianSampleCount;
            for (uint32_t i = 0; i < gaussianSampleCount; ++i)
                cb["gBlurKernel"][i] = kGaussianSamples[i];
        }
        var["gAOBuffer"] = blurInput;
        var["gBlurredAO"].setUav(blurOutputs[pass]->getUAV(0));
        var["gPointSampler"] = mpPointSampler;
        mpSSAOBlurPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
        blurInput = blurOutputs[pass];
    }
}

void FilamentPostProcess::executeFSR(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDst, const FilamentSettings& settings)
{
    if (!mpFSRPass || !pSrc || !pDst || !settings.enableFSR) return;
    const uint2 resolution = uint2(pDst->getWidth(), pDst->getHeight());
    auto var = mpFSRPass->getRootVar();
    if (var["PerFrameCB"].isValid())
    {
        var["PerFrameCB"]["gResolution"] = resolution;
        var["PerFrameCB"]["gSharpness"] = settings.fsrSharpness;
    }
    var["gSrc"] = pSrc;
    var["gDst"] = pDst;
    var["gLinearSampler"] = mpLinearSampler;
    mpFSRPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
}

void FilamentPostProcess::executePrePassSSAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings)
{
    if (!settings.enableSSAO || !pDepth) return;
    executeStructure(pRenderContext, pDepth);
    if (settings.ssaoMode == 1 && mpGTAOPass)
        executeGTAO(pRenderContext, pDepth, settings);
    else if (mpSSAOPass)
        executeSSAO(pRenderContext, pDepth, settings);
}

ref<Texture> FilamentPostProcess::getAOTexture(const FilamentSettings& settings) const
{
    if (settings.enableSSAO && mpAOBlurTarget)
        return mpAOBlurTarget;
    return mpWhiteTexture;
}

void FilamentPostProcess::executeColorGradingPrep(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDst, const FilamentSettings& settings)
{
    if (!mpColorGradingPrepPass || !pSrc || !pDst) return;
    const uint2 resolution = uint2(pSrc->getWidth(), pSrc->getHeight());

    auto var = mpColorGradingPrepPass->getRootVar();
    auto cb = var["PerFrameCB"];
    if (cb.isValid())
    {
        cb["gResolution"] = resolution;
        const bool postAO = settings.enableSSAO && !settings.forwardSSAO && mpAOBlurTarget;
        cb["gAOEnabled"] = postAO ? 1.0f : 0.0f;
        cb["gPostProcessShadow"] = (settings.postProcessShadow && settings.enableShadows && mpShadowVisibility) ? 1.0f : 0.0f;
    }
    var["gSrc"] = pSrc;
    var["gDst"] = pDst;
    var["gAO"] = (settings.enableSSAO && mpAOBlurTarget) ? mpAOBlurTarget : mpWhiteTexture;
    var["gShadow"] = (settings.postProcessShadow && settings.enableShadows && mpShadowVisibility) ? mpShadowVisibility : mpWhiteTexture;
    var["gSampler"] = mpLinearSampler;
    mpColorGradingPrepPass->execute(pRenderContext, uint3(resolution, 1));
}

void FilamentPostProcess::executeColorGradingComposite(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDst, const FilamentSettings& settings)
{
    if (!mpColorGradingPass || !pSrc || !pDst) return;
    const uint2 resolution = uint2(pSrc->getWidth(), pSrc->getHeight());

    auto var = mpColorGradingPass->getRootVar();
    auto cb = var["PerFrameCB"];
    if (cb.isValid())
    {
        cb["gResolution"] = resolution;
        cb["gToneMapping"] = (float)settings.toneMapping;
        cb["gExposure"] = settings.exposure;
        cb["gContrast"] = settings.contrast;
        cb["gVibrance"] = settings.vibrance;
        cb["gSaturation"] = settings.saturation;
        cb["gVignetteMidpoint"] = settings.enableVignette ? settings.vignetteMidpoint : 0.0f;
        cb["gVignetteRoundness"] = settings.vignetteRoundness;
        cb["gVignetteFeather"] = settings.vignetteFeather;
        cb["gVignetteColor"] = settings.vignetteColor;
        cb["gBloomStrength"] = settings.enableBloom ? settings.bloomStrength : 0.0f;
        cb["gBloomBlendMode"] = (float)settings.bloomBlendMode;
        cb["gPostProcessShadow"] = 0.0f;
        cb["gDithering"] = settings.dithering ? 1.0f : 0.0f;
        if (settings.enableColorGradingLUT)
        {
            updateColorGradingLUT(mpDevice, settings);
            cb["gUseLUT"] = 1.0f;
            const float lutSize = float(mLUTSize);
            cb["gLUTSize"] = float2(0.5f / lutSize, (lutSize - 1.0f) / lutSize);
        }
        else
        {
            cb["gUseLUT"] = 0.0f;
            cb["gLUTSize"] = float2(0.0f, 1.0f);
        }
    }

    var["gSrc"] = pSrc;
    var["gDst"] = pDst;
    var["gAO"] = mpWhiteTexture;
    var["gShadow"] = mpWhiteTexture;
    var["gLUTSampler"] = mpLinearSampler;

    if (settings.enableBloom && mpBloomMips[0])
    {
        var["gBloom"] = mpBloomMips[0];
        var["gSampler"] = mpLinearSampler;
    }
    else
    {
        var["gBloom"] = pSrc;
        var["gSampler"] = mpLinearSampler;
    }

    if (settings.enableColorGradingLUT && mpColorLUT)
        var["gColorLUT"] = mpColorLUT;
    else
        var["gColorLUT"] = mpIdentityLUT;

    mpColorGradingPass->execute(pRenderContext, uint3(resolution, 1));
}

Properties FilamentPostProcess::getProperties() const
{
    Properties props;
    props[kPostProcessingEnabled] = mSettings.postProcessingEnabled;
    props[kAntiAliasing] = mSettings.antiAliasing;
    props[kEnableBloom] = mSettings.enableBloom;
    props[kEnableSSAO] = mSettings.enableSSAO;
    props[kEnableDoF] = mSettings.enableDoF;
    props[kEnableFog] = mSettings.enableFog;
    props[kFogDensity] = mSettings.fogDensity;
    props[kFogStart] = mSettings.fogStart;
    props[kFogColor] = mSettings.fogColor;
    props[kEnableSSR] = mSettings.enableSSR;
    return props;
}

RenderPassReflection FilamentPostProcess::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kSrc, "Source texture").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth, "Depth buffer (enables DoF/TAA/fog/SSAO when connected)")
        .bindFlags(ResourceBindFlags::ShaderResource)
        .flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kMotionVec, "Motion vectors (for TAA when antiAliasing=2)")
        .bindFlags(ResourceBindFlags::ShaderResource)
        .flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kShadowMap, "Shadow map atlas (optional post-process shadow debug)")
        .bindFlags(ResourceBindFlags::ShaderResource)
        .flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addOutput(kDst, "post-effected output texture")
        .bindFlags(ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess)
        .format(ResourceFormat::RGBA32Float);
    return reflector;
}

void FilamentPostProcess::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pSrc = renderData.getTexture(kSrc);
    auto pDst = renderData.getTexture(kDst);
    if (!pSrc || !pDst)
    {
        logWarning("FilamentPostProcess::execute() - missing src or dst texture.");
        return;
    }

    auto pDepth = renderData.getTexture(kDepth);
    auto pMotionVec = renderData.getTexture(kMotionVec);
    auto pShadowMap = renderData.getTexture(kShadowMap);

    executeCustom(pRenderContext, pSrc, pDepth, pDst, mSettings, pShadowMap, pMotionVec, nullptr);
}

void FilamentPostProcess::executeCustom(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings, const ref<Texture>& pShadowMap, const ref<Texture>& pMotionVec, const ref<Texture>& pShadowMoments)
{
    if (!pSrc || !pDst) return;

    const uint2 resolution = uint2(pSrc->getWidth(), pSrc->getHeight());
    ref<Texture> currentInput = pSrc;
    const bool doFXAA = (settings.antiAliasing == 1);
    const bool doTAA = (settings.antiAliasing == 2);

    auto ensureTexture = [&](ref<Texture>& tex) {
        if (!tex || tex->getWidth() != resolution.x || tex->getHeight() != resolution.y)
        {
            tex = mpDevice->createTexture2D(resolution.x, resolution.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        }
    };

    // --- Stage 0: Shadow visibility (debug only; forward pass applies shadows by default) ---
    if (settings.postProcessShadow && settings.enableShadows && pDepth && mpShadowMapPass)
        executeShadowMap(pRenderContext, pDepth, settings, pShadowMap, pShadowMoments);

    // --- Stage 1: SSAO (skipped when forward pass already computed AO in prepass) ---
    if (settings.enableSSAO && !settings.forwardSSAO && pDepth && mpSSAOPass)
    {
        executeStructure(pRenderContext, pDepth);
        executeSSAO(pRenderContext, pDepth, settings);
    }

    // --- Stage 2: Color grading prep (AO + shadow on HDR, before TAA) ---
    ensureTexture(mpPrepTarget);
    executeColorGradingPrep(pRenderContext, currentInput, mpPrepTarget, settings);
    currentInput = mpPrepTarget;

    // --- Stage 3: TAA (Filament: before DoF) ---
    if (doTAA && mpTAAPass && pDepth)
    {
        ensureTexture(mpTAATarget);
        executeTAA(pRenderContext, currentInput, pDepth, mpTAATarget, settings, pMotionVec);
        currentInput = mpTAATarget;
        updateHistory(pRenderContext, mpTAATarget, pDepth, resolution.x, resolution.y);
    }

    // --- Stage 4: Depth of Field (after TAA) ---
    if (settings.enableDoF && pDepth && mpDoFPass)
    {
        ensureTexture(mpDoFTarget);
        executeDoF(pRenderContext, currentInput, pDepth, mpDoFTarget, settings);
        currentInput = mpDoFTarget;
    }

    // --- Stage 4b: SSR stub (structure depth for future reflection pass) ---
    if (settings.enableSSR && pDepth)
        executeSSR(pRenderContext, pDepth, settings);

    // --- Stage 4c: Fog (HDR, after DoF / before bloom + tone map) ---
    if (settings.enableFog && pDepth && mpFogPass)
    {
        ensureTexture(mpFogTarget);
        executeFog(pRenderContext, currentInput, pDepth, mpFogTarget, settings);
        currentInput = mpFogTarget;
    }

    // --- Stage 5: Bloom ---
    if (settings.enableBloom && mpBloomDownsamplePass && mpBloomUpsamplePass)
    {
        uint32_t levels = std::min((uint32_t)settings.bloomLevels, kMaxBloomLevels);
        updateBloomTextures(mpDevice, resolution.x, resolution.y, levels);

        ref<Texture> downSrc = currentInput;
        for (uint32_t i = 0; i < levels; ++i)
        {
            if (!mpBloomMips[i]) break;

            auto var = mpBloomDownsamplePass->getRootVar();
            var["PerFrameCB"]["gSrcRes"] = uint2(downSrc->getWidth(), downSrc->getHeight());
            var["PerFrameCB"]["gDstRes"] = uint2(mpBloomMips[i]->getWidth(), mpBloomMips[i]->getHeight());
            var["PerFrameCB"]["gBloomStrength"] = settings.bloomStrength;
            var["PerFrameCB"]["gThreshold"] = (i == 0) ? settings.bloomThreshold : 0.0f;
            var["gSrc"] = downSrc;
            var["gDst"] = mpBloomMips[i];
            var["gSampler"] = mpLinearSampler;

            mpBloomDownsamplePass->execute(pRenderContext, uint3(mpBloomMips[i]->getWidth(), mpBloomMips[i]->getHeight(), 1));
            downSrc = mpBloomMips[i];
        }

        ref<Texture> upSrc = downSrc;
        for (int32_t i = levels - 2; i >= 0; --i)
        {
            if (!mpBloomMips[i]) continue;

            ref<Texture> tempDst = mpDevice->createTexture2D(
                mpBloomMips[i]->getWidth(), mpBloomMips[i]->getHeight(),
                ResourceFormat::RGBA16Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);

            auto var = mpBloomUpsamplePass->getRootVar();
            var["PerFrameCB"]["gSrcRes"] = uint2(upSrc->getWidth(), upSrc->getHeight());
            var["PerFrameCB"]["gDstRes"] = uint2(mpBloomMips[i]->getWidth(), mpBloomMips[i]->getHeight());
            var["PerFrameCB"]["gBloomStrength"] = 1.0f;
            var["gSrc"] = upSrc;
            var["gHigherMip"] = mpBloomMips[i];
            var["gDst"] = tempDst;
            var["gSampler"] = mpLinearSampler;

            mpBloomUpsamplePass->execute(pRenderContext, uint3(mpBloomMips[i]->getWidth(), mpBloomMips[i]->getHeight(), 1));
            pRenderContext->blit(tempDst->getSRV(), mpBloomMips[i]->getRTV());

            upSrc = mpBloomMips[i];
        }
    }

    // --- Stage 6: Color grading composite (bloom + vignette + tone map) ---
    ref<Texture> cgTarget = pDst;
    if (doFXAA)
    {
        ensureTexture(mpColorGradingTarget);
        cgTarget = mpColorGradingTarget;
    }
    executeColorGradingComposite(pRenderContext, currentInput, cgTarget, settings);

    // --- Stage 7: FXAA ---
    ref<Texture> finalColor = cgTarget;
    if (doFXAA && mpFXAAPass)
    {
        auto var = mpFXAAPass->getRootVar();
        var["PerFrameCB"]["gResolution"] = resolution;
        var["gSrc"] = cgTarget;
        var["gDst"] = pDst;
        var["gSampler"] = mpLinearSampler;
        mpFXAAPass->execute(pRenderContext, uint3(resolution, 1));
        finalColor = pDst;
    }
    else if (doTAA)
    {
        pRenderContext->blit(cgTarget->getSRV(), pDst->getRTV());
        finalColor = pDst;
    }
    else if (cgTarget != pDst)
    {
        pRenderContext->blit(cgTarget->getSRV(), pDst->getRTV());
        finalColor = pDst;
    }

    // --- Stage 8: FSR RCAS sharpen (optional, after FXAA/TAA output) ---
    if (settings.enableFSR && mpFSRPass)
    {
        ensureTexture(mpFSRTarget);
        executeFSR(pRenderContext, finalColor, mpFSRTarget, settings);
        pRenderContext->blit(mpFSRTarget->getSRV(), pDst->getRTV());
        finalColor = pDst;
    }

    if (!doTAA && pDepth)
        updateHistory(pRenderContext, finalColor, pDepth, resolution.x, resolution.y);
}
