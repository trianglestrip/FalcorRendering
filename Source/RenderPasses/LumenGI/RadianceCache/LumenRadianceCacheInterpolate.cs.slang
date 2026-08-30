/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/

/**
 * C10 Wave 1 radiance-cache interpolation.
 *
 * One thread serves one view pixel.  A finite GBuffer surface is reconstructed
 * in world space (background pixels use a bounded camera-ray/clipmap
 * intersection), routed to the finest containing clipmap level, and converted
 * to the level-major indirection texture's 3-D grid. Up to eight neighboring
 * cells are gathered with trilinear weights. Invalid, stale, not-ready, or
 * non-finite probes are omitted and the remaining weights are renormalized.
 * If no valid probe remains, both outputs are explicitly zeroed: the caller
 * must apply its documented HWRT/other GI fallback rather than interpreting a
 * black cache sample as valid radiance.
 *
 * gRadianceCacheOutput: RGB incident radiance, A confidence/coverage.
 * gRadianceCacheHitDist: RGB incident radiance, A hit distance.
 */

#include "LumenRadianceCacheData.slang"

struct LumenRadianceCacheSample
{
    float3 radiance;
    float hitDistance;
    float confidence;
    bool valid;
    uint validity;
};

void lumenRadianceCacheResetOutputs(const uint2 pixel)
{
    gRadianceCacheOutput[pixel] = float4(0.f, 0.f, 0.f, 0.f);
    gRadianceCacheHitDist[pixel] = float4(0.f, 0.f, 0.f, 0.f);
    gRadianceCacheValidityOutput[pixel] = 0u;
}

bool lumenRadianceCacheLoadSample(const uint slot, out LumenRadianceCacheSample sample)
{
    sample.radiance = float3(0.f);
    sample.hitDistance = 0.f;
    sample.confidence = 0.f;
    sample.valid = false;
    sample.validity = 0u;

    if (slot == kLumenRadianceCacheInvalidSlot)
        return false;

    uint metaCount = 0u;
    uint worldPositionCount = 0u;
    uint radianceCount = 0u;
    uint validityCount = 0u;
    uint metaStride = 0u;
    uint worldPositionStride = 0u;
    uint radianceStride = 0u;
    uint validityStride = 0u;
    gProbeMeta.GetDimensions(metaCount, metaStride);
    gProbeWorldPos.GetDimensions(worldPositionCount, worldPositionStride);
    gProbeRadianceHitDist.GetDimensions(radianceCount, radianceStride);
    gProbeValidity.GetDimensions(validityCount, validityStride);
    if (slot >= metaCount || slot >= radianceCount || slot >= validityCount)
        return false;

    const uint4 meta = gProbeMeta[slot];
    if (!lumenRadianceCacheMetaValid(meta) || meta.x >= worldPositionCount)
        return false;

    const float4 worldPosition = gProbeWorldPos[meta.x];
    const float4 payload = gProbeRadianceHitDist[slot];
    const uint validity = gProbeValidity[slot];
    const bool isHit = (validity & kLumenRadianceCacheValidityHit) != 0u;
    const bool isSky = (validity & kLumenRadianceCacheValiditySky) != 0u;
    if (!lumenRadianceCacheFinite4(worldPosition) ||
        !lumenRadianceCacheFinite3(worldPosition.xyz) ||
        !lumenRadianceCachePayloadValid(payload) ||
        !lumenRadianceCacheProducerRecordValid(payload, validity) ||
        !(isHit || isSky) ||
        (isHit && !lumenRadianceCacheHitPayloadValid(payload)) ||
        (isSky && payload.a < kLumenRadianceCacheMissHitDistance))
        return false;

    sample.radiance = max(payload.rgb, float3(0.f));
    sample.hitDistance = isSky ? kLumenRadianceCacheMissHitDistance : max(payload.a, 0.f);
    sample.confidence = 1.f;
    sample.valid = true;
    sample.validity = validity;
    return true;
}

