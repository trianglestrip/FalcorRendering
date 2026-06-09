Texture2D<float> gSceneDepth;
Texture2D<float4> gScenePosW;
Texture2D<float4> gSceneNormW;
Texture2D<float4> gSceneFaceNormalW;
Texture2D<float2> gSceneTexC;
Texture2D<uint4> gSceneMtlData;

Texture2D<float> gNaniteDepth;
Texture2D<float4> gNanitePosition;
Texture2D<float4> gNaniteNormal;
Texture2D<float4> gNaniteTexCoord;
Texture2D<float4> gNaniteBaseColor;

RWTexture2D<float> gDepth;
RWTexture2D<float4> gPosW;
RWTexture2D<float4> gNormW;
RWTexture2D<float4> gFaceNormalW;
RWTexture2D<float2> gTexC;
RWTexture2D<uint4> gMtlData;
RWTexture2D<float4> gOutput;

cbuffer CompositeCB
{
    float2 gScreenSize;
    uint gHasSceneGBuffer;
};

bool isValidDepth(float depth)
{
    return depth < 1.f;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(gScreenSize))) return;

    const float naniteDepth = gNaniteDepth[pixel];
    const bool naniteValid = isValidDepth(naniteDepth);

    float sceneDepth = 1.f;
    bool sceneValid = false;
    if (gHasSceneGBuffer != 0)
    {
        sceneDepth = gSceneDepth[pixel];
        sceneValid = isValidDepth(sceneDepth);
    }

    const bool useNanite = naniteValid && (!sceneValid || naniteDepth < sceneDepth);

    if (useNanite)
    {
        const float4 pos = gNanitePosition[pixel];
        const float4 norm = gNaniteNormal[pixel];
        const float4 tex = gNaniteTexCoord[pixel];
        const float4 color = gNaniteBaseColor[pixel];

        gDepth[pixel] = naniteDepth;
        gPosW[pixel] = pos;
        gNormW[pixel] = norm;
        gFaceNormalW[pixel] = float4(norm.xyz, 1.f);
        gTexC[pixel] = tex.xy;
        gMtlData[pixel] = uint4(uint(tex.z), 0u, 0u, 0u);
        gOutput[pixel] = color;
    }
    else if (sceneValid)
    {
        gDepth[pixel] = sceneDepth;
        gPosW[pixel] = gScenePosW[pixel];
        gNormW[pixel] = gSceneNormW[pixel];
        gFaceNormalW[pixel] = gSceneFaceNormalW[pixel];
        gTexC[pixel] = gSceneTexC[pixel];
        gMtlData[pixel] = gSceneMtlData[pixel];
        gOutput[pixel] = float4(gSceneNormW[pixel].xyz * 0.5 + 0.5, 1.f);
    }
    else
    {
        gDepth[pixel] = 1.f;
        gPosW[pixel] = float4(0.f);
        gNormW[pixel] = float4(0.f);
        gFaceNormalW[pixel] = float4(0.f);
        gTexC[pixel] = float2(0.f);
        gMtlData[pixel] = uint4(0u);
        gOutput[pixel] = float4(0.f);
    }
}
