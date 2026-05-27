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

    // Fallback: If no lights, use PBR-style view-dependent shading
    if (gScene.getLightCount() == 0)
    {
        float3 N = normalize(sd.frame.N);
        float3 V = normalize(gScene.camera.getPosition() - sd.posW);
        float NoV = abs(dot(N, V));

        // Ambient skylight (Filament-like IBL without env map)
        float3 skyColor = float3(0.5f, 0.55f, 0.7f);
        float3 groundColor = float3(0.15f, 0.12f, 0.1f);
        float3 ambient = lerp(groundColor, skyColor, saturate(N.y * 0.5f + 0.5f));

        float3 albedo = props.diffuseReflectionAlbedo;
        if (all(albedo == float3(0.f))) albedo = float3(0.5f, 0.45f, 0.4f);

        // Diffuse term (Filament-style energy-conserving)
        float3 diffuse = albedo * ambient * 0.5f;

        // Specular term (simple Fresnel)
        float3 F0 = props.specularReflectance;
        if (all(F0 == float3(0.f))) F0 = float3(0.04f, 0.04f, 0.04f);
        float3 Fresnel = F0 + (1.0f - F0) * pow(1.0f - NoV, 5.0f);
        float roughness = clamp(props.roughness, 0.05f, 1.0f);
        float3 specular = Fresnel * ambient * (1.0f - roughness) * 0.25f;

        color = diffuse + specular;
    }

    return float4(color, 1.f);
}
