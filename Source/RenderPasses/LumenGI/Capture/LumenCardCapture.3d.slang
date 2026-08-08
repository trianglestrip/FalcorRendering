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

// LumenGI card capture pass (S2-C1, Agent C).
//
// A raster (3D) pass that re-renders one card's mesh into the page texel
// region of the Surface Cache atlases using a per-card orthographic camera.
// The host draws one card per draw call with:
//   - vertex/index buffers and TLAS geometry of the card's instance
//     (instanceID comes from the DRAW_ID semantic, equal to the scene
//     geometry instance ID stored in LumenCard::instanceID),
//   - a viewport set to
//     (texelX, texelY, kLumenSurfaceCacheTileSize, kLumenSurfaceCacheTileSize)
//     of the atlases, derived from the page ID with the Agent B formula,
//   - per-draw constants (LumenCardCaptureCB) identifying the card and page.
//
// The pixel shader evaluates the Falcor material system exactly like
// GBufferRaster (see Source/RenderPasses/GBuffer/GBuffer/GBufferRaster.3d.slang)
// and writes:
//   - material atlas   (RGBA8):   base color + opacity
//   - radiance atlas   (RGBA16F): cleared to zero (stale radiance erased)
//   - metadata atlas   (RGBA16F): linear depth, validity flags, normal octahedral
//   - params atlas     (RGBA16F): roughness + emissive (Agent C extension)
//
// Channel layouts and constants are frozen in LumenCardCaptureData.slang.

import Scene.Raster;
import Scene.Shading;
import Scene.Material.TextureSampler;
import Utils.Math.MathHelpers;
import Rendering.Materials.IMaterialInstance;
import Rendering.Materials.LobeType;
import LumenCardCaptureData;

// Falcor only emits is_valid_<name> defines for OPTIONAL channels. Required
// resources (gCards, gMaterialAtlas, gMetadataAtlas) are therefore defaulted
// to 1 here; optional ones (radiance/params/debug) default to 0 so the file
// is statically compilable before the host binds anything.
#ifndef is_valid_gCards
#define is_valid_gCards 1
#endif
#ifndef is_valid_gMaterialAtlas
#define is_valid_gMaterialAtlas 1
#endif
#ifndef is_valid_gMetadataAtlas
#define is_valid_gMetadataAtlas 1
#endif
#ifndef is_valid_gRadianceAtlas
#define is_valid_gRadianceAtlas 0
#endif
#ifndef is_valid_gMaterialParamsAtlas
#define is_valid_gMaterialParamsAtlas 0
#endif
#ifndef is_valid_gDebugTexture
#define is_valid_gDebugTexture 0
#endif

#define is_valid(name) (is_valid_##name != 0)

// Per-draw capture parameters. Field layout is documented in
// LumenCardCaptureData.slang. The host sets the viewport to the page texel
// region so SV_Position.xy is the atlas coordinate.
cbuffer LumenCardCaptureCB
{
    uint  gCardIndex;       ///< Index into gCards (host LumenCardScene card index).
    uint  gPageID;          ///< Page ID assigned by the allocator; 0 disables the draw.
    uint  gPagesPerSide;    ///< Tiles per side of the atlas grid (N in the pageID formula).
    uint2 gPageTexelOrigin; ///< (texelX, texelY) of the page, Agent B formula.
    float gNearMargin;      ///< Camera distance in front of the front face (meters).
};

StructuredBuffer<LumenCard> gCards;

RWTexture2D<float4> gMaterialAtlas;      // RGBA8: base color + opacity.
RWTexture2D<float4> gRadianceAtlas;      // RGBA16F: cleared to zero on capture.
RWTexture2D<float4> gMetadataAtlas;      // RGBA16F: depth, validity, normal octahedral.
RWTexture2D<float4> gMaterialParamsAtlas; // RGBA16F: roughness + emissive.
RWTexture2D<float4> gDebugTexture;       // Optional validity/fallback debug view.

#if !defined(INTERPOLATION_MODE)
#define INTERPOLATION_MODE linear
#endif

/** VS output. Mirrors the field set of Scene/Raster.slang's VSOut and adds
    viewDepth (linear depth from the card camera, exact under ortho).
*/
struct LumenCardCaptureVSOut
{
    INTERPOLATION_MODE float3 normalW    : NORMAL;   ///< Shading normal in world space (not normalized!).
    INTERPOLATION_MODE float4 tangentW   : TANGENT;  ///< Shading tangent in world space (not normalized!).
    INTERPOLATION_MODE float2 texC       : TEXCRD;   ///< Texture coordinate.
    INTERPOLATION_MODE float3 posW       : POSW;     ///< Position in world space.
    INTERPOLATION_MODE float  viewDepth  : VIEWDEPTH; ///< Linear depth from the card camera plane (meters).

