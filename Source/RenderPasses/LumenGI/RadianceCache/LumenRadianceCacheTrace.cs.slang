/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/

/** C10 bounded probe producer.

    This pass deliberately owns the scene-ray part of the cache producer.  The
    previous Wave-1 buildMain projected probe centers into the current frame and
    sampled diffuseRadianceHitDist; that is useful as a diagnostic seed but is
    not a probe trace and frequently samples empty/black pixels.  Here each
    allocated probe emits one deterministic ray.  A miss stores the scene
    environment as valid incident radiance (the same miss contract used by the
    Lumen probe path); a hit stores a conservative environment/visible-source
    estimate and the actual hit distance.  The cache remains diagnostic until
    bounded multi-sample lighting and final resolve integration are gated.
*/

#include "LumenRadianceCacheData.slang"
#include "../LumenGIData.slang"
#include "Scene/SceneDefines.slangh"

// The cache producer uses the same single-sample analytic-light NEE adapter as
// the existing Lumen secondary-hit path.  This is intentionally a bounded
// producer: eight deterministic rays and one light sample per allocated probe;
// it is not a replacement for the later multi-sample radiance-cache budget.
#ifndef SAMPLE_GENERATOR_TYPE
#define SAMPLE_GENERATOR_TYPE 1
#endif
#ifndef USE_ANALYTIC_LIGHTS
#define USE_ANALYTIC_LIGHTS 1
#endif
#ifndef USE_EMISSIVE_LIGHTS
#define USE_EMISSIVE_LIGHTS 1
#endif
#ifndef LUMEN_GI_HAS_EMISSIVE_SAMPLER
#define LUMEN_GI_HAS_EMISSIVE_SAMPLER 0
#endif

import Scene.Scene;
import Scene.RaytracingInline;
import Scene.Shading;
import Utils.Math.Ray;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.EmissiveLightSampler;
import RenderPasses.LumenGI.Lighting.LumenGILighting;
import RenderPasses.LumenGI.Lighting.LumenGILightSampling;

#if LUMEN_GI_HAS_EMISSIVE_SAMPLER
EmissiveLightSampler emissiveSampler;
#endif

static const float kProbeRayTMin = 0.001f;
static const float kProbeRayTMax = 1e30f;

float3 lumenRadianceCacheProbeDirection(const float3 worldPosition, const uint slot, const uint rayIndex)
{
    // A small deterministic direction set gives the bounded producer a real
    // chance to hit nearby geometry even when a probe is in an empty cell.  The
    // first two directions follow the camera axis; the remaining six cover the
    // world axes.  This is deliberately fixed (not a stochastic unbounded loop)
    // so the trace budget is visible and reproducible.
    const float3 cameraDelta = gCameraPosW.xyz - worldPosition;
    const float cameraDeltaLength2 = dot(cameraDelta, cameraDelta);
    // A probe can coincide with the camera center (or receive a malformed
    // camera constant during a resize).  Never feed normalize(0/NaN) into the
    // ray query; a deterministic forward fallback still preserves the
    // hit/sky validity contract and produces a legal black sky record if the
    // fallback ray misses.
    const float3 toCamera =
        (lumenRadianceCacheFinite3(cameraDelta) && lumenRadianceCacheFinite(cameraDeltaLength2) &&
         cameraDeltaLength2 > kLumenRadianceCacheEpsilon)
        ? cameraDelta * rsqrt(cameraDeltaLength2)
        : float3(0.f, 0.f, 1.f);
    if (rayIndex == 0u) return toCamera;
    if (rayIndex == 1u) return -toCamera;
    if (rayIndex == 2u) return float3(1.f, 0.f, 0.f);
    if (rayIndex == 3u) return float3(-1.f, 0.f, 0.f);
    if (rayIndex == 4u) return float3(0.f, 1.f, 0.f);
    if (rayIndex == 5u) return float3(0.f, -1.f, 0.f);
    if (rayIndex == 6u) return float3(0.f, 0.f, 1.f);
    // Keep slot in the sequence so neighboring probes do not all resolve to
    // the same final tie when the first seven directions are misses.
    return normalize(float3(0.f, 0.f, -1.f) + float3(0.f, 0.02f * float(slot & 3u), 0.f));
}

