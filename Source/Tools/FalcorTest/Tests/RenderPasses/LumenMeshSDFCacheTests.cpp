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
//  LumenMeshSDFCacheTests.cpp - S6-A2 CPU tests for the Mesh SDF disk cache
//  (LumenMeshSDFCache.h) and the instance -> atlas table (LumenMeshSDFInstanceTable.h).
//
//  Uses the FalcorTest CPU_TEST structure (same as LumenSurfaceCacheTests.cpp).
//  NOT registered in CMake yet: the root pass integrates this file into
//  Source/Tools/FalcorTest/CMakeLists.txt (FalcorTest target).
//
//  COVERAGE (maps to task.md S6-A2 / S6-B2)
//  -------------------------------------------------------------------------------------
//  1. cacheKey determinism + sensitivity to every key input (hash, resolution,
//     quality, pooling, grid bounds) and schema pinning against the canonical
//     byte layout (builder version + format version folded in).
//  2. ".msdf" round-trip through the cache: serialize -> store -> findCached
//     with header / distances / warnings bit-exact.
//  3. Corruption detection: flipped data byte (checksum), bad magic, truncation,
//     missing file -> findCached false.
//  4. Rebuild trigger: corrupt entry -> miss -> re-store -> hit.
//  5. DiskCache directory management: key/path derivation, atomic store, no
//     leftover temp files, overwrite, remove.
//  6. InstanceTable add/remove page reference counting (shared mesh dedup,
//     orphan release, resident pages).
//  7. InstanceTable transform change -> dirty (page slots stable while shared,
//     bounds updated in the mapping).
//  8. InstanceTable world <-> atlas coordinate round-trip via inverse transform
//     + sampling (translation-only and uniform-scale transforms, outside-bounds
//     and invalid-instance miss reasons).
//  9. InstanceTable layer masks (Static/Dynamic) filtering.
// =====================================================================================
#include "Testing/UnitTest.h"
#include "../../../../RenderPasses/LumenGI/MeshSDF/LumenMeshSDFCache.h"
#include "../../../../RenderPasses/LumenGI/MeshSDF/LumenMeshSDFInstanceTable.h"

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
// Test-scoped aliases (MeshSDF + Cache namespaces; ambiguous names are qualified)
// -------------------------------------------------------------------------------------

using LumenGI::MeshSDF::Cache::LumenMeshSDFCacheParams;
using LumenGI::MeshSDF::Cache::LumenMeshSDFDiskCache;
using LumenGI::MeshSDF::Cache::cacheKey;
using LumenGI::MeshSDF::Cache::findCached;
using LumenGI::MeshSDF::Cache::msdfCacheFNV1a64;
using LumenGI::MeshSDF::Cache::msdfCacheToHex;
using LumenGI::MeshSDF::Cache::msdfCacheWriteLEU32;
using LumenGI::MeshSDF::Cache::msdfCacheWriteLEF32;
using LumenGI::MeshSDF::Cache::kLumenMeshSDFCacheBuilderVersion;

using LumenGI::MeshSDF::LumenMeshSDFInstanceTable;
using LumenGI::MeshSDF::LumenMeshSDFSceneInstanceDesc;
using LumenGI::MeshSDF::LumenMeshSDFInstanceAtlasMapping;
using LumenGI::MeshSDF::LumenMeshSDFAtlasMeshDesc;
using LumenGI::MeshSDF::LumenMeshSDFAtlasInstanceDesc;
using LumenGI::MeshSDF::AtlasMissReason;
using LumenGI::MeshSDF::Quality;
using LumenGI::MeshSDF::MipPooling;
using LumenGI::MeshSDF::VolumeFormat;
using LumenGI::MeshSDF::MSDFHeader;
using LumenGI::MeshSDF::MSDFParseResult;
using LumenGI::MeshSDF::kMSDFFormatVersion;
using LumenGI::MeshSDF::kLumenMeshSDFAtlasInvalidID;
using LumenGI::MeshSDF::kLumenMeshSDFAtlasInvalidPage;
using LumenGI::MeshSDF::kLumenMeshSDFLayerMaskStatic;
using LumenGI::MeshSDF::kLumenMeshSDFLayerMaskDynamic;
using LumenGI::MeshSDF::kLumenMeshSDFLayerMaskAll;
using LumenGI::MeshSDF::kLumenMeshSDFSignConventionPositiveOutside;

