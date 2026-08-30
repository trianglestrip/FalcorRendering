/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 ***************************************************************************/

/** Bounded production rough-specular producer.

    This pass is intentionally separate from diffuseGI and RTXDI.  It traces a
    small GGX reflection cone from the current GBuffer surface, evaluates
    emissive/analytic hit lighting, and writes an independent reflected
    radiance channel.  It is a bounded first producer, not a claim of UE's
    multi-level probe history: temporal acceptance and denoising remain owned
    by a later rough-specular history wave.
*/

#include "Scene/SceneDefines.slangh"

import Scene.Scene;
import Scene.RaytracingInline;
import Scene.Shading;
import Utils.Math.Ray;
import Utils.Sampling.SampleGenerator;
import RenderPasses.LumenGI.Lighting.LumenGILighting;
import RenderPasses.LumenGI.Lighting.LumenGILightSampling;

#ifndef is_valid_gLinearZ
#define is_valid_gLinearZ 0
#endif
#ifndef is_valid_gViewW
#define is_valid_gViewW 0
#endif
#ifndef is_valid_gNormalRoughnessMaterialID
#define is_valid_gNormalRoughnessMaterialID 0
#endif
#ifndef is_valid_gRoughSpecularIndirect
#define is_valid_gRoughSpecularIndirect 0
#endif
#ifndef is_valid_gRoughSpecularValidity
#define is_valid_gRoughSpecularValidity 0
#endif

cbuffer LumenRoughSpecularTraceCB
{
    uint2 gFrameDim;
    uint gFrameIndex;
    float gMaxRoughness;
    float gMaxRadiance;
    float3 gCameraPosW;
    float gFocalLengthPx;
    float3 gCameraRightW;
    float gPrincipalX;
    float3 gCameraUpW;
    float gPrincipalY;
    float3 gCameraForwardW;
    uint gSampleCount;
};

#if is_valid_gLinearZ
Texture2D<float2> gLinearZ;
#endif
#if is_valid_gViewW
Texture2D<float4> gViewW;
#endif
#if is_valid_gNormalRoughnessMaterialID
Texture2D<float4> gNormalRoughnessMaterialID;
#endif

#if is_valid_gRoughSpecularIndirect
RWTexture2D<float4> gRoughSpecularIndirect;
#endif
#if is_valid_gRoughSpecularValidity
RWTexture2D<uint> gRoughSpecularValidity;
#endif

static const uint kRoughValiditySurface = 1u << 0;
static const uint kRoughValidityHit = 1u << 1;
static const uint kRoughValiditySky = 1u << 2;
static const uint kRoughValidityFinite = 1u << 3;
static const uint kRoughValidityDisabled = 1u << 4;
static const uint kRoughValidityNoHit = 1u << 5;
static const float kRoughEpsilon = 1e-6f;
static const float kRoughMaxRadiance = 10.f;

float3 roughDecodeOctNormal(float2 oct)
{
    float2 p = oct * 2.f - 1.f;
    float3 n = float3(p, 1.f - abs(p.x) - abs(p.y));
    if (n.z < 0.f)
        n.xy = (1.f - abs(n.yx)) * select(n.xy >= 0.f, 1.f, -1.f);
    const float len2 = dot(n, n);
    return len2 > kRoughEpsilon ? n * rsqrt(len2) : float3(0.f, 0.f, 1.f);
}

float roughHash(uint2 p, uint sample, uint frame)
{
    uint h = p.x * 1973u + p.y * 9277u + sample * 26699u + frame * 31847u + 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0x00ffffffu) / 16777216.f;
}

float3 roughEnvironment(float3 direction)
{
    float3 value = lumenGIEvaluateEnvironment(direction);
    return min(max(value, float3(0.f)), float3(kRoughMaxRadiance));
}

float3 roughHitRadiance(const HitInfo hit, float3 rayOrigin, float3 rayDirection, uint seed)
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
    const ShadingData sd = gScene.materials.prepareShadingData(vertex, materialID, -rayDirection);
    const ExplicitLodTextureSampler lod = ExplicitLodTextureSampler(0.f);
    const IMaterialInstance material = gScene.materials.getMaterialInstance(sd, lod);
    float3 value = max(lumenGIEvaluateHitEmission(sd, material), float3(0.f));
    SampleGenerator generator = SampleGenerator(uint2(seed, seed ^ 0x6d2b79f5u), gFrameIndex);
    LumenGILightSample lightSample;
    if (lumenGISampleAnalyticLight(sd.posW, sd.faceN, generator, lightSample))
        value += max(lumenGIEvaluateAnalyticLightingSample(sd, material, generator, lightSample), float3(0.f));
    return min(value, float3(kRoughMaxRadiance));
}

