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

cbuffer CameraCB
{
    float4x4 gInvViewProj;
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

// PCF Shadow Sampling - Hard (1 sample), D3D depth [0,1] increasing with distance in light space
float shadowSample_PCF_Hard(float2 shadowUV, float referenceDepth)
{
    float depth = gShadowMap.SampleLevel(gPointSampler, shadowUV, 0).r;
    return (referenceDepth <= depth + 1e-5) ? 1.0 : 0.0;
}

// PCF Shadow Sampling - Low (3x3 bilinear with gaussian weights, Castaño 2013)
float shadowSample_PCF_Low(float2 shadowUV, float referenceDepth)
{
    float2 texelSize = 1.0 / float2(gShadowAtlasSize);
    float2 uv = shadowUV;
    float sum = 0.0;
    sum += 4.0 * shadowSample_PCF_Hard(uv, referenceDepth);
    sum += 2.0 * shadowSample_PCF_Hard(uv + float2(0, -1) * texelSize, referenceDepth);
    sum += 2.0 * shadowSample_PCF_Hard(uv + float2(0, 1) * texelSize, referenceDepth);
    sum += 2.0 * shadowSample_PCF_Hard(uv + float2(-1, 0) * texelSize, referenceDepth);
    sum += 2.0 * shadowSample_PCF_Hard(uv + float2(1, 0) * texelSize, referenceDepth);
    sum += 1.0 * shadowSample_PCF_Hard(uv + float2(-1, -1) * texelSize, referenceDepth);
    sum += 1.0 * shadowSample_PCF_Hard(uv + float2(1, -1) * texelSize, referenceDepth);
    sum += 1.0 * shadowSample_PCF_Hard(uv + float2(-1, 1) * texelSize, referenceDepth);
    sum += 1.0 * shadowSample_PCF_Hard(uv + float2(1, 1) * texelSize, referenceDepth);
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

[numthreads(16, 16, 1)]
void shadowMapMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution))
        return;
    
    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5f) / float2(gResolution);
    
    float depth = gDepth.SampleLevel(gPointSampler, uv, 0).r;
    // D3D depth [0,1]: background cleared to 1 (far/sky)
    if (depth > 0.9999)
    {
        gDst[coord] = 1.0;
        return;
    }

    // Reconstruct world position from depth
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 world = mul(clip, gInvViewProj);
    world.xyz /= world.w;
    
    float viewSpaceDepth = length(world.xyz);
    int cascade = selectCascade(viewSpaceDepth);
    
    float4 lightSpacePos = mul(float4(world.xyz, 1.0), gLightViewProj[cascade]);
    lightSpacePos.xyz /= lightSpacePos.w;
    
    float2 shadowUV = lightSpacePos.xy * 0.5 + 0.5;
    float referenceDepth = lightSpacePos.z - gShadowBias;
    
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0)
    {
        gDst[coord] = 1.0;
        return;
    }
    
    float visibility;
    if (gShadowType == 2)
        visibility = shadowSample_VSM(float3(shadowUV, referenceDepth), referenceDepth);
    else if (gShadowType == 1)
        visibility = shadowSample_PCF_Low(shadowUV, referenceDepth);
    else
        visibility = shadowSample_PCF_Hard(shadowUV, referenceDepth);
    
    gDst[coord] = visibility;
}
