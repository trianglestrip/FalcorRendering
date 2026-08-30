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
//  LumenGI - S6-B3 Global Distance Field compose shader (compute)
//  -------------------------------------------------------------------------------------
//  Composes the S6-B1 Mesh SDF instances (residing in the S6-B2 atlas) into the GDF
//  clipmap: for every voxel of a dirty region, iterate the resident instances whose world
//  AABB overlaps the voxel, sample the atlas at the voxel center (mip 0), and store the
//  MINIMUM signed distance (positive outside, world units). Empty voxels store the level's
//  empty distance so sphere tracing walks straight through them.
//
//  BUDGETED LOCAL UPDATES (only dirty voxels are written)
//  -------------------------------------------------------------------------------------
//  The shader is dispatched once per frame over the host's dirty-region list
//  (gGDFDirtyRegions, derived from the CPU clipmap's dirtyRegions()/removedRegions()):
//      dispatch = (ceil(R / 8), regionCount, ceil(R*R / 8)) with [numthreads(8,1,8)]
//    * dispatchThreadID.x      = voxel X offset inside the region (0 .. dims.x-1)
//    * dispatchThreadID.y      = dirty-region index
//    * dispatchThreadID.z      = flattened voxel (Y, Z) inside the region (0 .. dims.y*dims.z-1)
//  Voxels outside the region's box, or a level >= levelCount, early-out before any
//  resource access, so non-dirty voxels are never read or written. Removed slabs
//  (kLumenGDFDirtyFlagRemoved) are cleared to the empty distance instead of recomposed.
//
//  STATIC / DYNAMIC LEVELS (separated)
//  -------------------------------------------------------------------------------------
//  * Level 0 is DYNAMIC (camera-centered, scrolls; recomposed on scroll): every instance
//    with layerMask & kLumenGDFLayerDynamic contributes.
//  * Levels >= kLumenGDFDynamicLevels are STATIC (origin-anchored, baked once): only
//    instances with layerMask & kLumenGDFLayerStatic contribute, so animated instances
//    never invalidate the far field.
//
//  DISTANCE CODE (mirror S6-B1)
//  -------------------------------------------------------------------------------------
//  Per logical level: identity distance values with a per-level quantRange codec. The
//  runtime staging atlas views below are R32Float and preserve the source values directly.
//  LumenGDFData.slang for the exact encode. The atlas sample returns OUTPUT units; the
//  instance's worldScalePerOutput converts to world units (exact for similarities,
//  conservative max-axis factor for non-uniformly scaled instances - matching the atlas).
//
//  RESOURCE LAYOUT (the root pass binds these; nothing here is host-owned)
//  -------------------------------------------------------------------------------------
//  cbuffer LumenGDFComposeCB   - LumenGDFClipmapParams gClipmap (see LumenGDFData.slang)
//  gGDFLevelTable              - StructuredBuffer<LumenGDFLevelParams>[levelCount]
//  gGDFInstances               - StructuredBuffer<LumenGDFInstance>[instanceCount]
//  gGDFDirtyRegions            - StructuredBuffer<LumenGDFDirtyRegion>[dirtyRegionCount]
//  gGDFLevel                   - RWTexture3D<float> UAV for the level in the current batch.
//  gFineAtlas / gCoarseAtlas   - S6-B2 atlas textures (SRV)
//  gPageTable / gVolumes / gInstances / gAtlas* scalars - S6-B2 atlas bindings (SRV)
// =====================================================================================

#include "LumenGDFData.slang"
#include "LumenMeshSDFAtlas.slang" // LumenMeshSDFAtlasBindings + lumenMeshSDFAtlasSample

// Falcor only emits is_valid_<name> defines for optional resources; everything the
// compose pass needs is required, so default everything to 1 so the file is statically
// compilable before the host binds anything. Any resource a host truly wants optional
// must define is_valid_<name> 0 BEFORE including this file.
#ifndef is_valid_gGDFLevel
#define is_valid_gGDFLevel 1
#endif
#ifndef is_valid_gGDFLevelTable
#define is_valid_gGDFLevelTable 1
#endif
#ifndef is_valid_gGDFInstances
#define is_valid_gGDFInstances 1
#endif
#ifndef is_valid_gGDFDirtyRegions
#define is_valid_gGDFDirtyRegions 1
#endif
#ifndef is_valid_gFineAtlas
#define is_valid_gFineAtlas 1
#endif
#ifndef is_valid_gCoarseAtlas
#define is_valid_gCoarseAtlas 1
#endif
#ifndef is_valid_gPageTable
#define is_valid_gPageTable 1
#endif
#ifndef is_valid_gVolumes
#define is_valid_gVolumes 1
#endif
#ifndef is_valid_gInstances
#define is_valid_gInstances 1
#endif

