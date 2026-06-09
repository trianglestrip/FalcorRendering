#include "NaniteShared.slangh"

StructuredBuffer<NaniteGpuHierarchyNode> gHierarchy;
StructuredBuffer<NaniteGpuCluster> gClusters;
StructuredBuffer<uint> gPageResidency;
StructuredBuffer<uint> gPageFallback;
StructuredBuffer<uint> gRootNodes;

RWStructuredBuffer<uint> gVisibleClusters;
RWStructuredBuffer<uint> gVisibleClusterCount;
RWStructuredBuffer<uint> gCullStats;
RWStructuredBuffer<uint> gPageRequests;
RWStructuredBuffer<uint> gClusterDebugReason;
RWStructuredBuffer<uint> gRasterDispatchArgs;

cbuffer CullCB
{
    NaniteCameraData gCamera;
};

bool frustumCullSphere(float3 center, float radius)
{
    float4 clip = mul(float4(center, 1.f), gCamera.viewProj);
    if (clip.w <= 0.f) return false;

    float3 ndc = clip.xyz / clip.w;
    const float margin = radius / max(clip.w, 1e-3);
    return all(abs(ndc.xy) <= 1.f + margin) && ndc.z >= -1.f - margin && ndc.z <= 1.f + margin;
}

bool backfaceConeCull(float3 center, float3 coneNormal, float coneAngle)
{
    float3 viewDir = normalize(gCamera.cameraPos - center);
    return dot(coneNormal, viewDir) >= cos(coneAngle);
}

float screenSpaceError(float3 center, float geometricError)
{
    float dist = max(length(gCamera.cameraPos - center), 1e-3);
    float projectionScale = gCamera.screenSize.y * 0.5f;
    return geometricError * projectionScale / dist;
}

float projectedRadiusPixels(float3 center, float radius)
{
    float dist = max(length(gCamera.cameraPos - center), 1e-3);
    return radius * gCamera.screenSize.y * 0.5f / dist;
}

bool lodAccept(float3 center, float geometricError)
{
    return screenSpaceError(center, geometricError) <= gCamera.lodErrorThreshold;
}

void recordClusterReason(uint clusterIndex, uint reason)
{
    if (clusterIndex < gCamera.clusterCount)
        gClusterDebugReason[clusterIndex] = reason;
}

void appendVisibleCluster(uint clusterIndex, bool finestLevel)
{
    InterlockedAdd(gCullStats[CULL_TESTED], 1);

    NaniteGpuCluster cluster = gClusters[clusterIndex];
    uint reason = CULL_VISIBLE;

    if (!frustumCullSphere(cluster.sphereCenter, cluster.sphereRadius))
    {
        reason = CULL_FRUSTUM;
    }
    else if (!backfaceConeCull(cluster.sphereCenter, cluster.coneNormal, cluster.coneAngle))
    {
        reason = CULL_BACKFACE;
    }
    else if (projectedRadiusPixels(cluster.sphereCenter, cluster.sphereRadius) < 1.f)
    {
        reason = CULL_SCREEN_SIZE;
    }
    else if (!finestLevel && !lodAccept(cluster.sphereCenter, cluster.geometricError))
    {
        reason = CULL_LOD;
    }
    else if (cluster.pageIndex < gPageResidency.length && gPageResidency[cluster.pageIndex] == 0)
    {
        uint fallbackPage = (cluster.pageIndex < gPageFallback.length) ? gPageFallback[cluster.pageIndex] : 0;
        if (fallbackPage < gPageResidency.length && gPageResidency[fallbackPage] != 0)
        {
            InterlockedAdd(gPageRequests[cluster.pageIndex], 1);
        }
        else
        {
            reason = CULL_PAGE_MISS;
            InterlockedAdd(gPageRequests[cluster.pageIndex], 1);
        }
    }

    recordClusterReason(clusterIndex, reason);

    if (reason != CULL_VISIBLE)
    {
        InterlockedAdd(gCullStats[reason], 1);
        return;
    }

    uint slot;
    InterlockedAdd(gVisibleClusterCount[0], 1, slot);
    gVisibleClusters[slot] = clusterIndex;
    InterlockedAdd(gCullStats[CULL_VISIBLE], 1);
}

void emitNodeClusters(NaniteGpuHierarchyNode node, bool finestLevel)
{
    for (uint i = 0; i < node.clusterCount; ++i)
        appendVisibleCluster(node.clusterOffset + i, finestLevel);
}

void traverseHierarchy(uint rootNodeIndex)
{
    const uint kMaxStackDepth = 32;
    uint stack[kMaxStackDepth];
    uint stackSize = 0;
    stack[stackSize++] = rootNodeIndex;

    while (stackSize > 0)
    {
        const uint nodeIndex = stack[--stackSize];
        NaniteGpuHierarchyNode node = gHierarchy[nodeIndex];

        if (!frustumCullSphere(node.sphereCenter, node.sphereRadius))
        {
            InterlockedAdd(gCullStats[CULL_FRUSTUM], 1);
            continue;
        }

        const bool finestLevel = (node.childNodeCount == 0);
        if (lodAccept(node.sphereCenter, node.maxError))
        {
            emitNodeClusters(node, finestLevel);
            continue;
        }

        if (node.childNodeCount > 0)
        {
            for (uint i = 0; i < node.childNodeCount; ++i)
            {
                if (stackSize < kMaxStackDepth)
                    stack[stackSize++] = node.childNodeOffset + i;
            }
        }
        else
        {
            emitNodeClusters(node, true);
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint rootListIndex = dispatchThreadId.x;
    if (rootListIndex >= gCamera.rootNodeCount) return;

    const uint rootNodeIndex = gRootNodes[rootListIndex];
    if (rootNodeIndex >= gCamera.hierarchyNodeCount) return;

    traverseHierarchy(rootNodeIndex);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void buildIndirectArgs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint visibleCount = gVisibleClusterCount[0];
    gRasterDispatchArgs[0] = visibleCount;
    gRasterDispatchArgs[1] = 1;
    gRasterDispatchArgs[2] = 1;
}
