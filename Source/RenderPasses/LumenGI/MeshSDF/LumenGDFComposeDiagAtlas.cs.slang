/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/
// C4 E2b: keep atlas SRVs and scalar uniforms live, with one level UAV.
#include "LumenMeshSDFAtlas.slang"

Texture3D<float> gFineAtlas;
Texture3D<float> gCoarseAtlas;
StructuredBuffer<uint> gPageTable;
StructuredBuffer<LumenMeshSDFVolumeDescriptor> gVolumes;
StructuredBuffer<LumenMeshSDFAtlasInstance> gInstances;
uniform uint gAtlasInstanceCount;
uniform uint gAtlasVolumeCount;
uniform uint gAtlasPagesPerSide;
RWTexture3D<float> gGDFLevel;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (any(dispatchThreadID != uint3(0, 0, 0))) return;
    uint marker = gAtlasInstanceCount ^ gAtlasVolumeCount ^ gAtlasPagesPerSide;
    marker ^= gPageTable[0];
    marker ^= gVolumes[0].resolution.x;
    marker ^= gInstances[0].meshID;
    marker ^= asuint(gFineAtlas.Load(int4(0, 0, 0, 0)));
    marker ^= asuint(gCoarseAtlas.Load(int4(0, 0, 0, 0)));
    gGDFLevel[uint3(0, 0, 0)] = float(marker & 0xffu);
}
