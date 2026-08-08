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

import Utils.Math.HashUtils;
import Utils.Math.MathHelpers;
import LumenGIData;
import RenderPasses.LumenGI.Capture.LumenCardCaptureData;

// Falcor's optional render-pass resources are specialized through
// is_valid_<shader resource name> defines. Keep local defaults so this shader
// can also be compiled before all optional channels are reflected by the host.
#ifndef is_valid_gCards
#define is_valid_gCards 0
#endif
#ifndef is_valid_gCardCoverage
#define is_valid_gCardCoverage 0
#endif
#ifndef is_valid_gLinearZ
#define is_valid_gLinearZ 0
#endif
#ifndef is_valid_gMotionVector
#define is_valid_gMotionVector 0
#endif
#ifndef is_valid_gMotionVectorW
#define is_valid_gMotionVectorW 0
#endif
#ifndef is_valid_gNormalRoughnessMaterialID
#define is_valid_gNormalRoughnessMaterialID 0
#endif
#ifndef is_valid_gConfidence
#define is_valid_gConfidence 0
#endif
#ifndef is_valid_gDiffuseRadianceHitDist
#define is_valid_gDiffuseRadianceHitDist 0
#endif
#ifndef is_valid_gLightingComponents
#define is_valid_gLightingComponents 0
#endif

#define is_valid(name) (is_valid_##name != 0)

cbuffer CB
{
    uint2 gFrameDim;
    uint gFrameIndex;
    uint gDebugMode;
    float4x4 gViewProj;      //< Camera view-projection (mul(M, v) convention), for the cards overlay.
    uint gCardCount;         //< Number of valid cards in gCards (0 when the overlay is disabled).
    float gCardCoverageValue; //< R channel of the cardCoverage output: allocated / total pages.
    float gAllocatedPages;   //< G channel: allocated page count.
    float gTotalPages;       //< B channel: total page capacity.
    float gFreePages;        //< A channel: free page count.
}

StructuredBuffer<LumenCard> gCards;

Texture2D<float2> gLinearZ;
Texture2D<float2> gMotionVector;
Texture2D<float4> gMotionVectorW;
Texture2D<float4> gNormalRoughnessMaterialID;
Texture2D<float> gConfidence;
Texture2D<float4> gDiffuseRadianceHitDist;
Texture2D<float4> gLightingComponents;

RWTexture2D<float4> gDebugOutput;
RWTexture2D<float> gCardCoverage;

static const float4 kMissingInputColor = float4(1.f, 0.f, 1.f, 1.f);

bool containsPixel(Texture2D<float> texture, uint2 pixel)
{
    uint width, height;
    texture.GetDimensions(width, height);
    return all(pixel < uint2(width, height));
}

bool containsPixel(Texture2D<float2> texture, uint2 pixel)
{
    uint width, height;
    texture.GetDimensions(width, height);
    return all(pixel < uint2(width, height));
}

bool containsPixel(Texture2D<float4> texture, uint2 pixel)
{
    uint width, height;
    texture.GetDimensions(width, height);
    return all(pixel < uint2(width, height));
}

float3 pseudocolor(uint value)
{
    const uint hash = jenkinsHash(value);
    return float3(uint3(hash, hash >> 8, hash >> 16) & 0xffu) / 255.f;
}

/** Monotonic Reinhard-style tonemap so HDR radiance views fit in the
    RGBA16F debug output without host-side exposure tuning. */
float3 tonemapRadiance(const float3 radiance)
{
    return radiance / (1.f + radiance);
}

/** True for pixels whose ray missed the scene, as encoded by the hit
    distance written by the miss shader (kLumenGIMissHitDistance). */
bool isMissPixel(const float4 radianceHitDist)
{
    return radianceHitDist.a >= kLumenGIMissHitDistance;
}

float4 visualizeNormal(uint2 pixel)
{
    if (is_valid(gNormalRoughnessMaterialID))
    {
        if (!containsPixel(gNormalRoughnessMaterialID, pixel))
            return kMissingInputColor;

        const float2 octNormal = gNormalRoughnessMaterialID[pixel].xy;
        const float3 normalW = oct_to_ndir_unorm(octNormal);
        return float4(normalW * 0.5f + 0.5f, 1.f);
    }

    return kMissingInputColor;
}

