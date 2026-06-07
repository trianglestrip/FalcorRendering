// Deferred SAO for MRT preview paths.
// Keeps Filament's SAO sampling and bilateral blur path, but reads normals from GBuffer.

cbuffer PerFrameCB
{
    float4 gResolution;        // xy=width/height, zw=1/width,1/height
    float2 gPositionParams;
    float  gInvRadiusSquared;
    float  gMinHorizonAngleSineSquared;
    float  gBias;
    float  gPeak2;
    float  gProjectionScaleRadius;
    float  gIntensity;
    float  gPower;
    float  gSpiralTurns;
    float2 gSampleCount;
    float2 gAngleIncCosSin;
    float  gInvFarPlane;
    int    gMaxLevel;
};

cbuffer CameraCB
{
    float4x4 gInvProj;
    float4x4 gInvView;
};

Texture2D<float> gStructureDepth;
Texture2D<float4> gNormalW;
RWTexture2D<float2> gDst;
SamplerState gPointSampler;

float interleavedGradientNoise(float2 screenPos)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(screenPos.xy, magic.xy)));
}

bool isSkyDepth(float depth)
{
    return depth >= 1.0;
}

float3 computeViewPosition(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 view = mul(clip, gInvProj);
    return view.xyz / view.w;
}

float3 computeWorldPosition(float2 uv, float depth)
{
    float3 viewPosition = computeViewPosition(uv, depth);
    float4 world = mul(float4(viewPosition, 1.0), gInvView);
    return world.xyz / world.w;
}

float3 sampleViewPosition(float2 uv, float lod)
{
    float depth = gStructureDepth.SampleLevel(gPointSampler, uv, lod).r;
    return computeViewPosition(uv, depth);
}

float3 sampleWorldPosition(float2 uv, float lod)
{
    float depth = gStructureDepth.SampleLevel(gPointSampler, uv, lod).r;
    return computeWorldPosition(uv, depth);
}

float3 sampleWorldNormal(float2 uv)
{
    float3 normalW = gNormalW.SampleLevel(gPointSampler, uv, 0).xyz * 2.0 - 1.0;
    float len2 = dot(normalW, normalW);
    return (len2 > 1e-8) ? normalW * rsqrt(len2) : float3(0.0, 1.0, 0.0);
}

float2 startPosition(float noise)
{
    float angle = ((2.0 * 3.14159265) * 2.4) * noise;
    return float2(cos(angle), sin(angle));
}

float2x2 tapAngleStep()
{
    float2 t = gAngleIncCosSin;
    return float2x2(t.x, t.y, -t.y, t.x);
}

float3 tapLocationFast(float i, float2 p, float noise)
{
    float radius = (i + noise + 0.5) * gSampleCount.y;
    return float3(p, radius * radius);
}

void computeAmbientOcclusionSAO(inout float occlusion, float i, float ssDiskRadius,
    float2 uv, float3 originW, float3 normalW, float viewDepth, float2 tapPosition, float noise)
{
    float3 tap = tapLocationFast(i, tapPosition, noise);
    float ssRadius = max(1.0, tap.z * ssDiskRadius);

    const float kLog2LodRate = 3.0;
    float level = clamp(floor(log2(ssRadius)) - kLog2LodRate, 0.0, float(gMaxLevel));
    float2 uvSample = uv + float2(ssRadius * tap.xy) * gResolution.zw;

    float3 p = sampleWorldPosition(uvSample, level);
    float3 v = p - originW;
    float vv = dot(v, v);
    float vn = dot(v, normalW);

    float w = max(0.0, 1.0 - vv * gInvRadiusSquared);
    w *= w;
    w *= step(vv * gMinHorizonAngleSineSquared, vn * vn);

    float sampleOcclusion = max(0.0, vn - (viewDepth * gBias)) / (vv + gPeak2);
    occlusion += w * sampleOcclusion;
}

[numthreads(16, 16, 1)]
void deferredSSAO(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution.xy)) return;

    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;

    float depth = gStructureDepth.SampleLevel(gPointSampler, uv, 0).r;
    if (isSkyDepth(depth))
    {
        gDst[dispatchThreadId.xy] = float2(1.0, 1.0);
        return;
    }

    float3 originW = computeWorldPosition(uv, depth);
    float3 normalW = sampleWorldNormal(uv);
    float viewDepth = abs(computeViewPosition(uv, depth).z);

    float noise = interleavedGradientNoise(coord);
    float2 tapPosition = startPosition(noise);
    float2x2 angleStep = tapAngleStep();
    float ssDiskRadius = gProjectionScaleRadius / max(viewDepth, 1e-4);

    float obscurance = 0.0;
    for (float i = 0.0; i < gSampleCount.x; i += 1.0)
    {
        computeAmbientOcclusionSAO(obscurance, i, ssDiskRadius, uv, originW, normalW, viewDepth, tapPosition, noise);
        tapPosition = mul(angleStep, tapPosition);
    }

    obscurance = sqrt(obscurance * gIntensity);
    float aoVisibility = pow(saturate(1.0 - obscurance), gPower);
    float packedZ = saturate(viewDepth * gInvFarPlane);
    gDst[dispatchThreadId.xy] = float2(aoVisibility, packedZ);
}
