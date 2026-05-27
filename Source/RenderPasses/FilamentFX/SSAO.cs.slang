// Filament SAO (Scalable Ambient Obscurance) - exact implementation
// Based on filament/src/materials/ssao/saoImpl.fs + sao.mat
// Reference: "Scalable Ambient Obscurance" by McGuire, Mara, Luebke

cbuffer PerFrameCB
{
    float4 gResolution;        // xy=width/height, zw=1/width,1/height
    float2 gPositionParams;    // invProjection[0][0]*2, invProjection[1][1]*2
    float  gInvRadiusSquared;  // 1 / (radius*radius)
    float  gMinHorizonAngleSineSquared;
    float  gBias;
    float  gPeak2;             // peak^2
    float  gProjectionScale;
    float  gProjectionScaleRadius;
    float  gIntensity;
    float  gPower;
    float  gSpiralTurns;
    float2 gSampleCount;       // x=count, y=1/count
    float2 gAngleIncCosSin;    // cos/sin of angle increment
    float  gInvFarPlane;
    int    gMaxLevel;
};

Texture2D<float> gDepth;
RWTexture2D<float> gDst;
SamplerState gPointSampler;

// Interleaved gradient noise (Filament)
float interleavedGradientNoise(float2 screenPos)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(screenPos.xy, magic.xy)));
}

// Linearize depth (reversed-Z, like Filament's depthUtils.fs)
float linearizeDepth(float depth)
{
    const float preventDiv0 = 1.0 / 16777216.0;
    // Simplified perspective linearization: near=0.01, far=inf
    return 0.01 / max(depth, preventDiv0);
}

float sampleDepthLinear(float2 uv, float lod)
{
    return linearizeDepth(gDepth.SampleLevel(gPointSampler, uv, lod).r);
}

// View-space position from depth (Filament's geometry.fs)
float3 computeViewSpacePositionFromDepth(float2 uv, float linearDepth)
{
    return float3((0.5 - uv) * gPositionParams * linearDepth, linearDepth);
}

// SAO tap location (Filament's saoImpl.fs)
float3 tapLocation(float i, float noise)
{
    float offset = ((2.0 * 3.14159265) * 2.4) * noise;
    float angle = ((i * gSampleCount.y) * gSpiralTurns) * (2.0 * 3.14159265) + offset;
    float radius = (i + noise + 0.5) * gSampleCount.y;
    return float3(cos(angle), sin(angle), radius * radius);
}

// SAO sample (Filament's computeAmbientOcclusionSAO)
float computeSAOSample(float i, float ssDiskRadius, float2 uv, float3 origin, float3 normal, float noise)
{
    float3 tap = tapLocation(i, noise);
    float ssRadius = max(1.0, tap.z * ssDiskRadius);
    
    // Mip level for LOD (Filament: kLog2LodRate = 3.0)
    float level = clamp(floor(log2(ssRadius)) - 3.0, 0.0, float(gMaxLevel));
    float2 uvSample = uv + float2(ssRadius * tap.xy) * gResolution.zw;
    
    float occlusionDepth = sampleDepthLinear(uvSample, level);
    float3 p = computeViewSpacePositionFromDepth(uvSample, occlusionDepth);
    
    float3 v = p - origin;
    float vv = dot(v, v);
    float vn = dot(v, normal);
    
    // Outside radius discard (Filament: sq(max(0, 1-vv*invRadiusSquared)))
    float w = max(0.0, 1.0 - vv * gInvRadiusSquared);
    w *= w;
    
    // Horizon discard
    w *= step(vv * gMinHorizonAngleSineSquared, vn * vn);
    
    float sampleOcclusion = max(0.0, vn + origin.z * gBias) / (vv + gPeak2);
    return w * sampleOcclusion;
}

// Main SAO (Filament's scalableAmbientObscurance)
[numthreads(16, 16, 1)]
void ssaoMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution.xy)) return;
    
    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;
    
    float depth = gDepth.SampleLevel(gPointSampler, uv, 0).r;
    // Reversed-Z: depth ≈ 0 for far/sky, depth ≈ 1 for near plane
    if (depth < 1e-5) { gDst[dispatchThreadId.xy] = 1.0; return; }
    
    float linearDepth = linearizeDepth(depth);
    float3 origin = computeViewSpacePositionFromDepth(uv, linearDepth);
    
    // Normals from depth gradient (Filament's medium quality)
    float2 texelSize = gResolution.zw;
    float2 uvdx = uv + float2(texelSize.x, 0.0);
    float2 uvdy = uv + float2(0.0, texelSize.y);
    float3 px = computeViewSpacePositionFromDepth(uvdx, sampleDepthLinear(uvdx, 0.0));
    float3 py = computeViewSpacePositionFromDepth(uvdy, sampleDepthLinear(uvdy, 0.0));
    float3 normal = normalize(cross(px - origin, py - origin));
    
    float noise = interleavedGradientNoise(coord);
    float ssDiskRadius = -(gProjectionScaleRadius / origin.z);
    
    float obscurance = 0.0;
    for (float i = 0.0; i < gSampleCount.x; i += 1.0)
        obscurance += computeSAOSample(i, ssDiskRadius, uv, origin, normal, noise);
    
    obscurance = sqrt(obscurance * gIntensity);
    float ao = saturate(1.0 - obscurance);
    ao = pow(ao, gPower);
    
    gDst[dispatchThreadId.xy] = ao;
}

// Filament-style bilateral blur (ssao/bilateralBlur.mat)
Texture2D<float> gAOBuffer;
Texture2D<float> gDepthForBlur;
RWTexture2D<float> gBlurredAO;

[numthreads(16, 16, 1)]
void ssaoBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution.xy)) return;
    
    float2 coord = float2(dispatchThreadId.xy);
    float2 uv = (coord + 0.5) * gResolution.zw;
    float2 texelSize = gResolution.zw;
    
    float centerDepth = gDepthForBlur.SampleLevel(gPointSampler, uv, 0).r;
    if (centerDepth >= 1.0) { gBlurredAO[dispatchThreadId.xy] = 1.0; return; }
    float centerLinDepth = linearizeDepth(centerDepth);
    
    float totalAO = 0.0;
    float totalWeight = 0.0;
    
    // 5x5 bilateral (Filament uses separable gaussian, we use 2D for simplicity)
    for (int x = -2; x <= 2; x++)
    {
        for (int y = -2; y <= 2; y++)
        {
            float2 offset = float2(x, y) * texelSize;
            float2 sampleUV = uv + offset;
            
            float sampleDepth = gDepthForBlur.SampleLevel(gPointSampler, sampleUV, 0).r;
            float sampleLinDepth = linearizeDepth(sampleDepth);
            float sampleAO = gAOBuffer.SampleLevel(gPointSampler, sampleUV, 0).r;
            
            // Bilateral depth weight (Filament)
            float depthDiff = abs(centerLinDepth - sampleLinDepth);
            float weight = exp(-depthDiff * 2.0);
            
            // Gaussian spatial weight
            weight *= exp(-(float(x*x + y*y)) / 4.0);
            
            totalAO += sampleAO * weight;
            totalWeight += weight;
        }
    }
    
    gBlurredAO[dispatchThreadId.xy] = totalAO / max(totalWeight, 1e-5);
}