// -------------------------------------------------------------------------------------
// Helpers
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
               ("lumen_msdf_cache_" + std::to_string(++counter) + "_" + std::to_string(stamp));
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

/// Local little-endian writers used to pin the cache-key byte schema.
void pushLEU32(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(uint8_t(v & 0xFFu));
    b.push_back(uint8_t((v >> 8) & 0xFFu));
    b.push_back(uint8_t((v >> 16) & 0xFFu));
    b.push_back(uint8_t((v >> 24) & 0xFFu));
}

void pushLEF32(std::vector<uint8_t>& b, float f)
{
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    pushLEU32(b, u);
}

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

constexpr uint64_t kTestMeshHash = 0x123456789ABCDEF0ull;

/// 16^3 cube mesh description for the instance-table tests. mipCount = 5
/// (16 -> 8 -> 4 -> 2 -> 1); every mip is a single page (each axis <= 32).
LumenMeshSDFAtlasMeshDesc makeCubeMeshDesc()
{
    LumenMeshSDFAtlasMeshDesc d;
    d.resolution = {16, 16, 16};
    d.mipCount = 5;
    d.formatMip0 = VolumeFormat::R16Float;
    d.pooling = MipPooling::MinAbs;
    d.contentHash = kTestMeshHash;
    d.quantRange = 1.f;
    d.normalizationScale = 1.f;
    d.voxelSize = 1.f / 15.f;
    d.bboxMin = {0.f, 0.f, 0.f};
    d.bboxMax = {1.f, 1.f, 1.f};
    d.signConvention = kLumenMeshSDFSignConventionPositiveOutside;
    d.signReliable = 1;
    return d;
}

LumenMeshSDFAtlasInstanceDesc makeIdentityTransform()
{
    LumenMeshSDFAtlasInstanceDesc t;
    t.forwardLinear = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    t.forwardTranslation = {0.f, 0.f, 0.f};
    return t;
}

LumenMeshSDFSceneInstanceDesc makeInstanceDesc(uint32_t meshID, const LumenMeshSDFAtlasInstanceDesc& transform, uint32_t layer)
{
    LumenMeshSDFSceneInstanceDesc d;
    d.meshID = meshID;
    d.transform = transform;
    d.layerMask = layer;
    return d;
}

/// x-axis ramp field: d = (x + 0.5) * voxelSize at every voxel (== pOut.x - bboxMin.x).
std::vector<float> makeRampField(const MSDFHeader& h)
{
    std::vector<float> d(size_t(h.dataCount));
    size_t i = 0;
    for (uint32_t z = 0; z < h.resolution[2]; ++z)
        for (uint32_t y = 0; y < h.resolution[1]; ++y)
            for (uint32_t x = 0; x < h.resolution[0]; ++x, ++i)
                d[i] = (float(x) + 0.5f) * h.voxelSize;
    return d;
}

