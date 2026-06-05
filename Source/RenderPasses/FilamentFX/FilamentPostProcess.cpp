#include "FilamentPostProcess.h"
#include <random>

namespace
{
    const char kSrc[] = "src";
    const char kDst[] = "dst";
    const char kColorGradingShader[] = "RenderPasses/FilamentFX/ColorGrading.cs.slang";
    const char kBloomShader[] = "RenderPasses/FilamentFX/Bloom.cs.slang";
    const char kFXAAShader[] = "RenderPasses/FilamentFX/FXAA.cs.slang";
    const char kStructureShader[] = "RenderPasses/FilamentFX/StructurePass.cs.slang";
    const char kSSAOShader[] = "RenderPasses/FilamentFX/SSAO.cs.slang";
    const char kShadowMapShader[] = "RenderPasses/FilamentFX/ShadowMap.cs.slang";
    const char kDoFShader[] = "RenderPasses/FilamentFX/DoF.cs.slang";
    const char kTAAShader[] = "RenderPasses/FilamentFX/TAA.cs.slang";

    const uint32_t kNoiseTextureSize = 4; // 4x4 noise texture for SSAO
    const uint32_t kMaxStructureLevels = 8;
}

FilamentPostProcess::FilamentPostProcess(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
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

    // Depth of Field
    ProgramDesc dofDesc;
    dofDesc.addShaderLibrary(kDoFShader).csEntry("dofMain");
    dofDesc.addCompilerArguments({"-Wno-30081"});
    mpDoFPass = ComputePass::create(mpDevice, dofDesc, defines);

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

    // Zero motion vector fallback (RG32Float)
    float zeroMotion[2] = {0.f, 0.f};
    mpZeroMotionTexture = pDevice->createTexture2D(1, 1, ResourceFormat::RG32Float, 1, 1, zeroMotion,
        ResourceBindFlags::ShaderResource);
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

    // Filament SAO parameters (matching PostProcessManager screenSpaceAmbientOcclusion)
    float sampleCount = (float)settings.ssaoSampleCount;
    float radius = settings.ssaoRadius;
    float invRadiusSquared = 1.0f / (radius * radius);
    float angleInc = (2.0f * 3.14159265f) / (sampleCount - 0.5f) * (float)settings.ssaoSpiralTurns;
    
    float2 posParams = settings.positionParams;
    float projectionScale = 0.5f * posParams.x * (float)resolution.x;
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
            cb["gSpiralTurns"] = (float)settings.ssaoSpiralTurns;
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
        var["gDst"] = mpAOBuffer;
        var["gPointSampler"] = mpPointSampler;

        mpSSAOPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
    }

    // Filament bilateral separable blur (MEDIUM: kernel 11, stddev 4)
    const float bilateralStdDev = 4.0f;
    const uint32_t kernelSize = 11;
    const uint32_t gaussianSampleCount = (kernelSize + 1) / 2;
    float kGaussianSamples[16] = {};
    for (uint32_t i = 0; i < gaussianSampleCount; ++i)
    {
        float x = (float)i;
        kGaussianSamples[i] = std::exp(-(x * x) / (2.0f * bilateralStdDev * bilateralStdDev));
    }

    const float farPlaneOverEdgeDistance = settings.farPlane / std::max(settings.ssaoBilateralThreshold, 1e-4f);
    const float2 axes[2] = { float2(1.f / resolution.x, 0.f), float2(0.f, 1.f / resolution.y) };
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
        var["gBlurredAO"] = blurOutputs[pass];
        var["gPointSampler"] = mpPointSampler;

        mpSSAOBlurPass->execute(pRenderContext, uint3(resolution.x, resolution.y, 1));
        blurInput = blurOutputs[pass];
    }
}

void FilamentPostProcess::executeShadowMap(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings, const ref<Texture>& pShadowMapDepth)
{
    if (!mpShadowMapPass || !pDepth) return;

    const uint2 resolution = uint2(pDepth->getWidth(), pDepth->getHeight());
    
    if (!mpShadowVisibility || mpShadowVisibility->getWidth() != resolution.x || mpShadowVisibility->getHeight() != resolution.y)
    {
        mpShadowVisibility = mpDevice->createTexture2D(resolution.x, resolution.y, ResourceFormat::R32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    }

    uint32_t shadowType = (uint32_t)settings.shadowType;
    if (shadowType == 2) shadowType = 1; // VSM not fully implemented, use PCF Low

    auto var = mpShadowMapPass->getRootVar();
    auto cb = var["PerFrameCB"];
    if (cb.isValid())
    {
        cb["gResolution"] = resolution;
        cb["gShadowType"] = shadowType;
        cb["gCascadeCount"] = (uint32_t)settings.shadowCascades;
        cb["gCascadeSplits"] = settings.cascadeSplits;
        cb["gShadowBias"] = settings.shadowBias;
        cb["gShadowAtlasSize"] = uint2(settings.shadowMapSize, settings.shadowMapSize);
    }

    auto camCB = var["CameraCB"];
    if (camCB.isValid())
    {
        camCB["gInvViewProj"] = settings.invViewProj;
        camCB["gCameraPos"] = settings.cameraPos;
    }

    var["gDepth"] = pDepth;
    var["gShadowMap"] = pShadowMapDepth ? pShadowMapDepth : mpDummyShadowMap;
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

void FilamentPostProcess::executePrePassSSAO(RenderContext* pRenderContext, const ref<Texture>& pDepth, const FilamentSettings& settings)
{
    if (!settings.enableSSAO || !pDepth || !mpSSAOPass) return;
    executeStructure(pRenderContext, pDepth);
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
    }

    var["gSrc"] = pSrc;
    var["gDst"] = pDst;
    var["gAO"] = mpWhiteTexture;
    var["gShadow"] = mpWhiteTexture;

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

    mpColorGradingPass->execute(pRenderContext, uint3(resolution, 1));
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

void FilamentPostProcess::executeCustom(RenderContext* pRenderContext, const ref<Texture>& pSrc, const ref<Texture>& pDepth, const ref<Texture>& pDst, const FilamentSettings& settings, const ref<Texture>& pShadowMap, const ref<Texture>& pMotionVec)
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
        executeShadowMap(pRenderContext, pDepth, settings, pShadowMap);

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

    if (!doTAA && pDepth)
        updateHistory(pRenderContext, finalColor, pDepth, resolution.x, resolution.y);
}