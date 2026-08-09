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
//  LumenGI - S6-B2 Mesh SDF atlas sampling (GPU side)
//  -------------------------------------------------------------------------------------
//  Implements the atlas bindings + sampling contract for the S6-B2 Mesh SDF atlas.
//  The CPU side (LumenMeshSDFAtlas.h, same directory) computes the exact same layout
//  and formulas; keep both files in sync.
//
//  FROZEN LAYOUT (see LumenMeshSDFAtlas.h for the full argument)
//  -------------------------------------------------------------------------------------
//  * Two shared 3D atlases (mixed mip formats force separate textures):
//        fine   : R16Float, hosts mip 0 of High-quality volumes (R16Float mip0)
//        coarse : R8Snorm,  hosts mips >= 1 and Low-quality mip 0
//  * Page = 32^3 texels (kLumenMeshSDFAtlasPageSize). Atlas geometry is
//    pagesPerSide^3 page slots (default 8^3 = 512 slots per atlas).
//  * A volume's mip m has dims d(m) = max(1, ceil(d(m-1)/2)) and is tiled into a
//    brick grid of ceil(d(m) / 32) per axis. All bricks of one mip are contiguous:
//    page table entry (instanceID, mip) = BASE page slot, brick index
//        brickIdx = bz*bx*by + by*bx + bx   (z-major, x-fastest)
//    pageSlot = baseSlot + brickIdx ; texel = pageBrick(pageSlot) * 32 + local where
//    pageBrick(s) = (s % P, (s / P) % P, s / (P*P)), P = a.pagesPerSide (the atlas texture
//    is a pagesPerSide^3 cube of 32^3 page bricks). The CPU mirror and the host atlas-texture
//    upload use the identical slot -> brick-origin layout.
//  * Page table: StructuredBuffer<uint>[instanceID * kLumenMeshSDFMaxMipCount + mip].
//    kLumenMeshSDFNotResident = not resident (atlas full / evicted / budget).
//
//  SAMPLING MATH (canonical; mirrors LumenMeshSDFAtlas.h)
//  -------------------------------------------------------------------------------------
//  Instance transform is the INVERSE affine (world -> object), applied ROW-WISE exactly
//  like the CPU mirror (LumenMeshSDFAtlas.h::worldToObject):
//      pObj.x = dot(invRow0.xyz, world) + invRow0.w
//      pObj.y = dot(invRow1.xyz, world) + invRow1.w
//      pObj.z = dot(invRow2.xyz, world) + invRow2.w
//  The volume lives in OUTPUT space (Agent G's .msdf contract):
//      pOut = pObj * normalizationScale
//      u    = (pOut - bboxMin) * invVoxelSize - 0.5      (mip-0 continuous coords)
//      uMip = u / 2^mip
//  Cells are clamped to [0, d(m)-1] exactly like LumenMeshSDFSampling.slang, then
//  each integer corner voxel maps through the brick grid to a page + texel.
//  Decode: fine atlas Load is float (R16Float); coarse atlas Load is R8_SNORM
//  converted (code/127) and is scaled by quantRange - identical to S6-B1. The final
//  distance is flipped to the math convention (positive outside) via signConvention.
//
//  NON-UNIFORM SCALE
//  -------------------------------------------------------------------------------------
//  The voxel-coordinate map is exact under ANY affine transform. The DISTANCE
//  field, however, is not a valid SDF after anisotropic scaling; instances with
//  kLumenMeshSDFInstanceFlagNonUniformScale return the distorted stored distance
//  (S6-B4 may choose HWRT fallback for them). worldScalePerOutput converts output
//  distances to world distances: exact for similarities, conservative (max-axis)
//  otherwise.
//
//  MISS CONTRACT
//  -------------------------------------------------------------------------------------
//  lumenMeshSDFAtlasSample never clamps by returning wrong data: every failure
//  returns a reason from LumenMeshSDFAtlasMissReason (mirrors the C++ mirror and
//  Agent G's LumenMeshSDFMissReason style). Out-of-volume-but-inside-instance
//  positions are NOT a miss - cells clamp to the border value exactly like S6-B1
//  (the .msdf padding extends the grid past the mesh). The optional stats buffer
//  records per-reason counters via InterlockedAdd for readback by the root pass.
// =====================================================================================

#include "LumenMeshSDFData.slang"

// -------------------------------------------------------------------------------------
// Frozen atlas constants (mirror LumenMeshSDFAtlas.h; keep both in sync)
// -------------------------------------------------------------------------------------