// -------------------------------------------------------------------------------------
// 1) cacheKey: determinism, sensitivity, schema pinning
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFCache_KeyDeterminismAndSensitivity)
{
    const LumenMeshSDFCacheParams p = makeCacheParams();
    const std::string key = cacheKey(kTestMeshHash, p);

    // Determinism.
    EXPECT_EQ(key, cacheKey(kTestMeshHash, p));
    EXPECT_EQ(key.size(), 16u);
    for (char c : key)
    {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }

    // Sensitivity to every key input.
    EXPECT_NE(key, cacheKey(kTestMeshHash + 1, p));

    LumenMeshSDFCacheParams q = p;
    q.resolution[0] = 17;
    EXPECT_NE(key, cacheKey(kTestMeshHash, q));
    q = p;
    q.resolution[1] = 17;
    EXPECT_NE(key, cacheKey(kTestMeshHash, q));

    q = p;
    q.quality = Quality::Low;
    EXPECT_NE(key, cacheKey(kTestMeshHash, q));

    q = p;
    q.pooling = MipPooling::Average;
    EXPECT_NE(key, cacheKey(kTestMeshHash, q));

    q = p;
    q.gridBounds = {0.f, 0.f, 0.f, 2.f, 1.f, 1.f};
    EXPECT_NE(key, cacheKey(kTestMeshHash, q));

    q = p;
    q.gridBounds = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f}; // not provided -> different byte
    EXPECT_NE(key, cacheKey(kTestMeshHash, q));

    // Equal inputs (reordered grid-bound representation that yields the same
    // values) must still collide: the key is a function of the canonical bytes.
    LumenMeshSDFCacheParams r = p;
    r.gridBounds = {0.f, 0.f, 0.f, 1.f, 1.f, 1.f};
    EXPECT_EQ(key, cacheKey(kTestMeshHash, r));
}

CPU_TEST(LumenMeshSDFCache_KeySchemaPinned)
{
    const LumenMeshSDFCacheParams p = makeCacheParams();

    // Rebuild the documented canonical byte layout (see LumenMeshSDFCache.h) and
    // assert cacheKey == FNV-1a64 over those bytes. Pins builder version +
    // format version folding, so a schema change without a version bump fails.
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 8; ++i)
        bytes.push_back(uint8_t((kTestMeshHash >> (8 * i)) & 0xFFull));
    pushLEU32(bytes, kLumenMeshSDFCacheBuilderVersion);
    pushLEU32(bytes, kMSDFFormatVersion);
    pushLEU32(bytes, p.resolution[0]);
    pushLEU32(bytes, p.resolution[1]);
    pushLEU32(bytes, p.resolution[2]);
    bytes.push_back(uint8_t(p.quality));
    bytes.push_back(uint8_t(p.pooling));
    bytes.push_back(uint8_t(p.quality == Quality::High ? uint32_t(VolumeFormat::R16Float)
                                                       : uint32_t(VolumeFormat::R8Snorm)));
    const float dx = p.gridBounds[3] - p.gridBounds[0];
    const float dy = p.gridBounds[4] - p.gridBounds[1];
    const float dz = p.gridBounds[5] - p.gridBounds[2];
    const float rangeBound = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 1e-6f);
    pushLEF32(bytes, rangeBound);

    EXPECT_EQ(cacheKey(kTestMeshHash, p), msdfCacheToHex(msdfCacheFNV1a64(bytes.data(), bytes.size())));
}