float4 visualizeLinearDepth(uint2 pixel)
{
    if (is_valid(gLinearZ))
    {
        if (!containsPixel(gLinearZ, pixel))
            return kMissingInputColor;

        // Linear Z has a large scene-dependent range. Log encoding keeps both
        // near-field geometry and distant surfaces visible without host tuning.
        const float linearDepth = max(abs(gLinearZ[pixel].x), 0.f);
        const float normalizedDepth = saturate(log2(1.f + linearDepth) / 16.f);
        return float4(normalizedDepth.xxx, 1.f);
    }

    return kMissingInputColor;
}

float4 visualizeMotion(uint2 pixel)
{
    if (is_valid(gMotionVector))
    {
        if (!containsPixel(gMotionVector, pixel))
            return kMissingInputColor;

        const float2 motion = gMotionVector[pixel];
        const float2 signedMotion = saturate(0.5f.xx + motion * 16.f);
        const float magnitude = saturate(length(motion) * 32.f);
        return float4(signedMotion, magnitude, 1.f);
    }

    if (is_valid(gMotionVectorW))
    {
        if (!containsPixel(gMotionVectorW, pixel))
            return kMissingInputColor;

        const float3 motionW = gMotionVectorW[pixel].xyz;
        return float4(saturate(motionW * 0.5f + 0.5f), 1.f);
    }

    return kMissingInputColor;
}

float4 visualizeMaterialID(uint2 pixel)
{
    if (is_valid(gNormalRoughnessMaterialID))
    {
        if (!containsPixel(gNormalRoughnessMaterialID, pixel))
            return kMissingInputColor;

        // GBufferRT stores the material ID in the two-bit alpha channel of
        // RGB10A2Unorm. Convert the normalized value back to its integer code.
        const uint materialID = uint(round(saturate(gNormalRoughnessMaterialID[pixel].w) * 3.f));
        return float4(pseudocolor(materialID), 1.f);
    }

    return kMissingInputColor;
}

float4 visualizeConfidence(uint2 pixel)
{
    if (is_valid(gConfidence))
    {
        if (!containsPixel(gConfidence, pixel))
            return kMissingInputColor;

        const float confidence = saturate(gConfidence[pixel]);
        return float4(confidence.xxx, 1.f);
    }

    return kMissingInputColor;
}

/** Direct lighting of the secondary hit: the total hit radiance (material
    emission plus analytic next-event lighting), excluding the environment
    contribution which only exists on miss pixels. */
float4 visualizeDirectLighting(uint2 pixel)
{
    if (is_valid(gDiffuseRadianceHitDist))
    {
        if (!containsPixel(gDiffuseRadianceHitDist, pixel))
            return kMissingInputColor;

        const float4 radianceHitDist = gDiffuseRadianceHitDist[pixel];
        const float3 radiance = isMissPixel(radianceHitDist) ? float3(0.f) : radianceHitDist.rgb;
        return float4(tonemapRadiance(radiance), 1.f);
    }

    return kMissingInputColor;
}

/** Environment contribution: the total radiance on miss pixels only. */
float4 visualizeEnvironmentOnly(uint2 pixel)
{
    if (is_valid(gDiffuseRadianceHitDist))
    {
        if (!containsPixel(gDiffuseRadianceHitDist, pixel))
            return kMissingInputColor;

        const float4 radianceHitDist = gDiffuseRadianceHitDist[pixel];
        const float3 radiance = isMissPixel(radianceHitDist) ? radianceHitDist.rgb : float3(0.f);
        return float4(tonemapRadiance(radiance), 1.f);
    }

    return kMissingInputColor;
}

/** Emissive contribution: the total hit radiance minus the analytic
    component. This derivation is exact because the hit radiance is the sum
    of the emissive and analytic components (both are stored post-clamp). */
