// Filament-style temporal anti-aliasing (simple history blend + depth rejection)
cbuffer PerFrameCB
{
    uint2 gResolution;
    float gFeedback;
    float gEnabled;
};

Texture2D<float4> gSrc;
Texture2D<float4> gHistory;
Texture2D<float> gDepth;
Texture2D<float> gHistoryDepth;
RWTexture2D<float4> gDst;
SamplerState gLinearSampler;

[numthreads(16, 16, 1)]
void taaMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution)) return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5f) / float2(gResolution);

    float4 current = gSrc.Load(int3(coord, 0));
    if (gEnabled < 0.5) {
        gDst[coord] = current;
        return;
    }

    float depth = gDepth.SampleLevel(gLinearSampler, uv, 0).r;
    float4 history = gHistory.SampleLevel(gLinearSampler, uv, 0);
    float historyDepth = gHistoryDepth.SampleLevel(gLinearSampler, uv, 0).r;

    float depthDiff = abs(depth - historyDepth);
    float reject = saturate(1.0 - depthDiff * 50.0);

    float feedback = gFeedback * reject;
    float3 blended = lerp(current.rgb, history.rgb, feedback);
    gDst[coord] = float4(blended, current.a);
}
