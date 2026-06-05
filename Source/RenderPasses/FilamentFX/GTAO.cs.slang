// Simplified GTAO (Ground Truth AO) - horizon-based alternative to SAO (Task-L)

cbuffer PerFrameCB
{
    float4 gResolution;
    float gIntensity;
    float gPower;
    float gInvFarPlane;
    float2 gPositionParams;
    float gGtaoRadius;
    int gGtaoSlices;
    int gGtaoSteps;
};

cbuffer CameraCB
{
    float4x4 gInvProj;
};

Texture2D<float> gStructureDepth;
RWTexture2D<float2> gDst;
SamplerState gPointSampler;

float interleavedGradientNoise(float2 screenPos)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(screenPos.xy, magic.xy)));
}

float linearizeDepth(float depth)
{
    const float preventDiv0 = 1.0 / 16777216.0;
    float4 p = gInvProj[2];
    float4 q = gInvProj[3];
    return (depth * p.z + q.z) / max(depth * p.w + q.w, preventDiv0);
}

bool isSkyDepth(float depth) { return depth > 0.9999; }

float sampleDepthLinear(float2 uv)
{
    return linearizeDepth(gStructureDepth.SampleLevel(gPointSampler, uv, 0).r);
}

float3 computeViewSpacePositionFromDepth(float2 uv, float linearDepth)
{
    return float3((0.5 - uv) * gPositionParams * linearDepth, linearDepth);
}

float3 computeViewSpaceNormalMediumQ(float2 uv, float3 position, float2 texel)
{
    float2 uvdx = uv + float2(texel.x, 0.0);
    float2 uvdy = uv + float2(0.0, texel.y);
    float3 px = computeViewSpacePositionFromDepth(uvdx, sampleDepthLinear(uvdx));
    float3 py = computeViewSpacePositionFromDepth(uvdy, sampleDepthLinear(uvdy));
    return normalize(cross(px - position, py - position));
}

[numthreads(16, 16, 1)]
void gtaoMain(uint3 dispatchThreadId : SV_DispatchThreadID)
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

    float linearDepth = linearizeDepth(depth);
    float3 origin = computeViewSpacePositionFromDepth(uv, linearDepth);
    float3 normal = computeViewSpaceNormalMediumQ(uv, origin, gResolution.zw);

    float noise = interleavedGradientNoise(coord);
    float angle = noise * 6.2831853;
    float occlusion = 0.0;
    const int slices = max(gGtaoSlices, 1);
    const int steps = max(gGtaoSteps, 1);

    for (int s = 0; s < slices; s++)
    {
        float sliceAngle = angle + (6.2831853 * float(s)) / float(slices);
        float2 sliceDir = float2(cos(sliceAngle), sin(sliceAngle));
        float horizonCos = -1.0;

        for (int i = 1; i <= steps; i++)
        {
            float t = float(i) / float(steps);
            float2 uvSample = uv + sliceDir * t * gGtaoRadius * gResolution.zw;
            float3 samplePos = computeViewSpacePositionFromDepth(uvSample, sampleDepthLinear(uvSample));
            float3 v = samplePos - origin;
            float vv = dot(v, v);
            if (vv < 1e-6) continue;
            float cosHorizon = dot(normalize(v), normal);
            float falloff = saturate(1.0 - vv / (gGtaoRadius * gGtaoRadius));
            horizonCos = max(horizonCos, cosHorizon * falloff);
        }

        occlusion += saturate(sqrt(1.0 - horizonCos * horizonCos));
    }

    occlusion /= float(slices);
    float aoVisibility = pow(saturate(1.0 - occlusion * gIntensity), gPower);
    float packedZ = saturate(origin.z * gInvFarPlane);
    gDst[dispatchThreadId.xy] = float2(aoVisibility, packedZ);
}