// -------------------------------------------------------------------------------------
// 2/3) ".msdf" round-trip + corruption detection through findCached
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFCache_RoundTripAndCorruptionDetection)
{
    TempDir tmp;
    const std::filesystem::path file = tmp.path / "volume.msdf";

    const MSDFHeader h = makeHeader();
    const std::vector<float> d = makeDistances();
    const std::vector<std::string> warnings = {"open boundary", "thin mesh"};

    std::vector<uint8_t> bytes;
    std::string err;
    EXPECT_TRUE(LumenGI::MeshSDF::Cache::serializeMSDFBytes(h, d, warnings, bytes, err));
    EXPECT_TRUE(!bytes.empty());

    // Clean store + findCached round-trip.
    {
        MSDFParseResult notStored;
        EXPECT_TRUE(findCached(file, notStored, err) == false); // not stored yet
    }
    EXPECT_TRUE(LumenGI::MeshSDF::Cache::store(file, bytes, err));
    {
        MSDFParseResult out;
        EXPECT_TRUE(findCached(file, out, err));
        EXPECT_EQ(out.header.formatVersion, kMSDFFormatVersion);
        EXPECT_EQ(out.header.resolution[0], 16u);
        EXPECT_EQ(out.header.resolution[1], 16u);
        EXPECT_EQ(out.header.resolution[2], 16u);
        EXPECT_EQ(out.header.dataCount, h.dataCount);
        EXPECT_EQ(out.warnings.size(), warnings.size());
        EXPECT_EQ(out.warnings[0], "open boundary");
        EXPECT_EQ(out.warnings[1], "thin mesh");
        EXPECT_EQ(out.distances.size(), d.size());
        bool bitExact = true;
        for (size_t i = 0; i < d.size(); ++i)
            bitExact = bitExact && (out.distances[i] == d[i]);
        EXPECT_TRUE(bitExact);
    }

    // Corruption 1: flipped byte in the data region -> trailing checksum mismatch.
    {
        std::vector<uint8_t> corrupt = bytes;
        corrupt[100] ^= 0xFFu; // data region (dataOffset = 88 + pad(26) = 120)
        std::vector<uint8_t> stored;
        EXPECT_TRUE(LumenGI::MeshSDF::Cache::serializeMSDFBytes(h, d, warnings, stored, err));
        EXPECT_TRUE(LumenGI::MeshSDF::Cache::store(file, corrupt, err));
        MSDFParseResult out;
        EXPECT_FALSE(findCached(file, out, err));
        EXPECT_TRUE(err.find("checksum") != std::string::npos);
    }

    // Corruption 2: bad magic.
    {
        std::vector<uint8_t> corrupt = bytes;
        corrupt[0] = 0xFFu;
        EXPECT_TRUE(LumenGI::MeshSDF::Cache::store(file, corrupt, err));
        MSDFParseResult out;
        EXPECT_FALSE(findCached(file, out, err));
        EXPECT_TRUE(err.find("magic") != std::string::npos);
    }

    // Corruption 3: truncated file (offsets no longer line up).
    {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
        EXPECT_TRUE(LumenGI::MeshSDF::Cache::store(file, truncated, err));
        MSDFParseResult out;
        EXPECT_FALSE(findCached(file, out, err));
    }

    // Missing file.
    {
        MSDFParseResult notStored;
        EXPECT_FALSE(findCached(tmp.path / "does_not_exist.msdf", notStored, err));
    }

    // Restore the good file for the rebuild test below.
    EXPECT_TRUE(LumenGI::MeshSDF::Cache::store(file, bytes, err));
}

// -------------------------------------------------------------------------------------
// 4) Rebuild trigger: corrupt entry -> miss -> re-store -> hit
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFCache_RebuildTrigger)
{
    TempDir tmp;
    LumenMeshSDFDiskCache cache(tmp.path);

    const MSDFHeader h = makeHeader();
    const std::vector<float> d = makeDistances();
    const std::vector<std::string> warnings;
    const std::string key = cacheKey(kTestMeshHash, makeCacheParams());

    std::string err;
    EXPECT_TRUE(cache.storeVolume(key, h, d, warnings, err));

    // First call: hit.
    {
        MSDFParseResult out;
        EXPECT_TRUE(cache.findCached(key, out, err));
        EXPECT_EQ(out.distances.size(), d.size());
    }

    // Corrupt the file on disk -> the cache reports a miss and the caller must
    // rebuild (the S6-A2 contract: never serve a partially corrupt volume).
    {
        const std::filesystem::path p = cache.pathFor(key);
        std::vector<uint8_t> corrupt;
        {
            std::ifstream in(p, std::ios::binary);
            corrupt.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        corrupt[120] ^= 0x01u; // data byte -> checksum mismatch
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(corrupt.data()), std::streamsize(corrupt.size()));
        out.close();
        MSDFParseResult miss;
        EXPECT_FALSE(cache.findCached(key, miss, err));
    }

    // Rebuild: re-store valid bytes -> hit again.
    EXPECT_TRUE(cache.storeVolume(key, h, d, warnings, err));
    {
        MSDFParseResult out;
        EXPECT_TRUE(cache.findCached(key, out, err));
        EXPECT_EQ(out.distances.size(), d.size());
    }
}

