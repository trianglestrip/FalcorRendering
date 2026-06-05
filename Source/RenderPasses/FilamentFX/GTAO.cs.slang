// Filament GTAO ported from filament/src/materials/ssao/gtao.mat + gtaoImpl.fs.
// Bent normals, SSCT and visibility-bitmask mode are intentionally omitted in this Falcor pass.

cbuffer PerFrameCB
{
    float4 gResolution;              // xy=width/height, zw=1/width,1/height
    float2 gPositionParams;          // inverse projection diagonal * 2
    float  gInvFarPlane;
    int    gMaxLevel;

    float  gProjectionScaleRadius;
    float  gIntensity;
    float2 gSliceCount;              // x=count, y=1/count
    float  gStepsPerSlice;

    float  gRadius;
    float  gInvRadiusSquared;
    float  gPower;
    float  gThicknessHeuristic;
};

cbuffer CameraCB
{
    float4x4 gInvProj;
};

Texture2D<float> gStructureDepth;
RWTexture2D<float2> gDst;
SamplerState gPointSampler;

static const float PI = 3.14159265358979323846;
static const float HALF_PI = 1.57079632679489661923;
static const float kLog2LodRate = 3.0;

float acosFast(float x)
{
    return acos(clamp(x, -1.0, 1.0));
}

float linearizeDepth(float depth)
{
    const float preventDiv0 = 1.0 / 16777216.0;
    float4 p = gInvProj[2];
    float4 q = gInvProj[3];
    return (depth * p.z + q.z) / max(depth * p.w + q.w, preventDiv0);
}

bool isSkyDepth(float depth)
{
    return depth > 0.9999;
}

float sampleDepthLinear(float2 uv, float lod)
{
    return linearizeDepth(gStructureDepth.SampleLevel(gPointSampler, uv, lod).r);
}

float3 computeViewSpacePositionFromDepth(float2 uv, float linearDepth)
{
    return float3((0.5 - uv) * gPositionParams * linearDepth, linearDepth);
}

float3 getViewSpacePosition(float2 uv, float level)
{
    return computeViewSpacePositionFromDepth(uv, sampleDepthLinear(uv, level));
}

float3 computeViewSpaceNormal(float2 uv, float depth, float3 position)
{
    float2 texel = gResolution.zw;
    float2 uvx = uv + float2(texel.x, 0.0);
    float2 uvy = uv + float2(0.0, texel.y);
    float3 px = computeViewSpacePositionFromDepth(uvx, sampleDepthLinear(uvx, 0.0));
    float3 py = computeViewSpacePositionFromDepth(uvy, sampleDepthLinear(uvy, 0.0));
    return normalize(cross(px - position, py - position));
}

float integrateArcCosWeight(float h, float n)
{
    float arc = -cos(2.0 * h - n) + cos(n) + 2.0 * h * sin(n);
    return 0.25 * arc;
}

float spatialDirectionNoise(float2 uv)
{
    int2 position = int2(uv * gResolution.xy);
    return (1.0 / 16.0) * (float(((position.x + position.y) & 3) << 2) + float(position.x & 3));
}

float spatialOffsetsNoise(float2 uv)
{
    int2 position = int2(uv * gResolution.xy);
    return 0.25 * float((position.y - position.x) & 3);
}

float updateHorizon(float sampleHorizonCos, float currentHorizonCos, float fallOff)
{
    return sampleHorizonCos > currentHorizonCos
        ? lerp(sampleHorizonCos, currentHorizonCos, fallOff)
        : lerp(currentHorizonCos, sampleHorizonCos, gThicknessHeuristic);
}

float calculateHorizonCos(float3 sampleDelta, float3 viewDir, float horizonCos)
{
    float sqSampleDist = dot(sampleDelta, sampleDelta);
    float invSampleDist = rsqrt(max(sqSampleDist, 1e-8));
    float fallOff = saturate(sqSampleDist * gInvRadiusSquared * 2.0);
    float sampleHorizonCos = dot(sampleDelta, viewDir) * invSampleDist;
    return updateHorizon(sampleHorizonCos, horizonCos, fallOff);
}

float groundTruthAmbientOcclusion(float2 uv, float3 origin, float3 normal)
{
    float3 viewDir = normalize(-origin);
    float ssRadius = -(gProjectionScaleRadius / origin.z);

    float noiseOffset = spatialOffsetsNoise(uv);
    float noiseDirection = spatialDirectionNoise(uv);
    float initialRayStep = frac(noiseOffset);
    float stepRadius = ssRadius / (gStepsPerSlice + 1.0);

    float visibility = 0.0;
    for (float i = 0.0; i < gSliceCount.x; i += 1.0)
    {
        float slice = (i + noiseDirection) * gSliceCount.y;
        float phi = slice * PI;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);
        float2 omega = float2(cosPhi, sinPhi);

        float3 direction = float3(cosPhi, sinPhi, 0.0);
        float3 orthoDirection = normalize(direction - dot(direction, viewDir) * viewDir);
        float3 axis = cross(orthoDirection, viewDir);
        float3 projNormal = normal - axis * dot(normal, axis);

        float signNorm = sign(dot(orthoDirection, projNormal));
        float projNormalLength = length(projNormal);
        float cosNorm = saturate(dot(projNormal, viewDir) / max(projNormalLength, 1e-5));
        float n = signNorm * acosFast(cosNorm);

        float horizonCos0 = -1.0;
        float horizonCos1 = -1.0;

        for (float j = 0.0; j < gStepsPerSlice; j += 1.0)
        {
            float2 sampleOffset = max((j + initialRayStep) * stepRadius, 1.0 + j) * omega;
            float sampleOffsetLength = length(sampleOffset);
            float level = clamp(floor(log2(max(sampleOffsetLength, 1e-4))) - kLog2LodRate, 0.0, float(gMaxLevel));

            float2 uvSampleOffset = sampleOffset * gResolution.zw;
            float3 samplePos0 = getViewSpacePosition(uv + uvSampleOffset, level);
            float3 samplePos1 = getViewSpacePosition(uv - uvSampleOffset, level);

            horizonCos0 = calculateHorizonCos(samplePos0 - origin, viewDir, horizonCos0);
            horizonCos1 = calculateHorizonCos(samplePos1 - origin, viewDir, horizonCos1);
        }

        float h0 = -acosFast(horizonCos1);
        float h1 = acosFast(horizonCos0);
        h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);
        h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);

        visibility += projNormalLength * (integrateArcCosWeight(h0, n) + integrateArcCosWeight(h1, n));
    }

    return 1.0 - saturate(visibility * gSliceCount.y);
}

[numthreads(16, 16, 1)]
void gtaoMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= uint2(gResolution.xy))) return;

    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;

    float depth = gStructureDepth.SampleLevel(gPointSampler, uv, 0).r;
    if (isSkyDepth(depth))
    {
        gDst[dispatchThreadId.xy] = float2(1.0, 1.0);
        return;
    }

    float z = linearizeDepth(depth);
    float3 origin = computeViewSpacePositionFromDepth(uv, z);
    float3 normal = computeViewSpaceNormal(uv, depth, origin);

    float occlusion = gIntensity > 0.0 ? groundTruthAmbientOcclusion(uv, origin, normal) : 0.0;
    float aoVisibility = pow(saturate(1.0 - occlusion), gPower);
    aoVisibility = lerp(1.0, aoVisibility, gIntensity);

    float packedZ = saturate(origin.z * gInvFarPlane);
    gDst[dispatchThreadId.xy] = float2(saturate(aoVisibility), packedZ);
}
