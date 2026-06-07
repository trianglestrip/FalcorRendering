/***************************************************************************
 # PBRT preview GBuffer pass.
 **************************************************************************/

import Scene.Raster;

struct GBufferOut
{
    float4 baseColor : SV_TARGET0;
    float4 normalW : SV_TARGET1;
    float4 material : SV_TARGET2;
    float4 emissive : SV_TARGET3;
    float4 viewDirW : SV_TARGET4;
    float4 ids : SV_TARGET5;
};

VSOut vsMain(VSIn vIn)
{
    return defaultVS(vIn);
}

GBufferOut psMain(VSOut vsOut, uint triangleIndex : SV_PrimitiveID)
{
    let lod = ImplicitLodTextureSampler();
    if (alphaTest(vsOut, triangleIndex, lod)) discard;

    float3 viewDir = normalize(gScene.camera.getPosition() - vsOut.posW);
    ShadingData sd = prepareShadingData(vsOut, triangleIndex, viewDir);
    let mi = gScene.materials.getMaterialInstance(sd, lod);
    let props = mi.getProperties(sd);

    float nLen = length(sd.frame.N);
    float3 N = (nLen > 1e-5f) ? (sd.frame.N / nLen) : float3(0.0f, 1.0f, 0.0f);
    float3 diffuseAlbedo = saturate(props.diffuseReflectionAlbedo);
    float3 specularAlbedo = max(props.specularReflectionAlbedo, props.specularReflectance);
    float roughness = max(saturate(props.roughness), 0.045f);
    float diffuseLum = dot(diffuseAlbedo, float3(0.2126f, 0.7152f, 0.0722f));
    float specLum = dot(saturate(specularAlbedo), float3(0.2126f, 0.7152f, 0.0722f));
    float metallic = saturate((specLum - 0.08f) / 0.72f) * (1.0f - saturate(diffuseLum * 4.0f));
    float3 dielectricF0 = max(props.specularReflectance, float3(0.04f, 0.04f, 0.04f));
    float3 baseColor = lerp(diffuseAlbedo, saturate(specularAlbedo), metallic);
    if (dot(baseColor, float3(1.0f)) <= 1e-4f)
        baseColor = float3(0.5f, 0.5f, 0.5f);
    float3 f0 = saturate(lerp(dielectricF0, baseColor, metallic));

    GBufferOut out;
    out.baseColor = float4(baseColor, 1.0f);
    out.normalW = float4(N * 0.5f + 0.5f, 1.0f);
    out.material = float4(roughness, f0);
    out.emissive = float4(props.emission, 1.0f);
    out.viewDirW = float4(viewDir * 0.5f + 0.5f, 1.0f);
    out.ids = float4(float(vsOut.materialID), float(vsOut.instanceID.index), 0.0f, 1.0f);
    return out;
}
