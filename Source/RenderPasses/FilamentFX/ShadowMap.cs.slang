// Filament-style Shadow Mapping with PCF
// Based on Filament's surface_shadowing.fs

cbuffer PerFrameCB
{
    uint2 gResolution;
    uint gShadowType; // 0: PCF Hard, 1: PCF Low (3x3 gaussian), 2: VSM
    uint gCascadeCount;
    float4 gCascadeSplits;
    float gShadowBias;
    float gShadowSlopeScaleBias;
    uint2 gShadowAtlasSize;
};

Texture2D<float> gDepth;
Texture2D<float> gShadowMap;     // Shadow depth map (or shadow atlas)
Texture2D<float2> gVarianceShadowMap; // VSM (if VSM mode)
RWTexture2D<float> gDst;         // Visibility output (1=lit, 0=shadowed)

SamplerState gPointSampler;
SamplerState gLinearSampler;
SamplerState gShadowSampler;     // Comparison sampler for PCF hardware

cbuffer ShadowCameraCB
{
    float4x4 gLightViewProj[4];  // Light View*Projection matrices for cascades
};

// PCF Shadow Sampling - Hard (1 sample)
float shadowSample_PCF_Hard(float3 shadowUV, float referenceDepth)
{
    float depth = gShadowMap.SampleLevel(gPointSampler, shadowUV.xy, 0).r;
    return (referenceDepth <= depth) ? 1.0 : 0.0;
}

// PCF Shadow Sampling - Low (3x3 bilinear with gaussian weights, Castaño 2013)
float shadowSample_PCF_Low(float3 shadowUV, float referenceDepth)
{
    float2 texelSize = 1.0 / float2(gShadowAtlasSize);
    
    float2 uv = shadowUV.xy;
    
    // 3x3 gaussian PCF
    float sum = 0.0;
    
    // Center (weight 4)
    sum += 4.0 * shadowSample_PCF_Hard(float3(uv, shadowUV.z), referenceDepth);
    
    // Cross (weight 2)
    sum += 2.0 * shadowSample_PCF_Hard(float3(uv + float2(0, -1) * texelSize, shadowUV.z), referenceDepth);
    sum += 2.0 * shadowSample_PCF_Hard(float3(uv + float2(0, 1) * texelSize, shadowUV.z), referenceDepth);
    sum += 2.0 * shadowSample_PCF_Hard(float3(uv + float2(-1, 0) * texelSize, shadowUV.z), referenceDepth);
    sum += 2.0 * shadowSample_PCF_Hard(float3(uv + float2(1, 0) * texelSize, shadowUV.z), referenceDepth);
    
    // Corners (weight 1)
    sum += 1.0 * shadowSample_PCF_Hard(float3(uv + float2(-1, -1) * texelSize, shadowUV.z), referenceDepth);
    sum += 1.0 * shadowSample_PCF_Hard(float3(uv + float2(1, -1) * texelSize, shadowUV.z), referenceDepth);
    sum += 1.0 * shadowSample_PCF_Hard(float3(uv + float2(-1, 1) * texelSize, shadowUV.z), referenceDepth);
    sum += 1.0 * shadowSample_PCF_Hard(float3(uv + float2(1, 1) * texelSize, shadowUV.z), referenceDepth);
    
    return sum / 16.0;
}

// VSM Shadow Sampling
float shadowSample_VSM(float3 shadowUV, float referenceDepth)
{
    float2 moments = gVarianceShadowMap.SampleLevel(gLinearSampler, shadowUV.xy, 0).xy;
    
    // Variance shadow mapping
    float p = (referenceDepth <= moments.x) ? 1.0 : 0.0;
    float variance = max(moments.y - moments.x * moments.x, 0.00001);
    float d = referenceDepth - moments.x;
    float pMax = variance / (variance + d * d);
    
    return max(p, pMax); // Reduce light bleeding
}

// Select cascade based on view-space depth
int selectCascade(float viewSpaceDepth)
{
    for (int i = 0; i < int(gCascadeCount); i++)
    {
        if (viewSpaceDepth < gCascadeSplits[i])
            return i;
    }
    return int(gCascadeCount) - 1;
}

// Reconstruct view-space position from depth
float3 reconstructViewPosFromUV(float2 uv, float linearDepth)
{
    return float3(uv * 2.0 - 1.0, 1.0) * linearDepth;
}


[numthreads(16, 16, 1)]
void shadowMapMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution))
        return;
    
    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5f) / float2(gResolution);
    
    // Read depth
    float depth = gDepth.SampleLevel(gPointSampler, uv, 0).r;
    if (depth <= 0.0 || depth >= 1.0)
    {
        gDst[coord] = 1.0;
        return;
    }
    
    // Reconstruct world/view position (simplified - using depth as view-space z)
    float3 viewPos = reconstructViewPosFromUV(uv, depth);
    float viewSpaceDepth = abs(viewPos.z);
    
    // Select cascade
    int cascade = selectCascade(viewSpaceDepth);
    
    // Transform to light space
    float4 lightSpacePos = mul(float4(viewPos, 1.0), gLightViewProj[cascade]);
    lightSpacePos.xyz /= lightSpacePos.w;
    
    // Transform from [-1,1] to [0,1] texture space
    float3 shadowUV;
    shadowUV.xy = lightSpacePos.xy * 0.5 + 0.5;
    shadowUV.z = lightSpacePos.z;
    
    // Check bounds
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0)
    {
        gDst[coord] = 1.0;
        return;
    }
    
    // Apply bias (slope-scale depth bias)
    float bias = gShadowBias;
    float referenceDepth = shadowUV.z - bias;
    
    // Sample shadow map
    float visibility;
    if (gShadowType == 2) // VSM
    {
        visibility = shadowSample_VSM(shadowUV, referenceDepth);
    }
    else if (gShadowType == 1) // PCF Low
    {
        visibility = shadowSample_PCF_Low(shadowUV, referenceDepth);
    }
    else // PCF Hard
    {
        visibility = shadowSample_PCF_Hard(shadowUV, referenceDepth);
    }
    
    // Blend between cascades for smooth transitions
    float cascadeBlend = 1.0;
    if (cascade > 0)
    {
        float prevSplit = gCascadeSplits[cascade - 1];
        float currentSplit = gCascadeSplits[cascade];
        float blendRange = (currentSplit - prevSplit) * 0.1;
        float t = (viewSpaceDepth - (currentSplit - blendRange)) / blendRange;
        cascadeBlend = saturate(t);
    }
    
    gDst[coord] = visibility;
}