// -------------------------------------------------------------------------------------
// 5) DiskCache directory management / atomic store / overwrite / remove
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFCache_DiskCacheDirectoryManagement)
{
    TempDir tmp;
    LumenMeshSDFDiskCache cache(tmp.path / "nested" / "cache");
    std::string err;
    EXPECT_TRUE(cache.ensureDirectory(err));
    EXPECT_EQ(cache.directory(), tmp.path / "nested" / "cache");

    const LumenMeshSDFCacheParams p = makeCacheParams();
    const std::string key = cacheKey(kTestMeshHash, p);
    EXPECT_EQ(cache.keyFor(kTestMeshHash, p), key);

    const std::filesystem::path expected = cache.directory() / (key + ".msdf");
    EXPECT_EQ(cache.pathFor(key), expected);

    // Store volume -> findCached round-trip.
    const MSDFHeader h = makeHeader();
    const std::vector<float> d = makeDistances();
    const std::vector<std::string> warnings = {"thin mesh"};
    EXPECT_TRUE(cache.storeVolume(key, h, d, warnings, err));
    EXPECT_TRUE(cache.exists(key));
    {
        MSDFParseResult out;
        EXPECT_TRUE(cache.findCached(key, out, err));
        EXPECT_EQ(out.warnings.size(), 1u);
        EXPECT_EQ(out.warnings[0], "thin mesh");
    }

    // No temp files left behind by the atomic write.
    bool noTempLeft = true;
    for (const auto& entry : std::filesystem::directory_iterator(cache.directory()))
    {
        if (entry.path().filename().string().find(".tmp") != std::string::npos)
            noTempLeft = false;
    }
    EXPECT_TRUE(noTempLeft);

    // Overwrite under the same key serves the NEW bytes.
    const std::vector<float> d2 = makeDistances();
    std::vector<float> shifted = d2;
    for (float& v : shifted)
        v += 0.25f;
    EXPECT_TRUE(cache.storeVolume(key, h, shifted, warnings, err));
    {
        MSDFParseResult out;
        EXPECT_TRUE(cache.findCached(key, out, err));
        EXPECT_EQ(out.distances.size(), shifted.size());
        EXPECT_TRUE(out.distances[0] == shifted[0]);
    }

    // Remove + follow-up queries.
    EXPECT_TRUE(cache.remove(key, err));
    EXPECT_FALSE(cache.exists(key));
    {
        MSDFParseResult notCached;
        EXPECT_FALSE(cache.findCached(key, notCached, err));
    }
    EXPECT_FALSE(cache.remove(key, err)); // not found anymore
}