float4 visualizeEmissiveOnly(uint2 pixel)
{
    if (is_valid(gDiffuseRadianceHitDist) && is_valid(gLightingComponents))
    {
        if (!containsPixel(gDiffuseRadianceHitDist, pixel) || !containsPixel(gLightingComponents, pixel))
            return kMissingInputColor;

        const float4 radianceHitDist = gDiffuseRadianceHitDist[pixel];
        const float3 radiance = isMissPixel(radianceHitDist) ? float3(0.f) : radianceHitDist.rgb - gLightingComponents[pixel].rgb;
        return float4(tonemapRadiance(max(radiance, float3(0.f))), 1.f);
    }

    return kMissingInputColor;
}

/** Analytic next-event contribution at the secondary hit. */
float4 visualizeAnalyticOnly(uint2 pixel)
{
    if (is_valid(gLightingComponents))
    {
        if (!containsPixel(gLightingComponents, pixel))
            return kMissingInputColor;

        return float4(tonemapRadiance(gLightingComponents[pixel].rgb), 1.f);
    }

    return kMissingInputColor;
}

/** Total indirect radiance (hit and miss pixels). */
float4 visualizeIndirectOnly(uint2 pixel)
{
    if (is_valid(gDiffuseRadianceHitDist))
    {
        if (!containsPixel(gDiffuseRadianceHitDist, pixel))
            return kMissingInputColor;

        return float4(tonemapRadiance(gDiffuseRadianceHitDist[pixel].rgb), 1.f);
    }

    return kMissingInputColor;
}

/** Binary mask of pixels whose sample was clamped by the radiance ceiling. */
float4 visualizeFireflyMask(uint2 pixel)
{
    if (is_valid(gLightingComponents))
    {
        if (!containsPixel(gLightingComponents, pixel))
            return kMissingInputColor;

        const float mask = gLightingComponents[pixel].a > 0.f ? 1.f : 0.f;
        return float4(mask.xxx, 1.f);
    }

    return kMissingInputColor;
}

/** Project a world-space point to viewport pixel coordinates through the
    camera view-projection. Returns false when the point is behind the camera
    (its clip-space w is non-positive), in which case screenPos is undefined.
*/
bool projectToScreen(float3 worldPos, out float2 screenPos)
{
    const float4 clip = mul(gViewProj, float4(worldPos, 1.f));
    if (clip.w <= 1e-6f)
        return false;
    const float3 ndc = clip.xyz / clip.w;
    screenPos = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * float2(gFrameDim);
    return true;
}

/** Distance from a point to a 2D segment (pixel units). */
float distanceToSegment(float2 p, float2 a, float2 b)
{
    const float2 ab = b - a;
    const float len2 = dot(ab, ab);
    if (len2 <= 1e-8f)
        return length(p - a);
    const float t = saturate(dot(p - a, ab) / len2);
    return length(p - (a + t * ab));
}

