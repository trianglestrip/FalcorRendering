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
    float3 baseColor = saturate(props.diffuseReflectionAlbedo);
    float3 specularAlbedo = max(props.specularReflectionAlbedo, props.specularReflectance);
    float roughness = saturate(props.roughness);
    float3 f0 = max(props.specularReflectance, specularAlbedo * 0.04f);

    GBufferOut out;
    out.baseColor = float4(baseColor, 1.0f);
    out.normalW = float4(N * 0.5f + 0.5f, 1.0f);
    out.material = float4(roughness, f0);
    out.emissive = float4(props.emission, 1.0f);
    out.viewDirW = float4(viewDir * 0.5f + 0.5f, 1.0f);
    out.ids = float4(float(vsOut.materialID), float(vsOut.instanceID.index), 0.0f, 1.0f);
    return out;
}
