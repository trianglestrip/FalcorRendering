#include "NaniteShared.slangh"

StructuredBuffer<NaniteGpuCluster> gClusters;
StructuredBuffer<NaniteGpuVertex> gVertices;
StructuredBuffer<uint> gIndices;
StructuredBuffer<uint> gVisibleClusters;
StructuredBuffer<uint> gVisibleClusterCount;

RWTexture2D<uint2> gVisibility;
RWTexture2D<float> gDepth;
RWTexture2D<uint> gClusterID;

cbuffer RasterCB
{
    NaniteCameraData gCamera;
};

float edgeFunction(float2 a, float2 b, float2 p)
{
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

float3 transformPoint(float3 p)
{
    float4 clip = mul(float4(p, 1.f), gCamera.viewProj);
    return clip.xyz / max(clip.w, 1e-6);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint listIndex = dispatchThreadId.x;
    if (listIndex >= gVisibleClusterCount[0]) return;

    const uint clusterIndex = gVisibleClusters[listIndex];
    NaniteGpuCluster cluster = gClusters[clusterIndex];

    for (uint tri = 0; tri < cluster.triangleCount; ++tri)
    {
        const uint i0 = gIndices[cluster.indexOffset + tri * 3 + 0];
        const uint i1 = gIndices[cluster.indexOffset + tri * 3 + 1];
        const uint i2 = gIndices[cluster.indexOffset + tri * 3 + 2];

        NaniteGpuVertex v0 = gVertices[cluster.vertexOffset + i0];
        NaniteGpuVertex v1 = gVertices[cluster.vertexOffset + i1];
        NaniteGpuVertex v2 = gVertices[cluster.vertexOffset + i2];

        float3 c0 = transformPoint(v0.position);
        float3 c1 = transformPoint(v1.position);
        float3 c2 = transformPoint(v2.position);

        if (any(isnan(c0)) || any(isnan(c1)) || any(isnan(c2))) continue;

        float2 p0 = c0.xy;
        float2 p1 = c1.xy;
        float2 p2 = c2.xy;

        float2 minP = min(min(p0, p1), p2);
        float2 maxP = max(max(p0, p1), p2);

        int2 pixelMin = int2((minP * 0.5 + 0.5) * gCamera.screenSize);
        int2 pixelMax = int2((maxP * 0.5 + 0.5) * gCamera.screenSize);
        pixelMin = max(pixelMin, int2(0, 0));
        pixelMax = min(pixelMax, int2(gCamera.screenSize) - 1);

        const float area = edgeFunction(p0, p1, p2);
        if (abs(area) < 1e-8) continue;

        for (int y = pixelMin.y; y <= pixelMax.y; ++y)
        {
            for (int x = pixelMin.x; x <= pixelMax.x; ++x)
            {
                float2 p = (float2(x, y) + 0.5) / gCamera.screenSize * 2.f - 1.f;
                const float w0 = edgeFunction(p1, p2, p);
                const float w1 = edgeFunction(p2, p0, p);
                const float w2 = edgeFunction(p0, p1, p);
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;

                const float invArea = 1.f / area;
                const float b0 = w0 * invArea;
                const float b1 = w1 * invArea;
                const float b2 = w2 * invArea;
                const float depth = b0 * c0.z + b1 * c1.z + b2 * c2.z;

                uint2 pix = uint2(x, y);
                float prevDepth = gDepth[pix];
                if (depth >= prevDepth) continue;

                gDepth[pix] = depth;
                gVisibility[pix] = uint2(clusterIndex, tri);
                gClusterID[pix] = clusterIndex;
            }
        }
    }
}