#define is_valid(name) (is_valid_##name != 0)

// -------------------------------------------------------------------------------------
// Constant buffer. Field list is the LumenGDFClipmapParams contract (LumenGDFData.slang):
//   float3 gClipmap.cameraCenter;   float gClipmap.emptyDistance;
//   uint   gClipmap.levelCount, gClipmap.dynamicLevelCount, gClipmap.resolution,
//   uint   gClipmap.instanceCount, gClipmap.dirtyRegionCount, gClipmap.frameIndex;
//   int3   gClipmap.scroll;
// -------------------------------------------------------------------------------------
cbuffer LumenGDFComposeCB
{
    LumenGDFClipmapParams gClipmap;
    // Keep all compose constants in one explicit CB. Falcor's implicit global
    // cbuffer cannot be combined reliably with this nested CB on the D3D12
    // descriptor path (C4 E2d reproduces E_INVALIDARG with only one scalar).
    uint gAtlasInstanceCount;               ///< Atlas instance count (== gInstances count).
    uint gAtlasVolumeCount;                 ///< Atlas volume count (== gVolumes count).
    uint gAtlasPagesPerSide;                ///< Atlas geometry: pagesPerSide^3 slots/atlas.
}

StructuredBuffer<LumenGDFLevelParams> gGDFLevelTable;
StructuredBuffer<LumenGDFInstance> gGDFInstances;
StructuredBuffer<LumenGDFDirtyRegion> gGDFDirtyRegions;

// One 3D texture per clipmap level. The host uses R32_FLOAT typed UAVs and applies the
// logical level codec in lumenGDFEncode/lumenGDFDecode.
RWTexture3D<float> gGDFLevel;

// S6-B2 atlas bindings, bound as flat globals so each one can be is_valid-guarded and so
// the aggregate below constructs the LumenMeshSDFAtlasBindings value lumenMeshSDFAtlasSample
// expects. Field order must match LumenMeshSDFAtlasBindings exactly.
Texture3D<float> gFineAtlas;                    ///< R32Float staging view of fine source values.
Texture3D<float> gCoarseAtlas;                  ///< R32Float staging view of normalized coarse values.
StructuredBuffer<uint> gPageTable;              ///< [instanceID * 12 + mip] = base page slot.
StructuredBuffer<LumenMeshSDFVolumeDescriptor> gVolumes; ///< Per mesh (deduplicated).
StructuredBuffer<LumenMeshSDFAtlasInstance> gInstances;  ///< Per registered instance.

// -------------------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------------------

/// True when the instance's world AABB overlaps the voxel's world AABB (conservative
/// under rotation - both AABBs are world-space boxes, so a false negative never happens,
/// only an occasional conservative positive that is resolved by the atlas sample).
bool lumenGDFVoxelOverlapsInstance(LumenGDFInstance inst, float3 voxelMin, float3 voxelMax)
{
    return all(voxelMin <= inst.boundsMax) && all(voxelMax >= inst.boundsMin);
}

