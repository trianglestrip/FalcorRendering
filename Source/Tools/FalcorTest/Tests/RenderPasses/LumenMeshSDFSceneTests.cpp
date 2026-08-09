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
//  LumenMeshSDFSceneTests.cpp - S6-A2 CPU tests for the Mesh SDF scene pipeline
//  orchestration component (LumenMeshSDFScene.h).
//
//  Uses the FalcorTest CPU_TEST structure (same as LumenSurfaceCacheTests.cpp /
//  LumenMeshSDFCacheTests.cpp).
//  NOT registered in CMake yet: the root pass integrates this file into
//  Source/Tools/FalcorTest/CMakeLists.txt (FalcorTest target).
//
//  COVERAGE (maps to the S6 scene-pipeline card)
//  -------------------------------------------------------------------------------------
//  1. Data-flow state machine: Unknown -> NeedsBuild -> Cached -> Uploaded; loaded
//     volumes are not re-probed; the cache file exists after the flow.
//  2. Cache hit / miss / corrupt-rebuild: a pre-populated entry is a hit (no build);
//     a corrupted entry is detected as NeedsBuild (never served) and rebuilt into a
//     valid cache entry.
//  3. Instance add / remove / transform: content dedup, transform change -> dirty list,
//     remove drops residency and instance count.
//  4. Layer masks + GDF interop: instancesForLayer() filtering and buildGDFInstanceList()
//     including the documented mesh/GDF bit-coincidence (mesh-Static bit0 -> GDF dynamic
//     level 0, mesh-Dynamic bit1 -> GDF static levels).
//  5. Budget degradation: enforceBudget() evicts the FARTHEST instance's pages
//     (deterministic), the evicted instance leaves the layer list, and restoreInstance()
//     re-residents it.
//  6. Scene reload: reload() clears the atlas/table while keeping registration;
//     applyScene() re-materializes from the disk cache as a HIT (no rebuild).
//  7. world -> atlas sample round-trip through the placeholder volume (upload -> sample).
// =====================================================================================
#include "Testing/UnitTest.h"
#include "../../../../RenderPasses/LumenGI/MeshSDF/LumenMeshSDFScene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Falcor
{
namespace
{

// -------------------------------------------------------------------------------------
// Test-scoped aliases
// -------------------------------------------------------------------------------------

using LumenGI::MeshSDF::Scene::LumenMeshSDFScene;
using LumenGI::MeshSDF::Scene::LumenMeshSDFSceneMeshDesc;
using LumenGI::MeshSDF::Scene::LumenMeshSDFGDFInstance;
using LumenGI::MeshSDF::Scene::VolumeState;

using LumenGI::MeshSDF::Cache::LumenMeshSDFCacheParams;
using LumenGI::MeshSDF::Cache::LumenMeshSDFDiskCache;
using LumenGI::MeshSDF::Cache::cacheKey;

using LumenGI::MeshSDF::LumenMeshSDFAtlasInstanceDesc;
using LumenGI::MeshSDF::LumenMeshSDFAtlasSampleResult;
using LumenGI::MeshSDF::AtlasMissReason;
using LumenGI::MeshSDF::Quality;
using LumenGI::MeshSDF::MipPooling;
using LumenGI::MeshSDF::MSDFHeader;
using LumenGI::MeshSDF::MSDFParseResult;
using LumenGI::MeshSDF::kMSDFFormatVersion;
using LumenGI::MeshSDF::kLumenMeshSDFAtlasInvalidID;
using LumenGI::MeshSDF::kLumenMeshSDFLayerMaskStatic;
using LumenGI::MeshSDF::kLumenMeshSDFLayerMaskDynamic;
using LumenGI::MeshSDF::kLumenMeshSDFSignConventionPositiveOutside;

// -------------------------------------------------------------------------------------
// Helpers (mirror LumenMeshSDFCacheTests.cpp)
// -------------------------------------------------------------------------------------

/// RAII temp directory (unique per test instance, auto-removed).
struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        static uint64_t counter = 0;
        const uint64_t stamp = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() /
               ("lumen_scene_" + std::to_string(++counter) + "_" + std::to_string(stamp));
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

/// 16^3 ".msdf" header (frozen grid; resolution >= 2 per the container contract).
MSDFHeader makeHeader()
{
    MSDFHeader h;
    h.formatVersion = kMSDFFormatVersion;
    h.resolution = {16, 16, 16};
    h.bboxMin = {0.f, 0.f, 0.f};
    h.bboxMax = {1.f, 1.f, 1.f};
    h.voxelSize = 1.f / 15.f;
    h.normalizationScale = 1.f;
    h.paddingWorld = 0.1f;
    h.signConvention = kLumenMeshSDFSignConventionPositiveOutside;
    h.signReliable = 1;
    h.dataCount = 16ull * 16 * 16;
    return h;
}

std::vector<float> makeDistances()
{
    std::vector<float> d(16ull * 16 * 16);
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = float(i) * 0.001f - 0.5f;
    return d;
}

/// Default cache key params (resolution + compression knobs + grid bounds).
LumenMeshSDFCacheParams makeCacheParams()
{
    LumenMeshSDFCacheParams p;
    p.resolution = {16, 16, 16};
    p.quality = Quality::High;
    p.pooling = MipPooling::MinAbs;
    p.gridBounds = {0.f, 0.f, 0.f, 1.f, 1.f, 1.f};
    return p;
}

LumenMeshSDFSceneMeshDesc makeMeshDesc(uint64_t hash)
{
    LumenMeshSDFSceneMeshDesc d;
    d.meshContentHash = hash;
    d.cacheParams = makeCacheParams();
    return d;
}

LumenMeshSDFAtlasInstanceDesc makeTransform(const float tx, const float ty, const float tz)
{
    LumenMeshSDFAtlasInstanceDesc t;
    t.forwardLinear = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    t.forwardTranslation = {tx, ty, tz};
    return t;
}

constexpr uint64_t kHashA = 0x1111222233334444ull;
constexpr uint64_t kHashB = 0xAAAA222233334444ull;

// -------------------------------------------------------------------------------------
// 1) Data-flow state machine
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFScene_StateMachine)
{
    TempDir tmp;
    LumenMeshSDFScene scene(tmp.path);
    const LumenMeshSDFSceneMeshDesc mesh = makeMeshDesc(kHashA);

    // Empty cache -> NeedsBuild.
    EXPECT_EQ(static_cast<uint32_t>(scene.probeMesh(mesh)), static_cast<uint32_t>(VolumeState::NeedsBuild));
    EXPECT_EQ(static_cast<uint32_t>(scene.meshState(mesh)), static_cast<uint32_t>(VolumeState::NeedsBuild));

    std::string err;
    // Builder -> Cached.
    EXPECT_TRUE(scene.buildMesh(mesh, err));
    EXPECT_EQ(static_cast<uint32_t>(scene.meshState(mesh)), static_cast<uint32_t>(VolumeState::Cached));

    // Convert + atlas register + upload -> Uploaded.
    uint32_t atlasMeshID = kLumenMeshSDFAtlasInvalidID;
    EXPECT_TRUE(scene.ensureMesh(mesh, atlasMeshID, err));
    EXPECT_NE(atlasMeshID, kLumenMeshSDFAtlasInvalidID);
    EXPECT_EQ(static_cast<uint32_t>(scene.meshState(mesh)), static_cast<uint32_t>(VolumeState::Uploaded));

    // Loaded volumes are not re-probed (in-memory data is authoritative).
    EXPECT_EQ(static_cast<uint32_t>(scene.probeMesh(mesh)), static_cast<uint32_t>(VolumeState::Uploaded));

    // The cache file now exists on disk.
    LumenMeshSDFDiskCache cache(tmp.path);
    EXPECT_TRUE(cache.exists(LumenMeshSDFScene::meshKey(mesh)));
}

// -------------------------------------------------------------------------------------
// 2) Cache hit / miss / corrupt-rebuild
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFScene_CacheHitMissCorruptRebuild)
{
    TempDir tmp;
    LumenMeshSDFDiskCache cache(tmp.path);
    const std::string key = cacheKey(kHashA, makeCacheParams());
    std::string err;

    // Pre-populate a KNOWN valid volume under the mesh's cache key.
    EXPECT_TRUE(cache.storeVolume(key, makeHeader(), makeDistances(), {}, err));

    // Hit path: no build; the volume matches the pre-populated header.
    {
        LumenMeshSDFScene scene(tmp.path);
        const LumenMeshSDFSceneMeshDesc mesh = makeMeshDesc(kHashA);
        EXPECT_EQ(static_cast<uint32_t>(scene.probeMesh(mesh)), static_cast<uint32_t>(VolumeState::Cached));
        uint32_t atlasMeshID = kLumenMeshSDFAtlasInvalidID;
        EXPECT_TRUE(scene.ensureMesh(mesh, atlasMeshID, err));
        EXPECT_EQ(scene.getStats().builds, 0u);
        MSDFHeader h;
        EXPECT_TRUE(scene.getMeshHeader(mesh, h));
        EXPECT_EQ(h.resolution[0], 16u);
    }

    // Corrupt the cache entry on disk (flip a data byte -> trailing checksum mismatch).
    {
        const std::filesystem::path p = cache.pathFor(key);
        std::vector<uint8_t> bytes;
        {
            std::ifstream in(p, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        bytes[120] ^= 0x01u; // data region -> checksum mismatch.
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
    }

    // A FRESH scene detects the corruption as NeedsBuild (never served) and rebuilds.
    {
        LumenMeshSDFScene scene(tmp.path);
        const LumenMeshSDFSceneMeshDesc mesh = makeMeshDesc(kHashA);
        EXPECT_EQ(static_cast<uint32_t>(scene.probeMesh(mesh)), static_cast<uint32_t>(VolumeState::NeedsBuild));
        EXPECT_EQ(scene.getStats().corruptionsDetected, 1u);
        uint32_t atlasMeshID = kLumenMeshSDFAtlasInvalidID;
        EXPECT_TRUE(scene.ensureMesh(mesh, atlasMeshID, err));
        EXPECT_EQ(static_cast<uint32_t>(scene.meshState(mesh)), static_cast<uint32_t>(VolumeState::Uploaded));
        EXPECT_EQ(scene.getStats().builds, 1u);

        // The cache file is valid again.
        MSDFParseResult parsed;
        EXPECT_TRUE(cache.findCached(key, parsed, err));
    }
}

// -------------------------------------------------------------------------------------
// 3) Instance add / remove / transform
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFScene_InstanceAddRemoveTransform)
{
    TempDir tmp;
    LumenMeshSDFScene scene(tmp.path);
    const LumenMeshSDFSceneMeshDesc mesh = makeMeshDesc(kHashA);
    std::string err;

    const uint32_t id0 = scene.addInstance(mesh, makeTransform(0.f, 0.f, 0.f), kLumenMeshSDFLayerMaskStatic, err);
    const uint32_t id1 = scene.addInstance(mesh, makeTransform(3.f, 0.f, 0.f), kLumenMeshSDFLayerMaskDynamic, err);
    EXPECT_NE(id0, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(id1, kLumenMeshSDFAtlasInvalidID);
    EXPECT_EQ(scene.meshCount(), 1u); // content dedup.
    EXPECT_EQ(scene.instanceCount(), 2u);
    EXPECT_TRUE(scene.isResident(id0));
    EXPECT_TRUE(scene.isResident(id1));

    // Transform change -> dirty scene instance list.
    scene.instanceTable().clearDirtyAll();
    EXPECT_TRUE(scene.dirtySceneInstanceIDs().empty());
    EXPECT_TRUE(scene.setInstanceTransform(id0, makeTransform(7.f, 0.f, 0.f)));
    const std::vector<uint32_t> dirty = scene.dirtySceneInstanceIDs();
    EXPECT_EQ(dirty.size(), 1u);
    EXPECT_TRUE(std::find(dirty.begin(), dirty.end(), id0) != dirty.end());
    // Shared mesh pages: no churn while id1 still references the mesh.
    EXPECT_TRUE(scene.isResident(id0));

    // Layer-mask change also marks dirty.
    scene.instanceTable().clearDirtyAll();
    EXPECT_TRUE(scene.setInstanceLayerMask(id1, kLumenMeshSDFLayerMaskStatic));
    EXPECT_EQ(scene.dirtySceneInstanceIDs().size(), 1u);

    // Remove.
    EXPECT_TRUE(scene.removeInstance(id0));
    EXPECT_EQ(scene.instanceCount(), 1u);
    EXPECT_FALSE(scene.isResident(id0));
    EXPECT_TRUE(scene.removeInstance(id1));
    EXPECT_EQ(scene.instanceCount(), 0u);
    // Invalid / already-removed handles are no-ops.
    EXPECT_FALSE(scene.removeInstance(id0));
    EXPECT_FALSE(scene.removeInstance(12345u));
}

// -------------------------------------------------------------------------------------
// 4) Layer masks + GDF interop
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFScene_LayerMaskAndGDFList)
{
    TempDir tmp;
    LumenMeshSDFScene scene(tmp.path);
    const LumenMeshSDFSceneMeshDesc mesh = makeMeshDesc(kHashA);
    std::string err;
    const uint32_t idS = scene.addInstance(mesh, makeTransform(0.f, 0.f, 0.f), kLumenMeshSDFLayerMaskStatic, err);
    const uint32_t idD = scene.addInstance(mesh, makeTransform(3.f, 0.f, 0.f), kLumenMeshSDFLayerMaskDynamic, err);
    EXPECT_NE(idS, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(idD, kLumenMeshSDFAtlasInvalidID);

    const std::vector<uint32_t> statics = scene.instancesForLayer(kLumenMeshSDFLayerMaskStatic);
    EXPECT_EQ(statics.size(), 1u);
    EXPECT_EQ(statics[0], idS);
    const std::vector<uint32_t> dynamics = scene.instancesForLayer(kLumenMeshSDFLayerMaskDynamic);
    EXPECT_EQ(dynamics.size(), 1u);
    EXPECT_EQ(dynamics[0], idD);

    // GDF bit-coincidence (documented in LumenMeshSDFScene.h): mesh-Static (bit0) feeds
    // the GDF dynamic level 0 (bit0); mesh-Dynamic (bit1) feeds the GDF static levels.
    constexpr uint32_t kGDFLayerDynamic = 1u << 0;
    constexpr uint32_t kGDFLayerStatic = 1u << 1;
    const std::vector<LumenMeshSDFGDFInstance> gdfDyn = scene.buildGDFInstanceList(kGDFLayerDynamic);
    EXPECT_EQ(gdfDyn.size(), 1u);
    EXPECT_EQ(gdfDyn[0].resident, 1u);
    EXPECT_EQ(gdfDyn[0].layerMask, kLumenMeshSDFLayerMaskStatic);
    EXPECT_NE(gdfDyn[0].atlasInstanceID, kLumenMeshSDFAtlasInvalidID);
    const std::vector<LumenMeshSDFGDFInstance> gdfStatic = scene.buildGDFInstanceList(kGDFLayerStatic);
    EXPECT_EQ(gdfStatic.size(), 1u);
    EXPECT_EQ(gdfStatic[0].layerMask, kLumenMeshSDFLayerMaskDynamic);

    // Re-classify idS as dynamic -> no mesh-Static (bit0) contributors remain: the GDF
    // dynamic list empties while the GDF static list grows to 2.
    EXPECT_TRUE(scene.setInstanceLayerMask(idS, kLumenMeshSDFLayerMaskDynamic));
    EXPECT_EQ(scene.buildGDFInstanceList(kGDFLayerDynamic).size(), 0u);
    EXPECT_EQ(scene.buildGDFInstanceList(kGDFLayerStatic).size(), 2u);
}

// -------------------------------------------------------------------------------------
// 5) Budget degradation (evict farthest) + restore
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFScene_BudgetEvictionAndRestore)
{
    TempDir tmp;
    LumenMeshSDFScene scene(tmp.path);
    const float cam[3] = {0.f, 0.f, 0.f};
    scene.setCamera(cam);
    std::string err;

    // Two DIFFERENT meshes so evicting the farthest instance frees its pages.
    const LumenMeshSDFSceneMeshDesc meshA = makeMeshDesc(kHashA);
    const LumenMeshSDFSceneMeshDesc meshB = makeMeshDesc(kHashB);
    const uint32_t a = scene.addInstance(meshA, makeTransform(0.f, 0.f, 0.f), kLumenMeshSDFLayerMaskStatic, err);
    const uint32_t b = scene.addInstance(meshB, makeTransform(100.f, 0.f, 0.f), kLumenMeshSDFLayerMaskStatic, err);
    EXPECT_NE(a, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(b, kLumenMeshSDFAtlasInvalidID);
    EXPECT_TRUE(scene.isResident(a));
    EXPECT_TRUE(scene.isResident(b));
    EXPECT_EQ(scene.instanceCount(), 2u);

    // Enforce a budget just below the current estimate: the farthest instance (b) must
    // be evicted, its pages freed, and the estimate must drop.
    const uint64_t total = scene.estimateGpuBytes();
    EXPECT_TRUE(scene.enforceBudget(total - 1));
    EXPECT_FALSE(scene.isResident(b));
    EXPECT_TRUE(scene.isResident(a));
    EXPECT_EQ(scene.getStats().evictions, 1u);
    EXPECT_EQ(scene.getStats().evictedInstances, 1u);
    EXPECT_LE(scene.estimateGpuBytes(), total - 1);

    // The evicted instance leaves the layer list.
    EXPECT_EQ(scene.instancesForLayer(kLumenMeshSDFLayerMaskStatic).size(), 1u);

    // Restore re-residents + re-uploads (re-allocation bumps the page generation).
    EXPECT_TRUE(scene.restoreInstance(b, err));
    EXPECT_TRUE(scene.isResident(b));
    EXPECT_EQ(scene.getStats().restores, 1u);
    EXPECT_EQ(scene.instancesForLayer(kLumenMeshSDFLayerMaskStatic).size(), 2u);
}

// -------------------------------------------------------------------------------------
// 6) Scene reload -> clear table; applyScene re-materializes (cache hit)
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFScene_ReloadClearsTable)
{
    TempDir tmp;
    LumenMeshSDFScene scene(tmp.path);
    const LumenMeshSDFSceneMeshDesc mesh = makeMeshDesc(kHashA);
    std::string err;
    const uint32_t id = scene.addInstance(mesh, makeTransform(1.f, 2.f, 3.f), kLumenMeshSDFLayerMaskStatic, err);
    EXPECT_NE(id, kLumenMeshSDFAtlasInvalidID);
    EXPECT_EQ(static_cast<uint32_t>(scene.meshState(mesh)), static_cast<uint32_t>(VolumeState::Uploaded));
    const uint64_t builds0 = scene.getStats().builds;
    EXPECT_EQ(scene.instanceTable().instanceCount(), 1u);

    // reload() clears the atlas + instance table ("场景 reload -> 清表") but keeps the
    // scene registration (mesh desc + instance transform/layer).
    scene.reload();
    EXPECT_EQ(scene.instanceTable().instanceCount(), 0u);
    EXPECT_EQ(static_cast<uint32_t>(scene.meshState(mesh)), static_cast<uint32_t>(VolumeState::Unknown));
    EXPECT_EQ(scene.instanceCount(), 1u);

    // applyScene() re-materializes from the DISK cache: a cache HIT, never a rebuild.
    EXPECT_TRUE(scene.applyScene(err));
    EXPECT_EQ(scene.instanceTable().instanceCount(), 1u);
    EXPECT_EQ(static_cast<uint32_t>(scene.meshState(mesh)), static_cast<uint32_t>(VolumeState::Uploaded));
    EXPECT_EQ(scene.getStats().builds, builds0);
    EXPECT_TRUE(scene.isResident(id)); // the scene instance handle stays valid.
}

// -------------------------------------------------------------------------------------
// 7) world -> atlas sampling through the placeholder volume (upload -> sample)
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFScene_WorldToAtlasSampling)
{
    TempDir tmp;
    LumenMeshSDFScene scene(tmp.path);
    const LumenMeshSDFSceneMeshDesc mesh = makeMeshDesc(kHashA); // placeholder x-ramp field.
    std::string err;
    const uint32_t id = scene.addInstance(mesh, makeTransform(0.f, 0.f, 0.f), kLumenMeshSDFLayerMaskStatic, err);
    EXPECT_NE(id, kLumenMeshSDFAtlasInvalidID);

    MSDFHeader h;
    EXPECT_TRUE(scene.getMeshHeader(mesh, h));
    const float vs = h.voxelSize;
    // Voxel center (4,4,4) with an identity transform: pOut = bboxMin + 4.5 * voxelSize.
    const float world[3] = {h.bboxMin[0] + 4.5f * vs, h.bboxMin[1] + 4.5f * vs, h.bboxMin[2] + 4.5f * vs};

    // world -> continuous voxel coords round-trip to the integer index.
    const std::array<float, 3> u = scene.worldToAtlasVoxel(id, world);
    EXPECT_TRUE(std::fabs(u[0] - 4.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(u[1] - 4.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(u[2] - 4.f) <= 1e-4f);

    // Sampling at the voxel center returns the ramp value (mip 0 fine page = raw float).
    const LumenMeshSDFAtlasSampleResult s = scene.worldToAtlasSample(id, world, 0);
    EXPECT_TRUE(s.resident);
    EXPECT_TRUE(std::fabs(s.distanceOutput - 4.5f * vs) <= 1e-4f);

    // Outside the instance's world AABB -> OutOfInstanceBounds miss.
    const float outside[3] = {100.f, 100.f, 100.f};
    const LumenMeshSDFAtlasSampleResult miss = scene.worldToAtlasSample(id, outside, 0);
    EXPECT_FALSE(miss.resident);
    EXPECT_EQ(static_cast<uint32_t>(miss.reason), static_cast<uint32_t>(AtlasMissReason::OutOfInstanceBounds));

    // Invalid scene instance -> NoInstance miss.
    const LumenMeshSDFAtlasSampleResult noInst = scene.worldToAtlasSample(12345u, world, 0);
    EXPECT_FALSE(noInst.resident);
    EXPECT_EQ(static_cast<uint32_t>(noInst.reason), static_cast<uint32_t>(AtlasMissReason::NoInstance));
}

} // namespace
} // namespace Falcor
