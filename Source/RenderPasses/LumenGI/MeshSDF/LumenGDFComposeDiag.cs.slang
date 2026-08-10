/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/
// =====================================================================================
//  LumenGI - S6-B3 compose dispatch diagnostics (E1)
//  -------------------------------------------------------------------------------------
//  This file is intentionally independent from the production compose shader.  It is
//  loaded only by a temporary host-side diagnostic pass and is not part of the normal
//  RenderGraph.  E1 isolates the smallest possible D3D12/Falcor dispatch contract:
//  one R32Float RWTexture3D UAV and one logical thread.
// =====================================================================================

RWTexture3D<float> gGDFLevel;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // The host binds a valid level texture.  The marker makes the UAV write observable
    // while avoiding every other GDF/atlas resource and all production shader logic.
    if (all(dispatchThreadID == uint3(0, 0, 0)))
        gGDFLevel[uint3(0, 0, 0)] = 0.f;
}
