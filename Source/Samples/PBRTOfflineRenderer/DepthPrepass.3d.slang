/***************************************************************************
 # Depth-only prepass for SSAO (Filament: structure + SSAO before color pass).
 **************************************************************************/
import Scene.Raster;

VSOut vsMain(VSIn vIn)
{
    return defaultVS(vIn);
}

void psMain(VSOut vsOut, uint triangleIndex: SV_PrimitiveID)
{
    let lod = ImplicitLodTextureSampler();
    if (alphaTest(vsOut, triangleIndex, lod)) discard;
}