bool lumenRadianceCacheGridCoordinate(
    const uint2 pixel,
    const uint3 gridDimensions,
    out float3 gridCoordinate,
    out uint selectedLevel
)
{
    gridCoordinate = float3(0.f);
    selectedLevel = 0u;
    if (any(pixel >= gFrameDim) || any(gridDimensions == uint3(0u)))
        return false;

    const float4 query = gViewW[pixel];
    const float4 depth = gLinearZ[pixel];
    if (!lumenRadianceCacheFinite4(query) || !lumenRadianceCacheFinite3(query.xyz))
        return false;

    // Reconstruct the surface point from Falcor's viewW + linearZ contract and
    // map it into the dynamic level-0 clipmap. This avoids treating a signed
    // view direction as a world position (which made the first Wave-1 snapshot
    // deterministically miss every indirection cell).
    const float focal = gCameraFocalPrincipal.x;
    if (!lumenRadianceCacheFinite(focal) || focal <= kLumenRadianceCacheEpsilon)
        return false;
    const float2 principal = gCameraFocalPrincipal.yz;
    const float3 viewW = normalize(query.xyz);
    if (!lumenRadianceCacheFinite3(viewW))
        return false;
    const float3 cameraPos = gCameraPosW.xyz;
    const float3 cacheCenter = gCacheCameraCenterBaseExtent.xyz;
    const float baseExtent = gCacheCameraCenterBaseExtent.w;
    if (!lumenRadianceCacheFinite3(cameraPos) || !lumenRadianceCacheFinite3(cacheCenter) ||
        !lumenRadianceCacheFinite(baseExtent) || baseExtent <= kLumenRadianceCacheEpsilon)
        return false;
    float3 worldPos = float3(0.f);
    const bool depthValid = lumenRadianceCacheFinite4(depth) &&
        depth.x > kLumenRadianceCacheEpsilon && depth.x < kLumenRadianceCacheMissHitDistance;
    if (depthValid)
    {
        const float z = depth.x;
        const float3 viewPos = float3(
            ((float(pixel.x) + 0.5f) - principal.x) / focal * z,
            ((float(pixel.y) + 0.5f) - principal.y) / focal * z,
            z
        );
        worldPos = cameraPos + gCameraRightW.xyz * viewPos.x -
            gCameraUpW.xyz * viewPos.y - gCameraForwardW.xyz * viewPos.z;
    }
    else
    {
        // Background pixels have no finite GBuffer depth. Intersect the camera
        // ray with the level-0 clipmap so sky/black-sky probes remain queryable
        // without borrowing a neighboring surface position.
        const float3 rayDirection = normalize(-viewW);
        if (!lumenRadianceCacheFinite3(rayDirection))
            return false;
        const float halfExtent = baseExtent * 0.5f;
        const float3 boundsMin = cacheCenter - float3(halfExtent);
        const float3 boundsMax = cacheCenter + float3(halfExtent);
        float tEnter = 0.f;
        float tExit = 1e30f;
        for (uint axis = 0u; axis < 3u; ++axis)
        {
            const float origin = cameraPos[axis];
            const float direction = rayDirection[axis];
            if (abs(direction) <= kLumenRadianceCacheEpsilon)
            {
                if (origin < boundsMin[axis] || origin > boundsMax[axis])
                    return false;
                continue;
            }
            const float ta = (boundsMin[axis] - origin) / direction;
            const float tb = (boundsMax[axis] - origin) / direction;
            tEnter = max(tEnter, min(ta, tb));
            tExit = min(tExit, max(ta, tb));
        }
        if (!lumenRadianceCacheFinite(tEnter) || !lumenRadianceCacheFinite(tExit) ||
            tExit <= tEnter + kLumenRadianceCacheEpsilon)
            return false;
        worldPos = cameraPos + rayDirection * (tEnter + 0.5f * (tExit - tEnter));
    }
    if (!lumenRadianceCacheFinite3(worldPos))
        return false;
    // The host packs the clipmap as [level * resolution + x, y, z].  Level 0
    // follows the camera; static levels are origin-centred and double their
    // extent at each step.  This is the same near/far ownership rule as the
    // CPU cache: choose the finest containing level, then reject points beyond
    // the coarsest footprint instead of silently clamping them into level 0.
    if (gridDimensions.y != gridDimensions.z || gridDimensions.x < gridDimensions.y ||
        (gridDimensions.x % gridDimensions.y) != 0u)
        return false;
    const uint levelCount = max(gridDimensions.x / gridDimensions.y, 1u);
    const float dynamicHalfExtent = baseExtent * 0.5f;
    const float3 dynamicDelta = abs(worldPos - cacheCenter);
    uint level = 0u;
    if (max(max(dynamicDelta.x, dynamicDelta.y), dynamicDelta.z) > dynamicHalfExtent)
    {
        float levelExtent = baseExtent;
        level = levelCount - 1u;
        for (uint candidate = 1u; candidate < levelCount; ++candidate)
        {
            levelExtent *= 2.f;
            const float staticHalfExtent = levelExtent * 0.5f;
            const float3 staticAbs = abs(worldPos);
            if (max(max(staticAbs.x, staticAbs.y), staticAbs.z) <= staticHalfExtent)
            {
                level = candidate;
                break;
            }
        }
    }

    float selectedExtent = baseExtent;
    for (uint i = 0u; i < level; ++i)
        selectedExtent *= 2.f;
    selectedLevel = level;
    const float3 selectedCenter = level == 0u ? cacheCenter : float3(0.f);
    const float3 normalizedQuery =
        (worldPos - (selectedCenter - float3(selectedExtent * 0.5f))) / selectedExtent;
    if (any(normalizedQuery < float3(0.f)) || any(normalizedQuery > float3(1.f)))
        return false;
    const float resolution = float(gridDimensions.y);
    gridCoordinate = float3(
        (float(level) + normalizedQuery.x) * resolution,
        normalizedQuery.y * resolution,
        normalizedQuery.z * resolution
    ) - float3(0.5f);
    return lumenRadianceCacheFinite3(gridCoordinate);
}

