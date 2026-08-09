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
//  LumenGI - S6-B3/S6-B4 Global Distance Field (GDF) shared contract (GPU side)
//  -------------------------------------------------------------------------------------
//  Single source of truth for the structures, constants and pure world<->voxel math
//  shared by the two GDF passes:
//      * LumenGDFCompose.cs.slang   (S6-B3): composes resident Mesh SDF instances
//        (S6-B1 volumes via the S6-B2 atlas) into the GDF clipmap textures.
//      * LumenGDFSphereTrace.3d.slang (S6-B4): sphere-traces the GDF textures.
//  Nothing here is owned by the host; the host binds the buffers/constants described
//  below. The layout mirrors the CPU contract in LumenGlobalDistanceField.h (S6-A3,
//  Agent A) field for field - see the per-field "host mirror" notes.
//
//  FIELD/FORMULA CORRESPONDENCE (LumenGlobalDistanceField.h, Agent A)
//  -------------------------------------------------------------------------------------
//      LumenGDFLevelParams::worldExtent      <-> LumenGDFLevel::worldExtent  = base * 2^m
//      LumenGDFLevelParams::voxelSize        <-> LumenGDFLevel::voxelSize    = extent / resolution
//      LumenGDFLevelParams::resolution       <-> LumenGDFLevel::resolution   (all levels share it)
//      LumenGDFClipmapParams::cameraCenter   <-> getCameraCenter()  (SNAPPED to level-0 voxel)
//      LumenGDFClipmapParams::levelCount     <-> getLevelCount()
//      LumenGDFClipmapParams::dynamicLevelCount <-> kDynamicLevels (frozen = 1)
//      LumenGDFClipmapParams::scroll         <-> scrollFromCameraMove() (informational)
//      LumenGDFDirtyRegion (axis/min/max/flags) <-> VoxelRange (axis/min/max/added)
//      lumenGDFWorldToIndex                  <-> worldToIndex()    (floor, inBounds)
//      lumenGDFIndexToWorld                  <-> indexToWorld()    (center + (i + 0.5)*vs)
//      lumenGDFLevelIndexForWorld            <-> levelIndexForWorld() (near->far selection)
//      lumenGDFInsideFootprint               <-> insideFootprint() (maxAbs(p-c) <= extent/2)
//      lumenGDFLevelCenter                   <-> levelCenter() (dynamic->camera, static->origin)
//
//  ROUNDING RULES (frozen, identical to the CPU side):
//      world -> voxel index: floor(vc) toward -infinity (negative coords floor negative).
//      gridMin_m = levelCenter_m - extent_m/2 = world coord of voxel 0's low corner.
//      indexToWorld: gridMin + (index + 0.5) * voxelSize  (voxel CENTER convention).
//  All math is deterministic IEEE binary-float, no randomness (mirrors Agent A).
//
//  GPU DATA FORMATS (frozen here, reference S6-B1 LumenMeshSDFData.slang)
//  -------------------------------------------------------------------------------------
//  One Texture3D per level. Level 0 (finest, dynamic) defaults to R16Float; levels
//  >= 1 default to R8Snorm with a per-level quantRange R stored in the level table.
//      R16Float: store/load the raw f32 value (relative error <= 2^-11).
//      R8Snorm : encode code = clamp(round(d / R * 127), -127, 127); a UAV/LOAD of an
//                R8_SNORM resource converts float<->code/127 automatically, so the
//                shader writes d / R clamped to [-1, 1] and reads back code/127, then
//                rescales by R. Absolute error <= R / 127 - identical codec to S6-B1.
//  The host MUST keep level.quantRange >= the largest |distance| it wants stored on an
//  R8Snorm level (values beyond R are clamped) and, for level 0's R16Float, keep the
//  empty distance <= 65504 (R16 float max).
//
//  DISTANCE CONVENTION: positive outside (math convention), world units at all times.
//  The GDF is always composed from resident, sign-reliable instances so no per-level
//  sign byte is needed (unlike the S6-B1 per-volume signConvention).
// =====================================================================================

