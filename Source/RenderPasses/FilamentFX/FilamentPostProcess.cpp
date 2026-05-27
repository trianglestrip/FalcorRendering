#include "FilamentPostProcess.h"
#include <random>

namespace
{
    const char kSrc[] = "src";
    const char kDst[] = "dst";
    const char kColorGradingShader[] = "RenderPasses/FilamentFX/ColorGrading.cs.slang";
    const char kBloomShader[] = "RenderPasses/FilamentFX/Bloom.cs.slang";
    const char kFXAAShader[] = "RenderPasses/FilamentFX/FXAA.cs.slang";
    const char kSSAOShader[] = "RenderPasses/FilamentFX/SSAO.cs.slang";
    const char kShadowMapShader[] = "RenderPasses/FilamentFX/ShadowMap.cs.slang";

    const uint32_t kNoiseTextureSize = 4; // 4x4 noise texture for SSAO
}

FilamentPostProcess::FilamentPostProcess(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    DefineList defines;
    
    // Color Grading + Tone Mapping
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

void FilamentPostProcess::updateAOTextures(ref<Device> pDevice, uint32_t width, uint32_t height)
{
    if (!mpAOBuffer || mpAOBuffer->getWidth() != width || mpAOBuffer->getHeight() != height)
    {
        mpAOBuffer = pDevice->createTexture2D(width, height, ResourceFormat::R32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }
    if (!mpAOBlurTarget || mpAOBlurTarget->getWidth() != width || mpAOBlurTarget->getHeight() != height)
    {
        mpAOBlurTarget = pDevice->createTexture2D(width, height, ResourceFormat::R32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }
}

void FilamentPostProcess::executeSSAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings)
{
    if (!mpSSAOPass || !mpSSAOBlurPass || !pDepth) return;

    const uint2 resolution = uint2(pDepth->getWidth(), pDepth->getHeight());
    updateAOTextures(mpDevice, resolution.x, resolution.y);

    // Filament SAO parameters
    float sampleCount = (float)settings.ssaoSampleCount;
    float radius = settings.ssaoRadius;
    float invRadiusSquared = 1.0f / (radius * radius);
    float angleInc = (2.0f * 3.14159265f) / (float)settings.ssaoSpiralTurns / sampleCount;

    // SSAO main pass (Filament SAO)
    {
        auto var = mpSSAOPass->getRootVar();
        auto cb = var["PerFrameCB"];
        if (cb.isValid())
        {
            cb["gResolution"] = float4((float)resolution.x, (float)resolution.y, 1.f/resolution.x, 1.f/resolution.y);
            cb["gPositionParams"]  = float2(2.0f, 2.0f); // invProjection[0][0]*2, invProjection[1][1]*2 simplified
            cb["gInvRadiusSquared"] = invRadiusSquared;
            cb["gMinHorizonAngleSineSquared"] = settings.ssaoMinHorizonAngleSineSquared;
            cb["gBias"]      = settings.ssaoBias;
            cb["gPeak2"]     = settings.ssaoPeak2;
            cb["gProjectionScale"] = settings.ssaoProjectionScale;
            cb["gProjectionScaleRadius"] = radius * settings.ssaoProjectionScale;
            cb["gIntensity"] = settings.ssaoIntensity / sampleCount;
            cb["gPower"]     = settings.ssaoPower;
            cb["gSpiralTurns"] = (float)settings.ssaoSpiralTurns;
            cb["gSampleCount"]  = float2(sampleCount, 1.0f / sampleCount);
            cb["gAngleIncCosSin"] = float2(cos(angleInc), sin(angleInc));
            cb["gInvFarPlane"] = 1.0f / 1000.0f;
            cb["gMaxLevel"]   = 5; // mip levels for depth pyramid
        }
        var["gDepth"] = pDepth;
        var["gDst"] = mpAOBuffer;
        var["gPointSampler"] = mpPointSampler;

        mpSSAOPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
    }

    // SSAO bilateral blur pass
    {
        auto var = mpSSAOBlurPass->getRootVar();
        auto cb = var["PerFrameCB"];
        if (cb.isValid())
        {
            cb["gResolution"] = float4((float)resolution.x, (float)resolution.y, 1.f/resolution.x, 1.f/resolution.y);
        }
        var["gAOBuffer"] = mpAOBuffer;
        var["gDepthForBlur"] = pDepth;
        var["gBlurredAO"] = mpAOBlurTarget;
        var["gPointSampler"] = mpPointSampler;

        mpSSAOBlurPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
    }
}

void FilamentPostProcess::executeShadowMap(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings)
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
    }
    var["gDepth"] = pDepth;
    var["gDst"] = mpShadowVisibility;
    var["gPointSampler"] = mpPointSampler;
    var["gLinearSampler"] = mpLinearSampler;

    mpShadowMapPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
}

