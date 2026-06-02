// Filament-style Depth of Field (CoC + gather blur)
cbuffer PerFrameCB
{
    uint2 gResolution;
    float gFocalDistance;
    float gAperture;
    float gMaxCoC;
    float gDoFEnabled;
};

Texture2D<float4> gSrc;
Texture2D<float> gDepth;
RWTexture2D<float4> gDst;
SamplerState gLinearSampler;

float linearizeDepth(float depth)
{
    return 0.1 / max(depth, 1.0 / 16777216.0);
}

float computeCoC(float linearDepth)
{
    if (gDoFEnabled < 0.5) return 0.0;
    float coc = abs(linearDepth - gFocalDistance) / max(gFocalDistance, 0.01);
    coc *= gAperture * 0.1;
    return clamp(coc * gMaxCoC, 0.0, gMaxCoC);
}

[numthreads(16, 16, 1)]
void dofMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution)) return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5f) / float2(gResolution);
    float2 texelSize = 1.0 / float2(gResolution);

    float depth = gDepth.SampleLevel(gLinearSampler, uv, 0).r;
    if (depth < 1e-5) {
        gDst[coord] = gSrc.Load(int3(coord, 0));
        return;
    }

    float linearDepth = linearizeDepth(depth);
    float coc = computeCoC(linearDepth);
    int radius = int(ceil(coc));

    if (radius <= 0) {
        gDst[coord] = gSrc.Load(int3(coord, 0));
        return;
    }

    float3 color = float3(0, 0, 0);
    float weightSum = 0.0;
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x == 0 && y == 0) continue;
            int2 offCoord = clamp(coord + int2(x, y), int2(0, 0), int2(gResolution) - 1);
            float2 offUV = (float2(offCoord) + 0.5) * texelSize;
            float offDepth = gDepth.SampleLevel(gLinearSampler, offUV, 0).r;
            float offCoc = computeCoC(linearizeDepth(offDepth));
            float w = 1.0 / (1.0 + abs(float(x)) + abs(float(y)) + abs(coc - offCoc));
            color += gSrc.Load(int3(offCoord, 0)).rgb * w;
            weightSum += w;
        }
    }
    float3 center = gSrc.Load(int3(coord, 0)).rgb;
    color = (center + color) / (1.0 + weightSum);
    gDst[coord] = float4(color, 1.0);
}
