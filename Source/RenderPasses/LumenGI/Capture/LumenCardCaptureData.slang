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

// LumenGI card capture shared data (S2-C1, Agent C).
//
// This module is the single source of truth for the card-capture pass
// (LumenCardCapture.3d.slang) and is consumed by the Surface Cache lighting
// pass (S3, Agent B) to decode what the capture wrote. Nothing here is owned
// by the host; the host binds the buffers described below. The constants
// mirror the two frozen CPU contracts byte for byte:
//
//   - Source/RenderPasses/LumenGI/Cards/LumenCardScene.h      (Agent A)
//   - Source/RenderPasses/LumenGI/SurfaceCache/LumenSurfaceCache.h (Agent B)
//
// Coordinate conventions (frozen here, shared with A and B):
//   - World space, Y-up, meters, matching the Falcor Scene conventions.
//   - Card face index: 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z. The capture camera is
//     placed outside the card's AABB on the face side and looks toward the
//     opposite direction (Lumen convention: the card "sees" the box from the
//     face normal direction).
//   - cardIndex = instanceIndex * kLumenCardFaceCount + faceIndex, where
//     instanceIndex is the LumenCardScene instance ordinal (NOT the scene
//     geometry instance ID).

// ---------------------------------------------------------------------------
// Agent A contract: LumenCardScene.h
// ---------------------------------------------------------------------------

/** Number of cards per supported static mesh instance (one per axis direction).
    Must match kLumenCardFaceCount in LumenCardScene.h and
    LumenCardPlacement.cs.slang.
*/
static const uint kLumenCardFaceCount = 6;

/** Sentinel for invalid mesh, instance, and card indices.
    Must match kLumenCardInvalidID in LumenCardScene.h.
*/
static const uint kLumenCardInvalidID = 0xFFFFFFFFu;

/** The world-space axis a card face looks along: faceIndex >> 1
    (0=x, 1=y, 2=z). Must match the convention in LumenCardScene.h.
*/
uint lumenCardFaceAxis(uint faceIndex)
{
    return faceIndex >> 1;
}

/** True when the face is on the positive side of its axis
    (faceIndex 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z, i.e. (faceIndex & 1) == 0).
*/
bool lumenCardFaceIsPositive(uint faceIndex)
{
    return (faceIndex & 1u) == 0u;
}

/** One card as produced by LumenCardScene (Agent A) and consumed by the
    placement pass. 96-byte layout, must match struct LumenCard in
    LumenCardScene.h and LumenCardPlacement.cs.slang byte for byte:
      +0   meshID (uint), instanceID (uint), faceIndex (uint), dirtyFlags (uint)
      +16  boundsMin (float3), +32 boundsMax (float3), +48 center (float3),
      +64  extent (float3), +80 priority (float), +84 padding.
    instanceID is the scene geometry instance ID and equals the TLAS instance
    index. The AABB (boundsMin/boundsMax) is the world-space AABB of the
    instance; all six cards of an instance share it (per-face slab refinement
    is future work). float3 members are 16-byte aligned in the GPU layout,
    which reproduces the host-side float4 offsets.
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

// ---------------------------------------------------------------------------
// Agent B contract: LumenSurfaceCache.h
// ---------------------------------------------------------------------------

/** Texels per page side. Every atlas tile is
    kLumenSurfaceCacheTileSize x kLumenSurfaceCacheTileSize texels.
    Must match kLumenSurfaceCacheTileSize in LumenSurfaceCache.h.
*/
static const uint kLumenSurfaceCacheTileSize = 16;

/** Page ID of an invalid/unallocated page. Valid page IDs are 1..getPageCount().
    Must match kInvalidPageID in LumenSurfaceCache.h.
*/
static const uint kInvalidPageID = 0;

/** Atlas layout and page ID contract (frozen with Agent B).
    For a square grid of N x N tiles:
        pageID = tileY * N + tileX + 1, with 0 <= tileX, tileY < N
        tileX  = (pageID - 1) % N
        tileY  = (pageID - 1) / N
        texelX = tileX * kLumenSurfaceCacheTileSize
        texelY = tileY * kLumenSurfaceCacheTileSize
*/

