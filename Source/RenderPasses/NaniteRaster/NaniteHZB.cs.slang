#include "NaniteShared.slangh"

// ---------------------------------------------------------------------------
// HZB builder
// ---------------------------------------------------------------------------

Texture2D<float> gDepth;

RWTexture2D<float> gHZBMip0;
RWTexture2D<float> gHZBMip1;
RWTexture2D<float> gOcclusionHeatmap;

cbuffer HzbCB
{
    float2 gScreenSize;
    uint gBuildHeatmap;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void buildMip0(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(gScreenSize))) return;
    gHZBMip0[pixel] = gDepth[pixel];
}

[shader("compute")]
[numthreads(8, 8, 1)]
void buildMip1(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy * 2;
    if (any(pixel >= uint2(gScreenSize))) return;

    float d = gHZBMip0[pixel];
    d = min(d, gHZBMip0[min(pixel + uint2(1, 0), uint2(gScreenSize) - 1)]);
    d = min(d, gHZBMip0[min(pixel + uint2(0, 1), uint2(gScreenSize) - 1)]);
    d = min(d, gHZBMip0[min(pixel + uint2(1, 1), uint2(gScreenSize) - 1)]);
    gHZBMip1[pixel / 2] = d;
}

// ---------------------------------------------------------------------------
// Occlusion culling
// ---------------------------------------------------------------------------

StructuredBuffer<NaniteGpuCluster> gClusters;
StructuredBuffer<uint> gVisibleClusters;
StructuredBuffer<uint> gVisibleClusterCount;
RWStructuredBuffer<uint> gFilteredClusters;
RWStructuredBuffer<uint> gFilteredClusterCount;
RWStructuredBuffer<uint> gCullStats;

Texture2D<float> gHZBMip0Occ;
Texture2D<float> gHZBMip1Occ;

cbuffer OcclusionCB
{
    float4x4 gViewProj;
    float3 gCameraPos;
    float2 gScreenSizeOcc;
    float2 gHZBMip1Size;
};

struct ProjectedBounds
{
    float2 uvMin;
    float2 uvMax;
    float nearDepth;
    bool valid;
};

ProjectedBounds projectBoundsConservative(float3 boundsMin, float3 boundsMax)
{
    ProjectedBounds result;
    result.valid = false;
    result.nearDepth = 1.f;
    result.uvMin = float2(1.f, 1.f);
    result.uvMax = float2(0.f, 0.f);

    [unroll]
    for (uint corner = 0; corner < 8; ++corner)
    {
        const float3 p = float3(
            (corner & 1) ? boundsMax.x : boundsMin.x,
            (corner & 2) ? boundsMax.y : boundsMin.y,
            (corner & 4) ? boundsMax.z : boundsMin.z);

        float4 clip = mul(float4(p, 1.f), gViewProj);
        if (clip.w <= 0.f)
        {
            result.valid = false;
            return result;
        }

        const float3 ndc = clip.xyz / clip.w;
        result.nearDepth = min(result.nearDepth, ndc.z);
        const float2 uv = ndc.xy * 0.5 + 0.5;
        result.uvMin = min(result.uvMin, uv);
        result.uvMax = max(result.uvMax, uv);
        result.valid = true;
    }

    const float3 extent = boundsMax - boundsMin;
    const float radius = length(extent) * 0.5;
    const float4 centerClip = mul(float4((boundsMin + boundsMax) * 0.5, 1.f), gViewProj);
    const float margin = radius / max(centerClip.w, 1e-3);
    result.uvMin = saturate(result.uvMin - margin);
    result.uvMax = saturate(result.uvMax + margin);
    return result;
}

uint selectHzbMip(uint2 pixMin, uint2 pixMax)
{
    const uint2 rectSize = pixMax - pixMin + 1;
    const uint maxDim = max(rectSize.x, rectSize.y);
    return maxDim > 4 ? 1u : 0u;
}

float sampleHzb(uint2 pixMin, uint2 pixMax, uint mipLevel)
{
    float hzbDepth = 1.f;

    if (mipLevel == 0)
    {
        [loop]
        for (uint y = pixMin.y; y <= pixMax.y; ++y)
        {
            [loop]
            for (uint x = pixMin.x; x <= pixMax.x; ++x)
            {
                hzbDepth = min(hzbDepth, gHZBMip0Occ[uint2(x, y)]);
            }
        }
    }
    else
    {
        const uint2 mipMin = pixMin / 2;
        uint2 mipMax = pixMax / 2;
        mipMax = min(mipMax, uint2(gHZBMip1Size) - 1);

        [loop]
        for (uint y = mipMin.y; y <= mipMax.y; ++y)
        {
            [loop]
            for (uint x = mipMin.x; x <= mipMax.x; ++x)
            {
                hzbDepth = min(hzbDepth, gHZBMip1Occ[uint2(x, y)]);
            }
        }
    }

    return hzbDepth;
}

bool testClusterOcclusion(NaniteGpuCluster cluster)
{
    const ProjectedBounds projected = projectBoundsConservative(cluster.boundsMin, cluster.boundsMax);
    if (!projected.valid) return false;

    const float2 uvMin = projected.uvMin;
    const float2 uvMax = projected.uvMax;
    if (any(uvMax <= uvMin)) return false;

    uint2 pixMin = uint2(uvMin * gScreenSizeOcc);
    uint2 pixMax = uint2(uvMax * gScreenSizeOcc);
    pixMax = min(pixMax, uint2(gScreenSizeOcc) - 1);

    const uint mipLevel = selectHzbMip(pixMin, pixMax);
    const float hzbDepth = sampleHzb(pixMin, pixMax, mipLevel);

    const float2 uvSize = uvMax - uvMin;
    const float depthBias = max(max(uvSize.x, uvSize.y), 1.f / max(gScreenSizeOcc.x, 1.f)) * 0.5f;
    return projected.nearDepth > hzbDepth + depthBias;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void occlusionCull(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint listIndex = dispatchThreadId.x;
    if (listIndex >= gVisibleClusterCount[0]) return;

    const uint clusterIndex = gVisibleClusters[listIndex];
    const NaniteGpuCluster cluster = gClusters[clusterIndex];

    InterlockedAdd(gCullStats[CULL_TESTED], 1);

    if (testClusterOcclusion(cluster))
    {
        InterlockedAdd(gCullStats[CULL_OCCLUDED], 1);
        return;
    }

    uint slot;
    InterlockedAdd(gFilteredClusterCount[0], 1, slot);
    gFilteredClusters[slot] = clusterIndex;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void heatmap(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(gScreenSize))) return;

    const float d = gDepth[pixel];
    const float h = gHZBMip0[pixel];
    gOcclusionHeatmap[pixel] = saturate((h - d) * 10.f + 0.5f);
}
