/***************************************************************************
 # Shadow depth-only pass - renders geometry from light's perspective.
 # C++ drives multi-cascade atlas rendering via per-cascade viewport + camera matrices.
 **************************************************************************/
import Scene.Raster;

VSOut vsMain(VSIn vIn)
{
    return defaultVS(vIn);
}

// Pixel shader: only writes depth (SV_TARGET not used, just depth output)
// In a depth-only pass, no pixel shader output is needed for color.
// We include a minimal psMain only for SV_IsFrontFace / alpha test.
void psMain(VSOut vsOut, uint triangleIndex: SV_PrimitiveID)
{
    // Alpha test: discard transparent fragments so they don't cast shadows
    let lod = ImplicitLodTextureSampler();
    if (alphaTest(vsOut, triangleIndex, lod)) discard;
    // No color output needed - only depth is written
}
