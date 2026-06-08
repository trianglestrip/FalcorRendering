/***************************************************************************
 # PBRT DXR RTGI shader.
 **************************************************************************/
import Scene.Raytracing;
import Utils.Sampling.TinyUniformSampleGenerator;
import Rendering.Lights.LightHelpers;

RWTexture2D<float4> gOutput;

cbuffer PerFrameCB
{
    float2 viewportDims;
    uint sampleIndex;
    uint enableRTGI;
    uint enableDirectLighting;
    uint enableShadows;
    float indirectIntensity;
    uint maxBounces;
};

struct PrimaryRayData
{
    float4 color;
    uint depth;
    float hitT;
};

struct ShadowRayData
{
    uint hit;
};

[shader("miss")]
void shadowMiss(inout ShadowRayData hitData)
{
    hitData.hit = 0u;
}

[shader("anyhit")]
void shadowAnyHit(inout ShadowRayData hitData, BuiltInTriangleIntersectionAttributes attribs)
{
    hitData.hit = 1u;
}

[shader("miss")]
void primaryMiss(inout PrimaryRayData hitData)
{
    hitData.color = float4(0.02f, 0.025f, 0.03f, 1.0f);
    hitData.hitT = -1.0f;
}

bool traceShadowRay(float3 origin, float3 dir, float dist)
{
    if (enableShadows == 0u)
        return false;

    ShadowRayData rayData;
    rayData.hit = 1u;
    RayDesc ray = { origin, 0.002f, dir, max(dist - 0.004f, 0.002f) };
    TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 1, rayTypeCount, 1, ray, rayData);
    return rayData.hit != 0u;
}

float3 makeTangent(float3 n)
{
    return normalize(abs(n.z) < 0.999f ? cross(float3(0.0f, 0.0f, 1.0f), n) : cross(float3(0.0f, 1.0f, 0.0f), n));
}

float3 sampleCosineHemisphere(float2 u, float3 n)
{
    const float phi = 6.28318530718f * u.x;
    const float r = sqrt(u.y);
    const float x = r * cos(phi);
    const float y = r * sin(phi);
    const float z = sqrt(max(0.0f, 1.0f - u.y));
    float3 t = makeTangent(n);
    float3 b = cross(n, t);
    return normalize(t * x + b * y + n * z);
}

float3 evalDirect(ShadingData sd, IMaterialInstance mi, inout TinyUniformSampleGenerator sg)
{
    if (enableDirectLighting == 0u)
        return float3(0.0f);

    float3 color = float3(0.0f);
    for (int i = 0; i < gScene.getLightCount(); i++)
    {
        AnalyticLightSample ls;
        if (evalLightApproximate(sd.posW, gScene.getLight(i), ls))
        {
            if (!traceShadowRay(sd.computeRayOrigin(), ls.dir, ls.distance))
                color += mi.eval(sd, ls.dir, sg) * ls.Li;
        }
    }
    return color;
}

[shader("closesthit")]
void primaryClosestHit(inout PrimaryRayData hitData, BuiltInTriangleIntersectionAttributes attribs)
{
    float3 rayDirW = WorldRayDirection();
    float hitT = RayTCurrent();
    uint triangleIndex = PrimitiveIndex();

    const GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, triangleIndex, attribs);
    uint materialID = gScene.getMaterialID(instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDirW);

    let lod = ExplicitLodTextureSampler(0.0f);
    let mi = gScene.materials.getMaterialInstance(sd, lod);
    let props = mi.getProperties(sd);

    uint3 launchIndex = DispatchRaysIndex();
    TinyUniformSampleGenerator sg = TinyUniformSampleGenerator(launchIndex.xy, sampleIndex + hitData.depth * 9781u);

    float3 color = props.emission + evalDirect(sd, mi, sg);

    if (enableRTGI != 0u && hitData.depth < maxBounces)
    {
        float3 n = normalize(sd.frame.N);
        n = dot(n, -rayDirW) < 0.0f ? -n : n;
        float3 bounceDir = sampleCosineHemisphere(sampleNext2D(sg), n);

        PrimaryRayData bounceData = {};
        bounceData.depth = hitData.depth + 1u;

        RayDesc bounceRay;
        bounceRay.Origin = sd.computeRayOrigin(false);
        bounceRay.Direction = bounceDir;
        bounceRay.TMin = 0.002f;
        bounceRay.TMax = 100000.0f;
        TraceRay(gScene.rtAccel, 0, 0xFF, 0, rayTypeCount, 0, bounceRay, bounceData);

        if (bounceData.hitT > 0.0f)
            color += props.diffuseReflectionAlbedo * bounceData.color.rgb * indirectIntensity;
    }

    hitData.color = float4(color, 1.0f);
    hitData.hitT = hitT;
}

[shader("anyhit")]
void primaryAnyHit(inout PrimaryRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.0f))
        IgnoreHit();
}

[shader("raygeneration")]
void rayGen()
{
    uint3 launchIndex = DispatchRaysIndex();
    RayDesc ray = gScene.camera.computeRayPinhole(launchIndex.xy, viewportDims).toRayDesc();

    PrimaryRayData hitData = {};
    hitData.depth = 0;
    TraceRay(gScene.rtAccel, 0, 0xFF, 0, rayTypeCount, 0, ray, hitData);
    gOutput[launchIndex.xy] = hitData.color;
}
