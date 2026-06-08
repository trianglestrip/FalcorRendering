/***************************************************************************
 # PBRT preview deferred IBL lighting pass.
 **************************************************************************/

#include "RenderPasses/FilamentFX/AO/FilamentAO.slangh"
#include "RenderPasses/FilamentFX/Lighting/FilamentPBR.slangh"
#include "RenderPasses/FilamentFX/Shadow/FilamentShadow.slangh"

#define MIN_PERCEPTUAL_ROUGHNESS 0.045f

cbuffer PerFrameCB
{
    uint2 gFrameDim;
    uint gSSAOEnabled;
    float gSunIntensity;
    float3 gSunColor;
    float3 gSunDirection;
    float gAmbientIntensity;
    uint gDebugView;
};

cbuffer FilamentIBLCB
{
    float gIblLuminance;
    float gIblRoughnessOneLevel;
    float gIblRotation;
    uint  gUseSH;
    float3 gIblSH[9];
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
    float4x4 gInvViewProj;
    float3 gCameraPos;
    float gDebugShadowPad;
};

Texture2D<float4> gBaseColor;
Texture2D<float4> gNormalW;
Texture2D<float4> gMaterial;
Texture2D<float4> gEmissive;
Texture2D<float4> gViewDirW;
Texture2D<float> gDepth;
Texture2D<float4> gIDs;
Texture2D<float> gShadowMap;
Texture2D<float2> gShadowMoments;
SamplerState gShadowPointSampler;

float3 rotateY(float3 v, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float3 filamentSHIrradiance(float3 n)
{
    float3 irradiance = gIblSH[0];
    irradiance += gIblSH[1] * n.y + gIblSH[2] * n.z + gIblSH[3] * n.x;
    irradiance += gIblSH[4] * (n.y * n.x) + gIblSH[5] * (n.y * n.z);
    irradiance += gIblSH[6] * (3.0f * n.z * n.z - 1.0f) + gIblSH[7] * (n.z * n.x);
    irradiance += gIblSH[8] * (n.x * n.x - n.y * n.y);
    return max(irradiance, 0.0f);
}

float3 evaluateStableFilamentIBL(float3 diffuseColor, float3 f0, float perceptualRoughness, float3 N, float3 V, float ao, float3 bentNormal)
{
    f0 = saturate(f0);
    float3 Nibl = rotateY(N, gIblRotation);
    float NoV = saturate(abs(dot(N, V)) + 1e-4f);

    float3 irradiance = filamentSHIrradiance(Nibl);
    float fresnelVisibility = pow(1.0f - NoV, 5.0f);
    float dfg0 = fresnelVisibility * (1.0f - perceptualRoughness);
    float dfg1 = lerp(0.45f, 1.0f, 1.0f - perceptualRoughness) * (1.0f - 0.35f * fresnelVisibility);
    float3 dfg = float3(dfg0, dfg1, dfg0);
    float3 E = lerp(dfg.xxx, dfg.yyy, f0);
    float3 diffuse = diffuseColor * irradiance * (1.0f - E) * singleBounceAO(ao);

    float3 R = rotateY(reflect(-V, N), gIblRotation);
    float3 dominantR = lerp(R, Nibl, perceptualRoughness * perceptualRoughness);
    dominantR = (dot(dominantR, dominantR) > 1e-6f) ? normalize(dominantR) : Nibl;
    float3 prefiltered = filamentSHIrradiance(dominantR);
    float3 specular = prefiltered * E * specularAO(NoV, ao, perceptualRoughness, N, V, bentNormal);

    return (diffuse + specular) * gIblLuminance;
}

float3 reconstructWorldPosition(float2 uv, float depth)
{
    float4 clip = float4(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0), depth, 1.0);
    float4 world = mul(gInvViewProj, clip);
    return world.xyz / world.w;
}

void getShadowDebugData(float3 worldPos, out int cascade, out float3 lightNdc, out float2 shadowUV, out float2 atlasUV, out float sampledDepth)
{
    float viewSpaceDepth = length(worldPos - gCameraPos);
    cascade = selectShadowCascade(viewSpaceDepth, gCascadeCount, gCascadeSplits);

    float4 lightSpacePos = mul(float4(worldPos, 1.0), gLightViewProj[cascade]);
    lightNdc = lightSpacePos.xyz / lightSpacePos.w;
    shadowUV = lightNdc.xy * 0.5 + 0.5;
    float4 atlasRect = gCascadeAtlasRect[cascade];
    atlasUV = shadowUV * atlasRect.zw + atlasRect.xy;
    sampledDepth = gShadowMap.SampleLevel(gShadowPointSampler, atlasUV, 0).r;
}

