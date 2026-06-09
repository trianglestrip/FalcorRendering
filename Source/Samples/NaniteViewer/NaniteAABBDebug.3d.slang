struct PerFrameCB
{
    float4x4 viewProj;
};

ConstantBuffer<PerFrameCB> gCB;

struct VSIn
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};

VSOut vsMain(VSIn vIn)
{
    VSOut vOut;
    vOut.pos = mul(gCB.viewProj, float4(vIn.pos, 1.f));
    vOut.color = vIn.color;
    return vOut;
}

float4 psMain(VSOut vOut) : SV_TARGET
{
    return vOut.color;
}
