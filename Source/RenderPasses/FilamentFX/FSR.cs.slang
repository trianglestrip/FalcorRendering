// AMD FSR1 RCAS-inspired sharpening (post tone-map / FXAA)
cbuffer PerFrameCB
{
    uint2 gResolution;
    float gSharpness;
};

Texture2D<float4> gSrc;
RWTexture2D<float4> gDst;
SamplerState gLinearSampler;

float3 applyRcas(float2 uv, float3 c, float sharpness)
{
    float2 texel = 1.0 / float2(gResolution);
    float3 n = gSrc.SampleLevel(gLinearSampler, uv + float2(0, -texel.y), 0).rgb;
    float3 s = gSrc.SampleLevel(gLinearSampler, uv + float2(0, texel.y), 0).rgb;
    float3 w = gSrc.SampleLevel(gLinearSampler, uv + float2(-texel.x, 0), 0).rgb;
    float3 e = gSrc.SampleLevel(gLinearSampler, uv + float2(texel.x, 0), 0).rgb;
    float3 mn = min(c, min(min(n, s), min(w, e)));
    float3 mx = max(c, max(max(n, s), max(w, e)));
    float3 amp = saturate(mx - mn);
    float3 sharp = (n + s + w + e) * 0.25 - c;
    return c + sharp * sharpness * (1.0 - amp);
}

[numthreads(16, 16, 1)]
void fsrRcasMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution))
        return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5) / float2(gResolution);
    float3 c = gSrc.SampleLevel(gLinearSampler, uv, 0).rgb;
    if (gSharpness > 1e-4)
        c = applyRcas(uv, c, gSharpness);
    gDst[coord] = float4(saturate(c), 1.0);
}
