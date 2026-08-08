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

// Card placement pass (S2-A1, placeholder stage).
//
// Consumes the CPU-side card list produced by LumenCardScene.h and writes one
// LumenCardPlacementOutput record per card: the world AABB min/max, the bounding
// sphere, and an estimated on-screen size. The pass is not wired into the
// RenderGraph yet; the host (root) binds the resources described below.
//
// Resource layout expected by this pass:
// - gCards:        StructuredBuffer<LumenCard>, element count == host getCardCount(),
//                  96 bytes per element. Must match struct LumenCard in LumenCardScene.h
//                  byte for byte (offsets asserted host-side).
// - gPlacement:    RWStructuredBuffer<LumenCardPlacementOutput>, same element count and
//                  index space as gCards.
// - gDebugTexture: optional RWTexture2D<float4> for per-card pseudocolor; skipped when
//                  not bound.
// - CB:            gCameraPosition (float3, world space), gTanHalfFov (float, tan of half
//                  the vertical FOV), gViewportHeightPx (float), gCardCount (uint). Screen-size
//                  estimation is disabled when gTanHalfFov or gViewportHeightPx is zero.
//
// Coordinate conventions (frozen for agents B and C):
// - World space, Y-up, meters, matching the Falcor Scene conventions.
// - Card bounds are world-space AABBs stored as float3 min/max (boundsMin/boundsMax).
// - Card face orientation: faceIndex 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z; the face normal
//   axis is faceIndex >> 1 and the positive axis direction is (faceIndex & 1) == 0.
//   The capture shader uses faceIndex to pick the projection direction.
// - cardIndex = instanceIndex * kLumenCardFaceCount + faceIndex, where instanceIndex
//   is the LumenCardScene instance ordinal (see LumenCardScene.h).

// Falcor's optional render-pass resources are specialized through
// is_valid_<shader resource name> defines. Keep local defaults so this shader
// compiles before any resources are bound by the host.
#ifndef is_valid_gCards
#define is_valid_gCards 0
#endif
#ifndef is_valid_gPlacement
#define is_valid_gPlacement 0
#endif
#ifndef is_valid_gDebugTexture
#define is_valid_gDebugTexture 0
#endif

#define is_valid(name) (is_valid_##name != 0)

/** Number of cards generated per supported static mesh instance.
    Must match kLumenCardFaceCount in LumenCardScene.h.
*/
static const uint kLumenCardFaceCount = 6;

/** One card as produced by the host. 96-byte layout:
    +0   meshID (uint), instanceID (uint), faceIndex (uint), dirtyFlags (uint)
    +16  boundsMin (float3), +32 boundsMax (float3), +48 center (float3),
    +64  extent (float3), +80 priority (float), +84 padding.
    instanceID is the scene geometry instance ID and equals the TLAS instance index.
    dirtyFlags is host bookkeeping and is expected to be zero after capture.
*/
struct LumenCard
{
    uint meshID;
    uint instanceID;
    uint faceIndex;
    uint dirtyFlags;
    float3 boundsMin;
    float3 boundsMax;
    float3 center;
    float3 extent;
    float priority;
};

/** Per-card placement result, one record per input card in the same index space.
    size: 96 bytes (float3 members are 16-byte aligned in the GPU layout).
*/
struct LumenCardPlacementOutput
{
    float3 boundsMin;      ///< World AABB min (meters).
    float3 boundsMax;      ///< World AABB max (meters).
    float3 sphereCenter;   ///< Bounding sphere center, equals the AABB center.
    float sphereRadius;    ///< Bounding sphere radius, 0.5 * length(extent).
    float3 screenSizePx;   ///< Estimated on-screen size in pixels per AABB axis; zero when view params are disabled.
    float viewDistance;    ///< Distance from the camera to sphereCenter (meters); zero when disabled.
    uint cardIndex;        ///< Input card index.
    uint faceIndex;        ///< Card face, copied from the input for verification.
};

cbuffer CB
{
    float3 gCameraPosition;  ///< World-space camera position.
    float gTanHalfFov;       ///< tan(half vertical FOV); zero disables screen-size estimation.
    float gViewportHeightPx; ///< Viewport height in pixels; zero disables screen-size estimation.
    uint gCardCount;         ///< Number of valid cards in gCards.
}

StructuredBuffer<LumenCard> gCards;
RWStructuredBuffer<LumenCardPlacementOutput> gPlacement;
RWTexture2D<float4> gDebugTexture;

float3 debugPseudocolor(uint cardIndex, uint faceIndex)
{
    // Face axis color (0:x, 1:y, 2:z) modulated by the card index so adjacent
    // cards stay distinguishable.
    const uint axis = faceIndex >> 1;
    const float3 axisColor = axis == 0 ? float3(1.f, 0.f, 0.f) : (axis == 1 ? float3(0.f, 1.f, 0.f) : float3(0.f, 0.f, 1.f));
    const float shade = 0.25f + 0.75f * frac(float(cardIndex & 0xffu) / 255.f);
    return axisColor * shade;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (!is_valid(gCards) || !is_valid(gPlacement))
        return;

    const uint cardIndex = dispatchThreadID.x;
    if (cardIndex >= gCardCount)
        return;

    const LumenCard card = gCards[cardIndex];

    LumenCardPlacementOutput output;
    output.boundsMin = card.boundsMin;
    output.boundsMax = card.boundsMax;
    output.sphereCenter = card.center;
    output.sphereRadius = 0.5f * length(card.extent);
    output.cardIndex = cardIndex;
    output.faceIndex = card.faceIndex;

    // Estimate the on-screen size of each AABB axis. The world height of one
    // pixel at the card distance is 2 * tan(fov/2) * distance / viewportHeight.
    // The estimate ignores orientation and perspective; it only feeds capture
    // priority / resolution heuristics.
    if (gTanHalfFov > 0.f && gViewportHeightPx > 0.f)
    {
        const float viewDistance = max(length(card.center - gCameraPosition), 1e-6f);
        const float metersPerPixel = (2.f * gTanHalfFov * viewDistance) / gViewportHeightPx;
        output.viewDistance = viewDistance;
        output.screenSizePx = card.extent / max(metersPerPixel, 1e-6f);
    }
    else
    {
        output.viewDistance = 0.f;
        output.screenSizePx = float3(0.f);
    }

    gPlacement[cardIndex] = output;

    if (is_valid(gDebugTexture))
    {
        uint2 texDim;
        gDebugTexture.GetDimensions(texDim.x, texDim.y);
        const uint width = max(texDim.x, 1u);
        const uint height = max(texDim.y, 1u);
        const uint2 pixel = uint2(cardIndex % width, (cardIndex / width) % height);
        gDebugTexture[pixel] = float4(debugPseudocolor(cardIndex, card.faceIndex), 1.f);
    }
}
