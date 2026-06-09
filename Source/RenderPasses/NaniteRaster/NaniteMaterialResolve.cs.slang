#include "NaniteShared.slangh"

StructuredBuffer<NaniteGpuCluster> gClusters;
StructuredBuffer<NaniteGpuVertex> gVertices;
StructuredBuffer<uint> gIndices;
StructuredBuffer<uint> gClusterDebugReason;

Texture2D<uint2> gVisibility;
Texture2D<float> gDepth;

RWTexture2D<float4> gPosition;
RWTexture2D<float4> gNormal;
RWTexture2D<float4> gTexCoord;
RWTexture2D<float4> gBaseColor;

cbuffer ResolveCB
{
    NaniteCameraData gCamera;
};

static const float3 kMaterialColors[8] =
{
    float3(0.8, 0.2, 0.2),
    float3(0.2, 0.8, 0.2),
    float3(0.2, 0.2, 0.8),
    float3(0.8, 0.8, 0.2),
    float3(0.8, 0.2, 0.8),
    float3(0.2, 0.8, 0.8),
    float3(0.9, 0.6, 0.3),
    float3(0.6, 0.6, 0.6),
};

float3 clusterDebugColor(uint clusterIndex)
{
    const float hue = float(clusterIndex * 0.6180339887);
    return float3(
        abs(sin(hue * 6.2831853)),
        abs(sin((hue + 0.33) * 6.2831853)),
        abs(sin((hue + 0.66) * 6.2831853)));
}

float3 cullReasonColor(uint reason)
{
    switch (reason)
    {
    case CULL_VISIBLE: return float3(0.1, 0.85, 0.2);
    case CULL_FRUSTUM: return float3(0.9, 0.15, 0.15);
    case CULL_BACKFACE: return float3(0.95, 0.55, 0.1);
    case CULL_SCREEN_SIZE: return float3(0.55, 0.55, 0.95);
    case CULL_LOD: return float3(0.95, 0.95, 0.2);
    case CULL_OCCLUDED: return float3(0.75, 0.1, 0.75);
    case CULL_PAGE_MISS: return float3(0.2, 0.8, 0.95);
    default: return float3(0.5, 0.5, 0.5);
    }
}

float screenSpaceError(float3 center, float geometricError)
{
    float dist = max(length(gCamera.cameraPos - center), 1e-3);
    float projectionScale = gCamera.screenSize.y * 0.5f;
    return geometricError * projectionScale / dist;
}

float3 lodDebugColor(NaniteGpuCluster cluster)
{
    const float error = screenSpaceError(cluster.sphereCenter, cluster.geometricError);
    const float t = saturate(error / max(gCamera.lodErrorThreshold, 1e-3));
    const float lodBand = saturate(float(cluster.lodLevel) / 8.f);
    return lerp(float3(0.1, 0.35 + 0.55 * (1.f - t), 0.95), float3(0.95, 0.25 + 0.5 * lodBand, 0.1), t);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(gCamera.screenSize))) return;

    const float depth = gDepth[pixel];
    if (depth >= 1.f)
    {
        gPosition[pixel] = float4(0.f);
        gNormal[pixel] = float4(0.f);
        gTexCoord[pixel] = float4(0.f);
        gBaseColor[pixel] = float4(0.f);
        return;
    }

    const uint2 vis = gVisibility[pixel];
    const uint clusterIndex = vis.x;
    const uint tri = vis.y;
    NaniteGpuCluster cluster = gClusters[clusterIndex];

    const uint i0 = gIndices[cluster.indexOffset + tri * 3 + 0];
    const uint i1 = gIndices[cluster.indexOffset + tri * 3 + 1];
    const uint i2 = gIndices[cluster.indexOffset + tri * 3 + 2];
    NaniteGpuVertex v0 = gVertices[cluster.vertexOffset + i0];
    NaniteGpuVertex v1 = gVertices[cluster.vertexOffset + i1];
    NaniteGpuVertex v2 = gVertices[cluster.vertexOffset + i2];

    const float3 n = normalize(cross(v1.position - v0.position, v2.position - v0.position));
    const float3 color = kMaterialColors[cluster.materialIndex % 8];

    gPosition[pixel] = float4(v0.position, 1.f);
    gNormal[pixel] = float4(n, 1.f);
    gTexCoord[pixel] = float4(v0.texCoord, float(cluster.materialIndex), 1.f);

    if (gCamera.debugMode == NANITE_DEBUG_CLUSTER_ID)
    {
        gBaseColor[pixel] = float4(clusterDebugColor(clusterIndex), 1.f);
    }
    else if (gCamera.debugMode == NANITE_DEBUG_DEPTH)
    {
        const float t = saturate(1.f - depth);
        gBaseColor[pixel] = float4(t, t, t, 1.f);
    }
    else if (gCamera.debugMode == NANITE_DEBUG_CULL_REASON)
    {
        const uint reason = (clusterIndex < gCamera.clusterCount) ? gClusterDebugReason[clusterIndex] : CULL_VISIBLE;
        gBaseColor[pixel] = float4(cullReasonColor(reason), 1.f);
    }
    else if (gCamera.debugMode == NANITE_DEBUG_LOD)
    {
        gBaseColor[pixel] = float4(lodDebugColor(cluster), 1.f);
    }
    else
    {
        gBaseColor[pixel] = float4(color, 1.f);
    }
}
