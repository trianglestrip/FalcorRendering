/***************************************************************************
 # Lightweight Filament-style forward pass for PBRT kitchen.
 **************************************************************************/

import Scene.Raster;
import Utils.Sampling.TinyUniformSampleGenerator;

cbuffer PerFrameCB
{
    uint2 gFrameDim;
    uint gSSAOEnabled;
    float gSunIntensity;
    float3 gSunColor;
    float3 gSunDirection;
    float gAmbientIntensity;
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

    uint2 pixelPos = uint2(vsOut.posH.xy);
    TinyUniformSampleGenerator sg = TinyUniformSampleGenerator(pixelPos, 0);

    float3 N = normalize(sd.frame.N);
    float3 albedo = props.diffuseReflectionAlbedo;
    if (all(albedo == float3(0.0f))) albedo = float3(0.5f, 0.47f, 0.42f);

    float3 color = props.emission;

    float3 ambientSky = float3(0.72f, 0.76f, 0.82f);
    float3 ambientGround = float3(0.34f, 0.30f, 0.26f);
    float hemi = saturate(N.y * 0.5f + 0.5f);
    color += albedo * lerp(ambientGround, ambientSky, hemi) * gAmbientIntensity;

    float3 sunL = normalize(-gSunDirection);
    if (gSunIntensity > 0.0f && dot(N, sunL) > 0.0f)
    {
        float sunScale = min(gSunIntensity * 0.00001f, 3.0f);
        color += mi.eval(sd, sunL, sg) * (gSunColor * sunScale);
    }

    return float4(color, 1.0f);
}
