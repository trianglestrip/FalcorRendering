// Exponential distance + height fog on HDR color (post-DoF, pre-tone-map)

cbuffer PerFrameCB
{
    uint2  gResolution;
    float  gFogEnabled;
    float  gFogDensity;
    float  gFogStart;
    float3 gFogColor;
};

cbuffer CameraCB
{
    float4x4 gInvProj;
    float4x4 gInvViewProj;
    float    gNearPlane;
    float    gFarPlane;
    float3   gCameraPos;
};

Texture2D<float4> gSrc;
Texture2D<float>  gDepth;
RWTexture2D<float4> gDst;
SamplerState gLinearSampler;

float linearizeDepth(float depth)
{
    const float preventDiv0 = 1.0 / 16777216.0;
    float4 p = gInvProj[2];
    float4 q = gInvProj[3];
    return (depth * p.z + q.z) / max(depth * p.w + q.w, preventDiv0);
}

float3 computeViewPosition(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 view = mul(gInvProj, clip);
    return view.xyz / view.w;
}

float3 computeWorldPosition(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 world = mul(gInvViewProj, clip);
    return world.xyz / world.w;
}

[numthreads(16, 16, 1)]
void fogMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution)) return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5) / float2(gResolution);

    float4 color = gSrc.Load(int3(coord, 0));
    if (gFogEnabled < 0.5)
    {
        gDst[coord] = color;
        return;
    }

    float depth = gDepth.SampleLevel(gLinearSampler, uv, 0).r;
    if (depth > 0.9999)
    {
        gDst[coord] = color;
        return;
    }

    float3 viewPos = computeViewPosition(uv, depth);
    float distance = length(viewPos);

    // Height falloff: denser fog at lower world-space altitude (minimal exponential height term)
    float3 worldPos = computeWorldPosition(uv, depth);
    float heightFactor = exp(-max(worldPos.y - gCameraPos.y, 0.0) * gFogDensity * 0.25);

    float opticalPath = gFogDensity * max(distance - gFogStart, 0.0) * heightFactor;
    float transmittance = exp(-opticalPath);
    float fogAmount = 1.0 - transmittance;

    color.rgb = lerp(color.rgb, gFogColor, fogAmount);
    gDst[coord] = color;
}
