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
//  LumenMeshSDFAtlasTests.cpp - CPU tests for the S6-B2 Mesh SDF atlas.
//  -------------------------------------------------------------------------------------
//  Structure mirrors LumenSurfaceCacheTests.cpp (CPU_TEST + EXPECT_*). NOT registered
//  in CMake by design (S6-B2 pure-code round); register in
//  Source/Tools/FalcorTest/CMakeLists.txt next to LumenSurfaceCacheTests.cpp when the
//  atlas is integrated.
//
//  COVERAGE (maps to task.md S6-B2 + S6-C2):
//  1. Frozen layout: page size, default geometry, bytes/page, instance table size
//     and field offsets, page table stride, mip/brick formulas
//  2. Mesh dedup + page sharing: N instances of one mesh -> one set of pages,
//     refcounts, shared page slots; same content + different resolution -> no share
//  3. Non-uniform scale + rotation: world->voxel round trip exact, distance approx
//     within tolerance, NonUniformScale flag, exact conservative world bounds
//  4. Atlas full: new instances degrade to non-resident (miss, no crash), per-reason
//     miss counters
//  5. Budget enforcement: deterministic LRU eviction sequence, bytes <= budget,
//     temporary over-budget counted when nothing is evictable
//  6. Eviction + reload: evicted mip misses, touchInstance() re-residents it with a
//     bumped generation; host re-upload restores samples
//  7. Sampling mirror: sphere field at mip0 and coarse mips, miss reasons
//     (OutOfInstanceBounds / InvalidMip / NoInstance), hit/miss counters
//  8. Multi-brick mip tiling: 64^3 mip0 spans 2^3 pages, sampling across bricks
//  9. Determinism: identical sequences produce identical stats and page tables
//  10. Resource stats: meshSdfBytes == resident bytes, within-budget reporting,
//      S6-B1 descriptor rebind hook
//  11. Low-quality mip0 lives in the coarse atlas and decodes through quantRange
// =====================================================================================

