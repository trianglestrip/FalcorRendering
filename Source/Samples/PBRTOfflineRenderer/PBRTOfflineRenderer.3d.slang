/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 # Uses PBRT native BSDF (mi.eval) for correct material evaluation.
 # Filament post-processing handles AO, Bloom, ColorGrading.
 **************************************************************************/
import Scene.Raster;
import Utils.Sampling.TinyUniformSampleGenerator;
import Rendering.Lights.LightHelpers;

VSOut vsMain(VSIn vIn) { return defaultVS(vIn); }

float4 psMain(VSOut vsOut, uint triangleIndex: SV_PrimitiveID) : SV_TARGET
{
    let lod = ImplicitLodTextureSampler();
    if (alphaTest(vsOut, triangleIndex, lod)) discard;

    float3 viewDir = normalize(gScene.camera.getPosition() - vsOut.posW);
    ShadingData sd = prepareShadingData(vsOut, triangleIndex, viewDir);
    let mi = gScene.materials.getMaterialInstance(sd, lod);
    let props = mi.getProperties(sd);
    uint2 pixelPos = uint2(vsOut.posH.xy);
    TinyUniformSampleGenerator sg = TinyUniformSampleGenerator(pixelPos, 0);

    float3 color = props.emission;

    // Direct lighting: use PBRT material's native BSDF (mi.eval)
    for (int i = 0; i < gScene.getLightCount(); i++)
    {
        AnalyticLightSample ls;
        evalLightApproximate(sd.posW, gScene.getLight(i), ls);
        color += mi.eval(sd, ls.dir, sg) * ls.Li;
    }

    // Fallback: If no lights, use simple view-dependent shading
    if (gScene.getLightCount() == 0)
    {
        float3 N = normalize(sd.frame.N);
        float3 V = normalize(gScene.camera.getPosition() - sd.posW);
        float3 ambientColor = float3(0.03f, 0.03f, 0.04f);
        float3 albedo = props.diffuseReflectionAlbedo;
        if (all(albedo == float3(0.f))) albedo = float3(0.5f, 0.45f, 0.4f);
        float fresnel = pow(1.f - abs(dot(N, V)), 3.f);
        float3 diffuse = albedo * 0.2f;
        float3 specular = float3(0.8f, 0.8f, 0.85f) * fresnel * 0.25f;
        color = ambientColor + diffuse + specular;
    }

    return float4(color, 1.f);
}