#ifndef __LUMEN_GDF_DATA_SLANG__
#define __LUMEN_GDF_DATA_SLANG__

#include "LumenMeshSDFData.slang" // kLumenMeshSDFFormatR16Float / R8Snorm + trace defaults

// -------------------------------------------------------------------------------------
// Frozen GDF constants (mirror LumenGlobalDistanceField.h / LumenMeshSDFData.slang)
// -------------------------------------------------------------------------------------

/// Hard cap on the number of clipmap levels. Mirror of kMaxGDFLevels (S6-A3).
static const uint kLumenGDFMaxLevels = 16;

/// Number of leading camera-following (scrollable) levels. Mirror of kDynamicLevels.
static const uint kLumenGDFDynamicLevels = 1;

/// Default voxels per side (all levels share one resolution). Mirror of the CPU default.
static const uint kLumenGDFDefaultResolution = 128;

/// Sphere-trace step budget default (mirror of kLumenMeshSDFMaxSteps).
static const uint kLumenGDFMaxSteps = 64;

/// Minimum sphere step = kLumenGDFMinStepScale * voxelSize of the sampled level.
static const float kLumenGDFMinStepScale = 0.25f;

/// A crossing is accepted as a hit when d <= hitEps = kLumenGDFHitEpsScale * voxelSize.
static const float kLumenGDFHitEpsScale = 1.0f;

/// Default tMax for the convenience traceGDF() entry (0 -> this). World meters.
static const float kLumenGDFMaxDistance = 65504.f;

/// GDF hit type: value equals Falcor HitType::SDFGrid (Source/Falcor/Scene/HitInfoType.slang,
/// SDFGrid = 6), so a GDF hit can be reported to consumers that speak Falcor hit types
/// without re-mapping. A host that wants to keep the GDF hit distinct from a scene SDFGrid
/// hit can still do so - the value is only a convention stored in LumenGDFTraceResult::hitType.
static const uint kLumenGDFHitType = 6;

/// Dirty-region flags (mirror VoxelRange::added / removed semantics).
static const uint kLumenGDFDirtyFlagAdded = 1u << 0;   ///< Region needs (re)compose from instances.
static const uint kLumenGDFDirtyFlagRemoved = 1u << 1; ///< Region must be cleared to empty distance.

/// Instance layer-mask bits. "Static" and "dynamic" here describe the LEVEL class, not
/// whether an instance is animated:
///   - DYNAMIC level 0 follows the camera and is recomposed whenever it scrolls, so it
///     can cheaply include BOTH static and dynamic instances (layerMask = 3).
///   - STATIC levels (>= 1) are baked once at the world origin; only STATIC instances
///     may feed them (layerMask & kLumenGDFLayerStatic), so a moving instance never
///     forces a far-field re-bake. This is the "static/dynamic separated" requirement.
static const uint kLumenGDFLayerDynamic = 1u << 0; ///< This instance may feed dynamic level 0.
static const uint kLumenGDFLayerStatic = 1u << 1;  ///< This instance may feed static levels >= 1.

/// Sentinel: no instance/region selected.
static const uint kLumenGDFInvalidID = 0xFFFFFFFFu;

// -------------------------------------------------------------------------------------
// Per-level parameters (host mirror: LumenGDFLevel in LumenGlobalDistanceField.h)
// -------------------------------------------------------------------------------------
// 32 bytes, Slang scalar layout == C layout (no 16-byte-aligned float3 members).
//     +0  resolution      voxels per side (== LumenGDFLevel::resolution)
//     +4  format          kLumenMeshSDFFormatR16Float (0, default level 0) / R8Snorm (1)
//     +8  worldExtent     world meters; == LumenGDFLevel::worldExtent = baseExtent * 2^m
//     +12 voxelSize       world meters; == LumenGDFLevel::voxelSize = extent / resolution
//     +16 quantRange      R8Snorm scale R of this level (encode/decode); unused for R16Float
//     +20 emptyDistance   world distance stored in empty voxels of this level (> 0 required
//                         by the host; <= quantRange for R8Snorm levels, <= 65504 for R16Float)
//     +24 flags           reserved (0 for now)
//     +28 pad
struct LumenGDFLevelParams
{
    uint resolution;   // +0
    uint format;       // +4
    float worldExtent; // +8
    float voxelSize;   // +12
    float quantRange;  // +16
    float emptyDistance; // +20
    uint flags;        // +24
    uint pad;          // +28
};

