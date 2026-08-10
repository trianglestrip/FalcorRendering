/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/
// =====================================================================================
//  LumenGI - S6-B3 compose dispatch diagnostics (E2)
//  -------------------------------------------------------------------------------------
//  Standalone all-resource binding probe.  This shader deliberately does not call the
//  production compose helpers; it only keeps each descriptor live and writes a compact
//  marker into the current level UAV.  The temporary host pass must bind resources in
//  the same order/names as LumenGDFCompose.cs.slang:
//    CB -> GDF buffers -> atlas SRVs -> atlas scalars -> one level UAV.
//  It is not part of the normal RenderGraph and must remain explicitly gated by the
//  host-side E2 diagnostic switch.
// =====================================================================================

#include "LumenGDFData.slang"
#include "LumenMeshSDFAtlas.slang"

cbuffer LumenGDFComposeCB
{
    LumenGDFClipmapParams gClipmap;
}

StructuredBuffer<LumenGDFLevelParams> gGDFLevelTable;
StructuredBuffer<LumenGDFInstance> gGDFInstances;
StructuredBuffer<LumenGDFDirtyRegion> gGDFDirtyRegions;

RWTexture3D<float> gGDFLevel;

Texture3D<float> gFineAtlas;
Texture3D<float> gCoarseAtlas;
StructuredBuffer<uint> gPageTable;
StructuredBuffer<LumenMeshSDFVolumeDescriptor> gVolumes;
StructuredBuffer<LumenMeshSDFAtlasInstance> gInstances;

uniform uint gAtlasInstanceCount;
uniform uint gAtlasVolumeCount;
uniform uint gAtlasPagesPerSide;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (any(dispatchThreadID != uint3(0, 0, 0)))
        return;

    // Current S6 resources are allocated with at least one element/texel.  The host
    // diagnostic is expected to run after ensureGDFResources() and atlas upload, so these
    // reads make every descriptor live without invoking the production sampling path.
    uint marker = gClipmap.levelCount ^ gClipmap.resolution;
    marker ^= gAtlasInstanceCount ^ gAtlasVolumeCount ^ gAtlasPagesPerSide;
    marker ^= gGDFLevelTable[0].resolution;
    marker ^= gGDFInstances[0].resident;
    marker ^= gGDFDirtyRegions[0].level;
    marker ^= gPageTable[0];
    marker ^= gVolumes[0].resolution.x;
    marker ^= gInstances[0].meshID;
    marker ^= asuint(gFineAtlas.Load(int4(0, 0, 0, 0)));
    marker ^= asuint(gCoarseAtlas.Load(int4(0, 0, 0, 0)));

    gGDFLevel[uint3(0, 0, 0)] = float(marker & 0xffu);
}
