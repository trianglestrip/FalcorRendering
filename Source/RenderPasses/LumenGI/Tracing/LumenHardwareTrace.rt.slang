/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/

#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.EmissiveLightSampler;
import LumenHitLighting;
import RenderPasses.LumenGI.LumenGIData;
import RenderPasses.LumenGI.Lighting.LumenGILighting;

#ifndef USE_ENV_LIGHT
#define USE_ENV_LIGHT 0
#endif
#ifndef is_valid_gViewW
#define is_valid_gViewW 0
#endif
#ifndef is_valid_gLumenGICounters
#define is_valid_gLumenGICounters 0
#endif
#ifndef is_valid_gLightingComponents
#define is_valid_gLightingComponents 0
#endif

#define is_valid(name) (is_valid_##name != 0)

static const float kRayTMax = FLT_MAX;

/** Number of 1D random samples the ray generation consumes for the 2D
    cosine-weighted direction sample. The hit lighting skips the reserved
    dimensions plus these samples so its random stream is decoupled from the
    direction sample while remaining deterministic.
*/
static const uint kLumenGIPrimaryRaySampleCount = 2;

cbuffer CB
{
    uint2 gFrameDim;
    uint gFrameIndex;
    uint gPRNGDimension;
}

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW;

RWTexture2D<float4> gDiffuseRadianceHitDist;
RWTexture2D<float4> gDiffuseGI;
RWTexture2D<float> gConfidence;

// Optional resources. Writes are guarded by the is_valid defines so the
// tracing path runs unmodified when the host has not wired them.
RWStructuredBuffer<uint4> gLumenGICounters;
RWTexture2D<float4> gLightingComponents;

// Optional emissive light sampler (LightBVH or Null depending on the
// _EMISSIVE_LIGHT_SAMPLER_TYPE define set by the host). The Null sampler
// ignores all calls, so the resource may stay unbound.
EmissiveLightSampler emissiveSampler;

/** Payload for the single diffuse indirect ray (24 bytes).
 *
 * The payload contains only the value returned to ray generation. All hit
 * attributes are normalized into Falcor's HitInfo locally in closest-hit
 * entry points, keeping geometry-specific data out of the payload.
 */
struct LumenIndirectPayload
{
    float3 radiance;
    float hitDistance;
    float confidence;
    uint flags;
};

static const uint kPayloadFlagHit = 1u << 0;
static const uint kPayloadFlagMiss = 1u << 1;

/** Reconstruct Falcor shading data from a unified hit record. */
ShadingData loadLumenShadingData(const HitInfo hit, const float3 rayOrigin, const float3 rayDirection)
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

    return gScene.materials.prepareShadingData(vertex, materialID, -rayDirection);
}

float3 getPrimaryRayDirection(const uint2 pixel, const uint2 frameDim)
{
    if (is_valid(gViewW))
        return -gViewW[pixel].xyz;

    return gScene.camera.computeRayPinhole(pixel, frameDim).dir;
}

float3 getDiffuseRayOrigin(const ShadingData sd, const HitType hitType)
{
    if (hitType == HitType::Curve)
        return sd.posW - sd.curveRadius * sd.frame.N;

    return sd.computeRayOrigin();
}

/** Sample one cosine-weighted direction in the primary diffuse hemisphere. */
float3 sampleDiffuseDirection(const ShadingData sd, inout SampleGenerator sampleGenerator)
{
    float pdf;
    const float3 localDirection = sample_cosine_hemisphere_concentric(sampleNext2D(sampleGenerator), pdf);
    return normalize(sd.frame.fromLocal(localDirection));
}

void traceDiffuseRay(const float3 origin, const float3 direction, inout LumenIndirectPayload payload)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.f;
    ray.TMax = kRayTMax;

    TraceRay(
        gScene.rtAccel,
        RAY_FLAG_NONE,
        0xff,
        0,
        rayTypeCount,
        0,
        ray,
        payload
    );
}

/** Apply the NaN/Inf and negative guards and the firefly clamp to radiance
    before it enters the payload or any output, and account the results in
    the optional GPU counters.
    \return Sanitized radiance. fireflyFlag is 1 when the sample was clamped.
*/
float3 finalizeLumenRadiance(const float3 rawRadiance, out float fireflyFlag)
{
    float3 radiance = rawRadiance;
    const bool nonFinite = any(isnan(radiance)) || any(isinf(radiance));
    const bool negative = any(radiance < 0.f);
    bool firefly = false;

    if (nonFinite)
    {
        radiance = float3(0.f);
    }
    else
    {
        radiance = max(radiance, float3(0.f));
        if (any(radiance > kLumenGIMaxRadiance))
        {
            radiance = min(radiance, float3(kLumenGIMaxRadiance));
            firefly = true;
        }
    }

    if (is_valid(gLumenGICounters))
    {
        if (nonFinite)
            InterlockedAdd(gLumenGICounters[uint(LumenGICounterIndex::NaNInfSamples)].x, 1u);
        if (firefly)
            InterlockedAdd(gLumenGICounters[uint(LumenGICounterIndex::FireflySamples)].x, 1u);
        if (negative)
            InterlockedAdd(gLumenGICounters[uint(LumenGICounterIndex::NegativeSamples)].x, 1u);
        InterlockedAdd(gLumenGICounters[uint(LumenGICounterIndex::TracedRays)].x, 1u);
    }

    fireflyFlag = firefly ? 1.f : 0.f;
    return radiance;
}