// -------------------------------------------------------------------------------------
// Clipmap-wide parameters (the CB contract; host mirror: LumenGlobalDistanceField)
// -------------------------------------------------------------------------------------
// 56 bytes. Arranged so every float3/int3 starts on a 16-byte boundary, so the layout
// is identical under Slang's uniform (cbuffer) and scalar (structured) rules.
//     +0   cameraCenter (float3)   SNAPPED camera center (getCameraCenter()); dynamic level center
//     +12  emptyDistance (float)   fallback empty distance for levels that do not set one
//     +16  levelCount              getLevelCount()
//     +20  dynamicLevelCount       kDynamicLevels (1)
//     +24  resolution              getResolution() (all levels share it)
//     +28  instanceCount           valid entries in the GDF instance StructuredBuffer
//     +32  dirtyRegionCount        valid entries in the dirty-region StructuredBuffer
//     +36  frameIndex              frame counter (stats / debug)
//     +40  scroll (int3)           scrollFromCameraMove(), level-0 voxel units; informational
//     +52  pad
struct LumenGDFClipmapParams
{
    float3 cameraCenter;      // +0  (host: getCameraCenter(), snapped)
    float emptyDistance;      // +12
    uint levelCount;          // +16 (host: getLevelCount())
    uint dynamicLevelCount;   // +20 (host: kDynamicLevels)
    uint resolution;          // +24 (host: getResolution())
    uint instanceCount;       // +28
    uint dirtyRegionCount;    // +32
    uint frameIndex;          // +36
    int3 scroll;              // +40 (host: scrollFromCameraMove(); informational)
    float pad;                // +52
};

// -------------------------------------------------------------------------------------
// Dirty region (host mirror: VoxelRange in LumenGlobalDistanceField.h)
// -------------------------------------------------------------------------------------
// One rectangular voxel box inside one level that must be (re)composed or cleared this
// frame. The host derives these from dirtyRegions()/removedRegions() on the CPU clipmap:
// after a scroll of s level-0 voxels on axis a (thick = min(|s|, resolution)), the newly
// covered slab is [R-thick, R-1] (s > 0) or [0, thick-1] (s < 0) on axis a, spanning
// [0, R-1] on the other two axes; `added` (bit0) vs `removed` (bit1) selects compose vs
// clear. min/max are INCLUSIVE level-local voxel indices.
// 36 bytes.
struct LumenGDFDirtyRegion
{
    uint level; // +0  which GDF level the box lives in
    uint axis;  // +4  0=x, 1=y, 2=z; informational only (mirror of VoxelRange::axis)
    int3 min;   // +8  inclusive lower corner (level-local voxel indices)
    int3 max;   // +20 inclusive upper corner
    uint flags; // +32 kLumenGDFDirtyFlag* (bit0 added, bit1 removed)
    uint pad;   // +36
};

// -------------------------------------------------------------------------------------
// GDF instance table (host trims each frame; one entry per instance that may feed the GDF)
// -------------------------------------------------------------------------------------
// The per-instance "mesh -> atlas reference" is atlasInstanceID: it indexes the S6-B2
// atlas instance table (LumenMeshSDFAtlasInstance), which carries the world transform
// (invRows), the world AABB (boundsMin/Max) and the meshID (-> volume descriptor +
// page table -> atlas pages). boundsMin/boundsMax are duplicated here so voxel culling
// never touches the atlas table; the host must keep them equal to the atlas entry.
// resident = 1 means "include this frame": the instance's volume is fully resident in
// the atlas (page table slots valid) AND the host wants it in the GDF (budget/pruning).
// layerMask selects the level classes (see kLumenGDFLayer* above).
// 40 bytes.
struct LumenGDFInstance
{
    uint atlasInstanceID; // +0  -> LumenMeshSDFAtlasInstance in the atlas table
    uint resident;        // +4  1 = include in compose this frame (atlas resident + host filter)
    uint layerMask;       // +8  kLumenGDFLayer* bits
    uint pad;             // +12
    float3 boundsMin;     // +16 world AABB min (voxel culling)
    float3 boundsMax;     // +28 world AABB max
};