#include "Testing/UnitTest.h"
#include "../../../../RenderPasses/LumenGI/MeshSDF/LumenMeshSDFAtlas.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Falcor
{
namespace
{

using namespace LumenGI::MeshSDF;

// EXPECT_EQ family has no float comparator; use an explicit near-assertion macro.
#define LUMEN_EXPECT_NEAR(actual, expected, tol)                                                     \
    EXPECT_MSG(                                                                                      \
        std::fabs(static_cast<float>(actual) - static_cast<float>(expected)) <= static_cast<float>(tol), \
        "|" #actual " - " #expected "| <= " #tol)

// -------------------------------------------------------------------------------------
// Synthetic sphere volume (the S6-B1 analytic-field pattern used by Agent G's tests)
// -------------------------------------------------------------------------------------

constexpr float kSphereCenter = 0.5f;
constexpr float kSphereRadius = 0.25f;
constexpr float kSphereQuantRange = 0.7f; // max |d| over [0,1]^3 corners ~ 0.616

LumenMeshSDFAtlasMeshDesc makeSphereMesh(
    uint32_t n,
    uint64_t contentHash,
    VolumeFormat formatMip0 = VolumeFormat::R16Float)
{
    LumenMeshSDFAtlasMeshDesc desc;
    desc.resolution = {n, n, n};
    desc.mipCount = 1 + static_cast<uint32_t>(std::ceil(std::log2(static_cast<float>(n))));
    desc.formatMip0 = formatMip0;
    desc.pooling = MipPooling::MinAbs;
    desc.contentHash = contentHash;
    desc.quantRange = kSphereQuantRange;
    desc.normalizationScale = 1.f;
    desc.voxelSize = 1.f / static_cast<float>(n);
    desc.bboxMin = {0.f, 0.f, 0.f};
    desc.bboxMax = {1.f, 1.f, 1.f};
    desc.signConvention = kLumenMeshSDFSignConventionPositiveOutside;
    desc.signReliable = 1;
    return desc;
}

/// Signed distance of the sphere at an OUTPUT-space point.
float sphereDistance(const float p[3])
{
    const float dx = p[0] - kSphereCenter;
    const float dy = p[1] - kSphereCenter;
    const float dz = p[2] - kSphereCenter;
    return std::sqrt(dx * dx + dy * dy + dz * dz) - kSphereRadius;
}

/// TEXTURE-STORED mip data for the sphere (see uploadVolumeFloats contract):
/// fine = true distance; coarse = distance / quantRange clamped to [-1, 1].
/// Voxel centers of mip m texel t sit at bboxMin + ((t + 0.5) << m) * voxelSize.
std::vector<float> sphereStoredData(const LumenMeshSDFAtlasMeshDesc& desc, uint32_t mip)
{
    const std::array<uint32_t, 3> dims = atlasMipDims(desc.resolution, mip);
    std::vector<float> data(static_cast<size_t>(dims[0]) * dims[1] * dims[2]);
    const float voxelSize = desc.voxelSize * static_cast<float>(uint64_t(1) << mip);
    const bool fine = (mip == 0 && desc.formatMip0 == VolumeFormat::R16Float);
    size_t i = 0;
    for (uint32_t z = 0; z < dims[2]; ++z)
    {
        for (uint32_t y = 0; y < dims[1]; ++y)
        {
            for (uint32_t x = 0; x < dims[0]; ++x)
            {
                const float p[3] = {
                    desc.bboxMin[0] + (static_cast<float>(x) + 0.5f) * voxelSize,
                    desc.bboxMin[1] + (static_cast<float>(y) + 0.5f) * voxelSize,
                    desc.bboxMin[2] + (static_cast<float>(z) + 0.5f) * voxelSize,
                };
                const float d = sphereDistance(p);
                data[i++] = fine ? d : std::max(-1.f, std::min(1.f, d / kSphereQuantRange));
            }
        }
    }
    return data;
}

/// Upload every mip of a mesh into its resident pages (CPU snapshot). The macro
/// expansion references `ctx`, so helpers must take the context by this name.
void uploadSphere(UnitTestContext& ctx, LumenMeshSDFAtlas& atlas, uint32_t meshID)
{
    LumenMeshSDFVolumeDescriptor vol;
    EXPECT_TRUE(atlas.getVolumeDescriptor(meshID, vol));
    LumenMeshSDFAtlasMeshDesc desc;
    desc.resolution = {vol.resolution[0], vol.resolution[1], vol.resolution[2]};
    desc.mipCount = vol.mipCount;
    desc.formatMip0 = vol.formatMip0 == static_cast<uint32_t>(VolumeFormat::R16Float) ? VolumeFormat::R16Float : VolumeFormat::R8Snorm;
    desc.quantRange = kSphereQuantRange;
    desc.normalizationScale = 1.f;
    desc.voxelSize = vol.voxelSize;
    desc.bboxMin = {vol.bboxMin[0], vol.bboxMin[1], vol.bboxMin[2]};
    desc.bboxMax = {vol.bboxMax[0], vol.bboxMax[1], vol.bboxMax[2]};
    for (uint32_t mip = 0; mip < vol.mipCount; ++mip)
    {
        EXPECT_TRUE(atlas.uploadVolumeFloats(meshID, mip, sphereStoredData(desc, mip)));
    }
}

/// Forward affine for an instance: world = Rz(angle) * diag(scale) * p + t.
LumenMeshSDFAtlasInstanceDesc makeInstance(
    const float scale[3],
    float angleDegZ,
    const float translate[3])
{
    const float c = std::cos(angleDegZ * 3.14159265358979f / 180.f);
    const float s = std::sin(angleDegZ * 3.14159265358979f / 180.f);
    LumenMeshSDFAtlasInstanceDesc inst;
    inst.forwardLinear = {
        c * scale[0], -s * scale[1], 0.f,
        s * scale[0], c * scale[1], 0.f,
        0.f, 0.f, scale[2],
    };
    inst.forwardTranslation = {translate[0], translate[1], translate[2]};
    return inst;
}

// -------------------------------------------------------------------------------------
// 1. Frozen layout
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_LayoutFrozen)
{
    EXPECT_EQ(kLumenMeshSDFAtlasPageSize, 32u);
    EXPECT_EQ(kLumenMeshSDFAtlasPageSizeBits, 5u);
    EXPECT_EQ(kLumenMeshSDFAtlasDefaultPagesPerSide, 8u);
    EXPECT_EQ(kLumenMeshSDFAtlasDefaultAtlasSizeTexels, 256u);
    EXPECT_EQ(kLumenMeshSDFAtlasMaxInstances, 4096u);

    // Page byte sizes (frozen formula: 32^3 texels * bytes per texel).
    EXPECT_EQ(kLumenMeshSDFAtlasBytesPerFinePage, 65536ull);
    EXPECT_EQ(kLumenMeshSDFAtlasBytesPerCoarsePage, 32768ull);

    // Default atlas capacities: 8^3 = 512 slots per atlas; full = 32 MiB + 16 MiB.
    LumenMeshSDFAtlas atlas;
    EXPECT_EQ(atlas.pagesPerSide(), 8u);
    EXPECT_EQ(atlas.capacityPagesPerAtlas(), 512ull);
    EXPECT_EQ(kLumenMeshSDFAtlasBytesPerFinePage * 512, 32ull * 1024 * 1024);
    EXPECT_EQ(kLumenMeshSDFAtlasBytesPerCoarsePage * 512, 16ull * 1024 * 1024);

    // GPU instance table entry: 84 bytes, frozen offsets.
    EXPECT_EQ(sizeof(LumenMeshSDFAtlasInstance), 84u);
    EXPECT_EQ(offsetof(LumenMeshSDFAtlasInstance, invRows), 0u);
    EXPECT_EQ(offsetof(LumenMeshSDFAtlasInstance, boundsMin), 48u);
    EXPECT_EQ(offsetof(LumenMeshSDFAtlasInstance, boundsMax), 60u);
    EXPECT_EQ(offsetof(LumenMeshSDFAtlasInstance, meshID), 72u);
    EXPECT_EQ(offsetof(LumenMeshSDFAtlasInstance, flags), 76u);
    EXPECT_EQ(offsetof(LumenMeshSDFAtlasInstance, worldScalePerOutput), 80u);

    // Page table stride and buffer size (pre-initialized to NotResident).
    EXPECT_EQ(kLumenMeshSDFMaxMipCount, 12u);
    const std::vector<uint32_t>& pageTable = atlas.getPageTableBuffer();
    EXPECT_EQ(pageTable.size(), static_cast<size_t>(4096) * 12);
    for (uint32_t v : pageTable)
    {
        EXPECT_EQ(v, kLumenMeshSDFNotResident);
    }

    // Mip/brick formulas on a 64^3 volume: mip0 spans 2^3 = 8 pages, mip1 1 page.
    EXPECT_EQ(atlasMipDims({64, 64, 64}, 0)[0], 64u);
    EXPECT_EQ(atlasMipDims({64, 64, 64}, 1)[0], 32u);
    EXPECT_EQ(atlasMipDims({3, 3, 3}, 1)[0], 2u); // ceil-halving
    EXPECT_EQ(atlasPagesForMip({64, 64, 64}, 0), 8u);
    EXPECT_EQ(atlasPagesForMip({64, 64, 64}, 1), 1u);
    EXPECT_EQ(atlasPagesForMip({32, 32, 32}, 0), 1u);
    EXPECT_EQ(atlasPagesForMip({33, 33, 33}, 0), 8u); // 2^3 bricks for 33^3
    EXPECT_TRUE(atlasKindForMip(VolumeFormat::R16Float, 0) == AtlasKind::Fine);
    EXPECT_TRUE(atlasKindForMip(VolumeFormat::R8Snorm, 0) == AtlasKind::Coarse);
    EXPECT_TRUE(atlasKindForMip(VolumeFormat::R16Float, 1) == AtlasKind::Coarse);
}

// -------------------------------------------------------------------------------------
// 2. Mesh dedup + page sharing
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_SharedPagesAcrossInstances)
{
    LumenMeshSDFAtlas atlas(4, 0, 0, 1); // 64 slots per atlas, min residency 1

    const uint32_t meshA = atlas.registerMesh(makeSphereMesh(16, 0xAAAA));
    EXPECT_NE(meshA, kLumenMeshSDFAtlasInvalidID);
    // Identical content -> same meshID (pages shared).
    EXPECT_EQ(atlas.registerMesh(makeSphereMesh(16, 0xAAAA)), meshA);
    // Same content, different resolution -> different mesh (no sharing).
    const uint32_t meshB = atlas.registerMesh(makeSphereMesh(32, 0xAAAA));
    EXPECT_NE(meshB, meshA);

    LumenMeshSDFAtlasInstanceDesc identity;
    const uint32_t i0 = atlas.registerInstance(meshA, identity);
    const uint32_t i1 = atlas.registerInstance(meshA, identity);
    const uint32_t i2 = atlas.registerInstance(meshA, identity);
    const uint32_t i3 = atlas.registerInstance(meshB, identity);
    EXPECT_NE(i0, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(i1, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(i2, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(i3, kLumenMeshSDFAtlasInvalidID);
    EXPECT_TRUE(atlas.isResident(i0));
    EXPECT_TRUE(atlas.isResident(i1));
    EXPECT_TRUE(atlas.isResident(i2));
    EXPECT_TRUE(atlas.isResident(i3));

    // 16^3 volume: mipCount = 5 (16,8,4,2,1) = 1 fine + 4 coarse pages.
    // 32^3 volume: mipCount = 6 (32,16,8,4,2,1) = 1 fine + 5 coarse pages.
    // Three instances of meshA share its 5 pages; meshB owns its 6.
    const LumenMeshSDFAtlasStats stats = atlas.getStats();
    EXPECT_EQ(stats.residentPages, 11u);
    EXPECT_EQ(stats.residentInstanceCount, 4u);
    EXPECT_EQ(stats.groupCount, 11u);
    EXPECT_EQ(stats.sharedGroupCount, 5u); // only meshA's groups are shared

    // Page slots of the shared instances are identical per mip.
    for (uint32_t mip = 0; mip < 5; ++mip)
    {
        EXPECT_EQ(atlas.pageSlot(i0, mip), atlas.pageSlot(i1, mip));
        EXPECT_EQ(atlas.pageSlot(i1, mip), atlas.pageSlot(i2, mip));
        EXPECT_TRUE(atlas.isResident(i0, mip));
    }
    EXPECT_TRUE(atlas.pageSlot(i0, 5) == kLumenMeshSDFAtlasInvalidPage); // beyond mipCount

    // Removing one instance leaves the shared pages resident (refcount 3 -> 2).
    atlas.removeInstance(i1);
    EXPECT_EQ(atlas.getStats().residentPages, 11u);
    EXPECT_FALSE(atlas.isResident(i1));
    EXPECT_TRUE(atlas.isResident(i0));

    // Removing all sharers destroys the group: pages are freed.
    atlas.removeInstance(i0);
    atlas.removeInstance(i2);
    EXPECT_EQ(atlas.getStats().residentPages, 6u); // only meshB remains
    EXPECT_EQ(atlas.getStats().releaseCount, 5u);  // meshA's 5 page-groups (mip0..mip4) each released once orphaned
}

// -------------------------------------------------------------------------------------
// 3. Non-uniform scale + rotation round trip
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_NonUniformScaleRoundTrip)
{
    LumenMeshSDFAtlas atlas(4, 0, 0, 1);
    const uint32_t mesh = atlas.registerMesh(makeSphereMesh(32, 0xBBBB));
    EXPECT_NE(mesh, kLumenMeshSDFAtlasInvalidID);

    // Non-uniform scale (2,3,4), rotation 30 deg around Z, translation.
    const float scale[3] = {2.f, 3.f, 4.f};
    const float translate[3] = {10.f, -5.f, 1.f};
    const LumenMeshSDFAtlasInstanceDesc inst = makeInstance(scale, 30.f, translate);
    const uint32_t instanceID = atlas.registerInstance(mesh, inst);
    EXPECT_NE(instanceID, kLumenMeshSDFAtlasInvalidID);
    EXPECT_TRUE(atlas.isResident(instanceID));

    // Instance flags: non-uniform scale must be detected.
    const std::vector<LumenMeshSDFAtlasInstance>& table = atlas.getInstanceTable();
    EXPECT_NE(table[instanceID].flags & kLumenMeshSDFInstanceFlagNonUniformScale, 0u);

    // World bounds: AABB of the 8 transformed output corners. With
    // F = Rz(30) * diag(2,3,4) and t = (10,-5,1): world = t + F*p.
    // Corner (1,0,0) maximizes x: 10 + 2*cos30; corner (0,1,0) minimizes x:
    // 10 - 3*sin30; corner (1,1,0) maximizes y: -5 + 2*sin30 + 3*cos30.
    const float c = std::cos(30.f * 3.14159265358979f / 180.f);
    const float s = std::sin(30.f * 3.14159265358979f / 180.f);
    LUMEN_EXPECT_NEAR(table[instanceID].boundsMin[0], 10.f - 3.f * s, 1e-4f);
    LUMEN_EXPECT_NEAR(table[instanceID].boundsMin[1], -5.f, 1e-4f);
    LUMEN_EXPECT_NEAR(table[instanceID].boundsMin[2], 1.f, 1e-4f);
    LUMEN_EXPECT_NEAR(table[instanceID].boundsMax[0], 10.f + 2.f * c, 1e-4f);
    LUMEN_EXPECT_NEAR(table[instanceID].boundsMax[1], -5.f + 2.f * s + 3.f * c, 1e-4f);
    LUMEN_EXPECT_NEAR(table[instanceID].boundsMax[2], 5.f, 1e-4f);

    // World -> voxel round trip is EXACT under the affine transform. Pick a world
    // point on the sphere surface and compare with the analytic inverse.
    const float pObj[3] = {0.75f, 0.5f, 0.5f}; // on the sphere (d = 0)
    const float w[3] = {
        translate[0] + c * scale[0] * pObj[0] - s * scale[1] * pObj[1],
        translate[1] + s * scale[0] * pObj[0] + c * scale[1] * pObj[1],
        translate[2] + scale[2] * pObj[2],
    };
    const std::array<float, 3> u = atlas.worldToVoxel(instanceID, w);
    // u = (pObj - bboxMin) * invVoxel - 0.5 -> pObj = (u + 0.5) * voxel
    LUMEN_EXPECT_NEAR(u[0], pObj[0] * 32.f - 0.5f, 1e-3f);
    LUMEN_EXPECT_NEAR(u[1], pObj[1] * 32.f - 0.5f, 1e-3f);
    LUMEN_EXPECT_NEAR(u[2], pObj[2] * 32.f - 0.5f, 1e-3f);

    // Sampling at the transformed surface point returns ~0 (the distance is
    // distorted by the anisotropy but stays within a few fine voxels of 0).
    uploadSphere(ctx, atlas, mesh);
    const LumenMeshSDFAtlasSampleResult r = atlas.sample(instanceID, w, 0);
    EXPECT_TRUE(r.resident);
    LUMEN_EXPECT_NEAR(r.distanceOutput, 0.f, 3.f / 32.f);

    // A point outside the world bounds misses with OutOfInstanceBounds.
    const float farPos[3] = {table[instanceID].boundsMax[0] + 10.f, 0.f, 0.f};
    const LumenMeshSDFAtlasSampleResult miss = atlas.sample(instanceID, farPos, 0);
    EXPECT_FALSE(miss.resident);
    EXPECT_TRUE(miss.reason == AtlasMissReason::OutOfInstanceBounds);
}

// -------------------------------------------------------------------------------------
// 4. Atlas full -> stable degradation to miss
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_AtlasFullDegradesToMiss)
{
    // Tiny atlas: 2^3 = 8 slots per texture.
    LumenMeshSDFAtlas atlas(2, 0, 0, 1);

    // Each 16^3 mesh needs 1 fine + 4 coarse pages. Fine: 8 slots, coarse: 8 slots
    // -> the coarse atlas fits exactly 2 meshes; the 3rd cannot be placed (all
    // groups are inside their min-residency window, so no eviction is possible).
    const uint32_t m0 = atlas.registerMesh(makeSphereMesh(16, 1));
    const uint32_t m1 = atlas.registerMesh(makeSphereMesh(16, 2));
    const uint32_t m2 = atlas.registerMesh(makeSphereMesh(16, 3));
    EXPECT_NE(m0, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(m1, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(m2, kLumenMeshSDFAtlasInvalidID);

    LumenMeshSDFAtlasInstanceDesc identity;
    const uint32_t i0 = atlas.registerInstance(m0, identity);
    const uint32_t i1 = atlas.registerInstance(m1, identity);
    const uint32_t i2 = atlas.registerInstance(m2, identity);
    EXPECT_NE(i0, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(i1, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(i2, kLumenMeshSDFAtlasInvalidID);

    EXPECT_TRUE(atlas.isResident(i0));
    EXPECT_TRUE(atlas.isResident(i1));
    EXPECT_FALSE(atlas.isResident(i2)); // stable degradation: registered but not resident

    const LumenMeshSDFAtlasStats stats = atlas.getStats();
    EXPECT_EQ(stats.residentInstanceCount, 2u);
    EXPECT_EQ(stats.nonResidentInstanceCount, 1u);
    EXPECT_EQ(stats.allocationFailureCount, 1u);
    EXPECT_EQ(stats.residentPages, 10u); // 2 meshes * 5 pages

    // Sampling the non-resident instance misses with InstanceNotResident (no crash).
    uploadSphere(ctx, atlas, m0);
    uploadSphere(ctx, atlas, m1);
    const float p[3] = {0.5f, 0.5f, 0.5f};
    const LumenMeshSDFAtlasSampleResult hit = atlas.sample(i0, p, 0);
    EXPECT_TRUE(hit.resident);
    const LumenMeshSDFAtlasSampleResult miss = atlas.sample(i2, p, 0);
    EXPECT_FALSE(miss.resident);
    EXPECT_TRUE(miss.reason == AtlasMissReason::InstanceNotResident);

    const LumenMeshSDFAtlasStats after = atlas.getStats();
    EXPECT_EQ(after.hitCount, 1u);
    EXPECT_EQ(after.missCount, 1u);
    EXPECT_EQ(after.missByReason[static_cast<size_t>(AtlasMissReason::InstanceNotResident)], 1u);
}

// -------------------------------------------------------------------------------------
// 5. Budget enforcement (deterministic LRU eviction sequence)
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_BudgetEnforcedDeterministic)
{
    // Budget: 2 fine + 6 coarse pages of bytes (327680 B). Atlas is large
    // (4^3 = 64 slots per texture) so only the byte budget drives evictions.
    // Each 16^3 mip is one page (1 fine + 4 coarse per mesh).
    constexpr uint64_t kBudgetBytes = 2 * kLumenMeshSDFAtlasBytesPerFinePage + 6 * kLumenMeshSDFAtlasBytesPerCoarsePage;
    LumenMeshSDFAtlas atlas(4, kBudgetBytes, 0, 1);
    EXPECT_EQ(atlas.getMemoryBudgetBytes(), kBudgetBytes);

    LumenMeshSDFAtlasInstanceDesc identity;
    const uint32_t mA = atlas.registerMesh(makeSphereMesh(16, 10));
    const uint32_t mB = atlas.registerMesh(makeSphereMesh(16, 11));
    const uint32_t mC = atlas.registerMesh(makeSphereMesh(16, 12));
    const uint32_t mD = atlas.registerMesh(makeSphereMesh(16, 13));

    // A at frame 0: 5 pages, 196608 B <= budget.
    const uint32_t iA = atlas.registerInstance(mA, identity);
    EXPECT_TRUE(atlas.isResident(iA));
    EXPECT_TRUE(atlas.isWithinBudget());

    atlas.endFrame(); // frame 1: A's groups are evictable (min residency 1).

    // B at frame 1: placements push bytes over budget; the budget pass evicts
    // A's oldest groups (mip0 fine) until within. The final placement lands
    // exactly at the budget (327680 B), so no further eviction is needed.
    const uint32_t iB = atlas.registerInstance(mB, identity);
    EXPECT_TRUE(atlas.isResident(iB));
    EXPECT_FALSE(atlas.isResident(iA, 0)); // A's mip0 evicted
    EXPECT_TRUE(atlas.isResident(iA, 1));  // A's mip1 still fits
    EXPECT_TRUE(atlas.isResident(iA, 2));
    EXPECT_TRUE(atlas.isWithinBudget());

    // C at frame 1: evicts A's last groups. Then B/C are inside their residency
    // window, so the budget pass gives up (overBudgetCount, never a failure) and
    // the atlas stays temporarily over budget while C becomes fully resident.
    const uint32_t iC = atlas.registerInstance(mC, identity);
    EXPECT_TRUE(atlas.isResident(iC));
    EXPECT_FALSE(atlas.isResident(iA)); // A fully evicted
    EXPECT_TRUE(atlas.isResident(iB));
    EXPECT_EQ(atlas.getStats().overBudgetCount, 2u); // C's last two placements
    EXPECT_EQ(atlas.getStats().allocationFailureCount, 0u);
    EXPECT_FALSE(atlas.isWithinBudget()); // documented temporary overage (10 pages > budget)

    // D at frame 2 (endFrame once): B is the oldest evictable cohort; D's
    // placements evict B's pages and then C's oldest mip, restoring the budget.
    atlas.endFrame();
    const uint32_t iD = atlas.registerInstance(mD, identity);
    EXPECT_TRUE(atlas.isResident(iD));
    EXPECT_FALSE(atlas.isResident(iB));    // B fully evicted
    EXPECT_FALSE(atlas.isResident(iC, 0)); // C's mip0 evicted for D's fine page
    EXPECT_TRUE(atlas.isResident(iC, 1));  // C's coarse mips still fit
    EXPECT_TRUE(atlas.isWithinBudget());

    const LumenMeshSDFAtlasStats final = atlas.getStats();
    EXPECT_EQ(final.groupEvictionCount, 11u); // 1 (B reg) + 4 (C reg) + 6 (D reg)
    EXPECT_EQ(final.residentInstanceCount, 1u);
    EXPECT_EQ(final.residentPages, 9u); // D only: 1 fine + 8 coarse
    EXPECT_EQ(final.residentBytes, kLumenMeshSDFAtlasBytesPerFinePage + 8 * kLumenMeshSDFAtlasBytesPerCoarsePage);
    EXPECT_EQ(final.overBudgetCount, 2u);
}

// -------------------------------------------------------------------------------------
// 6. Eviction + reload via touchInstance()
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_EvictionAndReload)
{
    // Byte budget of 2 fine + 9 coarse pages. Two 32^3 meshes (1 fine + 5 coarse
    // pages each) fit; B's last placement evicts A's mip0 by LRU (the budget
    // then lands exactly at its limit).
    constexpr uint64_t kBudgetBytes = 2 * kLumenMeshSDFAtlasBytesPerFinePage + 9 * kLumenMeshSDFAtlasBytesPerCoarsePage;
    LumenMeshSDFAtlas atlas(4, kBudgetBytes, 0, 1);
    LumenMeshSDFAtlasInstanceDesc identity;
    const uint32_t mA = atlas.registerMesh(makeSphereMesh(32, 20));
    const uint32_t mB = atlas.registerMesh(makeSphereMesh(32, 21));
    const uint32_t iA = atlas.registerInstance(mA, identity);
    EXPECT_TRUE(atlas.isResident(iA));
    atlas.endFrame();
    const uint32_t iB = atlas.registerInstance(mB, identity);
    EXPECT_TRUE(atlas.isResident(iB));
    EXPECT_FALSE(atlas.isResident(iA, 0)); // A's mip0 evicted by the budget pass
    EXPECT_TRUE(atlas.isResident(iA, 1));  // A's remaining mips still fit
    EXPECT_EQ(atlas.getStats().groupEvictionCount, 1u);

    // Sampling the evicted mip misses before the reload.
    uploadSphere(ctx, atlas, mA);
    const float p[3] = {0.75f, 0.5f, 0.5f};
    const LumenMeshSDFAtlasSampleResult miss = atlas.sample(iA, p, 0);
    EXPECT_FALSE(miss.resident);
    EXPECT_TRUE(miss.reason == AtlasMissReason::InstanceNotResident);
    EXPECT_EQ(atlas.getMipGeneration(iA, 0), 0u);

    // touchInstance() at frame 2 re-residents A's mip0; the budget pass evicts
    // A's own stale coarse mips one by one and finally B's oldest group (its
    // fine mip0: all of B's groups share the same touch frame and the first
    // inserted group wins the tie), so the instance is fully resident again
    // with bumped generations.
    atlas.endFrame();
    EXPECT_TRUE(atlas.touchInstance(iA));
    EXPECT_TRUE(atlas.isResident(iA));
    EXPECT_TRUE(atlas.isResident(iA, 0));
    EXPECT_GE(atlas.getMipGeneration(iA, 0), 1u); // stale-page detection
    EXPECT_EQ(atlas.getStats().groupEvictionCount, 7u); // 1 (B reg) + 6 (reload)
    EXPECT_FALSE(atlas.isResident(iB, 0)); // B's mip0 evicted during the reload

    // After the host re-uploads, the CPU mirror samples the reloaded pages.
    uploadSphere(ctx, atlas, mA);
    const LumenMeshSDFAtlasSampleResult hit = atlas.sample(iA, p, 0);
    EXPECT_TRUE(hit.resident);
    LUMEN_EXPECT_NEAR(hit.distanceOutput, 0.f, 2.f / 32.f);
}

// -------------------------------------------------------------------------------------
// 7. Sampling mirror: sphere field + miss reasons
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_SampleMirrorMatchesAnalytic)
{
    LumenMeshSDFAtlas atlas(4, 0, 0, 1);
    const uint32_t mesh = atlas.registerMesh(makeSphereMesh(32, 0x1234));
    EXPECT_NE(mesh, kLumenMeshSDFAtlasInvalidID);
    LumenMeshSDFAtlasInstanceDesc identity;
    const uint32_t instanceID = atlas.registerInstance(mesh, identity);
    EXPECT_TRUE(atlas.isResident(instanceID));
    uploadSphere(ctx, atlas, mesh);

    // On-surface and inside points at mip 0 (32^3 -> fine voxel = 1/32).
    const float onSurface[3] = {0.75f, 0.5f, 0.5f};
    const LumenMeshSDFAtlasSampleResult r0 = atlas.sample(instanceID, onSurface, 0);
    EXPECT_TRUE(r0.resident);
    LUMEN_EXPECT_NEAR(r0.distanceOutput, 0.f, 2.f / 32.f);

    const float inside[3] = {0.5f, 0.5f, 0.5f};
    const LumenMeshSDFAtlasSampleResult r1 = atlas.sample(instanceID, inside, 0);
    EXPECT_TRUE(r1.resident);
    LUMEN_EXPECT_NEAR(r1.distanceOutput, -kSphereRadius, 2.f / 32.f);

    const float outside[3] = {0.9f, 0.5f, 0.5f};
    const LumenMeshSDFAtlasSampleResult r2 = atlas.sample(instanceID, outside, 0);
    EXPECT_TRUE(r2.resident);
    LUMEN_EXPECT_NEAR(r2.distanceOutput, 0.15f, 3.f / 32.f);

    // Coarse mip (mip 3 = 4^3): stored codes rescaled by quantRange.
    const LumenMeshSDFAtlasSampleResult r3 = atlas.sample(instanceID, onSurface, 3);
    EXPECT_TRUE(r3.resident);
    LUMEN_EXPECT_NEAR(r3.distanceOutput, 0.f, 3.f / 4.f);

    // Miss reasons.
    const float far[3] = {5.f, 5.f, 5.f};
    const LumenMeshSDFAtlasSampleResult m1 = atlas.sample(instanceID, far, 0);
    EXPECT_FALSE(m1.resident);
    EXPECT_TRUE(m1.reason == AtlasMissReason::OutOfInstanceBounds);

    const LumenMeshSDFAtlasSampleResult m2 = atlas.sample(instanceID, onSurface, 99);
    EXPECT_FALSE(m2.resident);
    EXPECT_TRUE(m2.reason == AtlasMissReason::InvalidMip);

    const LumenMeshSDFAtlasSampleResult m3 = atlas.sample(9999, onSurface, 0);
    EXPECT_FALSE(m3.resident);
    EXPECT_TRUE(m3.reason == AtlasMissReason::NoInstance);

    // Out-of-volume but inside-instance positions clamp (S6-B1 behavior, not a miss).
    const float edge[3] = {1.0f, 0.5f, 0.5f};
    const LumenMeshSDFAtlasSampleResult r4 = atlas.sample(instanceID, edge, 0);
    EXPECT_TRUE(r4.resident);

    const LumenMeshSDFAtlasStats stats = atlas.getStats();
    EXPECT_EQ(stats.hitCount, 5u);
    EXPECT_EQ(stats.missCount, 3u);
    EXPECT_EQ(stats.missByReason[static_cast<size_t>(AtlasMissReason::OutOfInstanceBounds)], 1u);
    EXPECT_EQ(stats.missByReason[static_cast<size_t>(AtlasMissReason::InvalidMip)], 1u);
    EXPECT_EQ(stats.missByReason[static_cast<size_t>(AtlasMissReason::NoInstance)], 1u);
}

// -------------------------------------------------------------------------------------
// 8. Multi-brick mip tiling (64^3 mip0 -> 2^3 pages)
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_MultiBrickTiling)
{
    LumenMeshSDFAtlas atlas(4, 0, 0, 1);
    const uint32_t mesh = atlas.registerMesh(makeSphereMesh(64, 0x7777));
    EXPECT_NE(mesh, kLumenMeshSDFAtlasInvalidID);
    LumenMeshSDFAtlasInstanceDesc identity;
    const uint32_t instanceID = atlas.registerInstance(mesh, identity);
    EXPECT_TRUE(atlas.isResident(instanceID));

    // mip0 = 64^3 -> 2^3 = 8 fine pages (slots 0..7); mips 1..6 are 1 page each.
    EXPECT_EQ(atlas.getStats().residentPagesFine, 8u);
    EXPECT_EQ(atlas.getStats().residentPagesCoarse, 6u);
    for (uint32_t mip = 0; mip < 7; ++mip)
    {
        EXPECT_TRUE(atlas.isResident(instanceID, mip));
    }

    // Brick mapping: voxel (63,63,63) sits in brick (1,1,1) = base slot + 7.
    uploadSphere(ctx, atlas, mesh);
    const float corner[3] = {63.5f / 64.f, 63.5f / 64.f, 63.5f / 64.f};
    const LumenMeshSDFAtlasSampleResult r = atlas.sample(instanceID, corner, 0);
    EXPECT_TRUE(r.resident);
    LUMEN_EXPECT_NEAR(r.distanceOutput, sphereDistance(corner), 3.f / 64.f);

    // A point crossing brick boundaries (0.52, 0.52, 0.52) -> bricks (1,1,1).
    const float cross[3] = {0.52f, 0.52f, 0.52f};
    const LumenMeshSDFAtlasSampleResult r2 = atlas.sample(instanceID, cross, 0);
    EXPECT_TRUE(r2.resident);
    LUMEN_EXPECT_NEAR(r2.distanceOutput, sphereDistance(cross), 3.f / 64.f);
}

// -------------------------------------------------------------------------------------
// 9. Determinism
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_DeterministicReproducible)
{
    constexpr uint64_t kBudgetBytes = 2 * kLumenMeshSDFAtlasBytesPerFinePage + 6 * kLumenMeshSDFAtlasBytesPerCoarsePage;
    const auto run = [&](LumenMeshSDFAtlasStats& outStats, std::vector<uint32_t>& outPageTable)
    {
        LumenMeshSDFAtlas atlas(4, kBudgetBytes, 0, 1);
        LumenMeshSDFAtlasInstanceDesc identity;
        for (uint64_t m = 0; m < 5; ++m)
        {
            const uint32_t mesh = atlas.registerMesh(makeSphereMesh(16, m));
            const uint32_t inst = atlas.registerInstance(mesh, identity);
            EXPECT_TRUE(atlas.isResident(inst));
            atlas.endFrame();
        }
        outStats = atlas.getStats();
        atlas.refreshUploadBuffers();
        outPageTable = atlas.getPageTableBuffer();
    };

    LumenMeshSDFAtlasStats s1, s2;
    std::vector<uint32_t> t1, t2;
    run(s1, t1);
    run(s2, t2);

    EXPECT_EQ(s1.groupEvictionCount, s2.groupEvictionCount);
    EXPECT_EQ(s1.pagesEvictedCount, s2.pagesEvictedCount);
    EXPECT_EQ(s1.overBudgetCount, s2.overBudgetCount);
    EXPECT_EQ(s1.allocationFailureCount, s2.allocationFailureCount);
    EXPECT_EQ(s1.residentPages, s2.residentPages);
    EXPECT_EQ(s1.residentBytes, s2.residentBytes);
    EXPECT_TRUE(t1 == t2); // bit-identical page tables
}

// -------------------------------------------------------------------------------------
// 10. Resource stats + S6-B1 descriptor rebind hook
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_ResourceStatsAndBudgetReport)
{
    constexpr uint64_t kBudget = 512ull * 1024 * 1024;
    LumenMeshSDFAtlas atlas(2, 0, 0, 1);
    const uint32_t mesh = atlas.registerMesh(makeSphereMesh(16, 0x99));
    EXPECT_NE(mesh, kLumenMeshSDFAtlasInvalidID);
    LumenMeshSDFAtlasInstanceDesc identity;
    const uint32_t instanceID = atlas.registerInstance(mesh, identity);
    EXPECT_TRUE(atlas.isResident(instanceID));

    const uint64_t residentBytes = atlas.getResidentBytes();
    EXPECT_EQ(atlas.toResourceStats().meshSdfBytes, residentBytes);
    EXPECT_EQ(residentBytes, 4 * kLumenMeshSDFAtlasBytesPerCoarsePage + kLumenMeshSDFAtlasBytesPerFinePage);

    // LumenGIStats budget integration (meshSdfBytes participates in the totals).
    LumenGIStats stats;
    stats.memoryBudgetBytes = kBudget;
    stats.resources = atlas.toResourceStats();
    EXPECT_TRUE(stats.isWithinMemoryBudget());
    EXPECT_EQ(stats.getTotalMemoryBytes(), residentBytes);
    EXPECT_EQ(stats.getBudgetRemainingBytes(), kBudget - residentBytes);

    // Volume descriptor rebind hook (S6-B1 -> S6-B2): atlasPage + flag.
    LumenMeshSDFVolumeDescriptor vol;
    EXPECT_TRUE(atlas.getVolumeDescriptor(mesh, vol));
    EXPECT_EQ(vol.atlasPage, kLumenMeshSDFNotResident);
    EXPECT_EQ(vol.flags & kLumenMeshSDFFlagAtlasResident, 0u);
    atlas.applyResidencyToDescriptor(instanceID, vol);
    EXPECT_EQ(vol.atlasPage, atlas.pageSlot(instanceID, 0));
    EXPECT_NE(vol.flags & kLumenMeshSDFFlagAtlasResident, 0u);
    EXPECT_EQ(vol.resolution[0], 16u);
    EXPECT_EQ(vol.mipCount, 5u);
    EXPECT_EQ(vol.contentHashLo, 0x99u);
    EXPECT_NE(vol.flags & kLumenMeshSDFFlagQualityHigh, 0u);
    EXPECT_EQ(vol.flags & kLumenMeshSDFFlagSignReliable, kLumenMeshSDFFlagSignReliable);
}

