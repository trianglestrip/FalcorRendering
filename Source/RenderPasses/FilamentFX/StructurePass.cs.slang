// Filament Structure depth pyramid - aligns with PostProcessManager::structure() + mipmapDepth.mat
// Pass 1: copy main depth to structure mip 0
// Pass 2: min-depth 2x2 downsample chain for SSAO LOD (NEAREST_MIPMAP_NEAREST sampling)

cbuffer PerFrameCB
{
    uint2 gSrcRes;
    uint2 gDstRes;
    uint  gSrcMip;
};

Texture2D<float> gSrcDepth;
Texture2D<float> gStructureDepth;
RWTexture2D<float> gDst;
SamplerState gPointSampler;

[numthreads(16, 16, 1)]
void structureCopy(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gDstRes)) return;
    gDst[dispatchThreadId.xy] = gSrcDepth.Load(int3(dispatchThreadId.xy, 0));
}

float sampleStructureMip(uint2 texel, uint mip)
{
    float2 uv = (float2(texel) + 0.5) / float2(gSrcRes);
    return gStructureDepth.SampleLevel(gPointSampler, uv, (float)mip).r;
}

[numthreads(16, 16, 1)]
void structureMipmap(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gDstRes)) return;

    uint2 ocoord = dispatchThreadId.xy;
    uint2 base = ocoord * 2;

    // Min-depth downsample: preserve closest surface in D3D [0,1] depth (0=near, 1=far)
    float d0 = sampleStructureMip(base + uint2(0, 0), gSrcMip);
    float d1 = sampleStructureMip(base + uint2(1, 0), gSrcMip);
    float d2 = sampleStructureMip(base + uint2(0, 1), gSrcMip);
    float d3 = sampleStructureMip(base + uint2(1, 1), gSrcMip);

    gDst[ocoord] = min(min(d0, d1), min(d2, d3));
}