/// Texels per page side (32^3 texels per page). Frozen.
static const uint kLumenMeshSDFAtlasPageSize = 32;

/// log2(kLumenMeshSDFAtlasPageSize).
static const uint kLumenMeshSDFAtlasPageSizeBits = 5;

/// Default atlas geometry: 8^3 page slots per atlas texture.
static const uint kLumenMeshSDFAtlasDefaultPagesPerSide = 8;

/// Page table stride: one uint per (instance, mip) - must match
/// kLumenMeshSDFMaxMipCount in LumenMeshSDFData.slang.
static const uint kLumenMeshSDFAtlasPageTableStride = kLumenMeshSDFMaxMipCount;

/// Hard cap on registered instances (sizes the page table buffer).
static const uint kLumenMeshSDFAtlasMaxInstances = 4096;

/// Instance flag bits (mirror LumenMeshSDFAtlas.h).
static const uint kLumenMeshSDFInstanceFlagNonUniformScale = 1u << 0;

// -------------------------------------------------------------------------------------
// Atlas miss reasons (CPU mirror: AtlasMissReason in LumenMeshSDFAtlas.h)
// -------------------------------------------------------------------------------------

enum class LumenMeshSDFAtlasMissReason : uint
{
    None = 0,                ///< resident == true; reason not meaningful.
    NoInstance = 1,          ///< instanceID out of range, instance slot dead, or meshID invalid.
    InstanceNotResident = 2, ///< Page table entry is kLumenMeshSDFNotResident (atlas full / evicted / budget).
    OutOfInstanceBounds = 3, ///< World position outside the instance's world AABB.
    InvalidMip = 4,          ///< mip >= volume mipCount (or volume has no mips).
    PageCorrupt = 5,         ///< Computed page slot exceeds atlas capacity (guards corruption).
};

// -------------------------------------------------------------------------------------
// Instance table entry (BYTE-IDENTICAL to LumenMeshSDFAtlasInstance in the .h)
// -------------------------------------------------------------------------------------
// 84 bytes under Slang scalar layout == C layout. The root pass memcpys the C++
//  table into a StructuredBuffer. Frozen offsets:
//      +0 invRows[0].xyzw      world -> object affine rows; the inverse affine is applied
//                              ROW-WISE (matches LumenMeshSDFAtlas.h): pObj.x =
//                              dot(invRow0.xyz, world) + invRow0.w, pObj.y = dot(invRow1.xyz,
//                              world) + invRow1.w, pObj.z = dot(invRow2.xyz, world) + invRow2.w.
//      +16 invRows[1], +32 invRows[2]
//      +48 boundsMin.xyz, +60 boundsMax.xyz (world AABB)
//      +72 meshID (uint; 0xFFFFFFFF = dead slot -> NoInstance)
//      +76 flags (uint; kLumenMeshSDFInstanceFlag*)
//      +80 worldScalePerOutput (float)
struct LumenMeshSDFAtlasInstance
{
    float4 invRow0;
    float4 invRow1;
    float4 invRow2;
    float3 boundsMin;
    float3 boundsMax;
    uint meshID;
    uint flags;
    float worldScalePerOutput;
};

// -------------------------------------------------------------------------------------
// Bindings + result
// -------------------------------------------------------------------------------------

/// GPU-side atlas resources. The root pass binds the two atlas textures (created
/// from kLumenMeshSDFAtlasDefaultPagesPerSide^3 * page size texels), the page table
/// buffer (uint, kLumenMeshSDFAtlasMaxInstances * kLumenMeshSDFMaxMipCount entries),
/// the volume descriptors (96-byte LumenMeshSDFVolumeDescriptor per mesh) and the
/// instance table (LumenMeshSDFAtlasInstance per registered instance).
struct LumenMeshSDFAtlasBindings
{
    Texture3D<float> fineAtlas;    ///< R16Float; mip0 pages of High-quality volumes.
    Texture3D<float> coarseAtlas;  ///< R8Snorm; mips >= 1 and Low-quality mip 0.
    StructuredBuffer<uint> pageTable;      ///< [instanceID * 12 + mip] = base page slot.
    StructuredBuffer<LumenMeshSDFVolumeDescriptor> volumes; ///< Per mesh (deduplicated).
    StructuredBuffer<LumenMeshSDFAtlasInstance> instances;  ///< Per registered instance.
    uint instanceCount;
    uint volumeCount;
    uint pagesPerSide;  ///< Atlas geometry: pagesPerSide^3 slots per atlas.
};

