cbuffer PerFrameCB
{
    uint2 gResolution;
};

Texture2D<float4> gSrc;
RWTexture2D<float4> gDst;
SamplerState gSampler;

#define FXAA_EDGE_THRESHOLD_MIN 0.0312f
#define FXAA_EDGE_THRESHOLD 0.125f
#define FXAA_SUBPIX_TRIM 1.0f/4.0f
#define FXAA_SUBPIX_TRIM_SCALE 1.0f
#define FXAA_SUBPIX_CAP 3.0f/4.0f
#define FXAA_SEARCH_STEPS 8
#define FXAA_SEARCH_ACCELERATION 1
#define FXAA_SEARCH_THRESHOLD 1.0f/4.0f

float luma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

[numthreads(16, 16, 1)]
void fxaaMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= gResolution)) return;
    int2 coord = int2(dispatchThreadId.xy);
    float2 inverseScreenSize = 1.0f / float2(gResolution);
    float2 uv = (float2(coord) + 0.5f) * inverseScreenSize;

    float3 colorCenter = gSrc.SampleLevel(gSampler, uv, 0).rgb;
    float lumaCenter = luma(colorCenter);

    float lumaDown = luma(gSrc.SampleLevel(gSampler, uv + float2(0, -1) * inverseScreenSize, 0).rgb);
    float lumaUp = luma(gSrc.SampleLevel(gSampler, uv + float2(0, 1) * inverseScreenSize, 0).rgb);
    float lumaLeft = luma(gSrc.SampleLevel(gSampler, uv + float2(-1, 0) * inverseScreenSize, 0).rgb);
    float lumaRight = luma(gSrc.SampleLevel(gSampler, uv + float2(1, 0) * inverseScreenSize, 0).rgb);

    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));

    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD)) {
        gDst[coord] = float4(colorCenter, 1.0f);
        return;
    }

    float lumaDownLeft = luma(gSrc.SampleLevel(gSampler, uv + float2(-1, -1) * inverseScreenSize, 0).rgb);
    float lumaUpRight = luma(gSrc.SampleLevel(gSampler, uv + float2(1, 1) * inverseScreenSize, 0).rgb);
    float lumaUpLeft = luma(gSrc.SampleLevel(gSampler, uv + float2(-1, 1) * inverseScreenSize, 0).rgb);
    float lumaDownRight = luma(gSrc.SampleLevel(gSampler, uv + float2(1, -1) * inverseScreenSize, 0).rgb);

    float lumaDownUp = lumaDown + lumaUp;
    float lumaLeftRight = lumaLeft + lumaRight;
    float lumaLeftCorners = lumaDownLeft + lumaUpLeft;
    float lumaDownCorners = lumaDownLeft + lumaDownRight;
    float lumaRightCorners = lumaDownRight + lumaUpRight;
    float lumaUpCorners = lumaUpRight + lumaUpLeft;

    float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) + abs(-2.0 * lumaCenter + lumaDownUp) * 2.0 + abs(-2.0 * lumaRight + lumaRightCorners);
    float edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) + abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0 + abs(-2.0 * lumaDown + lumaDownCorners);

    bool isHorizontal = (edgeHorizontal >= edgeVertical);

    float luma1 = isHorizontal ? lumaDown : lumaLeft;
    float luma2 = isHorizontal ? lumaUp : lumaRight;
    float gradient1 = luma1 - lumaCenter;
    float gradient2 = luma2 - lumaCenter;

    bool is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength = isHorizontal ? inverseScreenSize.y : inverseScreenSize.x;
    float lumaLocalAverage = 0.0;
    if (is1Steepest) {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    } else {
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
    }

    float2 currentUv = uv;
    if (isHorizontal) {
        currentUv.y += stepLength * 0.5;
    } else {
        currentUv.x += stepLength * 0.5;
    }

    float2 offset = isHorizontal ? float2(inverseScreenSize.x, 0.0) : float2(0.0, inverseScreenSize.y);
    float2 uv1 = currentUv - offset;
    float2 uv2 = currentUv + offset;

    float lumaEnd1 = luma(gSrc.SampleLevel(gSampler, uv1, 0).rgb) - lumaLocalAverage;
    float lumaEnd2 = luma(gSrc.SampleLevel(gSampler, uv2, 0).rgb) - lumaLocalAverage;

    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    if (!reachedBoth) {
        for (int i = 2; i < FXAA_SEARCH_STEPS; i++) {
            if (!reached1) lumaEnd1 = luma(gSrc.SampleLevel(gSampler, uv1, 0).rgb) - lumaLocalAverage;
            if (!reached2) lumaEnd2 = luma(gSrc.SampleLevel(gSampler, uv2, 0).rgb) - lumaLocalAverage;
            reached1 = abs(lumaEnd1) >= gradientScaled;
            reached2 = abs(lumaEnd2) >= gradientScaled;
            reachedBoth = reached1 && reached2;
            if (!reached1) uv1 -= offset * FXAA_SEARCH_ACCELERATION;
            if (!reached2) uv2 += offset * FXAA_SEARCH_ACCELERATION;
            if (reachedBoth) break;
        }
    }

    float distance1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float distance2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);

    bool isDirection1 = distance1 < distance2;
    float distanceFinal = min(distance1, distance2);

    float edgeThickness = (distance1 + distance2);
    float pixelOffset = -distanceFinal / edgeThickness + 0.5;
    bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
    bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
    float subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
    float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
    float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * FXAA_SUBPIX_CAP;

    finalOffset = max(finalOffset, subPixelOffsetFinal);

    float2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += finalOffset * stepLength;
    } else {
        finalUv.x += finalOffset * stepLength;
    }

    float3 finalColor = gSrc.SampleLevel(gSampler, finalUv, 0).rgb;
    gDst[coord] = float4(finalColor, 1.0f);
}