    nointerpolation GeometryInstanceID instanceID : INSTANCE_ID;
    nointerpolation uint materialID              : MATERIAL_ID;

    float4 posH : SV_POSITION;
};

/** Camera position in front of the card's front face.
    The camera is at center - viewDir * (0.5*extent[axis] + nearMargin), so
    the front face lies nearMargin meters in front of the camera and the box
    spans [nearMargin, nearMargin + extent[axis]] along the view axis.
*/
float3 lumenCardCameraPosition(const LumenCard card, float3 viewDir, float nearMargin)
{
    const uint axis = lumenCardFaceAxis(card.faceIndex);
    const float halfDepth = 0.5f * lumenCardComponent(card.extent, axis);
    return card.center - viewDir * (halfDepth + nearMargin);
}

/** Orthographic clip position of a world-space point for the card camera.
    x/y span the card face (extent[t0]/extent[t1]), z is mapped to [0,1]
    over [nearMargin, nearMargin + extent[axis]]. Degenerate cards (zero
    extent on any used axis) return a degenerate position (w = 0) so nothing
    rasterizes.
*/
float4 lumenCardProject(const LumenCard card, float3 viewDir, float3 right, float3 up, float3 posW, float nearMargin)
{
    const uint2 axes = lumenCardFacePerpendicularAxes(card.faceIndex);
    const uint axis = lumenCardFaceAxis(card.faceIndex);
    const float halfW = 0.5f * lumenCardComponent(card.extent, axes.x);
    const float halfH = 0.5f * lumenCardComponent(card.extent, axes.y);
    const float depthRange = lumenCardComponent(card.extent, axis) + 2.f * nearMargin;

    if (halfW <= 1e-6f || halfH <= 1e-6f || depthRange <= 1e-6f)
        return float4(0.f, 0.f, 0.f, 0.f);

    const float3 camPos = lumenCardCameraPosition(card, viewDir, nearMargin);
    const float3 d = posW - camPos;

    const float clipX = dot(d, right) / halfW;
    const float clipY = dot(d, up) / halfH;
    const float clipZ = dot(d, viewDir) / depthRange; // [0,1] for points inside the box.
    return float4(clipX, clipY, clipZ, 1.f);
}

LumenCardCaptureVSOut vsMain(VSIn vIn)
{
    LumenCardCaptureVSOut vOut;

    const GeometryInstanceID instanceID = { vIn.instanceID };

    // No card/page: emit a degenerate triangle so nothing rasterizes.
    if (!is_valid(gCards) || gPageID == kInvalidPageID)
    {
        vOut.posH = float4(0.f, 0.f, 0.f, 0.f);
        vOut.posW = float3(0.f);
        vOut.viewDepth = 0.f;
        vOut.normalW = float3(0.f);
        vOut.tangentW = float4(0.f);
        vOut.texC = float2(0.f);
        vOut.instanceID = instanceID;
        vOut.materialID = gScene.getMaterialID(instanceID);
        return vOut;
    }

    const LumenCard card = gCards[gCardIndex];

    // The host normally sets gNearMargin; fall back to the frozen default
    // when the cbuffer value was never written.
    const float nearMargin = gNearMargin > 0.f ? gNearMargin : kLumenCardCaptureDefaultNearMargin;

    float3 viewDir, right, up;
    lumenCardFaceBasis(card.faceIndex, viewDir, right, up);

    float4x4 worldMat = gScene.getWorldMatrix(instanceID);
    float3 posW = mul(worldMat, float4(vIn.pos, 1.f)).xyz;

    vOut.posH = lumenCardProject(card, viewDir, right, up, posW, nearMargin);
    vOut.posW = posW;
    vOut.viewDepth = dot(posW - lumenCardCameraPosition(card, viewDir, nearMargin), viewDir);
    vOut.normalW = mul(gScene.getInverseTransposeWorldMatrix(instanceID), vIn.unpack().normal);
    float4 tangent = vIn.unpack().tangent;
    vOut.tangentW = float4(mul((float3x3)worldMat, tangent.xyz), tangent.w);
    vOut.texC = vIn.texC;
    vOut.instanceID = instanceID;
    vOut.materialID = gScene.getMaterialID(instanceID);

    return vOut;
}

/** Debug color for the validity/fallback debug view.
    Green = valid, yellow = alpha-test fallback, red = unsupported fallback,
    black = no surface (never written).
*/
float4 lumenCardDebugColor(uint flags)
{
    if (flags == kLumenCardMetadataFlagValid)
        return float4(0.f, 1.f, 0.f, 1.f);
    if (flags & kLumenCardMetadataFlagFallbackAlphaTest)
        return float4(1.f, 1.f, 0.f, 1.f);
    if (flags & kLumenCardMetadataFlagFallbackUnsupported)
        return float4(1.f, 0.f, 0.f, 1.f);
    return float4(0.f, 0.f, 0.f, 1.f);
}

