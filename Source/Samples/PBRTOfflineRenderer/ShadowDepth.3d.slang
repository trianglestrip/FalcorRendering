/***************************************************************************
 # Shadow depth-only pass - renders geometry from light's perspective.
 # C++ drives multi-cascade atlas rendering via per-cascade viewport + camera matrices.
 # VSM: writes EVSM moments (RG) to color attachment alongside D32 depth.
 **************************************************************************/
import Scene.Raster;
#include "RenderPasses/FilamentFX/FilamentShadow.slangh"

cbuffer ShadowDepthCB
{
    uint gOutputMoments;
    float gVsmExponent;
    float gVsmMaxMoment;
};

struct PSOut
{
    float2 moments : SV_TARGET0;
};

VSOut vsMain(VSIn vIn)
{
    return defaultVS(vIn);
}

PSOut psMain(VSOut vsOut, uint triangleIndex: SV_PrimitiveID)
{
    let lod = ImplicitLodTextureSampler();
    if (alphaTest(vsOut, triangleIndex, lod)) discard;

    PSOut out;
    out.moments = float2(0.0, 0.0);
    if (gOutputMoments != 0u)
    {
        float depth = vsOut.posH.z / vsOut.posH.w;
        float c = (gVsmExponent > 0.0) ? gVsmExponent : kVsmExponent;
        float maxMoment = (gVsmMaxMoment > 0.0) ? gVsmMaxMoment : kVsmMaxMoment;
        out.moments = computeDepthMomentsVSM(depth, c, maxMoment);
    }
    return out;
}
