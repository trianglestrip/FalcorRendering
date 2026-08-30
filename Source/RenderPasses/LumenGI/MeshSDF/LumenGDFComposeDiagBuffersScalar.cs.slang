/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/
// C4 E2c: test whether the explicit compose CB and Falcor's global uniform CB
// can coexist before atlas resources are introduced.
#include "LumenGDFData.slang"

cbuffer LumenGDFComposeCB
{
    LumenGDFClipmapParams gClipmap;
}

StructuredBuffer<LumenGDFLevelParams> gGDFLevelTable;
StructuredBuffer<LumenGDFInstance> gGDFInstances;
StructuredBuffer<LumenGDFDirtyRegion> gGDFDirtyRegions;
uniform uint gAtlasPagesPerSide;
RWTexture3D<float> gGDFLevel;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (any(dispatchThreadID != uint3(0, 0, 0))) return;
    uint marker = gClipmap.levelCount ^ gClipmap.resolution ^ gAtlasPagesPerSide;
    marker ^= gGDFLevelTable[0].resolution;
    marker ^= gGDFInstances[0].resident;
    marker ^= gGDFDirtyRegions[0].level;
    gGDFLevel[uint3(0, 0, 0)] = float(marker & 0xffu);
}
