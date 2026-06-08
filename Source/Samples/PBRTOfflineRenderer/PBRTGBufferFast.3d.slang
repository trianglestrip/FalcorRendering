/***************************************************************************
 # PBRT preview fast GBuffer pass.
 **************************************************************************/

import Scene.Scene;

struct GBufferOut
{
    float4 baseColor : SV_TARGET0;
    float4 normalW : SV_TARGET1;
    float4 material : SV_TARGET2;
    float4 emissive : SV_TARGET3;
    float4 viewDirW : SV_TARGET4;
    float4 ids : SV_TARGET5;
};

struct VSIn
{
    float3 pos                            : POSITION;
    float3 packedNormalTangentCurveRadius : PACKED_NORMAL_TANGENT_CURVE_RADIUS;
    float2 texC                           : TEXCOORD;
    uint instanceID                       : DRAW_ID;
};

struct VSOut
{
    linear float3 normalW                         : NORMAL;
    linear float3 posW                            : POSW;
    nointerpolation GeometryInstanceID instanceID : INSTANCE_ID;
    nointerpolation uint materialID               : MATERIAL_ID;
    float4 posH                                   : SV_POSITION;
};

VSOut vsMain(VSIn vIn)
{
    VSOut vOut;
    const GeometryInstanceID instanceID = { vIn.instanceID };
    const float4x4 worldMat = gScene.getWorldMatrix(instanceID);

    vOut.posW = mul(worldMat, float4(vIn.pos, 1.0f)).xyz;
    vOut.posH = mul(gScene.camera.getViewProj(), float4(vOut.posW, 1.0f));
    vOut.instanceID = instanceID;
    vOut.materialID = gScene.getMaterialID(instanceID);

    PackedStaticVertexData packedVertex;
    packedVertex.position = vIn.pos;
    packedVertex.packedNormalTangentCurveRadius = vIn.packedNormalTangentCurveRadius;
    packedVertex.texCrd = vIn.texC;
    vOut.normalW = mul(gScene.getInverseTransposeWorldMatrix(instanceID), packedVertex.unpack().normal);

    return vOut;
}

float3 fallbackMaterialColor(uint materialID)
{
    uint h = materialID * 1664525u + 1013904223u;
    float3 color = float3(
        float((h >> 0u) & 255u),
        float((h >> 8u) & 255u),
        float((h >> 16u) & 255u)) / 255.0f;
    return lerp(float3(0.35f, 0.38f, 0.42f), color, 0.35f);
}

GBufferOut psMain(VSOut vsOut, uint triangleIndex : SV_PrimitiveID)
{
    float3 viewDir = normalize(gScene.camera.getPosition() - vsOut.posW);
    float3 faceNormal = gScene.getFaceNormalW(vsOut.instanceID, triangleIndex);
    float3 N = normalize(length(vsOut.normalW) > 1e-5f ? vsOut.normalW : faceNormal);

    float3 baseColor = fallbackMaterialColor(vsOut.materialID);
    float roughness = 0.65f;
    float3 f0 = lerp(float3(0.04f), baseColor, 0.08f);
    float3 emissive = float3(0.0f);

    GBufferOut out;
    out.baseColor = float4(baseColor, 1.0f);
    out.normalW = float4(N * 0.5f + 0.5f, 1.0f);
    out.material = float4(roughness, f0);
    out.emissive = float4(emissive, 1.0f);
    out.viewDirW = float4(viewDir * 0.5f + 0.5f, 1.0f);
    out.ids = float4(float(vsOut.materialID), float(vsOut.instanceID.index), 0.0f, 1.0f);
    return out;
}