/** Write the per-pixel lighting components used by the debug pass.
    RGB holds the clamped analytic radiance; A holds the firefly flag. The
    emissive and environment components are derived by the debug pass from
    the total radiance (see LumenGIDebug.cs.slang).
*/
void writeLumenLightingComponents(const uint2 pixel, const float3 analyticRadiance, const float fireflyFlag)
{
    if (is_valid(gLightingComponents))
    {
        float3 clampedAnalytic = min(max(analyticRadiance, float3(0.f)), float3(kLumenGIMaxRadiance));
        if (any(isnan(clampedAnalytic)) || any(isinf(clampedAnalytic)))
            clampedAnalytic = float3(0.f);
        gLightingComponents[pixel] = float4(clampedAnalytic, fireflyFlag);
    }
}

/** Handle any supported secondary geometry through Falcor's unified HitInfo. */
void handleLumenIndirectHit(const HitInfo hit, inout LumenIndirectPayload payload)
{
    const float3 rayOrigin = WorldRayOrigin();
    const float3 rayDirection = WorldRayDirection();
    const ShadingData sd = loadLumenShadingData(hit, rayOrigin, rayDirection);

    const ExplicitLodTextureSampler lod = ExplicitLodTextureSampler(0.f);
    const IMaterialInstance materialInstance = gScene.materials.getMaterialInstance(sd, lod);

    // Deterministic random stream: same seed as the ray generation, skipping
    // the reserved dimensions and the primary direction sample so the
    // lighting decisions are decoupled from the direction sample.
    const uint2 pixel = DispatchRaysIndex().xy;
    SampleGenerator sampleGenerator = SampleGenerator(pixel, gFrameIndex);
    for (uint dimension = 0; dimension < gPRNGDimension; ++dimension)
        sampleNext1D(sampleGenerator);
    for (uint i = 0; i < kLumenGIPrimaryRaySampleCount; ++i)
        sampleNext1D(sampleGenerator);

    LumenGILightingComponents components;
    const LumenHitLightingResult lighting = evaluateLumenHitLighting(sd, materialInstance, emissiveSampler, sampleGenerator, components);

    float fireflyFlag;
    payload.radiance = finalizeLumenRadiance(lighting.radiance, fireflyFlag);
    payload.hitDistance = RayTCurrent();
    payload.confidence = saturate(lighting.confidence);
    payload.flags = kPayloadFlagHit;

    writeLumenLightingComponents(pixel, components.analyticRadiance, fireflyFlag);
}

[shader("raygeneration")]
void lumenGIRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dispatchDim = DispatchRaysDimensions().xy;
    if (any(pixel >= gFrameDim) || any(pixel >= dispatchDim))
        return;

    float3 unmodulatedRadiance = float3(0.f);
    float3 diffuseGI = float3(0.f);
    float hitDistance = 0.f;
    float confidence = 0.f;

    const HitInfo primaryHit = HitInfo(gVBuffer[pixel]);
    if (primaryHit.isValid())
    {
        const float3 primaryRayOrigin = gScene.camera.getPosition();
        const float3 primaryRayDirection = getPrimaryRayDirection(pixel, dispatchDim);
        const ShadingData primarySD = loadLumenShadingData(primaryHit, primaryRayOrigin, primaryRayDirection);

        const ExplicitLodTextureSampler lod = ExplicitLodTextureSampler(0.f);
        const IMaterialInstance materialInstance = gScene.materials.getMaterialInstance(primarySD, lod);
        const float3 diffuseReflectance = max(materialInstance.getProperties(primarySD).diffuseReflectionAlbedo, 0.f);

        SampleGenerator sampleGenerator = SampleGenerator(pixel, gFrameIndex);
        for (uint dimension = 0; dimension < gPRNGDimension; ++dimension)
            sampleNext1D(sampleGenerator);

        const float3 rayOrigin = getDiffuseRayOrigin(primarySD, primaryHit.getType());
        const float3 rayDirection = sampleDiffuseDirection(primarySD, sampleGenerator);

        LumenIndirectPayload payload = {};
        payload.hitDistance = kLumenGIMissHitDistance;
        traceDiffuseRay(rayOrigin, rayDirection, payload);

        unmodulatedRadiance = max(payload.radiance, 0.f);
        diffuseGI = unmodulatedRadiance * diffuseReflectance;
        hitDistance = payload.hitDistance;
        confidence = saturate(payload.confidence);
    }

    gDiffuseRadianceHitDist[pixel] = float4(unmodulatedRadiance, hitDistance);
    gDiffuseGI[pixel] = float4(diffuseGI, 1.f);
    gConfidence[pixel] = confidence;
}