// -------------------------------------------------------------------------------------
// Sphere-trace miss reasons (style mirrors LumenMeshSDFMissReason, S6-B1)
// -------------------------------------------------------------------------------------
enum class LumenGDFMissReason : uint
{
    None = 0,              ///< result.hit == true; missReason is not meaningful.
    NoGrid = 1,            ///< levelCount == 0 (no GDF bound).
    DegenerateRay = 2,     ///< Ray direction length ~ 0.
    RayMissedBounds = 3,   ///< Ray exited [tMin, tMax] without crossing the surface.
    MaxSteps = 4,          ///< Step budget exhausted inside [tMin, tMax].
};

// -------------------------------------------------------------------------------------
// Trace parameters / result (S6-B4 contract)
// -------------------------------------------------------------------------------------

/// Per-call overrides; all-zero fields mean "use frozen defaults".
struct LumenGDFTraceParams
{
    uint maxSteps;         // +0  0 -> kLumenGDFMaxSteps
    float minStepScale;    // +4  0 -> kLumenGDFMinStepScale
    float hitEpsScale;     // +8  0 -> kLumenGDFHitEpsScale
    float tMax;            // +12 convenience entry: 0 -> kLumenGDFMaxDistance (ignored by the
                           //     explicit tMin/tMax entry)
};

/// Result of one GDF sphere trace (S6-B4 output contract).
struct LumenGDFTraceResult
{
    bool hit;                 ///< Distance field <= hit epsilon at t (surface crossed).
    float t;                  ///< World-space hit distance along the unit direction.
    float d;                  ///< Signed distance (world) at the hit point.
    uint steps;               ///< Steps taken (stats / S6-C4).
    float minDistance;        ///< Min |d| observed along the traced segment (world units).
    uint hitType;             ///< kLumenGDFHitType on hit, 0 otherwise (Falcor-interop).
    LumenGDFMissReason missReason; ///< Valid when hit == false.
};

// -------------------------------------------------------------------------------------
// Pure world <-> voxel math. Mirrors the CPU formulas in LumenGlobalDistanceField.h so
// the GPU and CPU never disagree on a voxel's identity. These functions do NOT touch any
// resource; they only need the level table + clipmap params.
// -------------------------------------------------------------------------------------

/// World-space center of a level's grid: dynamic levels follow the snapped camera,
/// static levels are anchored at the world origin (host mirror: levelCenter()).
float3 lumenGDFLevelCenter(LumenGDFClipmapParams c, uint level)
{
    return (level < c.dynamicLevelCount) ? c.cameraCenter : float3(0.f, 0.f, 0.f);
}

/// gridMin_m = levelCenter_m - extent_m / 2: the world coordinate of voxel (0,0,0)'s low
/// corner (host mirror: worldToIndex/indexToWorld gridMin).
float3 lumenGDFGridMin(StructuredBuffer<LumenGDFLevelParams> levelTable, LumenGDFClipmapParams c, uint level)
{
    const LumenGDFLevelParams lp = levelTable[level];
    return lumenGDFLevelCenter(c, level) - float3(0.5f * lp.worldExtent);
}

/// True when maxAbs(p - center) <= extent/2, footprint includes its boundary
/// (host mirror: insideFootprint()).
bool lumenGDFInsideFootprint(LumenGDFLevelParams lp, float3 center, float3 p)
{
    const float3 d = abs(p - center);
    return max(d.x, max(d.y, d.z)) <= 0.5f * lp.worldExtent;
}

