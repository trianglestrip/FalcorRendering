/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/
// C4 E2d: isolate the explicit compose CB plus one Falcor global uniform.
// This is intentionally smaller than E2c and contains no structured buffers.
#include "LumenGDFData.slang"

cbuffer LumenGDFComposeCB
{
    LumenGDFClipmapParams gClipmap;
}

uniform uint gAtlasPagesPerSide;
RWTexture3D<float> gGDFLevel;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (any(dispatchThreadID != uint3(0, 0, 0))) return;
    const uint marker = gClipmap.levelCount ^ gClipmap.resolution ^ gAtlasPagesPerSide;
    gGDFLevel[uint3(0, 0, 0)] = float(marker & 0xffu);
}
