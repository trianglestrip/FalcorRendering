// Filament-style temporal anti-aliasing with Halton jitter + motion-vector history reprojection
cbuffer PerFrameCB
{
    uint2 gResolution;
    float gFeedback;
    float gEnabled;
    float2 gJitter;       // Current-frame subpixel jitter (normalized screen space)
    float gUseMotionVec;  // 1.0 when motion vector texture is bound
};

Texture2D<float4> gSrc;
Texture2D<float4> gHistory;
Texture2D<float> gDepth;
Texture2D<float> gHistoryDepth;
Texture2D<float2> gMotionVec;
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

    // Reproject history using screen-space motion vectors (Filament/Falcor convention).
    float2 motion = float2(0.f, 0.f);
    if (gUseMotionVec > 0.5f) {
        motion = gMotionVec.Load(int3(coord, 0));
        // Use longest motion vector in 3x3 neighborhood to reduce edge disocclusion.
        const int2 offsets[8] = {
            int2(-1, -1), int2(0, -1), int2(1, -1),
            int2(-1,  0),              int2(1,  0),
            int2(-1,  1), int2(0,  1), int2(1,  1)
        };
        [unroll]
        for (int i = 0; i < 8; ++i) {
            int2 ncoord = coord + offsets[i];
            if (any(ncoord < 0) || any(ncoord >= int2(gResolution))) continue;
            float2 m = gMotionVec.Load(int3(ncoord, 0));
            motion = dot(m, m) > dot(motion, motion) ? m : motion;
        }
    }

    float2 historyUV = uv + motion;
    float4 history = gHistory.SampleLevel(gLinearSampler, historyUV, 0);
    float historyDepth = gHistoryDepth.SampleLevel(gLinearSampler, historyUV, 0).r;

    float depthDiff = abs(depth - historyDepth);
    float reject = saturate(1.0 - depthDiff * 50.0);

    // Jitter is applied at rasterization; gJitter is available for future unjittering refinements.
    float feedback = gFeedback * reject;
    float3 blended = lerp(current.rgb, history.rgb, feedback);
    gDst[coord] = float4(blended, current.a);
}