/// Compose a single voxel of `level` at the absolute level-local index `voxel`.
///   * removed regions are cleared to the level's empty distance;
///   * otherwise the min over overlapping resident instances of the requested layer class
///     is stored; a voxel with no contributing instance stores the empty distance.
void lumenGDFComposeVoxel(LumenGDFClipmapParams c, uint level, int3 voxel, uint regionFlags)
{
    if (!is_valid(gGDFLevel) || !is_valid(gGDFLevelTable) || !is_valid(gGDFInstances))
        return;
    if (level >= c.levelCount)
        return;

    const LumenGDFLevelParams lp = gGDFLevelTable[level];
    const float emptyDistance = lumenGDFEmptyDistanceFor(lp, c);

    // Removed/stale slab: clear to empty so recycled voxels never serve stale surface data.
    if ((regionFlags & kLumenGDFDirtyFlagRemoved) != 0u)
    {
        gGDFLevel[uint3(voxel)] = lumenGDFEncode(lp, emptyDistance);
        return;
    }

    // Without the atlas bindings there is nothing to compose from: write empty (the host
    // must bind the S6-B2 atlas before dispatching the compose pass).
    if (!is_valid(gFineAtlas) || !is_valid(gCoarseAtlas) || !is_valid(gPageTable) || !is_valid(gVolumes))
    {
        gGDFLevel[uint3(voxel)] = lumenGDFEncode(lp, emptyDistance);
        return;
    }

    // Voxel world AABB (center +/- half voxel) for instance culling.
    const float3 voxelCenter = lumenGDFIndexToWorld(gGDFLevelTable, c, level, voxel);
    const float halfVoxel = 0.5f * max(lp.voxelSize, 1e-12f);
    const float3 voxelMin = voxelCenter - float3(halfVoxel);
    const float3 voxelMax = voxelCenter + float3(halfVoxel);

    const bool isDynamicLevel = level < c.dynamicLevelCount;
    const uint wantLayer = isDynamicLevel ? kLumenGDFLayerDynamic : kLumenGDFLayerStatic;

    float best = emptyDistance; // no contribution found -> empty
    for (uint i = 0; i < c.instanceCount; ++i)
    {
        const LumenGDFInstance inst = gGDFInstances[i];
        if (inst.resident == 0u)
            continue; // not atlas-resident this frame, or host-pruned
        if ((inst.layerMask & wantLayer) == 0u)
            continue; // instance does not feed this level class (static/dynamic separation)
        if (!lumenGDFVoxelOverlapsInstance(inst, voxelMin, voxelMax))
            continue; // voxel outside the instance's world AABB

        if (!is_valid(gInstances) || inst.atlasInstanceID >= gAtlasInstanceCount)
            continue; // guard against a stale atlas reference (never clamp silently)

        LumenMeshSDFAtlasInstance atlasInst = gInstances[inst.atlasInstanceID];
        if (atlasInst.meshID >= gAtlasVolumeCount)
            continue;

        LumenMeshSDFAtlasBindings atlas;
        atlas.fineAtlas = gFineAtlas;
        atlas.coarseAtlas = gCoarseAtlas;
        atlas.pageTable = gPageTable;
        atlas.volumes = gVolumes;
        atlas.instances = gInstances;
        atlas.instanceCount = gAtlasInstanceCount;
        atlas.volumeCount = gAtlasVolumeCount;
        atlas.pagesPerSide = gAtlasPagesPerSide;

        // Sample the mesh SDF atlas at the voxel center on mip 0. The sample returns the
        // OUTPUT-space distance (math convention); convert to world via the instance's
        // worldScalePerOutput (exact for similarities, conservative max-axis otherwise).
        const LumenMeshSDFAtlasSampleResult sample = lumenMeshSDFAtlasSample(atlas, inst.atlasInstanceID, voxelCenter, 0);
        if (sample.resident)
        {
            const float worldScale = max(atlasInst.worldScalePerOutput, 1e-12f);
            best = min(best, sample.distanceOutput * worldScale);
        }
    }

    gGDFLevel[uint3(voxel)] = lumenGDFEncode(lp, best);
}

// -------------------------------------------------------------------------------------
// Entry point
// -------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 1, 8)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (!is_valid(gGDFDirtyRegions))
        return;

    const uint regionIndex = dispatchThreadID.y;
    if (regionIndex >= gClipmap.dirtyRegionCount)
        return;

    const LumenGDFDirtyRegion region = gGDFDirtyRegions[regionIndex];
    if (region.level >= gClipmap.levelCount)
        return;

    const int3 dims = region.max - region.min + int3(1, 1, 1);
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0)
        return; // degenerate region - never touched

    const int x = int(dispatchThreadID.x);
    if (x >= dims.x)
        return;

    const uint yz = dispatchThreadID.z; // flattened (y, z) inside the region
    if (yz >= uint(dims.y * dims.z))
        return;

    const int3 voxel = region.min + int3(x, int(yz % uint(dims.y)), int(yz / uint(dims.y)));
    lumenGDFComposeVoxel(gClipmap, region.level, voxel, region.flags);
}
