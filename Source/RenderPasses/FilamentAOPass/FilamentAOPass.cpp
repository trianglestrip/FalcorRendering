/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#include "FilamentAOPass.h"

namespace
{
const char kDepth[] = "depth";
const char kNormalW[] = "normalW";
const char kAO[] = "ao";

const char kUseGBufferNormals[] = "useGBufferNormals";
const char kAOType[] = "aoType";
const char kResolution[] = "resolution";
const char kRadius[] = "radius";
const char kIntensity[] = "intensity";
const char kPower[] = "power";
const char kQuality[] = "quality";
const char kLowPass[] = "lowPass";
const char kBilateralThreshold[] = "bilateralThreshold";

void regFilamentAOPass(pybind11::module& m)
{
    pybind11::class_<FilamentAOPass, RenderPass, ref<FilamentAOPass>> pass(m, "FilamentAOPass");
}
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, FilamentAOPass>();
    ScriptBindings::registerBinding(regFilamentAOPass);
}

FilamentAOPass::FilamentAOPass(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    mSettings.enableSSAO = true;
    mSettings.forwardSSAO = true;
    mSettings.ssaoResolution = 1.0f;
    mSettings.ssaoRadius = 1.0f;
    mSettings.ssaoIntensity = 1.0f;
    mSettings.ssaoPower = 1.0f;
    mSettings.ssaoQuality = 1;
    mSettings.ssaoLowPassFilter = 1;
    mSettings.ssaoBilateralThreshold = 0.05f;
    parseProperties(props);
    mpFilamentAO = make_ref<FilamentPostProcess>(mpDevice, Properties{});
}

void FilamentAOPass::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kUseGBufferNormals)
            mUseGBufferNormals = value;
        else if (key == kAOType)
            mSettings.ssaoMode = value;
        else if (key == kResolution)
            mSettings.ssaoResolution = value;
        else if (key == kRadius)
        {
            mSettings.ssaoRadius = value;
            mSettings.gtaoRadius = mSettings.ssaoRadius;
        }
        else if (key == kIntensity)
            mSettings.ssaoIntensity = value;
        else if (key == kPower)
            mSettings.ssaoPower = value;
        else if (key == kQuality)
            mSettings.ssaoQuality = value;
        else if (key == kLowPass)
            mSettings.ssaoLowPassFilter = value;
        else if (key == kBilateralThreshold)
            mSettings.ssaoBilateralThreshold = value;
        else
            logWarning("Unknown property '{}' in FilamentAOPass properties.", key);
    }
}

Properties FilamentAOPass::getProperties() const
{
    Properties props;
    props[kUseGBufferNormals] = mUseGBufferNormals;
    props[kAOType] = mSettings.ssaoMode;
    props[kResolution] = mSettings.ssaoResolution;
    props[kRadius] = mSettings.ssaoRadius;
    props[kIntensity] = mSettings.ssaoIntensity;
    props[kPower] = mSettings.ssaoPower;
    props[kQuality] = mSettings.ssaoQuality;
    props[kLowPass] = mSettings.ssaoLowPassFilter;
    props[kBilateralThreshold] = mSettings.ssaoBilateralThreshold;
    return props;
}

RenderPassReflection FilamentAOPass::reflect(const CompileData& compileData)
{
    RenderPassReflection r;
    r.addInput(kDepth, "Depth buffer").bindFlags(ResourceBindFlags::ShaderResource);
    r.addInput(kNormalW, "World-space normal buffer, packed or signed depending on producer")
        .bindFlags(ResourceBindFlags::ShaderResource)
        .flags(RenderPassReflection::Field::Flags::Optional);
    r.addOutput(kAO, "Filament AO output: R=visibility, G=normalized view depth, BA=bent normal XY")
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    return r;
}

void FilamentAOPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pDepth = renderData.getTexture(kDepth);
    auto pAO = renderData.getTexture(kAO);
    auto pNormalW = renderData.getTexture(kNormalW);
    if (!pDepth || !pAO || !mpFilamentAO)
    {
        logWarning("FilamentAOPass::execute() - missing required resources.");
        return;
    }

    syncCameraSettings();

    if (mUseGBufferNormals && pNormalW)
        mpFilamentAO->executeDeferredSSAO(pRenderContext, pDepth, pNormalW, mSettings);
    else
        mpFilamentAO->executePrePassSSAO(pRenderContext, pDepth, mSettings);

    auto pFilamentAO = mpFilamentAO->getAOTexture(mSettings);
    if (pFilamentAO)
        pRenderContext->blit(pFilamentAO->getSRV(), pAO->getRTV());
}

void FilamentAOPass::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Use GBuffer normals", mUseGBufferNormals);
    Gui::DropdownList aoTypes = {{0, "SAO"}, {1, "GTAO"}};
    widget.dropdown("AO Type", aoTypes, (uint32_t&)mSettings.ssaoMode);
    widget.slider("Resolution", mSettings.ssaoResolution, 0.25f, 1.0f);
    widget.slider("Radius", mSettings.ssaoRadius, 0.05f, 10.0f);
    mSettings.gtaoRadius = mSettings.ssaoRadius;
    widget.slider("Intensity", mSettings.ssaoIntensity, 0.0f, 4.0f);
    widget.slider("Power", mSettings.ssaoPower, 0.25f, 8.0f);
    widget.slider("Quality", mSettings.ssaoQuality, 0, 3);
    widget.slider("Low Pass", mSettings.ssaoLowPassFilter, 0, 2);
    widget.slider("Bilateral Threshold", mSettings.ssaoBilateralThreshold, 0.001f, 0.2f);
}

void FilamentAOPass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
}

void FilamentAOPass::syncCameraSettings()
{
    if (!mpScene || !mpScene->getCamera()) return;

    const auto& pCamera = mpScene->getCamera();
    const float4x4 proj = pCamera->getProjMatrix();
    const float4x4 view = pCamera->getViewMatrix();
    mSettings.invProj = inverse(proj);
    mSettings.invView = inverse(view);
    mSettings.invViewProj = pCamera->getInvViewProjMatrix();
    mSettings.nearPlane = pCamera->getNearPlane();
    mSettings.farPlane = pCamera->getFarPlane();
    mSettings.cameraPos = pCamera->getPosition();
    mSettings.positionParams = float2(2.0f / proj[0][0], 2.0f / proj[1][1]);
}
