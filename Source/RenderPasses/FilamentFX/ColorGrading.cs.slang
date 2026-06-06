cbuffer PerFrameCB
{
    uint2 gResolution;
    float gToneMapping; // 0: ACES, 1: Filmic, 2: Linear, 3: Display
    float gExposure;
    float gContrast;
    float gVibrance;
    float gSaturation;
    float gVignetteMidpoint;
    float gVignetteRoundness;
    float gVignetteFeather;
    float3 gVignetteColor;
    float gBloomStrength;
    float gBloomBlendMode; // 0: Add, 1: Screen
    float gAOEnabled;          // 1.0 = enabled, 0.0 = disabled
    float gPostProcessShadow;  // 1.0 = debug multiply, 0.0 = forward shadows only (default)
    float gDithering;          // 1.0 = enabled
    float gUseLUT;             // 1.0 = sample 3D LUT instead of ColorGrade()
    float2 gLUTSize;           // x = 0.5/size, y = (size-1)/size (Filament pixel-center remap)
};

float interleavedGradientNoise(float2 screenPos)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(screenPos.xy, magic.xy)));
}

Texture2D<float4> gSrc;
Texture2D<float4> gBloom; // Mip 0 from bloom upsample
Texture2D<float>  gAO;     // Ambient occlusion (R channel, 0-1)
Texture2D<float>  gShadow; // Shadow visibility (R channel, 0-1)
Texture3D<float4> gColorLUT;
SamplerState gSampler;
SamplerState gLUTSampler;
RWTexture2D<float4> gDst;

// ACES Tone mapping (Narkowicz 2015)
float3 Tonemap_ACES(const float3 x) {
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Filmic Tone mapping (Hejl-Burgess-Dawson approximation)
float3 Tonemap_Filmic(const float3 x) {
    float3 X = max(float3(0.0f), x - 0.004f);
    float3 result = (X * (6.2f * X + 0.5f)) / (X * (6.2f * X + 1.7f) + 0.06f);
    return pow(result, 2.2f);
}

// Color Grading
float3 ColorGrade(float3 color) {
    // 1. Exposure
    color *= exp2(gExposure);

    // 2. Contrast
    color = saturate(color);
    color = pow(color, gContrast);

    // 3. Saturation & Luma
    float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    color = lerp(float3(luma), color, gSaturation);

    // 4. Vibrance (Simple approx)
    float maxColor = max(color.r, max(color.g, color.b));
    float minColor = min(color.r, min(color.g, color.b));
    float sat = maxColor - minColor;
    color = lerp(float3(luma), color, 1.0f + (gVibrance * (1.0f - (sign(maxColor) * sat))));

    return color;
}

// Filament-style 3D LUT sampling with pixel-center remap
float3 SampleColorLUT(float3 color)
{
    float3 coord = gLUTSize.x + saturate(color) * gLUTSize.y;
    return gColorLUT.SampleLevel(gLUTSampler, coord, 0).rgb;
}

// Pre-TAA pass: apply SSAO and shadow visibility to HDR color (Filament applies AO before TAA).
[numthreads(16, 16, 1)]
void colorGradingPrep(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution))
        return;

    int2 coord = int2(dispatchThreadId.xy);
    float4 src = gSrc.Load(int3(coord, 0));
    float3 color = src.rgb;
    float ambientWeight = src.a;

    if (gAOEnabled > 0.5f) {
        float2 uv = (float2(coord) + 0.5f) / float2(gResolution);
        float ao = gAO.SampleLevel(gSampler, uv, 0).r;
        ao = saturate(ao);
        color *= (1.0 - ambientWeight + ambientWeight * ao);
    }

    if (gPostProcessShadow > 0.5f) {
        float shadow = gShadow.Load(int3(coord, 0)).r;
        color *= shadow;
    }

    gDst[coord] = float4(color, ambientWeight);
}

[numthreads(16, 16, 1)]
void colorGradingMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution))
        return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5f) / float2(gResolution);
    
    float4 src = gSrc.Load(int3(coord, 0));
    float3 color = src.rgb;

    // Apply Bloom Composite
    if (gBloomStrength > 0.0f) {
        float3 bloomColor = gBloom.SampleLevel(gSampler, uv, 0).rgb * gBloomStrength;
        if (gBloomBlendMode == 1.0f) {
            // Screen blend
            color = 1.0f - (1.0f - color) * (1.0f - saturate(bloomColor));
        } else {
            // Additive blend
            color += bloomColor;
        }
    }

    // Apply Vignette
    if (gVignetteMidpoint > 0.0f) {
        float2 d = abs(uv - 0.5f) * gVignetteRoundness;
        float vfactor = length(d);
        vfactor = smoothstep(gVignetteMidpoint, gVignetteMidpoint + gVignetteFeather, vfactor);
        color = lerp(color, gVignetteColor, vfactor);
    }

    // Apply Color Grading (3D LUT path or real-time formula)
    if (gUseLUT > 0.5f) {
        color = SampleColorLUT(color);
    } else {
        color = ColorGrade(color);
    }

    // Apply Tone Mapping
    if (gToneMapping == 0.0f) {
        color = Tonemap_ACES(color);
    } else if (gToneMapping == 1.0f) {
        color = Tonemap_Filmic(color);
    } else if (gToneMapping == 3.0f) {
        // Display - assume already in display color space (simple sRGB gamma applied later)
        color = saturate(color);
    } // Linear is just pass-through

    // Filament dithering (reduce banding after tone map)
    if (gDithering > 0.5f) {
        float noise = interleavedGradientNoise(float2(coord)) / 255.0;
        color += noise - 0.5 / 255.0;
    }

    gDst[coord] = float4(saturate(color), 1.0f);
}
