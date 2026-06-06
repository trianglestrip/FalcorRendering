/***************************************************************************
 # PBRT preview deferred IBL lighting pass.
 **************************************************************************/

#include "RenderPasses/FilamentFX/FilamentAO.slangh"

#define MIN_PERCEPTUAL_ROUGHNESS 0.045f

cbuffer PerFrameCB
{
    uint2 gFrameDim;
    uint gSSAOEnabled;
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

Texture2D<float4> gBaseColor;
Texture2D<float4> gNormalW;
Texture2D<float4> gMaterial;
Texture2D<float4> gEmissive;
Texture2D<float4> gViewDirW;
Texture2D<float> gDepth;
Texture2D<float4> gIDs;

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

float3 evaluateStableFilamentIBL(float3 diffuseColor, float3 f0, float perceptualRoughness, float3 N, float3 V, float ao)
{
    float3 Nibl = rotateY(N, gIblRotation);
    float NoV = saturate(abs(dot(N, V)) + 1e-4f);

    float3 irradiance = filamentSHIrradiance(Nibl);
    float3 diffuse = diffuseColor * irradiance * ao;

    float3 R = rotateY(reflect(-V, N), gIblRotation);
    float3 specIrradiance = filamentSHIrradiance(normalize(lerp(R, Nibl, perceptualRoughness * perceptualRoughness)));
    float3 fresnel = f0 + (1.0f - f0) * pow(1.0f - NoV, 5.0f);
    float specAO = saturate(pow(NoV + ao, exp2(-16.0f * perceptualRoughness - 1.0f)) - 1.0f + ao);
    float clearCoatWeight = lerp(0.12f, 0.65f, 1.0f - perceptualRoughness);
    float3 specular = specIrradiance * fresnel * clearCoatWeight * specAO;

    return (diffuse + specular) * gIblLuminance;
}

float evaluateContactAO(uint2 pixel, float3 N, float depth)
{
    if (depth >= 0.9999f) return 1.0f;

    static const int2 offsets[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(2, 1), int2(-2, 1), int2(2, -1), int2(-2, -1)
    };

    float occ = 0.0f;
    int2 p = int2(pixel);
    int2 maxPixel = int2(gFrameDim) - 1;
    for (int i = 0; i < 8; ++i)
    {
        int2 q = clamp(p + offsets[i] * 3, int2(0, 0), maxPixel);
        float d = gDepth.Load(int3(q, 0));
        float closer = saturate((depth - d) * 3500.0f);
        float normalWeight = saturate(1.0f - abs(N.z) * 0.35f);
        occ += closer * normalWeight;
    }

    return saturate(1.0f - occ * 0.09f);
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
    ao = min(ao, evaluateContactAO(pixel, N, depth));

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

    float3 color = emissive + evaluateStableFilamentIBL(base.rgb, f0, perceptualRoughness, N, V, ao);
    return float4(color, 1.0f);
}