/** Tile column of a page per the Agent B layout contract.
    Returns 0xFFFFFFFF (kLumenCardInvalidID) for pageID 0.
*/
uint lumenCardPageTileX(uint pageID, uint pagesPerSide)
{
    return (pageID == 0u) ? kLumenCardInvalidID : ((pageID - 1u) % pagesPerSide);
}

/** Tile row of a page per the Agent B layout contract.
    Returns 0xFFFFFFFF (kLumenCardInvalidID) for pageID 0.
*/
uint lumenCardPageTileY(uint pageID, uint pagesPerSide)
{
    return (pageID == 0u) ? kLumenCardInvalidID : ((pageID - 1u) / pagesPerSide);
}

/** Texel-space origin (texelX, texelY) of a page inside the atlas,
    per the Agent B layout contract. Returns (0,0) for pageID 0.
*/
uint2 lumenCardPageTexelOrigin(uint pageID, uint pagesPerSide)
{
    return uint2(
        lumenCardPageTileX(pageID, pagesPerSide),
        lumenCardPageTileY(pageID, pagesPerSide)
    ) * kLumenSurfaceCacheTileSize;
}

// ---------------------------------------------------------------------------
// Atlas channel formats (frozen values; byte/texel identical to Agent B)
// ---------------------------------------------------------------------------

///< Material atlas: RGBA8, 4 bytes/texel (kLumenSurfaceCacheMaterialBytesPerTexel).
///<   R,G,B = base color (diffuse reflection albedo, linear)
///<   A     = opacity (1.0 opaque; 0.0 when the texel is alpha-test fallback)
static const uint kLumenCardCaptureMaterialBytesPerTexel = 4;

///< Radiance atlas: RGBA16F, 8 bytes/texel (kLumenSurfaceCacheRadianceBytesPerTexel).
///< Written by the Surface Cache lighting pass (S3). The capture pass clears
///< the page region to zero so stale radiance never survives a re-capture.
///<   R,G,B = direct radiance (linear)
///<   A     = indirect radiance (linear)
static const uint kLumenCardCaptureRadianceBytesPerTexel = 8;

///< Metadata atlas: RGBA16F, 8 bytes/texel (kLumenSurfaceCacheMetadataBytesPerTexel).
///<   R = linear depth in meters measured from the card capture camera plane
///<       along the face normal (positive toward the box; ortho projection so
///<       the interpolated value is exact).
///<   G = validity/flags as a small non-negative integer stored in a float
///<       (exact in 16F up to 2048; see kLumenCardMetadataFlag* below).
///<   B,A = shading normal octahedral-encoded (ndir_to_oct_unorm, [0,1]^2).
static const uint kLumenCardCaptureMetadataBytesPerTexel = 8;

///< (Agent C extension, optional) Material params atlas: RGBA16F, 8 bytes/texel.
///< Not part of the Agent B three-atlas contract; created and bound by the
///< root integration pass only when the capture debug views are enabled.
///<   R   = roughness (linear)
///<   G,B,A = emissive (linear RGB)
static const uint kLumenCardCaptureParamsBytesPerTexel = 8;

// ---------------------------------------------------------------------------
// Metadata validity flags (stored as float in the metadata atlas G channel)
// ---------------------------------------------------------------------------

///< Surface was captured and is valid for Surface Cache lighting.
static const uint kLumenCardMetadataFlagValid = 1u;

///< The texel failed the material alpha test: opacity was written as 0 and
///< the lighting pass must route this texel to hit lighting (fallback).
static const uint kLumenCardMetadataFlagFallbackAlphaTest = 2u;

///< The material is unsupported by the capture (no diffuse lobe or hair
///< material): the lighting pass must route this texel to hit lighting.
static const uint kLumenCardMetadataFlagFallbackUnsupported = 4u;

///< All fallback reasons combined.
static const uint kLumenCardMetadataFlagFallbackAny =
    kLumenCardMetadataFlagFallbackAlphaTest | kLumenCardMetadataFlagFallbackUnsupported;

// ---------------------------------------------------------------------------
// Capture pass constants (frozen)
// ---------------------------------------------------------------------------

///< Texture LOD used for all material texture sampling during capture.
///< Freezing Mip0 makes capture results deterministic and independent of
///< screen-space derivatives.
static const float kLumenCardCaptureTexLOD = 0.f;

