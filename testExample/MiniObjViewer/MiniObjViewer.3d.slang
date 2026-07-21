/***************************************************************************
 # Copyright (c) 2024, MiniFalcorSDK Test Example
 #--------------------------------------------------------------------------
 # Simple vertex/pixel shader for Mini OBJ Viewer
 **************************************************************************/
import Scene.Raster;
import Utils.Sampling.TinyUniformSampleGenerator;
import Rendering.Lights.LightHelpers;

VSOut vsMain(VSIn vIn)
{
    return defaultVS(vIn);
}

float4 psMain(VSOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_TARGET
{
    let lod = ImplicitLodTextureSampler();
    if (alphaTest(vsOut, triangleIndex, lod))
        discard;

    float3 viewDir = normalize(gScene.camera.getPosition() - vsOut.posW);
    ShadingData sd = prepareShadingData(vsOut, triangleIndex, viewDir);

    let mi = gScene.materials.getMaterialInstance(sd, lod);
    float3 color = mi.getProperties(sd).emission;

    uint3 launchIndex = uint3(vsOut.posH.xy, 0);
    TinyUniformSampleGenerator sg = TinyUniformSampleGenerator(launchIndex.xy, 0);

    for (uint i = 0; i < gScene.getLightCount(); i++)
    {
        AnalyticLightSample ls;
        evalLightApproximate(sd.posW, gScene.getLight(i), ls);
        color += mi.eval(sd, ls.dir, sg) * ls.Li;
    }

    color = color / (color + 1.0);
    return float4(color, 1.f);
}