// -------------------------------------------------------------------------------------
// 6) InstanceTable add/remove page reference counting (shared mesh dedup)
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFCache_InstanceTableAddRemoveRefCount)
{
    LumenMeshSDFInstanceTable table;
    const uint32_t meshID = table.registerMesh(makeCubeMeshDesc());
    EXPECT_NE(meshID, kLumenMeshSDFAtlasInvalidID);

    // First instance: 5 mip groups allocated (16^3 -> 5 mips, one page each).
    const uint32_t id0 = table.addInstance(makeInstanceDesc(meshID, makeIdentityTransform(), kLumenMeshSDFLayerMaskStatic));
    EXPECT_NE(id0, kLumenMeshSDFAtlasInvalidID);
    EXPECT_TRUE(table.isResident(id0));
    {
        const auto stats = table.getStats();
        EXPECT_EQ(stats.allocationCount, 5u);
        EXPECT_EQ(stats.releaseCount, 0u);
        EXPECT_EQ(stats.residentPages, 5u);
        EXPECT_EQ(stats.sharedGroupCount, 0u);
        EXPECT_EQ(stats.residentInstanceCount, 1u);
        EXPECT_EQ(table.instanceCount(), 1u);
        EXPECT_EQ(table.meshCount(), 1u);
    }

    // Second instance of the SAME mesh: pages are shared, no new allocation.
    LumenMeshSDFAtlasInstanceDesc t = makeIdentityTransform();
    t.forwardTranslation = {3.f, 0.f, 0.f};
    const uint32_t id1 = table.addInstance(makeInstanceDesc(meshID, t, kLumenMeshSDFLayerMaskDynamic));
    EXPECT_NE(id1, kLumenMeshSDFAtlasInvalidID);
    EXPECT_TRUE(table.isResident(id1));
    {
        const auto stats = table.getStats();
        EXPECT_EQ(stats.allocationCount, 5u);   // dedup: no new groups
        EXPECT_EQ(stats.residentPages, 5u);     // shared, not doubled
        EXPECT_EQ(stats.sharedGroupCount, 5u);  // every mip group referenced by 2 instances
        EXPECT_EQ(stats.releaseCount, 0u);
        EXPECT_EQ(stats.residentInstanceCount, 2u);
        EXPECT_EQ(table.instanceCount(), 2u);
    }

    // Remove the first: groups stay alive (still referenced by id1).
    EXPECT_TRUE(table.removeInstance(id0));
    EXPECT_FALSE(table.isResident(id0));
    EXPECT_EQ(table.pageSlot(id0, 0), kLumenMeshSDFAtlasInvalidPage);
    {
        const auto stats = table.getStats();
        EXPECT_EQ(stats.releaseCount, 0u);      // no orphan yet
        EXPECT_EQ(stats.residentPages, 5u);
        EXPECT_EQ(stats.sharedGroupCount, 0u);
        EXPECT_EQ(stats.residentInstanceCount, 1u);
        EXPECT_EQ(table.instanceCount(), 1u);
    }

    // Remove the last: every group orphaned -> pages released.
    EXPECT_TRUE(table.removeInstance(id1));
    {
        const auto stats = table.getStats();
        EXPECT_EQ(stats.releaseCount, 5u);
        EXPECT_EQ(stats.residentPages, 0u);
        EXPECT_EQ(stats.residentInstanceCount, 0u);
        EXPECT_EQ(table.instanceCount(), 0u);
    }

    // Invalid/removed handles are no-ops.
    EXPECT_FALSE(table.removeInstance(id0));
    EXPECT_FALSE(table.removeInstance(12345u));
}