///< Default distance (meters) of the capture camera in front of the card's
///< front face. The host may override this through the per-draw constant
///< buffer (gNearMargin); 0.01 keeps the near plane clear of the surface.
static const float kLumenCardCaptureDefaultNearMargin = 0.01f;

///< Face basis table: for a face index, the orthonormal camera frame used by
///< the capture and (in S3) by the world-position reconstruction.
///<   viewDir: direction the camera looks (from the face side into the box).
///<   up     : screen-space up of the captured page.
///<   right  : screen-space right of the captured page.
///<   The frame is frozen so that page texel (x,y) maps to world direction
///<   right * ((x + 0.5) / kLumenSurfaceCacheTileSize - 0.5) * extent[t0]
///<   + up * ((y + 0.5) / kLumenSurfaceCacheTileSize - 0.5) * extent[t1]
///<   + viewDir * depth,  where t0/t1 are the axes perpendicular to the face
///<   axis (t0 = (axis + 1) % 3, t1 = (axis + 2) % 3) and depth is the linear
///<   depth from the metadata atlas.
void lumenCardFaceBasis(uint faceIndex, out float3 viewDir, out float3 right, out float3 up)
{
    const uint axis = lumenCardFaceAxis(faceIndex);
    const bool positive = lumenCardFaceIsPositive(faceIndex);

    if (axis == 0u)
    {
        // X faces: camera on the +X side looking toward -X (face 0), or the
        // -X side looking toward +X (face 1).
        viewDir = positive ? float3(-1.f, 0.f, 0.f) : float3(1.f, 0.f, 0.f);
        up = float3(0.f, 1.f, 0.f);
    }
    else if (axis == 1u)
    {
        // Y faces: camera above/below looking toward -Y (face 2) or +Y (face 3).
        viewDir = positive ? float3(0.f, -1.f, 0.f) : float3(0.f, 1.f, 0.f);
        up = float3(0.f, 0.f, -1.f);
    }
    else
    {
        // Z faces: camera on the +Z side looking toward -Z (face 4), or the
        // -Z side looking toward +Z (face 5).
        viewDir = positive ? float3(0.f, 0.f, -1.f) : float3(0.f, 0.f, 1.f);
        up = float3(0.f, 1.f, 0.f);
    }
    right = cross(up, viewDir);
}

/** Axes perpendicular to the card face axis, in the same order as the
    capture priority formula in LumenCardScene.h (t0 = (axis+1)%3, t1 = (axis+2)%3).
*/
uint2 lumenCardFacePerpendicularAxes(uint faceIndex)
{
    const uint axis = lumenCardFaceAxis(faceIndex);
    return uint2((axis + 1u) % 3u, (axis + 2u) % 3u);
}

/** Component of a float3 along the given world axis (0=x, 1=y, 2=z). */
float lumenCardComponent(float3 v, uint axis)
{
    if (axis == 0u)
        return v.x;
    if (axis == 1u)
        return v.y;
    return v.z;
}

// ---------------------------------------------------------------------------
// Per-draw capture parameters
//
// The cbuffer itself is declared in LumenCardCapture.3d.slang (the pass
// program) and NOT here, so that S3 lighting shaders can import this module
// without picking up a constant buffer they never bind. Field list (host
// binds by name, see LumenCardCapture.3d.slang):
//
//   uint  gCardIndex;     // Index into gCards (host LumenCardScene card index).
//   uint  gPageID;        // Page ID assigned by the allocator; 0 disables the draw.
//   uint  gPagesPerSide;  // Tiles per side of the atlas grid (N in the pageID formula).
//   uint2 gPageTexelOrigin; // (texelX, texelY) of the page, Agent B formula.
//   float gNearMargin;    // Camera distance in front of the front face (meters).
//
// The host draws one card per draw call with the viewport set to the page
// texel region (texelX, texelY, kLumenSurfaceCacheTileSize,
// kLumenSurfaceCacheTileSize), so SV_Position.xy inside the pixel shader is
// directly the atlas coordinate. gPageTexelOrigin is derived by the host from
// gPageID with the Agent B formula (lumenCardPageTexelOrigin) and is provided
// for debug writes and validation only; it does not affect the render targets.