void psMain(LumenCardCaptureVSOut vOut, uint triangleIndex : SV_PrimitiveID)
{
    if (!is_valid(gCards) || !is_valid(gMaterialAtlas) || !is_valid(gMetadataAtlas))
        return;
    if (gPageID == kInvalidPageID)
        return;

    const int2 ipos = int2(vOut.posH.xy);

    // Interpolated view depth is exact under the orthographic projection.
    const float depth = vOut.viewDepth;

    // Setup the interpolated vertex data (same fields as
    // Scene/Raster.slang's prepareVertexData()).
    float3 faceNormalW = gScene.getFaceNormalW(vOut.instanceID, triangleIndex);
    VertexData v;
    v.posW = vOut.posW;
    v.texC = vOut.texC;
    v.normalW = normalize(vOut.normalW);
    v.faceNormalW = faceNormalW;
    v.tangentW.xyz = normalize(vOut.tangentW.xyz);
    v.tangentW.w = sign(vOut.tangentW.w);
    v.curveRadius = 0.f;
    v.coneTexLODValue = 0.f;

    // Clear stale radiance whenever the page is (re)captured.
    if (is_valid(gRadianceAtlas))
        gRadianceAtlas[ipos] = float4(0.f);

    // Alpha test: a failing texel is written with opacity 0 and the
    // alpha-test fallback marker instead of being discarded, so the
    // lighting pass can route it to hit lighting (Lumen convention).
    let lod = ExplicitLodTextureSampler(kLumenCardCaptureTexLOD);
    if (gScene.materials.alphaTest(v, vOut.materialID, lod))
    {
        gMaterialAtlas[ipos] = float4(0.f, 0.f, 0.f, 0.f);
        gMetadataAtlas[ipos] = float4(
            depth,
            float(kLumenCardMetadataFlagFallbackAlphaTest),
            0.f, 0.f
        );
        if (is_valid(gMaterialParamsAtlas))
            gMaterialParamsAtlas[ipos] = float4(0.f);
        if (is_valid(gDebugTexture))
            gDebugTexture[ipos] = lumenCardDebugColor(kLumenCardMetadataFlagFallbackAlphaTest);
        return;
    }

    // View direction points from the hit toward the camera (-rayDir); the
    // card camera looks along viewDir.
    float3 viewDir, right, up;
    lumenCardFaceBasis(gCards[gCardIndex].faceIndex, viewDir, right, up);
    const float3 V = -viewDir;

    ShadingData sd = gScene.materials.prepareShadingData(v, vOut.materialID, V);

    // Backfaces of single-sided materials are not part of the surface and
    // must not be captured (equivalent to back-face culling; the host may
    // also cull them).
    if (!sd.frontFacing && !sd.mtl.isDoubleSided())
        return;

    // Create the material instance and query its properties, exactly like
    // GBufferRaster. For double-sided materials the material system flips
    // the shading frame for backfacing hits internally (flipShadingNormal in
    // setupMaterialInstance), so the guide normal below is already oriented
    // toward the card camera.
    let mi = gScene.materials.getMaterialInstance(sd, lod, 0);
    let props = mi.getProperties(sd);

    // Unsupported material: no diffuse reflection lobe (e.g. delta-specular
    // or volume-only surfaces) cannot be represented in the material atlas;
    // store what we have and mark the texel as unsupported fallback.
    const uint lobeTypes = mi.getLobeTypes(sd);
    const bool unsupported = (lobeTypes & (uint)LobeType::DiffuseReflection) == 0u;
    const uint flags = unsupported
        ? (kLumenCardMetadataFlagValid | kLumenCardMetadataFlagFallbackUnsupported)
        : kLumenCardMetadataFlagValid;

    // Material atlas: base color + opacity. Opacity is 1.0 for captured
    // opaque surfaces; alpha-tested texels are handled above with opacity 0.
    gMaterialAtlas[ipos] = float4(props.diffuseReflectionAlbedo, 1.f);

    // Metadata atlas: linear depth, validity flags, octahedral normal.
    const float2 octNormal = ndir_to_oct_unorm(props.guideNormal);
    gMetadataAtlas[ipos] = float4(depth, float(flags), octNormal);

    // Params atlas (Agent C extension): roughness + emissive.
    if (is_valid(gMaterialParamsAtlas))
        gMaterialParamsAtlas[ipos] = float4(props.roughness, props.emission);

    if (is_valid(gDebugTexture))
        gDebugTexture[ipos] = lumenCardDebugColor(flags);
}
