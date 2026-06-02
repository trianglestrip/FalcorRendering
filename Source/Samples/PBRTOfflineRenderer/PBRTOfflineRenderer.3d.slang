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

    float3 N = normalize(sd.frame.N);
    float3 albedo = props.diffuseReflectionAlbedo;
    if (all(albedo == float3(0.f))) albedo = float3(0.5f, 0.45f, 0.4f);

    // Indirect skylight (Filament surface_light_indirect - SSAO modulates this in post)
    float3 skyColor = float3(0.5f, 0.55f, 0.7f);
    float3 groundColor = float3(0.15f, 0.12f, 0.1f);
    float3 skylight = lerp(groundColor, skyColor, saturate(N.y * 0.5f + 0.5f));
    float3 indirect = albedo * skylight * 0.35f;

    float3 color = props.emission + indirect;

    // Direct lighting (not multiplied by screen-space AO)
    for (int i = 0; i < gScene.getLightCount(); i++)
    {
        AnalyticLightSample ls;
        evalLightApproximate(sd.posW, gScene.getLight(i), ls);
        color += mi.eval(sd, ls.dir, sg) * ls.Li;
    }

    // Ambient weight in alpha for post-process AO (luma ratio of indirect vs total)
    float lumTotal = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    float lumIndirect = dot(indirect, float3(0.2126f, 0.7152f, 0.0722f));
    float ambientWeight = lumIndirect / max(lumTotal, 1e-4f);
    ambientWeight = saturate(ambientWeight);

    return float4(color, ambientWeight);
}