[shader("miss")]
void lumenIndirectMiss(inout LumenIndirectPayload payload)
{
    const float3 rawRadiance = lumenGIEvaluateEnvironment(WorldRayDirection());

    float fireflyFlag;
    payload.radiance = finalizeLumenRadiance(rawRadiance, fireflyFlag);
    payload.hitDistance = kLumenGIMissHitDistance;
    payload.confidence = 1.f;
    payload.flags = kPayloadFlagMiss;

    writeLumenLightingComponents(DispatchRaysIndex().xy, float3(0.f), fireflyFlag);
}

[shader("anyhit")]
void lumenTriangleAnyHit(inout LumenIndirectPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const GeometryInstanceID instanceID = getGeometryInstanceID();
    const VertexData vertex = getVertexData(instanceID, PrimitiveIndex(), attributes);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(vertex, materialID, 0.f))
        IgnoreHit();
}

[shader("closesthit")]
void lumenTriangleClosestHit(inout LumenIndirectPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    TriangleHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attributes.barycentrics;
    handleLumenIndirectHit(HitInfo(hit), payload);
}

/** Alpha test for displaced triangle meshes.

    The base triangle's interpolated vertex data carries the texture
    coordinates that the material's alpha texture is sampled with; the
    displacement only moves the surface, it does not change the UVs. This
    mirrors the triangle anyhit above. Note that visibility queries
    (SceneRayQuery) run with RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH without
    FORCE_OPAQUE when alpha testing is enabled, so this anyhit also applies to
    inline visibility rays.
*/
[shader("anyhit")]
void lumenDisplacedTriangleAnyHit(inout LumenIndirectPayload payload, DisplacedTriangleMeshIntersector::Attribs attributes)
{
    const GeometryInstanceID instanceID = getGeometryInstanceID();
    const VertexData vertex = gScene.getVertexData(instanceID, PrimitiveIndex(), float3(attributes.barycentrics, 0.f));
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(vertex, materialID, 0.f))
        IgnoreHit();
}

[shader("intersection")]
void lumenDisplacedTriangleIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    DisplacedTriangleMeshIntersector::Attribs attributes;
    float hitT;
    if (DisplacedTriangleMeshIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attributes, hitT))
        ReportHit(hitT, 0, attributes);
}

[shader("closesthit")]
void lumenDisplacedTriangleClosestHit(
    inout LumenIndirectPayload payload,
    DisplacedTriangleMeshIntersector::Attribs attributes
)
{
    DisplacedTriangleHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attributes.barycentrics;
    hit.displacement = attributes.displacement;
    handleLumenIndirectHit(HitInfo(hit), payload);
}

// Curve and SDF grid hit groups deliberately have no anyhit shader:
// - Curves are traced as linear swept spheres for hair-style materials,
//   which do not support alpha masking in Falcor's material system. Falcor's
//   own GBufferRT and PathTracer hit groups follow the same convention.
// - SDF grids have no per-surface texture coordinates (the hit record only
//   carries the voxel hit data), so there is no alpha texture to sample.
// Both geometry types are therefore treated as opaque, which is the Falcor
// baseline behavior.

[shader("intersection")]
void lumenCurveIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    CurveIntersector::Attribs attributes;
    float hitT;
    if (CurveIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attributes, hitT))
        ReportHit(hitT, 0, attributes);
}

[shader("closesthit")]
void lumenCurveClosestHit(inout LumenIndirectPayload payload, CurveIntersector::Attribs attributes)
{
    CurveHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attributes.barycentrics;
    handleLumenIndirectHit(HitInfo(hit), payload);
}

[shader("intersection")]
void lumenSdfGridIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    SDFGridHitData hitData;
    float hitT;
    if (SDFGridIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), hitData, hitT))
        ReportHit(hitT, 0, hitData);
}

[shader("closesthit")]
void lumenSdfGridClosestHit(inout LumenIndirectPayload payload, SDFGridHitData hitData)
{
    SDFGridHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.hitData = hitData;
    handleLumenIndirectHit(HitInfo(hit), payload);
}