/// Result of one atlas sample (CPU mirror: LumenMeshSDFAtlasSampleResult).
struct LumenMeshSDFAtlasSampleResult
{
    bool resident;              ///< True when the sample read a resident page.
    float distanceOutput;       ///< Math-convention distance, OUTPUT units.
    uint pageSlot;              ///< Base page slot of the mip's brick grid.
    LumenMeshSDFAtlasMissReason reason; ///< Valid when resident == false.
};

// -------------------------------------------------------------------------------------
// Optional GPU statistics buffer (per-frame counters, read back by the root pass)
// -------------------------------------------------------------------------------------

static const uint kLumenMeshSDFAtlasStatSamples = 0;      ///< Total sample calls.
static const uint kLumenMeshSDFAtlasStatHits = 1;         ///< Resident samples.
static const uint kLumenMeshSDFAtlasStatNoInstance = 2;
static const uint kLumenMeshSDFAtlasStatNotResident = 3;
static const uint kLumenMeshSDFAtlasStatOutOfBounds = 4;
static const uint kLumenMeshSDFAtlasStatInvalidMip = 5;
static const uint kLumenMeshSDFAtlasStatPageCorrupt = 6;
static const uint kLumenMeshSDFAtlasStatCount = 7;        ///< Buffer size (uint).

// -------------------------------------------------------------------------------------
// Coordinate transforms
// -------------------------------------------------------------------------------------

/// Mip dims of a volume: d(0) = resolution, d(m) = max(1, ceil(d(m-1)/2)).
/// Formula is IDENTICAL to lumenMeshSDFMipDims in LumenMeshSDFSampling.slang
/// (kept local here because this module binds descriptors, not volume textures).
uint3 lumenMeshSDFAtlasMipDims(LumenMeshSDFVolumeDescriptor vol, uint mip)
{
    uint3 dims = vol.resolution;
    for (uint m = 0; m < mip; ++m)
        dims = max(uint3(1, 1, 1), (dims + uint3(1, 1, 1)) >> 1);
    return dims;
}

/// World -> object via the stored inverse affine rows. The inverse affine is stored as
/// three ROWS of the inverse matrix (pObj.x = dot(invRow0.xyz, world) + invRow0.w, ...),
/// so each output component consumes exactly ONE row - byte-identical to the CPU mirror
/// LumenMeshSDFAtlas.h::worldToObject.
/// FIXED: this function previously applied the inverse by COLUMNS (invRow0.xyz*w.x +
/// invRow1.xyz*w.y + invRow2.xyz*w.z), i.e. it computed M^T*world + t instead of M*world
/// + t. For a rotated instance the result was the transpose of the intended mapping, so
/// atlas samples of rotated instances were mirrored/reflected; for the axis-aligned
/// identity-like matrices used by the CPU unit tests the two coincide, which is why the
/// bug went unnoticed. Keep the formula below in sync with the .h.
float3 lumenMeshSDFAtlasWorldToObject(LumenMeshSDFAtlasInstance inst, float3 worldPos)
{
    return float3(
        dot(inst.invRow0.xyz, worldPos),
        dot(inst.invRow1.xyz, worldPos),
        dot(inst.invRow2.xyz, worldPos))
        + float3(inst.invRow0.w, inst.invRow1.w, inst.invRow2.w);
}

/// Continuous mip-0 voxel coords (voxel-center convention, S6-B1); voxel i sits at
/// u == i. Exact under any affine instance transform.
float3 lumenMeshSDFAtlasWorldToVoxel(LumenMeshSDFAtlasBindings a, uint instanceID, float3 worldPos)
{
    LumenMeshSDFAtlasInstance inst = a.instances[instanceID];
    LumenMeshSDFVolumeDescriptor vol = a.volumes[inst.meshID];
    const float3 pObj = lumenMeshSDFAtlasWorldToObject(inst, worldPos);
    const float3 pOut = pObj * vol.normalizationScale;
    return (pOut - vol.bboxMin) * vol.invVoxelSize - 0.5f;
}

/// Output distance -> world distance (exact for similarities, conservative
/// max-axis factor for non-uniformly scaled instances).
float lumenMeshSDFAtlasOutputToWorldDistance(LumenMeshSDFAtlasInstance inst, float distanceOutput)
{
    return distanceOutput * inst.worldScalePerOutput;
}

