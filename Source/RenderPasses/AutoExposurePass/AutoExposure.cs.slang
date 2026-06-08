// UE-style Auto Exposure — 2-pass compute pipeline
// Pass 1: Downsample scene color → log-luminance (64×64)
// Pass 2: Build 256-bin histogram → compute percentile-based exposure → eye adaptation
// Based on Unreal Engine 5's auto-exposure (histogram + percentile metering).

#include "RenderPasses/FilamentFX/Utils/FilamentGeometry.slangh"

// ---- Shared resources ----
Texture2D<float4>  gColorInput;
SamplerState       gLinearSampler;
SamplerState       gPointSampler;

// ===================================================================
// Pass 1: Compute Log-Luminance (downsample 4×4 blocks)
// ===================================================================
cbuffer Pass1CB
{
    uint2 gInputSize;
    uint2 gOutputSize;
};

RWTexture2D<float> gLuminanceOut;

[numthreads(16, 16, 1)]
void computeLuminance(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid.xy >= gOutputSize)) return;

    int2 base = int2(dtid.xy) * 4;
    float totalLum = 0.0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        int2 c = clamp(base + int2(x, y), int2(0), int2(gInputSize) - 1);
        float3 col = gColorInput.Load(int3(c, 0)).rgb;
        totalLum += max(dot(col, float3(0.2126, 0.7152, 0.0722)), 0.0);
    }
    gLuminanceOut[dtid.xy] = log2(max(totalLum / 16.0, 1e-6));
}

// ===================================================================
// Pass 2: Histogram + Percentile Exposure + Eye Adaptation (1 thread group)
// ===================================================================
cbuffer Pass2CB
{
    uint   gHistogramBins;
    float  gInvLogLumRange;   // 1 / (histogramMax - histogramMin)
    float  gHistogramMin;
    float  gHistogramMax;
    float  gLowPercent;        // shadow percentile  (UE default: 0.80)
    float  gHighPercent;       // highlight percentile (UE default: 0.98)
    float  gTargetMiddleGrey;  // 18% grey (0.18)
    float  gExposureComp;      // EV100 compensation offset
    float  gMinEV100;
    float  gMaxEV100;
    float  gAdaptationUp;      // speed when scene gets brighter
    float  gAdaptationDown;    // speed when scene gets darker
    float  gDeltaTime;
    float  gCurrentExposure;
};

Texture2D<float>           gLuminanceInput;
RWStructuredBuffer<uint>   gHistogramOut;
RWStructuredBuffer<float4> gExposureOut;    // .x = adapted, .y = target, .z = avgLum

groupshared uint gs_hist[256];

[numthreads(256, 1, 1)]
void computeExposure(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    // --- Step A: clear shared histogram ---
    gs_hist[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();

    // --- Step B: bin luminance samples ---
    uint2 lumDim;
    gLuminanceInput.GetDimensions(lumDim.x, lumDim.y);
    uint totalPixels = lumDim.x * lumDim.y;
    for (uint index = gi; index < totalPixels; index += 256u)
    {
        int2 coord = int2(index % lumDim.x, index / lumDim.x);
        float logLum = gLuminanceInput[coord];
        float norm = (logLum - gHistogramMin) * gInvLogLumRange;
        uint bin = uint(clamp(norm, 0.0, 1.0) * 255.0 + 0.5);
        InterlockedAdd(gs_hist[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // Write histogram (debug / readback)
    if (gi < 256u) gHistogramOut[gi] = gs_hist[gi];
    GroupMemoryBarrierWithGroupSync();

    // --- Step C: compute percentile exposure (thread 0) ---
    if (gi != 0u) return;

    uint totalCount = 0u;
    [unroll] for (uint i = 0u; i < 256u; ++i) totalCount += gs_hist[i];

    if (totalCount == 0u)
    {
        gExposureOut[0] = float4(gCurrentExposure, gCurrentExposure, 0.0, 0.0);
        return;
    }

    float invTotal = 1.0 / float(totalCount);
    uint lowTarget  = uint(float(totalCount) * gLowPercent);
    uint highTarget = uint(float(totalCount) * gHighPercent);

    float lowLogLum  = gHistogramMin;
    float highLogLum = gHistogramMin;
    uint accum = 0u;

    for (uint i = 0u; i < 256u; ++i)
    {
        accum += gs_hist[i];
        float binLogLum = gHistogramMin + float(i) / 255.0 * (gHistogramMax - gHistogramMin);
        if (accum <= lowTarget)  lowLogLum  = binLogLum;
        if (accum <= highTarget) highLogLum = binLogLum;
    }

    // UE approach: use low+high percentile average → middle grey mapping
    float avgLogLum = (lowLogLum + highLogLum) * 0.5;
    float avgLum = exp2(avgLogLum);

    float targetExp = gTargetMiddleGrey / max(avgLum, 1e-5);
    targetExp *= exp2(gExposureComp);
    targetExp = clamp(targetExp, exp2(-gMaxEV100), exp2(-gMinEV100));

    // Eye adaptation
    float speed = (targetExp > gCurrentExposure) ? gAdaptationUp : gAdaptationDown;
    float adapted = gCurrentExposure + (targetExp - gCurrentExposure) * saturate(speed * gDeltaTime);

    gExposureOut[0] = float4(adapted, targetExp, avgLum, 0.0);
}