Properties FilamentPostProcess::getProperties() const
{
    return Properties();
}

RenderPassReflection FilamentPostProcess::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kSrc, "Source texture").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kDst, "post-effected output texture")
        .bindFlags(ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess)
        .format(ResourceFormat::RGBA32Float);
    return reflector;
}

void FilamentPostProcess::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pSrc = renderData.getTexture(kSrc);
    auto pDst = renderData.getTexture(kDst);
    FilamentSettings defaultSettings;
    executeCustom(pRenderContext, pSrc, nullptr, pDst, defaultSettings);
}

void FilamentPostProcess::executeCustom(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings)
{
    if (!pSrc || !pDst) return;

    const uint2 resolution = uint2(pSrc->getWidth(), pSrc->getHeight());
    ref<Texture> currentInput = pSrc;

    // --- Pipeline Stage 0: Shadow Map (if enabled + depth available) ---
    if (settings.enableShadows && pDepth && mpShadowMapPass)
    {
        executeShadowMap(pRenderContext, pDepth, settings);
    }

    // --- Pipeline Stage 1: SSAO (if enabled + depth available) ---
    if (settings.enableSSAO && pDepth && mpSSAOPass)
    {
        executeSSAO(pRenderContext, pDepth, settings);
    }

    // --- Pipeline Stage 2: Bloom ---
    if (settings.enableBloom && mpBloomDownsamplePass && mpBloomUpsamplePass)
    {
        uint32_t levels = std::min((uint32_t)settings.bloomLevels, kMaxBloomLevels);
        updateBloomTextures(mpDevice, resolution.x, resolution.y, levels);

        // Downsample chain
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

        // Upsample chain (tent filter reconstruction)
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

    // --- Pipeline Stage 3: Color Grading, Tone Mapping, Vignette, Bloom Composite ---
    ref<Texture> cgTarget = pDst;
    bool doFXAA = (settings.antiAliasing == 1);
    
    if (doFXAA)
    {
        if (!mpColorGradingTarget || mpColorGradingTarget->getWidth() != resolution.x || mpColorGradingTarget->getHeight() != resolution.y)
        {
            mpColorGradingTarget = mpDevice->createTexture2D(resolution.x, resolution.y, ResourceFormat::RGBA32Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        }
        cgTarget = mpColorGradingTarget;
    }

    if (mpColorGradingPass)
    {
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
        }

        var["gSrc"] = currentInput;
        var["gDst"] = cgTarget;
        if (settings.enableBloom && mpBloomMips[0])
        {
            var["gBloom"] = mpBloomMips[0];
            var["gSampler"] = mpLinearSampler;
        }
        else
        {
            var["gBloom"] = currentInput;
            var["gSampler"] = mpLinearSampler;
        }

        mpColorGradingPass->execute(pRenderContext, uint3(resolution, 1));
    }

    // --- Pipeline Stage 4: FXAA ---
    if (doFXAA && mpFXAAPass)
    {
        auto var = mpFXAAPass->getRootVar();
        var["PerFrameCB"]["gResolution"] = resolution;
        var["gSrc"] = cgTarget;
        var["gDst"] = pDst;
        var["gSampler"] = mpLinearSampler;
        mpFXAAPass->execute(pRenderContext, uint3(resolution, 1));
    }
}