/** Project a probe world position to a source-texture pixel. */
bool lumenRadianceCacheProjectToSource(const float3 worldPosition, out uint2 sourcePixel)
{
    sourcePixel = uint2(0u);
    if (any(gSourceDim == uint2(0u)) || !lumenRadianceCacheFinite3(worldPosition))
        return false;

    const float4 clip = mul(gViewProj, float4(worldPosition, 1.f));
    // Keep the matrix in the producer ABI as a finite-data guard, but do not
    // use clip.w for visibility: Falcor backends differ in clip-space depth/Y
    // conventions. The camera-basis projection below is authoritative.
    if (!lumenRadianceCacheFinite4(clip))
        return false;

    // Use the same pinhole convention as ScreenProbeTrace rather than relying
    // on the backend-specific clip-space Y/depth orientation of gViewProj.
    const float3 rel = worldPosition - gCameraPosW.xyz;
    const float3 viewPos = float3(
        dot(rel, gCameraRightW.xyz),
        -dot(rel, gCameraUpW.xyz),
        -dot(rel, gCameraForwardW.xyz)
    );
    const float focal = gCameraFocalPrincipal.x;
    if (!lumenRadianceCacheFinite3(viewPos) || viewPos.z <= kLumenRadianceCacheEpsilon ||
        !lumenRadianceCacheFinite(focal) || focal <= kLumenRadianceCacheEpsilon)
        return false;

    const float2 sourcePosition = float2(
        viewPos.x / viewPos.z * focal + gCameraFocalPrincipal.y,
        viewPos.y / viewPos.z * focal + gCameraFocalPrincipal.z
    );
    const int2 nearest = int2(floor(sourcePosition));
    if (any(nearest < int2(0)) || any(nearest >= int2(gSourceDim)))
        return false;
    sourcePixel = uint2(nearest);
    return true;
}

/**
 * Producer stage: sample the current HWRT source once per allocated probe.
 * The previous payload is intentionally declared in the shared ABI for the
 * next history wave, but an invalid current projection writes zero alpha here;
 * it must not silently resurrect stale radiance.
 */