float4 main(float2 texC : TEXCOORD) : SV_TARGET
{
    uint2 pixel = min(uint2(texC * float2(gFrameDim)), gFrameDim - 1u);
    float4 base = gBaseColor.Load(int3(pixel, 0));
    float4 normalPacked = gNormalW.Load(int3(pixel, 0));
    float4 material = gMaterial.Load(int3(pixel, 0));
    float3 emissive = gEmissive.Load(int3(pixel, 0)).rgb;
    float3 V = normalize(gViewDirW.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    float depth = gDepth.Load(int3(pixel, 0));
    float2 ids = gIDs.Load(int3(pixel, 0)).xy;

    float3 N = normalize(normalPacked.xyz * 2.0f - 1.0f);
    float roughness = material.x;
    float3 f0 = material.yzw;
    float perceptualRoughness = max(saturate(roughness), MIN_PERCEPTUAL_ROUGHNESS);
    float ao = (gSSAOEnabled != 0u) ? evaluateSSAO(texC) : 1.0f;
    float3 bentNormal = (gSSAOEnabled != 0u) ? evaluateSSAOBentNormal(texC) : N;
    float3 worldPos = reconstructWorldPosition(texC, depth);
    float shadowVis = evaluateShadowVisibility(
        worldPos,
        gCameraPos,
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

    if (gDebugView == 1u) return float4(base.rgb, 1.0f);
    if (gDebugView == 2u) return float4(N * 0.5f + 0.5f, 1.0f);
    if (gDebugView == 3u)
    {
        uint id = uint(ids.x + 0.5f);
        return float4(float(id & 255u), float((id >> 8u) & 255u), float((id >> 16u) & 255u), 255.0f) / 255.0f;
    }
    if (gDebugView == 4u)
    {
        uint id = uint(ids.y + 0.5f);
        return float4(float(id & 255u), float((id >> 8u) & 255u), float((id >> 16u) & 255u), 255.0f) / 255.0f;
    }
    if (gDebugView == 5u) return float4(ao.xxx, 1.0f);
    if (gDebugView == 6u) return float4(shadowVis.xxx, 1.0f);
    if (gDebugView == 7u) {
        float atlasDepth = gShadowMap.SampleLevel(gShadowPointSampler, texC, 0).r;
        return float4(atlasDepth.xxx, 1.0f);
    }
    if (gDebugView >= 8u && gDebugView <= 11u) {
        int cascade = 0;
        float3 lightNdc = float3(0.0f);
        float2 shadowUV = float2(0.0f);
        float2 atlasUV = float2(0.0f);
        float sampledDepth = 1.0f;
        getShadowDebugData(worldPos, cascade, lightNdc, shadowUV, atlasUV, sampledDepth);
        if (gDebugView == 8u) {
            float band = (gCascadeCount > 1u) ? float(cascade) / float(gCascadeCount - 1u) : 0.0f;
            return float4(band, 1.0f - band, frac(float(cascade) * 0.37f), 1.0f);
        }
        if (gDebugView == 9u) {
            float valid = (shadowUV.x >= 0.0f && shadowUV.x <= 1.0f && shadowUV.y >= 0.0f && shadowUV.y <= 1.0f) ? 1.0f : 0.0f;
            return float4(saturate(shadowUV), valid, 1.0f);
        }
        if (gDebugView == 10u) {
            float valid = (lightNdc.z >= 0.0f && lightNdc.z <= 1.0f) ? 1.0f : 0.0f;
            return float4(saturate(lightNdc.z).xxx * valid, 1.0f);
        }
        float delta = lightNdc.z - sampledDepth;
        return float4(saturate(delta * 20.0f + 0.5f), saturate(sampledDepth), saturate(lightNdc.z), 1.0f);
    }

    float3 color = emissive + evaluateStableFilamentIBL(base.rgb, f0, perceptualRoughness, N, V, ao, bentNormal);
    color += base.rgb * gAmbientIntensity;

    float3 sunL = normalize(-gSunDirection);
    float NoL = saturate(dot(N, sunL));
    if (gSunIntensity > 0.0f && NoL > 0.0f)
    {
        float sunScale = min(gSunIntensity * 0.00001f, 3.0f);
        float3 sunRadiance = gSunColor * sunScale;
        PixelParams pixel;
        pixel.diffuseColor = base.rgb;
        pixel.perceptualRoughness = perceptualRoughness;
        pixel.roughness = perceptualRoughness * perceptualRoughness;
        pixel.f0 = f0;
        pixel.dfg = float3(0.0f);
        pixel.energyCompensation = float3(1.0f);
        color += surfaceShading(pixel, sunL, V, N, 1.0f) * sunRadiance * shadowVis;
    }

    return float4(color, 1.0f);
}