float3 lumenRadianceCacheEnvironment(const float3 direction)
{
    float3 value = lumenGIEvaluateEnvironment(direction);
    if (any(isnan(value)) || any(isinf(value)))
        value = float3(0.f);
    return min(max(value, float3(0.f)), float3(kLumenGIMaxRadiance));
}

float3 lumenRadianceCacheHitEmission(const HitInfo hit, const float3 rayOrigin, const float3 rayDirection)
{
    VertexData vertex = {};
    uint materialID = 0u;
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        vertex = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(triangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit displacedHit = hit.getDisplacedTriangleHit();
        vertex = gScene.getVertexData(displacedHit, -rayDirection);
        materialID = gScene.getMaterialID(displacedHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        const CurveHit curveHit = hit.getCurveHit();
        vertex = gScene.getVertexDataFromCurve(curveHit);
        materialID = gScene.getMaterialID(curveHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_SDF_GRID)
    if (hit.getType() == HitType::SDFGrid)
    {
        const SDFGridHit sdfHit = hit.getSDFGridHit();
        vertex = gScene.getVertexDataFromSDFGrid(sdfHit, rayOrigin, rayDirection);
        materialID = gScene.getMaterialID(sdfHit.instanceID);
    }
#endif
    const ShadingData sd = gScene.materials.prepareShadingData(vertex, materialID, -rayDirection);
    const ExplicitLodTextureSampler lod = ExplicitLodTextureSampler(0.f);
    const IMaterialInstance material = gScene.materials.getMaterialInstance(sd, lod);
    return max(material.getProperties(sd).emission, float3(0.f));
}

float3 lumenRadianceCacheHitLighting(
    const HitInfo hit,
    const float3 rayOrigin,
    const float3 rayDirection,
    const uint slot,
    out float confidence
)
{
    confidence = 0.f;
    VertexData vertex = {};
    uint materialID = 0u;
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        vertex = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(triangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit displacedHit = hit.getDisplacedTriangleHit();
        vertex = gScene.getVertexData(displacedHit, -rayDirection);
        materialID = gScene.getMaterialID(displacedHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        const CurveHit curveHit = hit.getCurveHit();
        vertex = gScene.getVertexDataFromCurve(curveHit);
        materialID = gScene.getMaterialID(curveHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_SDF_GRID)
    if (hit.getType() == HitType::SDFGrid)
    {
        const SDFGridHit sdfHit = hit.getSDFGridHit();
        vertex = gScene.getVertexDataFromSDFGrid(sdfHit, rayOrigin, rayDirection);
        materialID = gScene.getMaterialID(sdfHit.instanceID);
    }
#endif

    const ShadingData sd = gScene.materials.prepareShadingData(vertex, materialID, -rayDirection);
    const ExplicitLodTextureSampler lod = ExplicitLodTextureSampler(0.f);
    const IMaterialInstance material = gScene.materials.getMaterialInstance(sd, lod);
    SampleGenerator sampleGenerator = SampleGenerator(uint2(slot, 0u), gCurrentFrame);
    const float3 emission = lumenGIEvaluateHitEmission(sd, material);
    float3 analytic = float3(0.f);
#if USE_ANALYTIC_LIGHTS
    LumenGILightSample lightSample;
    if (lumenGISampleAnalyticLight(sd.posW, sd.faceN, sampleGenerator, lightSample))
        analytic = lumenGIEvaluateAnalyticLightingSample(sd, material, sampleGenerator, lightSample);
#endif
    float3 emissive = float3(0.f);
#if USE_EMISSIVE_LIGHTS && LUMEN_GI_HAS_EMISSIVE_SAMPLER
    LumenGILightSample emissiveSample;
    if (lumenGISampleEmissiveLight(emissiveSampler, sd.posW, sd.faceN, true, sampleGenerator, emissiveSample))
        emissive = lumenGIEvaluateEmissiveLightingSample(sd, material, sampleGenerator, emissiveSample);
#endif
    const float3 radiance = max(emission + analytic + emissive, float3(0.f));
    confidence = (any(radiance > 0.f) || any(emission > 0.f)) ? 1.f : 0.5f;
    return min(radiance, float3(kLumenGIMaxRadiance));
}

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
    uint validity = 0u;
    uint worldCount = 0u;
    uint worldStride = 0u;
    gProbeWorldPos.GetDimensions(worldCount, worldStride);
    const uint4 meta = gProbeMeta[slot];
    if (lumenRadianceCacheMetaProducerValid(meta) && meta.x < worldCount)
    {
        const float4 world = gProbeWorldPos[meta.x];
        if (lumenRadianceCacheFinite4(world))
        {
            const float3 origin = world.xyz;
            float3 radiance = float3(0.f);
            float closestHitT = kLumenRadianceCacheMissHitDistance;
            for (uint rayIndex = 0u; rayIndex < 8u; ++rayIndex)
            {
                const float3 direction = lumenRadianceCacheProbeDirection(origin, slot, rayIndex);
                const Ray ray = Ray(origin, direction, kProbeRayTMin, kProbeRayTMax);
                float hitT = 0.f;
                SceneRayQuery<0> query;
                const HitInfo hit = query.traceRay(ray, hitT);
                const bool hitValid = hit.isValid() && isfinite(hitT) && hitT > 0.f;
                if (hitValid && hitT < closestHitT)
                {
                    closestHitT = hitT;
                    float hitConfidence = 0.f;
                    radiance = max(
                        lumenRadianceCacheEnvironment(direction),
                        lumenRadianceCacheHitLighting(hit, origin, direction, slot + rayIndex, hitConfidence)
                    );
                    validity |= kLumenRadianceCacheValidityHit;
                }
            }
            // Environment is a legal Lumen miss source.  A sky-only probe keeps
            // an explicit sky bit and miss distance; a hit carries a finite
            // distance even when the legal radiance is black.
            if ((validity & kLumenRadianceCacheValidityHit) == 0u)
            {
                radiance = lumenRadianceCacheEnvironment(lumenRadianceCacheProbeDirection(origin, slot, 0u));
                validity |= kLumenRadianceCacheValiditySky;
            }
            validity |= kLumenRadianceCacheValidityRadiance | kLumenRadianceCacheValidityProducer;
            const float distance = closestHitT < kLumenRadianceCacheMissHitDistance
                ? closestHitT : kLumenGIMissHitDistance;
            output = float4(radiance, distance);
        }
    }

    // Publish only records that carry an explicit hit/sky provenance and a
    // matching distance encoding.  RGB is intentionally not part of validity:
    // black incident radiance is a legal result for both a hit and a sky miss.
    // This producer-side gate keeps malformed records from reaching the
    // indirection/interpolation path while preserving valid black samples.
    if (!lumenRadianceCacheProducerRecordValid(output, validity))
    {
        // Keep a provenance-bearing record available for the interpolation
        // diagnostic even when a backend quantizes the miss distance at the
        // structured-buffer boundary.  The strict hit/sky bits still gate
        // consumption; only a record with explicit producer+radiance and one
        // exclusive provenance bit may take this compatibility path.  This
        // avoids turning a legal black sky sample into an indistinguishable
        // invalid zero while retaining the typed validity contract.
        const bool hasProvenance =
            (validity & (kLumenRadianceCacheValidityProducer | kLumenRadianceCacheValidityRadiance)) ==
                (kLumenRadianceCacheValidityProducer | kLumenRadianceCacheValidityRadiance) &&
            (((validity & kLumenRadianceCacheValidityHit) != 0u) !=
             ((validity & kLumenRadianceCacheValiditySky) != 0u)) &&
            lumenRadianceCacheFinite4(output) && lumenRadianceCacheNonNegative3(output.rgb);
        if (hasProvenance)
        {
            if ((validity & kLumenRadianceCacheValiditySky) != 0u)
                output.a = kLumenRadianceCacheMissHitDistance;
            else if (output.a <= 0.f || output.a >= kLumenRadianceCacheMissHitDistance)
                output.a = kLumenRadianceCacheEpsilon;
        }
        else
        {
            output = float4(0.f);
            validity = 0u;
        }
    }

    gProbeRadianceHitDistOut[slot] = output;
    gProbeValidityOut[slot] = validity;
}