[shader("compute")]
[numthreads(64, 1, 1)]
void buildMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint slot = dispatchThreadID.x;
    uint metaCount = 0u;
    uint metaStride = 0u;
    gProbeMeta.GetDimensions(metaCount, metaStride);
    if (slot >= metaCount)
        return;

    float4 output = float4(0.f);
    uint sourceWidth = 0u;
    uint sourceHeight = 0u;
    gDiffuseRadianceHitDist.GetDimensions(sourceWidth, sourceHeight);
    uint viewWidth = 0u;
    uint viewHeight = 0u;
    gViewW.GetDimensions(viewWidth, viewHeight);
    const uint4 meta = gProbeMeta[slot];
    uint worldPositionCount = 0u;
    uint worldPositionStride = 0u;
    uint screenPositionCount = 0u;
    uint screenPositionStride = 0u;
    gProbeWorldPos.GetDimensions(worldPositionCount, worldPositionStride);
    gProbeScreenPos.GetDimensions(screenPositionCount, screenPositionStride);
    if (lumenRadianceCacheMetaProducerValid(meta) && meta.x < worldPositionCount)
    {
        const float4 worldPosition = gProbeWorldPos[meta.x];
        uint2 sourcePixel;
        const float4 screenPosition = gProbeScreenPos[slot];
        if (slot < screenPositionCount && lumenRadianceCacheFinite4(worldPosition) &&
            lumenRadianceCacheFinite4(screenPosition) && screenPosition.z > 0.f &&
            screenPosition.x >= 0.f && screenPosition.y >= 0.f &&
            lumenRadianceCacheFinite3(screenPosition.xyz))
        {
            sourcePixel = uint2(floor(screenPosition.xy));
            if (sourcePixel.x < sourceWidth && sourcePixel.y < sourceHeight &&
                sourcePixel.x < viewWidth && sourcePixel.y < viewHeight)
            {
                // Probe centers often land in empty space. Search a small source
                // neighborhood for a valid current-frame hit rather than turning
                // every empty projection into an all-black cache slot. This is a
                // bounded diagnostic seed; the production wave will replace it by
                // a scene-ray probe trace.
                float3 radianceSum = float3(0.f);
                float hitDistanceSum = 0.f;
                float weightSum = 0.f;
                for (int oy = -2; oy <= 2; ++oy)
                {
                    for (int ox = -2; ox <= 2; ++ox)
                    {
                        const int2 q = int2(sourcePixel) + int2(ox, oy);
                        if (any(q < int2(0)) || q.x >= int(sourceWidth) || q.y >= int(sourceHeight))
                            continue;
                        const uint2 uq = uint2(q);
                        const float4 viewGuide = gViewW[uq];
                        const float4 linearDepth = gLinearZ[uq];
                        const float4 source = gDiffuseRadianceHitDist[uq];
                        if (!lumenRadianceCacheFinite4(viewGuide) ||
                            !lumenRadianceCacheFinite4(linearDepth) || linearDepth.x <= 0.f ||
                            !lumenRadianceCacheSourcePayloadValid(source))
                            continue;
                        const float w = 1.f / (1.f + float(ox * ox + oy * oy));
                        radianceSum += max(source.rgb, float3(0.f)) * w;
                        hitDistanceSum += max(source.a, 0.f) * w;
                        weightSum += w;
                    }
                }
                if (weightSum > kLumenRadianceCacheEpsilon)
                {
                    output = float4(radianceSum / weightSum, hitDistanceSum / weightSum);
                }
            }
        }
    }

    gProbeRadianceHitDistOut[slot] = output;
}

