// Filament IBL shader resources (bound from FilamentIBL C++).
// Included by forward shaders; do not modify Scene::EnvMap.

cbuffer FilamentIBLCB
{
    float gIblLuminance;
    float gIblRoughnessOneLevel;
    float gIblRotation;
    uint  gUseSH;
    float3 gIblSH[9];
};

TextureCube gIblSpecular;
Texture2D gIblDFG;
SamplerState gIblLinearSampler;
