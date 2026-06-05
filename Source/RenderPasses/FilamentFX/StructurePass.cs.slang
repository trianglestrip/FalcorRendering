// Filament Structure depth pyramid - aligns with PostProcessManager::structure() + mipmapDepth.mat
// Pass 1: copy main depth to structure mip 0
// Pass 2: rotated-grid depth mip chain used by Filament SSAO.

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

    // Filament mipmapDepth.mat uses a rotated grid single subsample:
    // texelFetch(depth, 2 * icoord + ivec2(icoord.y & 1, icoord.x & 1), 0).
    uint2 rotated = base + uint2(ocoord.y & 1u, ocoord.x & 1u);
    gDst[ocoord] = sampleStructureMip(rotated, gSrcMip);
}
