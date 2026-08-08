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
// =====================================================================================
//  LumenGI - S6-B1 Mesh SDF runtime contract (GPU side)
//  -------------------------------------------------------------------------------------
//  Shared layout + constants for the runtime-side representation of a ".msdf"
//  volume produced by the S6-A1 Mesh SDF builder (Source/Tools/MeshSDFBuilder,
//  Agent D). Consumed by LumenMeshSDFSampling.slang (sampling/trace implementation)
//  and by the root LumenGI pass, which creates the GPU textures and uploads the
//  volume descriptor (LumenMeshSDFVolumeDescriptor) as a constant buffer.
//
//  THE .msdf CONTRACT (must be read together with MeshSDFBuilder.h)
//  -------------------------------------------------------------------------------------
//  * Distances are stored at VOXEL CENTERS: voxel (x,y,z) center is at
//        p_out = bboxMin + (i + 0.5) * voxelSize          (output space)
//  * Distances are in OUTPUT space (normalized when the builder ran with
//    normalize == true). World <-> output: p_out = p_world * normalizationScale,
//    so world = output / normalizationScale. Distances scale the same way.
//  * signConvention byte 0 == positive outside (distance > 0 outside).
//    signReliable == 0 means the field is unsupported (fall back to HWRT).
//  * Voxel data is x-fastest (row-major xyz) - texel (x,y,z) of the GPU texture
//    holds voxel (x,y,z). dataCount == nx*ny*nz.
//
//  GPU LAYOUT (frozen by S6-B1)
//  -------------------------------------------------------------------------------------
//  * One Texture3D per mip level (mixed formats force separate texture objects):
//      - mip 0 (finest): R16Float (format 0) in High quality, or R8Snorm (format 1)
//        in Low quality. R16Float needs no scalar rescale; the CPU encodes the
//        f32 field to f16 bits (round-to-nearest-even), relative error <= 2^-11.
//      - mips >= 1: R8Snorm with a per-volume scalar scale R stored in
//        descriptor.quantRange. Encode code = clamp(round(d / R * 127), -127, 127);
//        decode d = max(int8(code) / 127, -1) * R. Absolute error <= R / 127.
//  * Mip dims: mip[0] = .msdf resolution; mip[m] = max(1, ceil(mip[m-1] / 2)).
//    Mip m texel t covers mip-0 voxels [2^m * t, 2^m * (t+1)).
//  * mipCount = 1 + ceil(log2(max(resolution))), capped at kLumenMeshSDFMaxMipCount.
//  * Voxel-center convention: continuous voxel coord u = (p_out - bboxMin) /
//    voxelSize - 0.5; voxel i sits at u == i. This differs from Falcor's NDSDFGrid
//    (corner-sampled) - do not copy its "+0.5 of width+1" formulas here.
//  * No padding texels: the builder's paddingWorld already extends the grid past
//    the mesh. Sampling clamps continuous voxel coords to the valid cell range
//    (see lumenMeshSDFClampCell), which is conservative for outside points.
//  * Mip generation (CPU, LumenMeshSDF.cpp): min-abs pooling by default
//    (kLumenMeshSDFPoolMinAbs) - pooled |d| is a lower bound on every child |d|
//    at voxel centers, i.e. steps never overshoot relative to the fine field.
//    Average pooling (kLumenMeshSDFPoolAverage) is the optional quality mode.
//
//  QUANTIZATION ARGUMENT (frozen values; see S6-B1 report)
//  -------------------------------------------------------------------------------------
//  Near-surface precision decides hit quality, so the finest mip keeps 16-bit
//  float (relative error 4.88e-4, exact zero, subnormals to ~6e-8 in normalized
//  units). Coarse mips are only stepped on when |d| >= 2 mips voxels (see
//  lumenMeshSDFSelectMip), so a uniform absolute error of R/127 ~ 0.008
//  (normalized) is far below the step scale there. R8Unorm (offset map
//  d' = (d + R) / (2R), step 2R/255) would cost the same bytes but loses the
//  exact-zero codec point and needs an offset - frozen choice is R8Snorm.
//
//  S6-B2 (atlas) / S6-A2 (cache) hooks
//  -------------------------------------------------------------------------------------
//  * S6-B2: descriptor.atlasPage / kLumenMeshSDFFlagAtlasResident let the atlas
//    host rebind the volume. The world<->voxel math is unchanged by the atlas;
//    only the texture fetch (volume.mips[m]) becomes an atlas-slice fetch.
//  * S6-A2: meshContentHash is NOT stored in the .msdf file (it lives in the
//    JSON report). The host passes it into LumenMeshSDFVolumeDescriptor
//    (contentHashLo/Hi) and LumenMeshSDF::computeCacheKey() derives the disk
//    cache key from it + version + resolution + quality + pooling + quantRange.
// =====================================================================================