void lumenRadianceCacheAccumulateTap(
    const int3 cell,
    const uint3 gridDimensions,
    const float weight,
    inout float3 radianceSum,
    inout float hitDistanceSum,
    inout float weightSum,
    inout uint validityMask
)
{
    if (weight <= kLumenRadianceCacheEpsilon ||
        any(cell < int3(0)) ||
        any(cell >= int3(gridDimensions)))
        return;

    const uint slot = gIndirection.Load(int4(cell, 0));
    LumenRadianceCacheSample sample;
    if (!lumenRadianceCacheLoadSample(slot, sample) || !sample.valid)
        return;

    radianceSum += sample.radiance * weight;
    hitDistanceSum += sample.hitDistance * weight;
    weightSum += weight * sample.confidence;
    validityMask |= sample.validity;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= gFrameDim))
        return;

    // These counters are cleared and copied by the host once per dispatch.  They
    // are deliberately GPU-owned: queryAttempts is not inferred from frame
    // dimensions, and hits/misses are reconciled from the same validity path
    // that writes the output texture.
    InterlockedAdd(gRadianceCacheQueryCounters[kLumenRadianceCacheQueryAttempts], 1u);

    uint gridWidth = 0u;
    uint gridHeight = 0u;
    uint gridDepth = 0u;
    gIndirection.GetDimensions(gridWidth, gridHeight, gridDepth);
    const uint3 gridDimensions = uint3(gridWidth, gridHeight, gridDepth);

    float3 gridCoordinate;
    uint selectedLevel = 0u;
    if (!lumenRadianceCacheGridCoordinate(pixel, gridDimensions, gridCoordinate, selectedLevel))
    {
        lumenRadianceCacheResetOutputs(pixel);
        InterlockedAdd(gRadianceCacheQueryCounters[kLumenRadianceCacheQueryMisses], 1u);
        InterlockedAdd(gRadianceCacheLevelCounters[selectedLevel * kLumenRadianceCacheLevelStride + kLumenRadianceCacheLevelAttempts], 1u);
        InterlockedAdd(gRadianceCacheLevelCounters[selectedLevel * kLumenRadianceCacheLevelStride + kLumenRadianceCacheLevelMisses], 1u);
        InterlockedAdd(gRadianceCacheLevelCounters[selectedLevel * kLumenRadianceCacheLevelStride + kLumenRadianceCacheLevelFallback], 1u);
        return;
    }
    const uint levelCounterBase = selectedLevel * kLumenRadianceCacheLevelStride;
    InterlockedAdd(gRadianceCacheLevelCounters[levelCounterBase + kLumenRadianceCacheLevelAttempts], 1u);
    InterlockedAdd(gRadianceCacheLevelCounters[levelCounterBase + kLumenRadianceCacheLevelSamples], 1u);

    const float3 baseFloat = floor(gridCoordinate);
    const int3 baseCell = int3(baseFloat);
    const float3 fraction = saturate(gridCoordinate - baseFloat);
    float3 radianceSum = float3(0.f);
    float hitDistanceSum = 0.f;
    float weightSum = 0.f;
    uint validityMask = 0u;

    // Fixed tap order makes the result deterministic and gives a nearest/8-way
    // fallback when some clipmap cells are not resident.
    for (uint z = 0u; z < 2u; ++z)
    {
        for (uint y = 0u; y < 2u; ++y)
        {
            for (uint x = 0u; x < 2u; ++x)
            {
                const float wx = (x == 0u) ? (1.f - fraction.x) : fraction.x;
                const float wy = (y == 0u) ? (1.f - fraction.y) : fraction.y;
                const float wz = (z == 0u) ? (1.f - fraction.z) : fraction.z;
                lumenRadianceCacheAccumulateTap(
                    baseCell + int3(x, y, z), gridDimensions, wx * wy * wz,
                    radianceSum, hitDistanceSum, weightSum, validityMask
                );
            }
        }
    }

    if (weightSum <= kLumenRadianceCacheEpsilon ||
        !lumenRadianceCacheFinite3(radianceSum) ||
        !lumenRadianceCacheFinite(hitDistanceSum))
    {
        // Explicit invalid-cache semantics.  A downstream fallback is required
        // and must not be inferred from an RGB black value alone.
        lumenRadianceCacheResetOutputs(pixel);
        InterlockedAdd(gRadianceCacheQueryCounters[kLumenRadianceCacheQueryMisses], 1u);
        InterlockedAdd(gRadianceCacheLevelCounters[levelCounterBase + kLumenRadianceCacheLevelMisses], 1u);
        InterlockedAdd(gRadianceCacheLevelCounters[levelCounterBase + kLumenRadianceCacheLevelFallback], 1u);
        return;
    }

    const float3 radiance = max(radianceSum / weightSum, float3(0.f));
    const float hitDistance = max(hitDistanceSum / weightSum, 0.f);
    const float confidence = saturate(weightSum);
    if (!lumenRadianceCacheFinite3(radiance) ||
        !lumenRadianceCacheFinite(hitDistance) ||
        !lumenRadianceCacheFinite(confidence))
    {
        lumenRadianceCacheResetOutputs(pixel);
        InterlockedAdd(gRadianceCacheQueryCounters[kLumenRadianceCacheQueryMisses], 1u);
        InterlockedAdd(gRadianceCacheLevelCounters[levelCounterBase + kLumenRadianceCacheLevelMisses], 1u);
        InterlockedAdd(gRadianceCacheLevelCounters[levelCounterBase + kLumenRadianceCacheLevelFallback], 1u);
        return;
    }

    gRadianceCacheOutput[pixel] = float4(radiance, confidence);
    gRadianceCacheHitDist[pixel] = float4(radiance, hitDistance);
    gRadianceCacheValidityOutput[pixel] = validityMask;
    InterlockedAdd(gRadianceCacheQueryCounters[kLumenRadianceCacheQueryHits], 1u);
    InterlockedAdd(gRadianceCacheLevelCounters[levelCounterBase + kLumenRadianceCacheLevelHits], 1u);
    if (hitDistance < kLumenRadianceCacheMissHitDistance)
        InterlockedAdd(gRadianceCacheLevelCounters[levelCounterBase + kLumenRadianceCacheLevelValidHitDistance], 1u);
}