// -------------------------------------------------------------------------------------
// Page mapping + fetch
// -------------------------------------------------------------------------------------

/// Map an integer voxel coordinate of a mip to (page slot, atlas texel).
/// Canonical formula (mirrors the CPU mirror + the host atlas-texture layout):
///     brick = voxel / pageSize ; local = voxel % pageSize ;
///     slot = baseSlot + (bz*bx*by + by*bx + bx) ;
///     texel = pageBrick(slot) * pageSize + local
/// The atlas texture is a pagesPerSide^3 cube of 32^3 page bricks; page slot s occupies
/// the brick with grid coordinates (s % P, (s / P) % P, s / (P*P)) (P = a.pagesPerSide), so
///     texel = ((s % P), ((s / P) % P), (s / (P*P))) * pageSize + local.
/// Returns false and sets PageCorrupt when the slot exceeds atlas capacity (guards
/// corrupted page tables; never clamps silently).
bool lumenMeshSDFAtlasVoxelToPageAndTexel(
    LumenMeshSDFAtlasBindings a,
    LumenMeshSDFVolumeDescriptor vol,
    uint mip,
    uint baseSlot,
    int3 voxel,
    out uint pageSlot,
    out int3 texel,
    out LumenMeshSDFAtlasMissReason reason)
{
    const uint3 dims = lumenMeshSDFAtlasMipDims(vol, mip);
    const uint3 bricks = (dims + kLumenMeshSDFAtlasPageSize - 1u) / kLumenMeshSDFAtlasPageSize;
    const int3 brick = int3(voxel) / int(kLumenMeshSDFAtlasPageSize);
    const int3 local = int3(voxel) % int(kLumenMeshSDFAtlasPageSize);
    const uint brickIdx = uint(brick.z) * (bricks.x * bricks.y) + uint(brick.y) * bricks.x + uint(brick.x);
    const uint capacity = a.pagesPerSide * a.pagesPerSide * a.pagesPerSide;
    if (baseSlot + brickIdx >= capacity)
    {
        reason = LumenMeshSDFAtlasMissReason::PageCorrupt;
        return false;
    }
    pageSlot = baseSlot + brickIdx;
    const int P = int(a.pagesPerSide);
    const int s = int(pageSlot);
    const int3 pageBrick = int3(s % P, (s / P) % P, s / (P * P));
    texel = pageBrick * int(kLumenMeshSDFAtlasPageSize) + local;
    return true;
}

// -------------------------------------------------------------------------------------
// Sampling
// -------------------------------------------------------------------------------------

