/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/
// C4 E2a: keep the constant buffer and GDF structured-buffer descriptors live.
#include "LumenGDFData.slang"

cbuffer LumenGDFComposeCB
{
    LumenGDFClipmapParams gClipmap;
}

StructuredBuffer<LumenGDFLevelParams> gGDFLevelTable;
StructuredBuffer<LumenGDFInstance> gGDFInstances;
StructuredBuffer<LumenGDFDirtyRegion> gGDFDirtyRegions;
RWTexture3D<float> gGDFLevel;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (any(dispatchThreadID != uint3(0, 0, 0))) return;
    uint marker = gClipmap.levelCount ^ gClipmap.resolution;
    marker ^= gGDFLevelTable[0].resolution;
    marker ^= gGDFInstances[0].resident;
    marker ^= gGDFDirtyRegions[0].level;
    gGDFLevel[uint3(0, 0, 0)] = float(marker & 0xffu);
}