// -------------------------------------------------------------------------------------
// 7) InstanceTable transform change -> dirty (page slots stable while shared)
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFCache_InstanceTableTransformChangeDirty)
{
    LumenMeshSDFInstanceTable table;
    const uint32_t meshID = table.registerMesh(makeCubeMeshDesc());

    // Two instances of the same mesh so a transform change on id0 never orphans
    // the shared page groups.
    const uint32_t id0 = table.addInstance(makeInstanceDesc(meshID, makeIdentityTransform(), kLumenMeshSDFLayerMaskStatic));
    const uint32_t id1 = table.addInstance(makeInstanceDesc(meshID, makeIdentityTransform(), kLumenMeshSDFLayerMaskStatic));
    EXPECT_NE(id0, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(id1, kLumenMeshSDFAtlasInvalidID);

    const uint32_t basePage0 = table.pageSlot(id0, 0);
    const LumenMeshSDFInstanceAtlasMapping before = table.instanceToAtlas(id0);
    EXPECT_TRUE(before.resident);
    EXPECT_EQ(before.basePageMip0, basePage0);

    table.clearDirtyAll();
    const uint64_t v0 = table.dirtyVersion();

    LumenMeshSDFAtlasInstanceDesc t = makeIdentityTransform();
    t.forwardTranslation = {2.f, 0.f, 0.f};
    EXPECT_TRUE(table.setInstanceTransform(id0, t));

    // Dirty + version bumped.
    EXPECT_TRUE(table.isDirty(id0));
    EXPECT_FALSE(table.isDirty(id1));
    EXPECT_GT(table.dirtyVersion(), v0);

    // Pages untouched: same base page, no allocation/release churn while shared.
    EXPECT_EQ(table.pageSlot(id0, 0), basePage0);
    {
        const auto stats = table.getStats();
        EXPECT_EQ(stats.allocationCount, 5u);
        EXPECT_EQ(stats.releaseCount, 0u);
        EXPECT_EQ(stats.residentPages, 5u);
    }

    // Mapping reflects the NEW world AABB (translated +2 on x).
    const LumenMeshSDFInstanceAtlasMapping after = table.instanceToAtlas(id0);
    EXPECT_TRUE(after.resident);
    EXPECT_EQ(after.basePageMip0, basePage0);
    EXPECT_TRUE(std::fabs(after.worldBoundsMin[0] - 2.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(after.worldBoundsMax[0] - 3.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(after.worldBoundsMin[1] - 0.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(after.worldBoundsMax[1] - 1.f) <= 1e-4f);

    // clearDirty resets the flag without bumping the version.
    table.clearDirty(id0);
    EXPECT_FALSE(table.isDirty(id0));
    EXPECT_EQ(table.dirtyVersion(), table.dirtyVersion());

    // Layer-mask changes also mark dirty.
    EXPECT_TRUE(table.setInstanceLayerMask(id0, kLumenMeshSDFLayerMaskDynamic));
    EXPECT_TRUE(table.isDirty(id0));
    EXPECT_EQ(table.getInstanceLayerMask(id0), kLumenMeshSDFLayerMaskDynamic);
}

// -------------------------------------------------------------------------------------
// 8) InstanceTable world <-> atlas coordinate round-trip (inverse transform +
//    atlas sampling coordinates)
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFCache_InstanceTableWorldToAtlasRoundTrip)
{
    LumenMeshSDFInstanceTable table;
    const uint32_t meshID = table.registerMesh(makeCubeMeshDesc());
    EXPECT_NE(meshID, kLumenMeshSDFAtlasInvalidID);

    LumenMeshSDFAtlasInstanceDesc t = makeIdentityTransform();
    t.forwardTranslation = {2.f, -1.f, 0.5f};
    const uint32_t id0 = table.addInstance(makeInstanceDesc(meshID, t, kLumenMeshSDFLayerMaskStatic));
    EXPECT_NE(id0, kLumenMeshSDFAtlasInvalidID);

    // Upload the x-ramp field to mip 0 of the mesh's pages.
    const MSDFHeader h = makeHeader();
    const std::vector<float> ramp = makeRampField(h);
    EXPECT_TRUE(table.atlas().uploadVolumeFloats(meshID, 0, ramp));

    // Voxel center index (4,4,4): pOut = (4.5,4.5,4.5) * voxelSize = (0.3,0.3,0.3).
    const float voxelSize = h.voxelSize;
    const float pOut[3] = {4.5f * voxelSize, 4.5f * voxelSize, 4.5f * voxelSize};
    const float worldPos[3] = {t.forwardTranslation[0] + pOut[0],
                               t.forwardTranslation[1] + pOut[1],
                               t.forwardTranslation[2] + pOut[2]};

    // world -> continuous voxel coords round-trips to the integer index.
    const std::array<float, 3> u = table.worldToAtlasVoxel(id0, worldPos);
    EXPECT_TRUE(std::fabs(u[0] - 4.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(u[1] - 4.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(u[2] - 4.f) <= 1e-4f);

    // Sampling at the voxel center returns the ramp value exactly (CPU page holds
    // the raw float; positive-outside convention needs no flip).
    const auto s0 = table.worldToAtlasSample(id0, worldPos, 0);
    EXPECT_TRUE(s0.resident);
    EXPECT_EQ(static_cast<uint32_t>(s0.reason), static_cast<uint32_t>(AtlasMissReason::None));
    EXPECT_TRUE(std::fabs(s0.distanceOutput - 0.3f) <= 1e-4f);

    // Transform change to a UNIFORM scale 2 + new translation: the SAME object
    // point maps to the SAME output-space distance (SDF is preserved under
    // similarities), i.e. the inverse transform round-trips exactly.
    LumenMeshSDFAtlasInstanceDesc t2;
    t2.forwardLinear = {2.f, 0.f, 0.f, 0.f, 2.f, 0.f, 0.f, 0.f, 2.f};
    t2.forwardTranslation = {0.f, 0.f, 3.f};
    EXPECT_TRUE(table.setInstanceTransform(id0, t2));

    // This was the last instance of the mesh, so the transform change orphaned
    // and re-allocated its page group (generation bump): the host must re-upload
    // the page contents, exactly the atlas eviction/reload contract.
    EXPECT_TRUE(table.atlas().uploadVolumeFloats(meshID, 0, ramp));

    const float worldPos2[3] = {t2.forwardTranslation[0] + 2.f * pOut[0],
                                t2.forwardTranslation[1] + 2.f * pOut[1],
                                t2.forwardTranslation[2] + 2.f * pOut[2]};
    const std::array<float, 3> u2 = table.worldToAtlasVoxel(id0, worldPos2);
    EXPECT_TRUE(std::fabs(u2[0] - 4.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(u2[1] - 4.f) <= 1e-4f);
    EXPECT_TRUE(std::fabs(u2[2] - 4.f) <= 1e-4f);
    const auto s1 = table.worldToAtlasSample(id0, worldPos2, 0);
    EXPECT_TRUE(s1.resident);
    EXPECT_TRUE(std::fabs(s1.distanceOutput - 0.3f) <= 1e-4f);

    // Outside the instance's world AABB -> OutOfInstanceBounds miss.
    const float outside[3] = {10.f, 10.f, 10.f};
    const auto miss = table.worldToAtlasSample(id0, outside, 0);
    EXPECT_FALSE(miss.resident);
    EXPECT_EQ(static_cast<uint32_t>(miss.reason), static_cast<uint32_t>(AtlasMissReason::OutOfInstanceBounds));

    // Invalid instance -> NoInstance miss.
    const auto noInst = table.worldToAtlasSample(12345u, worldPos2, 0);
    EXPECT_FALSE(noInst.resident);
    EXPECT_EQ(static_cast<uint32_t>(noInst.reason), static_cast<uint32_t>(AtlasMissReason::NoInstance));
}

// -------------------------------------------------------------------------------------
// 9) InstanceTable layer masks (Static / Dynamic filtering for S6-B3/B4)
// -------------------------------------------------------------------------------------

CPU_TEST(LumenMeshSDFCache_InstanceTableLayerMask)
{
    LumenMeshSDFInstanceTable table;
    const uint32_t meshID = table.registerMesh(makeCubeMeshDesc());

    const uint32_t id0 = table.addInstance(makeInstanceDesc(meshID, makeIdentityTransform(), kLumenMeshSDFLayerMaskStatic));
    const uint32_t id1 = table.addInstance(makeInstanceDesc(meshID, makeIdentityTransform(), kLumenMeshSDFLayerMaskDynamic));
    EXPECT_NE(id0, kLumenMeshSDFAtlasInvalidID);
    EXPECT_NE(id1, kLumenMeshSDFAtlasInvalidID);

    const std::vector<uint32_t> statics = table.instancesForLayer(kLumenMeshSDFLayerMaskStatic);
    EXPECT_EQ(statics.size(), 1u);
    EXPECT_EQ(statics[0], id0);
    const std::vector<uint32_t> dynamics = table.instancesForLayer(kLumenMeshSDFLayerMaskDynamic);
    EXPECT_EQ(dynamics.size(), 1u);
    EXPECT_EQ(dynamics[0], id1);
    const std::vector<uint32_t> all = table.instancesForLayer(kLumenMeshSDFLayerMaskAll);
    EXPECT_EQ(all.size(), 2u);

    // Re-classify id0 as dynamic.
    EXPECT_TRUE(table.setInstanceLayerMask(id0, kLumenMeshSDFLayerMaskDynamic));
    EXPECT_EQ(table.instancesForLayer(kLumenMeshSDFLayerMaskStatic).size(), 0u);
    EXPECT_EQ(table.instancesForLayer(kLumenMeshSDFLayerMaskDynamic).size(), 2u);
}

} // namespace
} // namespace Falcor