/// Sample the atlas for (instanceID, worldPos, mip) and return the math-convention
/// distance in OUTPUT units, or a miss reason. Never clamps silently; the miss
/// reasons mirror the CPU-side contract. Out-of-volume-but-inside-instance
/// positions clamp to the border value (S6-B1 behavior, conservative for outside).
LumenMeshSDFAtlasSampleResult lumenMeshSDFAtlasSample(
    LumenMeshSDFAtlasBindings a,
    uint instanceID,
    float3 worldPos,
    uint mip)
{
    LumenMeshSDFAtlasSampleResult result;
    result.resident = false;
    result.distanceOutput = 0.f;
    result.pageSlot = kLumenMeshSDFNotResident;
    result.reason = LumenMeshSDFAtlasMissReason::None;

    if (instanceID >= a.instanceCount)
    {
        result.reason = LumenMeshSDFAtlasMissReason::NoInstance;
        return result;
    }
    LumenMeshSDFAtlasInstance inst = a.instances[instanceID];
    if (inst.meshID >= a.volumeCount)
    {
        result.reason = LumenMeshSDFAtlasMissReason::NoInstance;
        return result;
    }
    if (any(worldPos < inst.boundsMin) || any(worldPos > inst.boundsMax))
    {
        result.reason = LumenMeshSDFAtlasMissReason::OutOfInstanceBounds;
        return result;
    }
    LumenMeshSDFVolumeDescriptor vol = a.volumes[inst.meshID];
    if (vol.mipCount == 0 || mip >= vol.mipCount)
    {
        result.reason = LumenMeshSDFAtlasMissReason::InvalidMip;
        return result;
    }

    const uint baseSlot = a.pageTable[instanceID * kLumenMeshSDFAtlasPageTableStride + mip];
    if (baseSlot == kLumenMeshSDFNotResident)
    {
        result.reason = LumenMeshSDFAtlasMissReason::InstanceNotResident;
        return result;
    }

    // Continuous mip-m voxel coords, clamped to the valid cell range (S6-B1 rule:
    // clamping is conservative for outside points - the .msdf padding already
    // extends the grid past the mesh).
    const float3 u = lumenMeshSDFAtlasWorldToVoxel(a, instanceID, worldPos) / float(1u << mip);
    const uint3 dims = lumenMeshSDFAtlasMipDims(vol, mip);
    const uint3 maxCoord = dims - uint3(1, 1, 1);
    const float3 cell = clamp(u, float3(0.f, 0.f, 0.f), float3(maxCoord));
    const uint3 base = min(uint3(cell), maxCoord);
    const uint3 bx = uint3(base.x < maxCoord.x ? 1u : 0u, base.y < maxCoord.y ? 1u : 0u,
                           base.z < maxCoord.z ? 1u : 0u);
    const float3 f = cell - float3(base);

    const bool fine = (mip == 0 && vol.formatMip0 == kLumenMeshSDFFormatR16Float);

    // Load one stored corner texel. The texture is selected by a data-dependent branch instead
    // of a local resource variable: DXC rejects "local resource not guaranteed to map to unique
    // global resource" when a Texture3D local is assigned from a runtime-selected binding.
    float stored = 0.f;
    float corner[8];
    for (uint c = 0; c < 8; ++c)
    {
        const int3 voxel = int3(base) + int3(c & 1u ? bx.x : 0u, c & 2u ? bx.y : 0u, c & 4u ? bx.z : 0u);
        uint pageSlot;
        int3 texel;
        LumenMeshSDFAtlasMissReason reason;
        if (!lumenMeshSDFAtlasVoxelToPageAndTexel(a, vol, mip, baseSlot, voxel, pageSlot, texel, reason))
        {
            result.reason = reason;
            return result;
        }
        if (fine)
            stored = a.fineAtlas.Load(int4(texel, 0)).x;
        else
            stored = a.coarseAtlas.Load(int4(texel, 0)).x;
        corner[c] = fine ? stored : stored * vol.quantRange; // R8SNorm code/127 * R (S6-B1 decode)
    }

    const float c0 = lerp(lerp(corner[0], corner[1], f.x), lerp(corner[2], corner[3], f.x), f.y);
    const float c1 = lerp(lerp(corner[4], corner[5], f.x), lerp(corner[6], corner[7], f.x), f.y);
    float distance = lerp(c0, c1, f.z);

    // Math convention: positive outside. Never hardcoded (S6-B1 rule).
    if (vol.signConvention != kLumenMeshSDFSignConventionPositiveOutside)
    {
        distance = -distance;
    }

    result.resident = true;
    result.distanceOutput = distance;
    result.pageSlot = baseSlot;
    return result;
}

/// Counted variant: records the outcome into a per-frame stats buffer
/// (see kLumenMeshSDFAtlasStat*) via InterlockedAdd. `stats` must be sized
/// kLumenMeshSDFAtlasStatCount uints and zeroed by the host before the dispatch;
/// the host reads it back after the frame.
LumenMeshSDFAtlasSampleResult lumenMeshSDFAtlasSampleCounted(
    LumenMeshSDFAtlasBindings a,
    RWStructuredBuffer<uint> stats,
    uint instanceID,
    float3 worldPos,
    uint mip)
{
    InterlockedAdd(stats[kLumenMeshSDFAtlasStatSamples], 1);
    LumenMeshSDFAtlasSampleResult result = lumenMeshSDFAtlasSample(a, instanceID, worldPos, mip);
    if (result.resident)
    {
        InterlockedAdd(stats[kLumenMeshSDFAtlasStatHits], 1);
    }
    else
    {
        uint index = kLumenMeshSDFAtlasStatNoInstance;
        switch (result.reason)
        {
        case LumenMeshSDFAtlasMissReason::InstanceNotResident: index = kLumenMeshSDFAtlasStatNotResident; break;
        case LumenMeshSDFAtlasMissReason::OutOfInstanceBounds: index = kLumenMeshSDFAtlasStatOutOfBounds; break;
        case LumenMeshSDFAtlasMissReason::InvalidMip: index = kLumenMeshSDFAtlasStatInvalidMip; break;
        case LumenMeshSDFAtlasMissReason::PageCorrupt: index = kLumenMeshSDFAtlasStatPageCorrupt; break;
        default: break; // NoInstance
        }
        InterlockedAdd(stats[index], 1);
    }
    return result;
}
