#include "RenderPasses/FilamentFX/Utils/FilamentGeometry.slangh"
#include "RenderPasses/FilamentFX/Utils/FilamentNoise.slangh"

cbuffer PerFrameCB
{
    uint2   gResolution;
    float4x4 gInvProj;
    float4x4 gInvViewProj;
    float3  gCameraPos;
    float   gNearPlane;
    float   gFarPlane;
    float2  gPositionParams;
    float   gRadius;
    float   gIntensity;
    float   gBias;
    float   gPower;
    uint    gSampleCount;
    uint    gNormalMode;
};

cbuffer BlurCB
{
    uint2  gBlurResolution;
    float2 gBlurAxis;
    uint   gBlurRadius;
    float  gBlurSharpness;
};

Texture2D<float> gDepth;
Texture2D<float4> gNormalW;
RWTexture2D<float4> gDst;
Texture2D<float4> gAOInput;
RWTexture2D<float4> gBlurredAO;
SamplerState gPointSampler;
SamplerState gLinearSampler;

float3 decodeNormal(float4 n)
{
    float3 normal = (gNormalMode == 0u) ? (n.xyz * 2.0 - 1.0) : n.xyz;
    float len2 = dot(normal, normal);
    return len2 > 1e-8 ? normal * rsqrt(len2) : float3(0.0, 1.0, 0.0);
}

float sampleDepth(float2 uv)
{
    return gDepth.SampleLevel(gPointSampler, uv, 0).r;
}

float3 reconstructWorld(float2 uv, float depth)
{
    return FilamentComputeWorldPosition(uv, depth, gInvViewProj);
}

float2 spiralDirection(float i, float noise)
{
    const float goldenAngle = 2.39996323;
    float angle = i * goldenAngle + noise * 6.2831853;
    return float2(cos(angle), sin(angle));
}

[numthreads(16, 16, 1)]
void deferredAOMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution)) return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5) / float2(gResolution);
    float depth = sampleDepth(uv);
    if (depth >= 0.9999)
    {
        gDst[coord] = float4(1.0, 1.0, 0.0, 0.0);
        return;
    }

    float3 originW = reconstructWorld(uv, depth);
    float3 originV = FilamentComputeViewPosition(uv, depth, gInvProj);
    float viewDepth = max(abs(originV.z), 1e-4);
    float3 normalW = decodeNormal(gNormalW.SampleLevel(gPointSampler, uv, 0));

    float projectionScale = min(0.5 * (2.0 / max(gPositionParams.x, 1e-5)) * float(gResolution.x),
                                0.5 * (2.0 / max(gPositionParams.y, 1e-5)) * float(gResolution.y));
    float screenRadius = max(1.0, projectionScale * gRadius / viewDepth);
    float noise = FilamentInterleavedGradientNoise(float2(coord));

    float occlusion = 0.0;
    uint sampleCount = clamp(gSampleCount, 1u, 64u);
    for (uint i = 0; i < sampleCount; ++i)
    {
        float fi = float(i);
        float radial = sqrt((fi + 0.5 + noise) / float(sampleCount));
        float2 offset = spiralDirection(fi, noise) * radial * screenRadius;
        float2 sampleUV = uv + offset / float2(gResolution);
        float sampleZ = sampleDepth(sampleUV);
        if (sampleZ >= 0.9999) continue;

        float3 sampleW = reconstructWorld(sampleUV, sampleZ);
        float3 delta = sampleW - originW;
        float dist2 = dot(delta, delta);
        if (dist2 <= 1e-8 || dist2 > gRadius * gRadius) continue;

        float invDist = rsqrt(dist2);
        float nd = dot(normalW, delta * invDist);
        float range = saturate(1.0 - sqrt(dist2) / gRadius);
        range *= range;
        occlusion += max(0.0, nd - gBias) * range;
    }

    occlusion = (occlusion / float(sampleCount)) * gIntensity;
    float visibility = pow(saturate(1.0 - occlusion), gPower);
    float packedDepth = saturate(viewDepth / max(gFarPlane, 1.0));
    gDst[coord] = float4(visibility, packedDepth, 0.0, 0.0);
}

[numthreads(16, 16, 1)]
void deferredAOBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gBlurResolution)) return;

    int2 coord = int2(dispatchThreadId.xy);
    float4 center = gAOInput.Load(int3(coord, 0));
    float depth = center.g;
    float total = 1.0;
    float ao = center.r;

    int radius = int(min(gBlurRadius, 8u));
    int2 axis = int2(gBlurAxis.x > 0.5 ? 1 : 0, gBlurAxis.y > 0.5 ? 1 : 0);
    for (int i = 1; i <= radius; ++i)
    {
        float spatial = exp(-float(i * i) / max(float(radius * radius), 1.0));
        for (int side = -1; side <= 1; side += 2)
        {
            int2 p = clamp(coord + axis * i * side, int2(0), int2(gBlurResolution) - 1);
            float4 s = gAOInput.Load(int3(p, 0));
            float w = spatial * exp(-abs(s.g - depth) * gBlurSharpness);
            ao += s.r * w;
            total += w;
        }
    }

    gBlurredAO[coord] = float4(ao / max(total, 1e-5), depth, center.ba);
}