#ifndef __LUMEN_MESH_SDF_DATA_SLANG__
#define __LUMEN_MESH_SDF_DATA_SLANG__

// -------------------------------------------------------------------------------------
// Frozen format / layout constants
// -------------------------------------------------------------------------------------

/// Per-mip volume formats (see GPU LAYOUT above). Values are frozen; the C++
/// side mirrors them as kLumenMeshSDFFormat* in LumenMeshSDF.h.
static const uint kLumenMeshSDFFormatR16Float = 0;
static const uint kLumenMeshSDFFormatR8Snorm = 1;

/// Maximum number of mip levels a volume may have (covers grids up to 2048^3).
static const uint kLumenMeshSDFMaxMipCount = 12;

/// Mip pooling modes (must match kLumenMeshSDFPool* in LumenMeshSDF.h).
static const uint kLumenMeshSDFPoolMinAbs = 0; ///< Frozen default: conservative lower bound.
static const uint kLumenMeshSDFPoolAverage = 1; ///< Quality mode: signed average.

/// atlasPage value when the volume is NOT resident in the S6-B2 atlas
/// (direct per-volume textures are used instead).
static const uint kLumenMeshSDFNotResident = 0xFFFFFFFFu;

/// Volume descriptor flag bits (descriptor.flags).
static const uint kLumenMeshSDFFlagSignReliable = 1u << 0; ///< .msdf signReliable == 1.
static const uint kLumenMeshSDFFlagQualityHigh = 1u << 1;  ///< 1 = R16Float mip0, 0 = R8Snorm.
static const uint kLumenMeshSDFFlagPoolMinAbs = 1u << 2;  ///< 1 = min-abs pooling (default).
static const uint kLumenMeshSDFFlagAtlasResident = 1u << 3; ///< S6-B2: volume lives in atlas.

/// Sign convention from the .msdf header byte. Only PositiveOutside = 0 exists
/// today (MeshSDFBuilder.h SignConvention); sampling honors the byte anyway.
static const uint kLumenMeshSDFSignConventionPositiveOutside = 0;

// -------------------------------------------------------------------------------------
// Sphere trace frozen parameters (defaults; LumenMeshSDFTraceParams overrides)
// -------------------------------------------------------------------------------------

/// Step budget per trace call (S6-B4 may request a smaller budget per ray).
static const uint kLumenMeshSDFMaxSteps = 64;

/// Minimum step = kLumenMeshSDFMinStepScale * voxelSizeWorld (guards against
/// stalls when |d| collapses near the surface).
static const float kLumenMeshSDFMinStepScale = 0.25f;

/// A crossing is accepted as a hit when d <= 0 AND |d| <= kLumenMeshSDFHitEpsScale
/// * voxelSizeWorld (the "|d| small enough" clause of the S6-B1 contract).
static const float kLumenMeshSDFHitEpsScale = 1.0f;

/// Central-difference normal delta = kLumenMeshSDFNormalDeltaScale * voxelSizeWorld.
static const float kLumenMeshSDFNormalDeltaScale = 0.5f;

/// Mip selection: use the coarsest mip whose voxel count per step stays >= 1,
/// i.e. floor(log2(|d| / voxelSize)) clamped to [0, mipCount-1].
/// A sample on mip m is at least 2^m fine voxels from any finer detail, so the
/// uniform R/127 quantization error of coarse mips stays a small fraction of a
/// coarse voxel at the step scale where it is used.

/// Miss reasons reported by the trace contract (result.missReason).
enum class LumenMeshSDFMissReason : uint
{
    None = 0,             ///< result.hit == true; missReason is not meaningful.
    NoGrid = 1,           ///< No volume bound for the requested volume id.
    SignUnreliable = 2,   ///< .msdf signReliable == 0 (open/thin mesh) - fall back to HWRT.
    RayMissedBounds = 3,  ///< Ray exited [tMin, tMax] without crossing the surface.
    MaxSteps = 4,         ///< Step budget exhausted inside [tMin, tMax].
};

// -------------------------------------------------------------------------------------
// Volume descriptor (96 bytes, Slang scalar layout == C++ layout)
// -------------------------------------------------------------------------------------
// MUST stay byte-identical to LumenMeshSDFVolumeDescriptor in LumenMeshSDF.h.
// The root pass memcpys the C++ struct into a constant buffer. No float4/float2
// members: 12-byte float3 members are packed as-is under Slang scalar layout.
struct LumenMeshSDFVolumeDescriptor
{
    uint3 resolution;            // +0  mip0 voxel counts (x fastest, from .msdf)
    uint formatMip0;             // +12 kLumenMeshSDFFormatR16Float / R8Snorm (quality)
    uint formatCoarse;           // +16 kLumenMeshSDFFormatR8Snorm (frozen)
    uint mipCount;               // +20 number of mip levels (1..kLumenMeshSDFMaxMipCount)