/// Pick the level that should own `worldPos` (host mirror: levelIndexForWorld()):
///  1. NEAR FIELD -> the dynamic levels (finest first): level 0 whenever
///     maxAbs(p - cameraCenter) <= extent_0 / 2.
///  2. FAR FIELD  -> the finest static level whose origin-anchored footprint contains p.
///  3. FALLBACK   -> the coarsest remaining level (out-of-footprint samples are clamped;
///     the sampler reports inBounds == false there).
uint lumenGDFLevelIndexForWorld(StructuredBuffer<LumenGDFLevelParams> levelTable, LumenGDFClipmapParams c, float3 worldPos)
{
    const uint dynamicCount = min(c.dynamicLevelCount, c.levelCount);
    for (uint m = 0; m < dynamicCount; ++m)
    {
        if (lumenGDFInsideFootprint(levelTable[m], c.cameraCenter, worldPos))
            return m;
    }
    const float3 origin = float3(0.f, 0.f, 0.f);
    for (uint m = dynamicCount; m < c.levelCount; ++m)
    {
        if (lumenGDFInsideFootprint(levelTable[m], origin, worldPos))
            return m;
    }
    return c.levelCount > 0u ? c.levelCount - 1u : 0u;
}

/// World -> level-local voxel index (host mirror: worldToIndex()):
/// vc = (worldPos - gridMin_m) / voxelSize_m; index = floor(vc); inBounds requires every
/// component in [0, resolution). Floor, not truncation - negative coordinates floor down.
void lumenGDFWorldToIndex(
    StructuredBuffer<LumenGDFLevelParams> levelTable,
    LumenGDFClipmapParams c,
    uint level,
    float3 worldPos,
    out int3 index,
    out bool inBounds)
{
    const LumenGDFLevelParams lp = levelTable[level];
    const float3 gridMin = lumenGDFGridMin(levelTable, c, level);
    const float3 vc = (worldPos - gridMin) / max(lp.voxelSize, 1e-12f);
    index = int3(floor(vc));
    const int R = int(lp.resolution);
    inBounds = index.x >= 0 && index.x < R && index.y >= 0 && index.y < R && index.z >= 0 && index.z < R;
}

/// Inverse of worldToIndex: the world-space center of the voxel at `index` (host mirror:
/// indexToWorld()): gridMin_m + (index + 0.5) * voxelSize_m. Always well-defined; the
/// result is inside the footprint only when `index` was in bounds.
float3 lumenGDFIndexToWorld(
    StructuredBuffer<LumenGDFLevelParams> levelTable,
    LumenGDFClipmapParams c,
    uint level,
    int3 index)
{
    const LumenGDFLevelParams lp = levelTable[level];
    const float3 gridMin = lumenGDFGridMin(levelTable, c, level);
    return gridMin + (float3(index) + float3(0.5f, 0.5f, 0.5f)) * lp.voxelSize;
}

// -------------------------------------------------------------------------------------
// Quantized encode / decode (identical codec to S6-B1, see the file header)
// -------------------------------------------------------------------------------------

/// World distance -> value to store. R16Float takes the raw value; R8Snorm writes d / R
/// clamped to [-1, 1] (the R8_SNORM resource converts float <-> code/127 automatically).
float lumenGDFEncode(LumenGDFLevelParams lp, float worldDistance)
{
    if (lp.format == kLumenMeshSDFFormatR8Snorm)
        return clamp(worldDistance / max(lp.quantRange, 1e-12f), -1.f, 1.f);
    return worldDistance;
}

/// Stored value -> world distance. R16Float is identity; R8Snorm loads code/127 and
/// rescales by R.
float lumenGDFDecode(LumenGDFLevelParams lp, float stored)
{
    if (lp.format == kLumenMeshSDFFormatR8Snorm)
        return stored * max(lp.quantRange, 1e-12f);
    return stored;
}

/// Empty distance for a level: the level's own emptyDistance, falling back to the CB
/// default or a few voxels when the host did not populate it.
float lumenGDFEmptyDistanceFor(LumenGDFLevelParams lp, LumenGDFClipmapParams c)
{
    if (lp.emptyDistance > 0.f)
        return lp.emptyDistance;
    if (c.emptyDistance > 0.f)
        return c.emptyDistance;
    return 8.f * max(lp.voxelSize, 1e-12f);
}

#endif // __LUMEN_GDF_DATA_SLANG__
