// Filament Bloom - exact implementation
// Based on filament/src/materials/bloom/bloomDownsample.mat + bloomUpsample.mat
// Reference: "Next Generation Post-Processing in CoD Advanced Warfare" by Jimenez

cbuffer PerFrameCB
{
    uint2 gSrcRes;
    uint2 gDstRes;
    float gBloomStrength;
    float gThreshold;
};

Texture2D<float4> gSrc;
Texture2D<float4> gHigherMip;
RWTexture2D<float4> gDst;
SamplerState gSampler;

// Filament's box4x4 helper
float3 box4x4(float3 s0, float3 s1, float3 s2, float3 s3)
{
    return (s0 + s1 + s2 + s3) * 0.25;
}

// Filament bloomDownsample - 5 boxes of 4x4 (13 unique samples)
[numthreads(16, 16, 1)]
void bloomDownsample(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gDstRes)) return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5) / float2(gDstRes);

    // 13 texture fetches (same as Filament bloomDownsample)
    float3 c  = gSrc.SampleLevel(gSampler, uv, 0).rgb;
    float3 lt = gSrc.SampleLevel(gSampler, uv, 0, int2(-1, -1)).rgb;
    float3 rt = gSrc.SampleLevel(gSampler, uv, 0, int2( 1, -1)).rgb;
    float3 rb = gSrc.SampleLevel(gSampler, uv, 0, int2( 1,  1)).rgb;
    float3 lb = gSrc.SampleLevel(gSampler, uv, 0, int2(-1,  1)).rgb;
    float3 lt2= gSrc.SampleLevel(gSampler, uv, 0, int2(-2, -2)).rgb;
    float3 rt2= gSrc.SampleLevel(gSampler, uv, 0, int2( 2, -2)).rgb;
    float3 rb2= gSrc.SampleLevel(gSampler, uv, 0, int2( 2,  2)).rgb;
    float3 lb2= gSrc.SampleLevel(gSampler, uv, 0, int2(-2,  2)).rgb;
    float3 l  = gSrc.SampleLevel(gSampler, uv, 0, int2(-2,  0)).rgb;
    float3 t  = gSrc.SampleLevel(gSampler, uv, 0, int2( 0, -2)).rgb;
    float3 r  = gSrc.SampleLevel(gSampler, uv, 0, int2( 2,  0)).rgb;
    float3 b  = gSrc.SampleLevel(gSampler, uv, 0, int2( 0,  2)).rgb;

    // Five 4x4 boxes (Filament's bloomDownsample)
    float3 c0 = box4x4(lt, rt, rb, lb);
    float3 c1 = box4x4(c, l, t, lt2)
              + box4x4(c, r, t, rt2)
              + box4x4(c, r, b, rb2)
              + box4x4(c, l, b, lb2);

    // Weighted average (Filament: c0*0.5 + c1*0.125)
    float3 color = c0 * 0.5 + c1 * 0.125;

    // Threshold (fireflies suppression)
    if (gThreshold > 0.0)
    {
        float brightness = max(color.r, max(color.g, color.b));
        float contribution = max(0.0, brightness - gThreshold);
        contribution /= max(brightness, 1e-6);
        color *= contribution;
    }

    gDst[coord] = float4(color, 1.0);
}

// Filament bloomUpsample - HIGH quality (tent filter + 9 samples)
[numthreads(16, 16, 1)]
void bloomUpsample(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gDstRes)) return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5) / float2(gDstRes);

    // texel size in SOURCE (smaller mip) space
    float2 d = 1.0 / float2(gSrcRes);

    // Filament HIGH quality upsample
    float3 c0, c1;
    c0  = gSrc.SampleLevel(gSampler, uv + float2(-d.x, -d.y), 0).rgb;
    c0 += gSrc.SampleLevel(gSampler, uv + float2( d.x, -d.y), 0).rgb;
    c0 += gSrc.SampleLevel(gSampler, uv + float2( d.x,  d.y), 0).rgb;
    c0 += gSrc.SampleLevel(gSampler, uv + float2(-d.x,  d.y), 0).rgb;
    c0 += 4.0 * gSrc.SampleLevel(gSampler, uv, 0).rgb;

    c1  = gSrc.SampleLevel(gSampler, uv + float2(-d.x,   0), 0).rgb;
    c1 += gSrc.SampleLevel(gSampler, uv + float2(   0, -d.y), 0).rgb;
    c1 += gSrc.SampleLevel(gSampler, uv + float2( d.x,   0), 0).rgb;
    c1 += gSrc.SampleLevel(gSampler, uv + float2(   0,  d.y), 0).rgb;

    float3 upsampled = (c0 + 2.0 * c1) * (1.0 / 16.0);

    // Composite with higher mip
    float3 higher = gHigherMip.Load(int3(coord, 0)).rgb;
    float3 result = higher + upsampled * gBloomStrength;

    gDst[coord] = float4(result, 1.0);
}