/** Card placement overlay (S2 gate): wireframe boxes around each card-ized
    instance's world AABB, drawn over a dimmed tonemapped radiance backdrop so
    the placement is verifiable against the rendered geometry.

    Deterministic and exact: every AABB is projected with the camera
    view-projection and the box edges are rasterized as 2D segments in pixel
    space. All six faces of an instance share the same world AABB (per-face
    slab refinement is future work), so only face 0 of each instance is drawn
    to avoid six identical overdraws. No randomness anywhere.
*/
float4 visualizeCardsOverlay(uint2 pixel)
{
    if (!is_valid(gCards) || gCardCount == 0u)
        return kMissingInputColor;

    // Dimmed tonemapped radiance gives geometric context for the overlay.
    float3 background = float3(0.f);
    if (is_valid(gDiffuseRadianceHitDist) && containsPixel(gDiffuseRadianceHitDist, pixel))
        background = 0.25f * tonemapRadiance(gDiffuseRadianceHitDist[pixel].rgb);

    const float lineWidth = 1.5f;
    const float2 pixelF = float2(pixel) + 0.5f; //< Pixel-center in the projection's pixel space.

    for (uint c = 0; c < gCardCount; ++c)
    {
        const LumenCard card = gCards[c];
        if (card.faceIndex != 0u)
            continue;

        const float3 corners[8] = {
            float3(card.boundsMin.x, card.boundsMin.y, card.boundsMin.z),
            float3(card.boundsMax.x, card.boundsMin.y, card.boundsMin.z),
            float3(card.boundsMin.x, card.boundsMax.y, card.boundsMin.z),
            float3(card.boundsMax.x, card.boundsMax.y, card.boundsMin.z),
            float3(card.boundsMin.x, card.boundsMin.y, card.boundsMax.z),
            float3(card.boundsMax.x, card.boundsMin.y, card.boundsMax.z),
            float3(card.boundsMin.x, card.boundsMax.y, card.boundsMax.z),
            float3(card.boundsMax.x, card.boundsMax.y, card.boundsMax.z),
        };

        float2 screen[8];
        bool behind = false;
        for (int i = 0; i < 8; ++i)
        {
            if (!projectToScreen(corners[i], screen[i]))
            {
                behind = true;
                break;
            }
        }
        if (behind)
            continue;

        float2 lo = min(
            min(min(screen[0], screen[1]), min(screen[2], screen[3])),
            min(min(screen[4], screen[5]), min(screen[6], screen[7]))
        );
        float2 hi = max(
            max(max(screen[0], screen[1]), max(screen[2], screen[3])),
            max(max(screen[4], screen[5]), max(screen[6], screen[7]))
        );
        if (any(pixelF < lo - lineWidth) || any(pixelF > hi + lineWidth))
            continue;

        const uint2 edges[12] = {
            uint2(0, 1), uint2(2, 3), uint2(4, 5), uint2(6, 7), // x-direction edges
            uint2(0, 2), uint2(1, 3), uint2(4, 6), uint2(5, 7), // y-direction edges
            uint2(0, 4), uint2(1, 5), uint2(2, 6), uint2(3, 7), // z-direction edges
        };
        for (int e = 0; e < 12; ++e)
        {
            if (distanceToSegment(pixelF, screen[edges[e].x], screen[edges[e].y]) <= lineWidth)
                return float4(pseudocolor(c) * 0.85f + background * 0.15f, 1.f);
        }
    }

    return float4(background, 1.f);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= gFrameDim))
        return;

    float4 output = float4(0.f, 0.f, 0.f, 1.f);

    switch (gDebugMode)
    {
    case kLumenDebugNone:
        break;
    case kLumenDebugNormal:
        output = visualizeNormal(pixel);
        break;
    case kLumenDebugLinearDepth:
        output = visualizeLinearDepth(pixel);
        break;
    case kLumenDebugMotion:
        output = visualizeMotion(pixel);
        break;
    case kLumenDebugMaterialID:
        output = visualizeMaterialID(pixel);
        break;
    case kLumenDebugConfidence:
        output = visualizeConfidence(pixel);
        break;
    case kLumenDebugDirectLighting:
        output = visualizeDirectLighting(pixel);
        break;
    case kLumenDebugEnvironmentOnly:
        output = visualizeEnvironmentOnly(pixel);
        break;
    case kLumenDebugEmissiveOnly:
        output = visualizeEmissiveOnly(pixel);
        break;
    case kLumenDebugAnalyticOnly:
        output = visualizeAnalyticOnly(pixel);
        break;
    case kLumenDebugIndirectOnly:
        output = visualizeIndirectOnly(pixel);
        break;
    case kLumenDebugFireflyMask:
        output = visualizeFireflyMask(pixel);
        break;
    case kLumenDebugCardsOverlay:
        output = visualizeCardsOverlay(pixel);
        break;
    default:
        output = kMissingInputColor;
        break;
    }

    gDebugOutput[pixel] = output;

    // Scriptable page-coverage channel: the same scalar is written to every
    // pixel so Python can read it with get_output().to_numpy(). R32Float to
    // avoid half-float encoding ambiguity in the diagnostic channel.
    if (is_valid(gCardCoverage))
        gCardCoverage[pixel] = gCardCoverageValue;
}