    float3 bboxMin;              // +24 output-space grid min (from .msdf)
    float3 bboxMax;              // +36 output-space grid max (from .msdf)
    float voxelSize;             // +48 output-space voxel size (uniform per axis)
    float invVoxelSize;          // +52 1 / voxelSize
    float normalizationScale;    // +56 world = output / normalizationScale (from .msdf)
    float invNormalizationScale; // +60 1 / normalizationScale
    float quantRange;            // +64 R: snorm scale of this volume (max |d| over mip0)

    uint signConvention;         // +68 from .msdf (0 = positive outside)
    uint signReliable;           // +72 from .msdf (0 = unsupported field)
    float paddingWorld;          // +76 informational only (world units)

    uint atlasPage;              // +80 S6-B2: atlas page, or kLumenMeshSDFNotResident
    uint contentHashLo;          // +84 S6-A2: meshContentHash low 32 bits
    uint contentHashHi;          // +88 S6-A2: meshContentHash high 32 bits
    uint flags;                  // +92 kLumenMeshSDFFlag* bitfield
};

/// CPU -> GPU volume binding: descriptor + one texture per mip. Mixed formats
/// (R16Float mip0, R8Snorm mips 1..) force separate texture objects; the atlas
/// path (S6-B2) replaces the texture array with an atlas-slice fetch but keeps
/// the same descriptor and sampling contract.
struct LumenMeshSDFVolume
{
    LumenMeshSDFVolumeDescriptor desc;
    Texture3D<float> mips[kLumenMeshSDFMaxMipCount];
};

/// Per-call trace overrides; all-zero/one fields mean "use frozen defaults".
struct LumenMeshSDFTraceParams
{
    uint maxSteps;         // 0 -> kLumenMeshSDFMaxSteps
    float minStepScale;    // 0 -> kLumenMeshSDFMinStepScale
    float hitEpsScale;     // 0 -> kLumenMeshSDFHitEpsScale
    float normalDeltaScale; // 0 -> kLumenMeshSDFNormalDeltaScale
};

/// Result of one sphere trace call (S6-B4 consumes this).
struct LumenMeshSDFTraceResult
{
    bool hit;                ///< Surface crossed at t (d <= 0 and |d| <= hit eps).
    float t;                 ///< World-space hit distance along the ray (tMin-based).
    float d;                 ///< Signed distance at the hit point (math convention).
    float3 normal;           ///< Surface normal at hit (central differences), world space.
    uint steps;              ///< Steps taken (for stats / S6-C4).
    float minAbsDistance;    ///< Min |d| observed along the segment (world units).
    LumenMeshSDFMissReason missReason; ///< Valid when hit == false.
};

// -------------------------------------------------------------------------------------
// Sampling / trace contract (implemented in LumenMeshSDFSampling.slang)
// -------------------------------------------------------------------------------------
// The signed-distance convention is read from volume.desc.signConvention - it is
// NEVER hardcoded. All entry points below work in the mathematical convention
// (positive outside) regardless of the stored byte; signReliable == 0 makes the
// trace entry points return LumenMeshSDFMissReason::SignUnreliable immediately.

/// World-space signed distance at mip 0 (math convention: positive outside).
float lumenMeshSDFSignedDistanceWorld(LumenMeshSDFVolume vol, float3 worldPos);

/// Output-space signed distance sampled at an explicit mip (manual trilinear).
float lumenMeshSDFSignedDistanceOutput(LumenMeshSDFVolume vol, float3 outputPos, uint mip);

/// Mip for a given |d| (output units): coarsest mip with 2^mip * voxelSize <= |d|.
uint lumenMeshSDFSelectMip(LumenMeshSDFVolume vol, float absDistanceOutput);

/// Surface normal at worldPos via central differences on mip 0 (world space).
float3 lumenMeshSDFNormal(LumenMeshSDFVolume vol, float3 worldPos);

/// Sphere trace contract: returns hit / miss + per-step minimum distance.
LumenMeshSDFTraceResult lumenMeshSDFTrace(
    LumenMeshSDFVolume vol,
    float3 rayOriginWorld,
    float3 rayDirWorld,
    float tMin,
    float tMax
);

/// Sphere trace with explicit parameter overrides.
LumenMeshSDFTraceResult lumenMeshSDFTrace(
    LumenMeshSDFVolume vol,
    LumenMeshSDFTraceParams params,
    float3 rayOriginWorld,
    float3 rayDirWorld,
    float tMin,
    float tMax
);

#endif // __LUMEN_MESH_SDF_DATA_SLANG__
