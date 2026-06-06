/***************************************************************************
 # Lightweight Filament-style forward pass for PBRT kitchen.
 **************************************************************************/

import Scene.Raster;
#include "RenderPasses/FilamentFX/FilamentAO.slangh"

#define MIN_PERCEPTUAL_ROUGHNESS 0.045f

cbuffer FilamentIBLCB
{
    float gIblLuminance;
    float gIblRoughnessOneLevel;
    float gIblRotation;
    uint  gUseSH;
    float3 gIblSH[9];
};

float perceptualRoughnessToRoughness(float perceptualRoughness)
{
    return perceptualRoughness * perceptualRoughness;
}

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

VSOut vsMain(VSIn vIn)
{
    return defaultVS(vIn);
}

float4 psMain(VSOut vsOut, uint triangleIndex: SV_PrimitiveID) : SV_TARGET0
{
    let lod = ImplicitLodTextureSampler();
    if (alphaTest(vsOut, triangleIndex, lod)) discard;

    float3 viewDir = normalize(gScene.camera.getPosition() - vsOut.posW);
    ShadingData sd = prepareShadingData(vsOut, triangleIndex, viewDir);
    let mi = gScene.materials.getMaterialInstance(sd, lod);
    let props = mi.getProperties(sd);

    float nLen = length(sd.frame.N);
    float3 N = (nLen > 1e-5f) ? (sd.frame.N / nLen) : float3(0.0f, 1.0f, 0.0f);
    float3 albedo = props.diffuseReflectionAlbedo;
    float3 specularAlbedo = max(props.specularReflectionAlbedo, props.specularReflectance);
    float rough = saturate(props.roughness);

    float2 uv = (vsOut.posH.xy + 0.5f) / float2(gFrameDim);
    float ao = (gSSAOEnabled != 0u) ? evaluateSSAO(uv) : 1.0f;

    if (gDebugView == 1u) return float4(saturate(albedo), 1.0f);
    if (gDebugView == 2u) return float4(N * 0.5f + 0.5f, 1.0f);
    if (gDebugView == 3u)
    {
        uint id = vsOut.materialID;
        return float4(float(id & 255u), float((id >> 8u) & 255u), float((id >> 16u) & 255u), 255.0f) / 255.0f;
    }
    if (gDebugView == 4u)
    {
        uint id = vsOut.instanceID.index;
        return float4(float(id & 255u), float((id >> 8u) & 255u), float((id >> 16u) & 255u), 255.0f) / 255.0f;
    }
    if (gDebugView == 5u) return float4(ao.xxx, 1.0f);

    float perceptualRoughness = max(saturate(rough), MIN_PERCEPTUAL_ROUGHNESS);
    float3 f0 = max(props.specularReflectance, specularAlbedo * 0.04f);

    float3 color = props.emission + evaluateStableFilamentIBL(albedo, f0, perceptualRoughness, N, viewDir, ao);

    float3 indirectColor = color;

    float3 sunL = normalize(-gSunDirection);
    float NoL = saturate(dot(N, sunL));
    if (gSunIntensity > 0.0f && NoL > 0.0f)
    {
        float glossy = saturate(1.0f - rough * 0.8f);
        float specLum = dot(specularAlbedo, float3(0.2126f, 0.7152f, 0.0722f));
        float sunScale = min(gSunIntensity * 0.00001f, 3.0f);
        float3 sunRadiance = gSunColor * sunScale;
        color += albedo * sunRadiance * NoL;

        if (specLum > 0.0f)
        {
            float3 H = normalize(sunL + viewDir);
            float NoH = saturate(dot(N, H));
            float VoH = saturate(dot(viewDir, H));
            float specPower = lerp(8.0f, 160.0f, glossy);
            float3 f0 = max(props.specularReflectance, specularAlbedo * 0.04f);
            float3 fresnel = f0 + (specularAlbedo - f0) * pow(1.0f - VoH, 5.0f);
            float spec = pow(NoH, specPower) * glossy * NoL;
            color += fresnel * sunRadiance * spec * 0.5f;
        }
    }

    float3 sunContrib = color - indirectColor;
    float ambientWeight = 1.0f;
    color = indirectColor + sunContrib;

    return float4(color, ambientWeight);
}
