// Separable Gaussian blur for EVSM shadow moments (Filament evsm/gaussian.mat)
// Blurs RG32F atlas in-place via ping-pong (horizontal then vertical pass)

cbuffer PerFrameCB
{
    uint2 gResolution;
    int2 gBlurDir;           // (1,0) horizontal, (0,1) vertical
    int gBlurRadius;
    int gBlurSampleCount;
    float gBlurKernel[32];
    int2 gBoundsMin;         // Atlas tile bounds in texels (inclusive)
    int2 gBoundsMax;         // Atlas tile bounds in texels (inclusive)
};

Texture2D<float2> gSrcMoments;
RWTexture2D<float2> gDstMoments;
SamplerState gPointSampler;

[numthreads(16, 16, 1)]
void evsmBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution))
        return;

    int2 coord = int2(dispatchThreadId.xy);

    // Skip pixels outside the cascade tile bounds (atlas-safe blur)
    if (any(coord < gBoundsMin) || any(coord > gBoundsMax))
    {
        gDstMoments[coord] = gSrcMoments.Load(int3(coord, 0));
        return;
    }

    float2 result = gSrcMoments.Load(int3(clamp(coord, gBoundsMin, gBoundsMax), 0)) * gBlurKernel[0];

    for (int i = 1; i < gBlurSampleCount; ++i)
    {
        int2 offset = gBlurDir * i;
        int2 posCoord = clamp(coord + offset, gBoundsMin, gBoundsMax);
        int2 negCoord = clamp(coord - offset, gBoundsMin, gBoundsMax);
        float w = gBlurKernel[i];
        result += gSrcMoments.Load(int3(posCoord, 0)) * w;
        result += gSrcMoments.Load(int3(negCoord, 0)) * w;
    }

    gDstMoments[coord] = result;
}