void roughWrite(uint2 pixel, float4 value, uint validity)
{
#if is_valid_gRoughSpecularIndirect
    gRoughSpecularIndirect[pixel] = value;
#endif
#if is_valid_gRoughSpecularValidity
    gRoughSpecularValidity[pixel] = validity;
#endif
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= gFrameDim))
        return;

    uint validity = 0u;
#if !(is_valid_gLinearZ && is_valid_gViewW && is_valid_gNormalRoughnessMaterialID)
    roughWrite(pixel, float4(0.f), kRoughValidityDisabled);
    return;
#else
    const float depth = gLinearZ[pixel].x;
    const float4 viewSample = gViewW[pixel];
    const float4 normalRoughness = gNormalRoughnessMaterialID[pixel];
    if (!isfinite(depth) || depth <= 0.f || !all(isfinite(viewSample)) ||
        !all(isfinite(normalRoughness)))
    {
        roughWrite(pixel, float4(0.f), kRoughValidityNoHit);
        return;
    }

    const float3 N = roughDecodeOctNormal(normalRoughness.xy);
    const float3 Vraw = viewSample.xyz;
    const float vLen2 = dot(Vraw, Vraw);
    if (!(vLen2 > kRoughEpsilon) || !isfinite(vLen2))
    {
        roughWrite(pixel, float4(0.f), kRoughValidityNoHit);
        return;
    }
    const float3 V = Vraw * rsqrt(vLen2);
    const float roughness = saturate(min(normalRoughness.z, gMaxRoughness));
    const float3 origin = gCameraPosW - V * depth;
    const float3 ideal = normalize(reflect(-V, N));
    float3 tangent = normalize(abs(N.z) < 0.999f ? cross(N, float3(0.f, 0.f, 1.f)) :
        cross(N, float3(0.f, 1.f, 0.f)));
    float3 bitangent = normalize(cross(N, tangent));
    float3 sum = float3(0.f);
    float validSamples = 0.f;
    const uint sampleCount = clamp(gSampleCount, 1u, 8u);
    for (uint s = 0u; s < sampleCount; ++s)
    {
        const float u0 = roughHash(pixel, s * 2u, gFrameIndex);
        const float u1 = roughHash(pixel, s * 2u + 1u, gFrameIndex);
        const float phi = 6.283185307179586f * u0;
        const float cosTheta = sqrt(max(0.f, (1.f - u1) /
            (1.f + (roughness * roughness - 1.f) * u1)));
        const float sinTheta = sqrt(max(0.f, 1.f - cosTheta * cosTheta));
        const float3 halfVector = normalize(
            tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta) + N * cosTheta);
        const float3 direction = normalize(reflect(-V, halfVector));
        if (dot(direction, N) <= 0.f)
            continue;
        SceneRayQuery<0> query;
        float hitT = 0.f;
        const Ray ray = Ray(origin + N * 0.01f, direction, 0.001f, 1e30f);
        const HitInfo hit = query.traceRay(ray, hitT);
        if (hit.isValid() && isfinite(hitT) && hitT > 0.f)
        {
            sum += roughHitRadiance(hit, ray.origin, direction, pixel.x + pixel.y * gFrameDim.x + s);
            validity |= kRoughValidityHit;
        }
        else
        {
            sum += roughEnvironment(direction);
            validity |= kRoughValiditySky;
        }
        validSamples += 1.f;
    }

    if (validSamples <= 0.f)
    {
        roughWrite(pixel, float4(0.f), kRoughValidityNoHit);
        return;
    }
    validity |= kRoughValiditySurface | kRoughValidityFinite;
    const float confidence = saturate(validSamples / float(sampleCount));
    const float3 radiance = min(max(sum / validSamples, float3(0.f)),
        float3(gMaxRadiance > 0.f ? gMaxRadiance : kRoughMaxRadiance));
    roughWrite(pixel, float4(radiance, confidence), validity);
#endif
}
