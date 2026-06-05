/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 # Uses PBRT native BSDF (mi.eval) for correct material evaluation.
 # Filament-style forward IBL + SSAO + CSM shadows.
 **************************************************************************/
import Scene.Raster;
import Utils.Sampling.TinyUniformSampleGenerator;
import Utils.Math.MathHelpers;
import Rendering.Lights.LightHelpers;
#include "RenderPasses/FilamentFX/FilamentShadow.slangh"
#include "RenderPasses/FilamentFX/FilamentIBLData.slang"
#include "RenderPasses/FilamentFX/FilamentIBL.slangh"
#include "RenderPasses/FilamentFX/FilamentAO.slangh"

cbuffer PerFrameCB
{
    uint2 gFrameDim;
    uint gSSAOEnabled;
    float gSunIntensity;
    float3 gSunColor;
};

cbuffer ShadowCB
{
    uint gShadowEnabled;
    uint gShadowType;
    uint gCascadeCount;
    float gShadowBias;
    float4 gCascadeSplits;
    float4x4 gLightViewProj[4];
    float4 gCascadeAtlasRect[4];
    float2 gShadowAtlasSize;
    float3 gShadowSunDir;
    float gVsmExponent;
    float gVsmLightBleedReduction;
};

Texture2D<float> gShadowMap;
Texture2D<float2> gShadowMoments;
SamplerState gShadowPointSampler;

struct PSOut
{
    float4 color : SV_TARGET0;
};

VSOut vsMain(VSIn vIn) { return defaultVS(vIn); }

float2 computeMotionVector(const VSOut vsOut, const int2 ipos)
{
    float2 pixelPos = float2(ipos) + float2(0.5, 0.5);
    return calcMotionVector(pixelPos, vsOut.prevPosH, float2(gFrameDim))
        + float2(gScene.camera.data.jitterX, -gScene.camera.data.jitterY);
}

PSOut psMain(VSOut vsOut, uint triangleIndex: SV_PrimitiveID)
{
    let lod = ImplicitLodTextureSampler();
    if (alphaTest(vsOut, triangleIndex, lod)) discard;

    float3 viewDir = normalize(gScene.camera.getPosition() - vsOut.posW);
    ShadingData sd = prepareShadingData(vsOut, triangleIndex, viewDir);
    let mi = gScene.materials.getMaterialInstance(sd, lod);
    let props = mi.getProperties(sd);
    uint2 pixelPos = uint2(vsOut.posH.xy);
    TinyUniformSampleGenerator sg = TinyUniformSampleGenerator(pixelPos, 0);

    float3 N = normalize(sd.frame.N);
    float3 albedo = props.diffuseReflectionAlbedo;
    if (all(albedo == float3(0.f))) albedo = float3(0.5f, 0.45f, 0.4f);

    PixelParams pixel;
    pixel.diffuseColor = albedo;
    pixel.perceptualRoughness = saturate(props.roughness);
    pixel.roughness = perceptualRoughnessToRoughness(pixel.perceptualRoughness);
    pixel.f0 = max(props.specularReflectance, float3(0.04f, 0.04f, 0.04f));
    pixel.dfg = float3(0.f);
    pixel.energyCompensation = float3(1.f);

    float2 uv = (float2(pixelPos) + 0.5f) / float2(gFrameDim);
    float ssao = (gSSAOEnabled != 0u) ? evaluateSSAO(uv) : 1.0f;

    float3 indirect = evaluateIBL(pixel, N, viewDir, ssao);
    float3 color = props.emission + indirect;

    float3 cameraPos = gScene.camera.getPosition();
    float shadowVis = evaluateShadowVisibility(
        sd.posW,
        cameraPos,
        gShadowEnabled,
        gShadowType,
        gCascadeCount,
        gShadowBias,
        gCascadeSplits,
        gLightViewProj,
        gCascadeAtlasRect,
        gShadowAtlasSize,
        gShadowMap,
        gShadowMoments,
        gShadowPointSampler,
        gVsmExponent,
        gVsmLightBleedReduction);

    float3 sunL = normalize(-gShadowSunDir);
    float NoL = saturate(dot(N, sunL));
    if (NoL > 0.0f && gSunIntensity > 0.0f)
    {
        float3 sunRadiance = gSunColor * gSunIntensity;
        color += mi.eval(sd, sunL, sg) * sunRadiance * shadowVis;
    }

    PSOut out;
    out.color = float4(color, 1.0f);
    return out;
}

