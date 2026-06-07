// Filament SAO (Scalable Ambient Obscurance) - strict alignment with saoImpl.fs + sao.mat
// Falcor uses D3D depth in [0,1]: 0=near, 1=far/sky (clear depth to 1)

cbuffer PerFrameCB
{
    float4 gResolution;        // xy=width/height, zw=1/width,1/height
    float2 gPositionParams;    // invProjection[0][0]*2, invProjection[1][1]*2
    float  gInvRadiusSquared;
    float  gMinHorizonAngleSineSquared;
    float  gBias;
    float  gPeak2;
    float  gProjectionScaleRadius;
    float  gIntensity;
    float  gPower;
    float  gSpiralTurns;
    float2 gSampleCount;       // x=count, y=1/count
    float2 gAngleIncCosSin;
    float  gInvFarPlane;
    int    gMaxLevel;
    float2 gBlurAxis;          // separable blur direction in UV space
    float  gFarPlaneOverEdgeDistance;
    int    gBlurSampleCount;
    float  gBlurKernel[16];
};

cbuffer CameraCB
{
    float4x4 gInvProj;         // clip -> view (inverse projection)
    float    gNearPlane;
    float    gFarPlane;
};

Texture2D<float> gDepth;
Texture2D<float> gStructureDepth;
Texture2D<float2> gAOBuffer;
RWTexture2D<float2> gDst;
RWTexture2D<float2> gBlurredAO;
SamplerState gPointSampler;

float interleavedGradientNoise(float2 screenPos)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(screenPos.xy, magic.xy)));
}

// Filament depthUtils.fs linearizeDepth (viewFromClip = inverse(projection))
float linearizeDepth(float depth)
{
    const float preventDiv0 = 1.0 / 16777216.0;
    float4 p = gInvProj[2];
    float4 q = gInvProj[3];
    return (depth * p.z + q.z) / max(depth * p.w + q.w, preventDiv0);
}

// D3D [0,1]: cleared background = 1 (far/sky)
bool isSkyDepth(float depth)
{
    return depth >= 1.0;
}

float sampleDepthLinear(float2 uv, float lod)
{
    return linearizeDepth(gStructureDepth.SampleLevel(gPointSampler, uv, lod).r);
}

float3 computeViewPosition(float2 uv, float depth)
{
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 view = mul(clip, gInvProj);
    return view.xyz / view.w;
}

float3 sampleViewPosition(float2 uv, float lod)
{
    float depth = gStructureDepth.SampleLevel(gPointSampler, uv, lod).r;
    return computeViewPosition(uv, depth);
}

// ssaoUtils.fs
float2 packDepth(float normalizedDepth)
{
    float z = clamp(normalizedDepth, 0.0, 1.0);
    float t = floor(256.0 * z);
    float hi = t * (1.0 / 256.0);
    float lo = (256.0 * z) - t;
    return float2(hi, lo);
}

float unpackDepth(float2 depth)
{
    return depth.x * (256.0 / 257.0) + depth.y * (1.0 / 257.0);
}

// saoImpl.fs - fast spiral path
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
    float2 uv, float3 origin, float3 normal, float2 tapPosition, float noise)
{
    float3 tap = tapLocationFast(i, tapPosition, noise);
    float ssRadius = max(1.0, tap.z * ssDiskRadius);

    const float kLog2LodRate = 3.0;
    float level = clamp(floor(log2(ssRadius)) - kLog2LodRate, 0.0, float(gMaxLevel));
    float2 uvSample = uv + float2(ssRadius * tap.xy) * gResolution.zw;

    float3 p = sampleViewPosition(uvSample, level);

    float3 v = p - origin;
    float vv = dot(v, v);
    float vn = dot(v, normal);

    float w = max(0.0, 1.0 - vv * gInvRadiusSquared);
    w *= w;
    w *= step(vv * gMinHorizonAngleSineSquared, vn * vn);

    float sampleOcclusion = max(0.0, vn + (origin.z * gBias)) / (vv + gPeak2);
    occlusion += w * sampleOcclusion;
}

// geometry.fs medium quality normals
float3 computeViewSpaceNormalMediumQ(float2 uv, float3 position, float2 texel)
{
    float2 uvdx = uv + float2(texel.x, 0.0);
    float2 uvdy = uv + float2(0.0, texel.y);
    float3 px = sampleViewPosition(uvdx, 0.0);
    float3 py = sampleViewPosition(uvdy, 0.0);
    float3 dpdx = px - position;
    float3 dpdy = py - position;
    return normalize(cross(dpdx, dpdy));
}

[numthreads(16, 16, 1)]
void ssaoMain(uint3 dispatchThreadId : SV_DispatchThreadID)
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

    float3 origin = computeViewPosition(uv, depth);
    float3 normal = computeViewSpaceNormalMediumQ(uv, origin, gResolution.zw);

    float noise = interleavedGradientNoise(coord);
    float2 tapPosition = startPosition(noise);
    float2x2 angleStep = tapAngleStep();

    // Falcor/D3D projection paths can produce either signed view-space Z convention.
    // SAO only needs a positive screen-space disk radius.
    float ssDiskRadius = gProjectionScaleRadius / max(abs(origin.z), 1e-4);

    float obscurance = 0.0;
    for (float i = 0.0; i < gSampleCount.x; i += 1.0)
    {
        computeAmbientOcclusionSAO(obscurance, i, ssDiskRadius, uv, origin, normal, tapPosition, noise);
        tapPosition = mul(angleStep, tapPosition);
    }

    obscurance = sqrt(obscurance * gIntensity);
    float aoVisibility = pow(saturate(1.0 - obscurance), gPower);

    // Normalized view-space Z for bilateral depth rejection (Filament packs to gb; we store linear)
    float packedZ = saturate(abs(origin.z) * gInvFarPlane);
    gDst[dispatchThreadId.xy] = float2(aoVisibility, packedZ);
}

// bilateralBlur.mat - separable bilateral using packed depth in AO.g
float bilateralWeight(float depth, float sampleDepth)
{
    float diff = (sampleDepth - depth) * gFarPlaneOverEdgeDistance;
    return max(0.0, 1.0 - diff * diff);
}

void bilateralTap(inout float sum, inout float totalWeight, float weight, float depth, float2 uv)
{
    float2 data = gAOBuffer.SampleLevel(gPointSampler, uv, 0).rg;
    float b = weight * bilateralWeight(depth, data.g);
    sum += data.r * b;
    totalWeight += b;
}

[numthreads(16, 16, 1)]
void ssaoBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution.xy)) return;

    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;

    float2 center = gAOBuffer.SampleLevel(gPointSampler, uv, 0).rg;
    if (center.g >= 0.9999 && center.r >= 0.9999)
    {
        gBlurredAO[dispatchThreadId.xy] = center;
        return;
    }

    float depth = center.g;
    float totalWeight = gBlurKernel[0];
    float sum = center.r * totalWeight;

    float2 offset = gBlurAxis;
    for (int i = 1; i < gBlurSampleCount; i++)
    {
        float weight = gBlurKernel[i];
        bilateralTap(sum, totalWeight, weight, depth, uv + offset);
        bilateralTap(sum, totalWeight, weight, depth, uv - offset);
        offset += gBlurAxis;
    }

    float ao = sum / max(totalWeight, 1e-5);
    ao += (interleavedGradientNoise(coord) - 0.5) / 255.0;
    gBlurredAO[dispatchThreadId.xy] = float2(saturate(ao), depth);
}
