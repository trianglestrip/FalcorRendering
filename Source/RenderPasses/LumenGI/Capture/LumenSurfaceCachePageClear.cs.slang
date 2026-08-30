/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/

// Clears the capture-owned portion of a Surface Cache page before raster capture.
// A raster card can leave texels untouched (backface, alpha-test, unsupported material,
// or simply no covered triangle).  Clearing the complete 16x16 page prevents those
// texels from retaining radiance/material/metadata from a previous page owner.

import LumenCardCaptureData;

StructuredBuffer<uint> gPageIDs;
RWTexture2D<float4> gMaterialAtlas;
RWTexture2D<float4> gRadianceAtlas;
RWTexture2D<float4> gMetadataAtlas;
RWTexture2D<uint> gCaptureOrderAtlas;

cbuffer LumenSurfaceCachePageClearCB
{
    uint gPageCount;
    uint gPagesPerSide;
    uint2 gAtlasSize;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
    if (dispatchID.z >= gPageCount || dispatchID.x >= kLumenSurfaceCacheTileSize ||
        dispatchID.y >= kLumenSurfaceCacheTileSize || gPagesPerSide == 0u)
        return;

    const uint pageID = gPageIDs[dispatchID.z];
    if (pageID == kInvalidPageID || pageID > gPagesPerSide * gPagesPerSide)
        return;

    const uint2 origin = lumenCardPageTexelOrigin(pageID, gPagesPerSide);
    const uint2 texel = origin + dispatchID.xy;
    if (any(texel >= gAtlasSize))
        return;

    gMaterialAtlas[texel] = float4(0.f);
    gRadianceAtlas[texel] = float4(0.f);
    gMetadataAtlas[texel] = float4(0.f);
    gCaptureOrderAtlas[texel] = 0xffffffffu;
}