// -------------------------------------------------------------------------------------
// 11. Low-quality volumes put mip0 into the coarse atlas
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFAtlas_LowQualityMip0UsesCoarseAtlas)
{
    LumenMeshSDFAtlas atlas(2, 0, 0, 1);
    const uint32_t mesh = atlas.registerMesh(makeSphereMesh(16, 0xABC, VolumeFormat::R8Snorm));
    EXPECT_NE(mesh, kLumenMeshSDFAtlasInvalidID);
    LumenMeshSDFAtlasInstanceDesc identity;
    const uint32_t instanceID = atlas.registerInstance(mesh, identity);
    EXPECT_TRUE(atlas.isResident(instanceID));

    // All 5 pages (incl. mip0) live in the coarse atlas now.
    EXPECT_EQ(atlas.getStats().residentPagesFine, 0u);
    EXPECT_EQ(atlas.getStats().residentPagesCoarse, 5u);

    // Sampling decodes through quantRange (stored = d / quantRange).
    uploadSphere(ctx, atlas, mesh);
    const float onSurface[3] = {0.75f, 0.5f, 0.5f};
    const LumenMeshSDFAtlasSampleResult r = atlas.sample(instanceID, onSurface, 0);
    EXPECT_TRUE(r.resident);
    LUMEN_EXPECT_NEAR(r.distanceOutput, 0.f, 2.f / 16.f);
}

#undef LUMEN_EXPECT_NEAR

} // namespace
} // namespace Falcor
