cbuffer PerFrameCB
{
    uint2 gResolution;
    float gBloomIntensity;
    float gSSAORadius;
    float gToneMapping;
    
    float gDoFEnabled;
    float gDofFocalDistance;
    float gDofLensRadius;
    
    float gVignetteIntensity;
    
    float gColorGradingExposure;
    float gColorGradingContrast;
    float gColorGradingSaturation;
};

Texture2D<float4> gSrc;
Texture2D<float> gDepth;
RWTexture2D<float4> gDst;

// Utility functions
float3 rgb2hsv(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 hsv2rgb(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

[numthreads(16, 16, 1)]
void runPostFX(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution))
        return;

    int2 coord = int2(dispatchThreadId.xy);
    float2 uv = (float2(coord) + 0.5f) / float2(gResolution);
    
    float4 color = gSrc.Load(int3(coord, 0));
    float depth = gDepth.Load(int3(coord, 0)); // Using point load for depth

    // 1. Depth of Field (Very Fake Approximation)
    if (gDoFEnabled > 0.0f)
    {
        // Falcor depth is usually z/w (0..1) or linear depending on pass. Let's assume standard linear or raw depth for this approximation
        // Compute a simple CoC (Circle of Confusion)
        float coc = abs(depth - gDofFocalDistance) * gDofLensRadius * 0.1f;
        coc = clamp(coc, 0.0f, 5.0f); // Max blur radius
        
        if (coc > 0.5f)
        {
            float3 blurredColor = color.rgb;
            float weightSum = 1.0f;
            
            // Simple box blur for DoF
            int r = int(coc);
            for (int y = -r; y <= r; ++y)
            {
                for (int x = -r; x <= r; ++x)
                {
                    if (x == 0 && y == 0) continue;
                    int2 offsetCoord = clamp(coord + int2(x, y), int2(0, 0), int2(gResolution) - 1);
                    blurredColor += gSrc.Load(int3(offsetCoord, 0)).rgb;
                    weightSum += 1.0f;
                }
            }
            color.rgb = blurredColor / weightSum;
        }
    }

    // 2. SSAO (Fake screen space approximation using depth differences)
    if (gSSAORadius > 0.0f)
    {
        float ao = 0.0f;
        int r = max(1, int(gSSAORadius * 5.0f));
        float totalWeight = 0.0f;
        
        // Check surrounding depth values
        for (int i = 0; i < 4; ++i)
        {
            int2 offset = int2(
                (i == 0) ? r : ((i == 1) ? -r : 0),
                (i == 2) ? r : ((i == 3) ? -r : 0)
            );
            int2 offsetCoord = clamp(coord + offset, int2(0, 0), int2(gResolution) - 1);
            float d2 = gDepth.Load(int3(offsetCoord, 0));
            
            // If neighbor is significantly closer, it might occlude
            if (depth - d2 > 0.001f && depth - d2 < 0.1f)
            {
                ao += 1.0f;
            }
            totalWeight += 1.0f;
        }
        
        ao = 1.0f - (ao / totalWeight) * 0.5f; // Max 50% darkening
        color.rgb *= ao;
    }

    // 3. Bloom (Simple single-pass approximation - very hacky, real bloom needs downsample/upsample)
    if (gBloomIntensity > 0.0f)
    {
        // Extract bright parts
        float luminance = dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        if (luminance > 1.0f)
        {
            color.rgb += color.rgb * gBloomIntensity * 0.2f;
        }
    }

    // 4. Color Grading
    if (gColorGradingExposure != 1.0f || gColorGradingContrast != 1.0f || gColorGradingSaturation != 1.0f)
    {
        // Exposure
        color.rgb *= gColorGradingExposure;
        
        // Contrast
        color.rgb = clamp(lerp(float3(0.5f), color.rgb, gColorGradingContrast), 0.0f, 100.0f);
        
        // Saturation
        float3 hsv = rgb2hsv(color.rgb);
        hsv.y *= gColorGradingSaturation;
        color.rgb = hsv2rgb(hsv);
    }

    // 5. Vignette
    if (gVignetteIntensity > 0.0f)
    {
        float2 centeredUv = uv - 0.5f;
        float dist = length(centeredUv);
        float vignette = smoothstep(0.8f, 0.2f, dist * (1.0f + gVignetteIntensity));
        color.rgb *= vignette;
    }

    // 6. Tone mapping (ACES filmic approximation)
    if (gToneMapping > 0.0f)
    {
        float a = 2.51f;
        float b = 0.03f;
        float c = 2.43f;
        float d = 0.59f;
        float e = 0.14f;
        color.rgb = clamp((color.rgb * (a * color.rgb + b)) / (color.rgb * (c * color.rgb + d) + e), 0.0f, 1.0f);
    }

    // Output
    gDst[dispatchThreadId.xy] = color;
}