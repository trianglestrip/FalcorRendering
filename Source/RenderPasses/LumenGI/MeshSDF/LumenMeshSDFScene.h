// =====================================================================================
//  LumenGI - S6-A2 Mesh SDF scene pipeline orchestration (CPU side, header-only)
//  -------------------------------------------------------------------------------------
//  The scene-level glue between the S6 Mesh SDF host components. A scene registers
//  instances (mesh content identity + world affine + layer mask); this component turns
//  them into resident atlas instances by plumbing the whole S6 host pipeline on demand:
//
//      scene instance (meshID + worldTransform + layerMask)
//        -> mesh  = (meshContentHash, cacheParams)              [S6-A2 cache key inputs]
//        -> cache lookup   LumenMeshSDFCache.h    key -> "<key>.msdf" on disk
//        -> on MISS: builder (MeshSDFBuilder.exe, path configurable, or the inline
//           placeholder) produces a valid ".msdf" -> atomic store -> cache hit
//        -> convert .msdf -> atlas mesh desc + per-mip page data
//           (production: Agent G's buildVolume(), injected as a VolumeConverter;
//            default: an inline reference converter mirroring the S6-B1 mip chain and
//            the atlas TEXTURE-STORED encoding so CPU tests link standard lib only)
//        -> registerMesh() (dedup) + uploadVolumeFloats()  [LumenMeshSDFInstanceTable.h]
//        -> addInstance() -> stable scene instance handle
//
//  VOLUME STATE MACHINE (drives the data flow; see meshState() / probeMesh() /
//  buildMesh() / ensureMesh() / evictFarthestInstance() / restoreInstance()):
//
//      Unknown     initial; not yet probed.
//      Cached      a valid ".msdf" exists on disk (verified by findCached) but the
//                  volume has not been converted / registered in the atlas yet.
//      NeedsBuild  cache miss OR a corrupt cache entry (corruption is detected by
//                  findCached and NEVER served); the builder must run.
//      Building    the builder is producing the volume (synchronous today; the state
//                  is reserved so a future async build can observe it in flight).
//      Uploaded    volume converted + atlas mesh registered + pages uploaded; the
//                  instance can be sampled.
//      Evicted     pages freed by the scene budget policy (evictFarthestInstance) or
//                  by the atlas LRU; restoreInstance() re-places + re-uploads.
//      Error       builder / converter / atlas produced invalid data; stays until a
//                  reload() or a retry with different inputs.
//
//  UPDATE SEMANTICS
//  -------------------------------------------------------------------------------------
//  * transform / layerMask changes on a registered instance mark the instance-table
//    entry dirty (dirtySceneInstanceIDs()); the root pass uploads the GPU tables after
//    a dirty frame. While a mesh's pages are still referenced by other instances a
//    transform change does not churn pages (instance table contract).
//  * reload() clears the atlas + instance table and every volume's runtime state but
//    keeps the scene registration; applyScene() re-materializes everything from the
//    disk cache (a cache HIT after reload, never a rebuild).
//
//  BUDGET / DEGRADATION
//  -------------------------------------------------------------------------------------
//  estimateGpuBytes() = resident atlas pages + the standalone mip-chain GPU bytes of
//  every loaded volume (an upper-bound "atlas + volume" estimate; pages shared across
//  instances are counted once per mesh). enforceBudget() evicts the FARTHEST resident
//  instance's pages from the camera until the estimate fits the budget (deterministic:
//  squared-distance to the instance world AABB center, ties broken toward the larger
//  scene instance ID). Eviction is a degradation, never a crash: the GDF instance list
//  excludes evicted instances and samples return NotResident.
//
//  GDF COMPOSE INTEROP
//  -------------------------------------------------------------------------------------
//  buildGDFInstanceList() emits LumenMeshSDFGDFInstance - a CPU mirror of
//  `LumenGDFInstance` in LumenGDFData.slang (40 bytes, frozen) - ready for the root
//  pass to upload as the gGDFInstances StructuredBuffer. LAYER/BIT COINCIDENCE: the
//  instance table uses kLumenMeshSDFLayerMaskStatic = 1<<0 / Dynamic = 1<<1 while the
//  GDF level classes use kLumenGDFLayerDynamic = 1<<0 / Static = 1<<1; the physical bits
//  coincide, so the instance layerMask is passed through unchanged (a Static-mesh
//  instance, bit0, feeds the GDF dynamic level 0; a Dynamic-mesh instance, bit1, feeds
//  the static far field). Documented so the caller knows the mapping is intentional.
//
//  SCOPE / CONSTRAINTS
//  -------------------------------------------------------------------------------------
//  * PURE C++17, header-only, standard library only. Includes LumenMeshSDFCache.h,
//    LumenMeshSDFInstanceTable.h and LumenMeshSDF.h READ-ONLY: it uses their types and
//    inline helpers but NEVER calls a symbol defined in LumenMeshSDF.cpp (buildVolume /
//    estimateMemoryBytes / computeCacheKey are not linkable from a header-only TU).
//    Production conversion is injected via setConverter(); the default inline
//    referenceConverter() and placeholderBuilder() keep standalone + tests self-
//    contained.
//  * No Falcor include, no CMake target (root pass integrates).
//  * Deterministic (no randomness), NOT thread-safe (single render-loop thread).
//  * Syntax check: cl /Zs /std:c++17 /EHsc LumenMeshSDFScene.h
// =====================================================================================
#pragma once

#include "LumenMeshSDFCache.h"         // READ-ONLY: Cache:: cache key / disk cache / inline .msdf serialization
#include "LumenMeshSDFInstanceTable.h" // READ-ONLY: instance table + atlas + types/constants
#include "LumenMeshSDF.h"              // READ-ONLY: MSDFHeader / MSDFParseResult / BuildParams / enums

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace LumenGI
{
namespace MeshSDF
{
namespace Scene
{

// -------------------------------------------------------------------------------------
// Volume state machine
// -------------------------------------------------------------------------------------

/// Per-mesh volume lifecycle state (see the file header for the transition table).
enum class VolumeState : uint8_t
{
    Unknown = 0, ///< Registered but never probed.
    Cached,      ///< Valid ".msdf" on disk, not yet materialized in the atlas.
    NeedsBuild,  ///< Cache miss / corrupt entry; the builder must run.
    Building,    ///< Builder running (synchronous today; reserved for async).
    Uploaded,    ///< Converted + atlas mesh registered + pages uploaded.
    Evicted,     ///< Pages freed by budget policy / atlas LRU.
    Error,       ///< Builder / converter / atlas failure.
    Count,
};
static_assert(static_cast<size_t>(VolumeState::Count) <= 8, "VolumeState must fit the stats counter array");

// -------------------------------------------------------------------------------------
// Host input + intermediate types
// -------------------------------------------------------------------------------------

/// Scene-level mesh reference: geometry identity + the S6-A2 cache key inputs.
struct LumenMeshSDFSceneMeshDesc
{
    uint64_t meshContentHash = 0;        ///< Geometry identity (Agent D contract).
    Cache::LumenMeshSDFCacheParams cacheParams; ///< resolution + quality + pooling + grid bounds (cache key inputs).
    std::filesystem::path sourcePath;    ///< Optional source handed to the external builder.
};

/// Output of the volume converter: the atlas registration description plus per-mip
/// float page data ready for atlas().uploadVolumeFloats() (TEXTURE-STORED semantics:
/// fine pages hold the float distance, coarse pages hold d / quantRange clamped to
/// [-1, 1] - the sample path rescales by quantRange, identical to the GPU).
struct LumenMeshSDFConvertedVolume
{
    LumenMeshSDFAtlasMeshDesc atlasMesh; ///< Feeds instanceTable().registerMesh() (dedup key).
    std::vector<std::vector<float>> mipFloats; ///< mipFloats[m].size() == mip m voxel count.
    size_t gpuBytes = 0;                 ///< Standalone-volume GPU bytes (mip chain, S6-B1 formula).
    size_t stagingBytes = 0;             ///< CPU bytes of mipFloats (held for re-upload after eviction).
};

/// CPU mirror of `LumenGDFInstance` in LumenGDFData.slang (40 bytes, frozen layout).
/// The root pass uploads a list of these as the gGDFInstances StructuredBuffer.
struct LumenMeshSDFGDFInstance
{
    uint32_t atlasInstanceID = kLumenMeshSDFAtlasInvalidID; ///< -> LumenMeshSDFAtlasInstance (invRows + bounds + meshID).
    uint32_t resident = 0;     ///< 1 = include in compose this frame (atlas resident + host filter).
    uint32_t layerMask = 0;    ///< kLumenGDFLayer* bits (bit-compatible with the mesh layer mask).
    uint32_t pad = 0;          ///< +12 padding.
    std::array<float, 3> boundsMin = {0.f, 0.f, 0.f}; ///< +16 world AABB min (voxel culling).
    std::array<float, 3> boundsMax = {0.f, 0.f, 0.f}; ///< +28 world AABB max.
};
static_assert(sizeof(LumenMeshSDFGDFInstance) == 40, "GDF instance entry must be 40 bytes (LumenGDFData.slang)");
static_assert(offsetof(LumenMeshSDFGDFInstance, boundsMin) == 16, "boundsMin offset frozen");
static_assert(offsetof(LumenMeshSDFGDFInstance, boundsMax) == 28, "boundsMax offset frozen");

/// Snapshot of scene counters and derived state (see getStats()).
struct LumenMeshSDFSceneStats
{
    uint32_t registeredMeshes = 0;       ///< Distinct cache-key mesh records.
    std::array<uint32_t, 8> stateCounts = {}; ///< Per VolumeState record count.
    uint32_t activeInstances = 0;        ///< Registered, non-removed scene instances.
    uint32_t residentInstances = 0;      ///< active, non-evicted and atlas-resident.
    uint32_t evictedInstances = 0;       ///< active but budget-evicted.
    uint64_t cacheLookups = 0;           ///< Disk cache probes (findCached calls from probe/ensure).
    uint64_t cacheHits = 0;              ///< Valid .msdf found.
    uint64_t cacheMisses = 0;            ///< Missing file.
    uint64_t corruptionsDetected = 0;    ///< File exists but failed validation (rebuilt, never served).
    uint64_t builds = 0;                 ///< Builder runs that produced a valid volume.
    uint64_t conversions = 0;            ///< .msdf -> atlas mesh + page data conversions.
    uint64_t evictions = 0;              ///< evictFarthestInstance() calls that removed pages.
    uint64_t restores = 0;               ///< restoreInstance() calls that re-residented pages.
    uint64_t estimatedGpuBytes = 0;      ///< estimateGpuBytes() at snapshot time.
    uint64_t residentBytes = 0;          ///< Atlas resident pages (GPU bytes) at snapshot time.
    uint64_t budgetBytes = 0;            ///< Scene budget in effect; 0 = unlimited.
};

// -------------------------------------------------------------------------------------
// Builder / converter interfaces
// -------------------------------------------------------------------------------------

/// Builds a ".msdf" volume for `mesh` into `targetPath`. Returns false and sets `err`
/// on failure. The scene validates the produced file before storing it in the cache.
using VolumeBuilder = std::function<bool(
    const LumenMeshSDFSceneMeshDesc& mesh,
    const std::filesystem::path& targetPath,
    std::string& err
)>;

/// Converts a parsed ".msdf" volume into atlas page data. The production wiring injects
/// Agent G's buildVolume() here; the default referenceConverter() mirrors the S6-B1 mip
/// chain so CPU tests and standalone use do not need LumenMeshSDF.cpp.
using VolumeConverter = std::function<bool(
    const MSDFParseResult& parsed,
    const BuildParams& params,
    LumenMeshSDFConvertedVolume& out,
    std::string& err
)>;

// -------------------------------------------------------------------------------------
// Inline layout helpers (mirrors of S6-B1 formulas; reuses the atlas inline helpers)
// -------------------------------------------------------------------------------------

/// Mip count: 1 + ceil(log2(max(res))) capped at kLumenMeshSDFMaxMipCount (S6-B1 rule).
inline uint32_t sceneMipCount(const std::array<uint32_t, 3>& resolution)
{
    const uint32_t maxDim = std::max(resolution[0], std::max(resolution[1], resolution[2]));
    uint32_t count = 1;
    for (uint32_t d = maxDim; d > 1; d = (d + 1) / 2)
        ++count;
    return std::min(count, kLumenMeshSDFMaxMipCount);
}

/// Standalone-volume GPU bytes for the mip chain (mirror of estimateMemoryBytes;
/// inline so a header-only TU can budget without linking LumenMeshSDF.cpp).
inline size_t sceneVolumeGpuBytes(const MSDFHeader& header, Quality quality)
{
    const VolumeFormat mip0 = quality == Quality::High ? VolumeFormat::R16Float : VolumeFormat::R8Snorm;
    const uint32_t mipCount = sceneMipCount(header.resolution);
    std::array<uint32_t, 3> dims = header.resolution;
    size_t total = 0;
    for (uint32_t m = 0; m < mipCount; ++m)
    {
        const size_t voxels = size_t(dims[0]) * size_t(dims[1]) * size_t(dims[2]);
        const size_t bpp = (m == 0 && mip0 == VolumeFormat::R16Float) ? kBytesPerVoxelR16Float : kBytesPerVoxelR8Snorm;
        total += voxels * bpp;
        for (int a = 0; a < 3; ++a)
            dims[a] = std::max<uint32_t>(1, (dims[a] + 1) / 2);
    }
    return total;
}

/// Min-abs mip pooling (mirror of poolMipLevel MinAbs; inline for header-only use).
inline std::vector<float> scenePoolMinAbs(
    const std::vector<float>& src,
    const std::array<uint32_t, 3>& dimsPrev,
    const std::array<uint32_t, 3>& dimsCur
)
{
    std::vector<float> out(size_t(dimsCur[0]) * size_t(dimsCur[1]) * size_t(dimsCur[2]));
    for (uint32_t tz = 0; tz < dimsCur[2]; ++tz)
    {
        const uint32_t zLo = 2u * tz;
        const uint32_t zHi = std::min(2u * tz + 1u, dimsPrev[2] - 1u);
        for (uint32_t ty = 0; ty < dimsCur[1]; ++ty)
        {
            const uint32_t yLo = 2u * ty;
            const uint32_t yHi = std::min(2u * ty + 1u, dimsPrev[1] - 1u);
            for (uint32_t tx = 0; tx < dimsCur[0]; ++tx)
            {
                const uint32_t xLo = 2u * tx;
                const uint32_t xHi = std::min(2u * tx + 1u, dimsPrev[0] - 1u);
                float bestAbs = 3.4e38f;
                float bestVal = 0.f;
                for (uint32_t z = zLo; z <= zHi; ++z)
                    for (uint32_t y = yLo; y <= yHi; ++y)
                        for (uint32_t x = xLo; x <= xHi; ++x)
                        {
                            const float v = src[(size_t(z) * dimsPrev[1] + y) * dimsPrev[0] + x];
                            const float a = std::fabs(v);
                            if (a < bestAbs)
                            {
                                bestAbs = a;
                                bestVal = v;
                            }
                        }
                out[(size_t(tz) * dimsCur[1] + ty) * dimsCur[0] + tx] = bestVal;
            }
        }
    }
    return out;
}

/// R8Snorm scale R = max |d| over the mip-0 field (1.0 for a degenerate all-zero field).
inline float sceneFieldQuantRange(const std::vector<float>& distances)
{
    float q = 0.f;
    for (float v : distances)
        q = std::max(q, std::fabs(v));
    if (q < 1e-30f)
        q = 1.f;
    return q;
}

/// Derive the atlas mesh description from a parsed ".msdf" + build params (feeds
/// registerMesh(); resolution / mipCount / quantRange etc. are OUTPUT-space exactly as
/// in Agent G's contract). Returns false for a malformed header.
inline bool sceneAtlasMeshDescFromParsed(
    const MSDFParseResult& parsed,
    const BuildParams& params,
    LumenMeshSDFAtlasMeshDesc& out
)
{
    const MSDFHeader& h = parsed.header;
    if (!Cache::msdfCacheValidGridHeader(h))
        return false;
    out.resolution = h.resolution;
    out.mipCount = sceneMipCount(h.resolution);
    out.formatMip0 = params.quality == Quality::High ? VolumeFormat::R16Float : VolumeFormat::R8Snorm;
    out.pooling = params.pooling;
    out.contentHash = params.meshContentHash;
    out.quantRange = sceneFieldQuantRange(parsed.distances);
    out.normalizationScale = h.normalizationScale;
    out.voxelSize = h.voxelSize;
    out.bboxMin = h.bboxMin;
    out.bboxMax = h.bboxMax;
    out.signConvention = h.signConvention;
    out.signReliable = h.signReliable;
    return atlasValidateMeshDesc(out);
}

// -------------------------------------------------------------------------------------
// Default builder / converter (standalone + tests; the root pass injects the real ones)
// -------------------------------------------------------------------------------------

/// Default builder: writes a deterministic placeholder ".msdf" (x-axis ramp field,
/// d = (x + 0.5) * voxelSize) derived from the cache params' resolution + grid bounds.
/// This is what makes the pipeline runnable without MeshSDFBuilder.exe; it is also what
/// the corruption-rebuild tests rely on (deterministic output -> reproducible cache).
inline bool placeholderBuilder(
    const LumenMeshSDFSceneMeshDesc& mesh,
    const std::filesystem::path& targetPath,
    std::string& err
)
{
    const std::array<uint32_t, 3>& res = mesh.cacheParams.resolution;
    if (res[0] < 2 || res[1] < 2 || res[2] < 2)
    {
        err = "placeholder builder requires resolution >= 2 per axis";
        return false;
    }

    const std::array<float, 6>& g = mesh.cacheParams.gridBounds;
    const bool boundsProvided = g[0] < g[3] && g[1] < g[4] && g[2] < g[5];
    const std::array<float, 3> bmin = boundsProvided ? std::array<float, 3>{g[0], g[1], g[2]}
                                                     : std::array<float, 3>{0.f, 0.f, 0.f};
    const std::array<float, 3> bmax = boundsProvided ? std::array<float, 3>{g[3], g[4], g[5]}
                                                     : std::array<float, 3>{1.f, 1.f, 1.f};

    const float maxRes = float(std::max(res[0], std::max(res[1], res[2])));
    const float maxExtent = std::max(bmax[0] - bmin[0], std::max(bmax[1] - bmin[1], bmax[2] - bmin[2]));
    const float voxelSize = maxExtent / (maxRes - 1.f); // voxel-center convention.

    MSDFHeader h;
    h.formatVersion = kMSDFFormatVersion;
    h.resolution = res;
    h.bboxMin = bmin;
    h.bboxMax = bmax;
    h.voxelSize = voxelSize;
    h.normalizationScale = 1.f;
    h.paddingWorld = 0.1f;
    h.signConvention = kLumenMeshSDFSignConventionPositiveOutside;
    h.signReliable = 1;
    h.dataCount = uint64_t(res[0]) * uint64_t(res[1]) * uint64_t(res[2]);

    std::vector<float> d(size_t(h.dataCount));
    size_t i = 0;
    for (uint32_t z = 0; z < res[2]; ++z)
        for (uint32_t y = 0; y < res[1]; ++y)
            for (uint32_t x = 0; x < res[0]; ++x, ++i)
                d[i] = (float(x) + 0.5f) * voxelSize; // d == pOut.x - bboxMin.x

    std::vector<uint8_t> bytes;
    std::string serr;
    if (!Cache::serializeMSDFBytes(h, d, {}, bytes, serr))
    {
        err = "placeholder serialize failed: " + serr;
        return false;
    }
    std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        err = "cannot open builder output: " + targetPath.string();
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.flush();
    if (!out)
    {
        err = "failed writing builder output: " + targetPath.string();
        return false;
    }
    return true;
}

/// Builder wrapper for an external MeshSDFBuilder.exe (path configurable via
/// setExternalBuilderPath()). The exe's CLI contract is defined by the MeshSDFBuilder
/// tool / the root pass; this wrapper only assembles the command line and checks the
/// exit code - the scene validates the produced ".msdf" before storing it.
inline bool externalExeBuilder(
    const std::filesystem::path& exe,
    const LumenMeshSDFSceneMeshDesc& mesh,
    const std::filesystem::path& targetPath,
    std::string& err
)
{
    if (exe.empty())
    {
        err = "MeshSDFBuilder.exe path is not set";
        return false;
    }
    std::string cmd = "\"" + exe.string() + "\"";
    if (!mesh.sourcePath.empty())
        cmd += " \"" + mesh.sourcePath.string() + "\"";
    cmd += " \"" + targetPath.string() + "\"";
    cmd += " " + std::to_string(mesh.cacheParams.resolution[0]) + " " +
           std::to_string(mesh.cacheParams.resolution[1]) + " " +
           std::to_string(mesh.cacheParams.resolution[2]);
    const int rc = std::system(cmd.c_str());
    if (rc != 0)
    {
        err = "MeshSDFBuilder.exe exited with code " + std::to_string(rc);
        return false;
    }
    return true;
}

/// Default converter: mirrors the S6-B1 mip chain + the atlas TEXTURE-STORED encoding
/// (mip 0 stores the raw float distance; mips >= 1 store clamp(d / R, -1, 1) where
/// R = max |d| over mip 0). Production swaps this for a buildVolume()-based converter.
inline bool referenceConverter(
    const MSDFParseResult& parsed,
    const BuildParams& params,
    LumenMeshSDFConvertedVolume& out,
    std::string& err
)
{
    out = LumenMeshSDFConvertedVolume{};
    const MSDFHeader& h = parsed.header;
    if (!sceneAtlasMeshDescFromParsed(parsed, params, out.atlasMesh))
    {
        err = "cannot derive atlas mesh desc from parsed volume (bad header)";
        return false;
    }

    const float R = out.atlasMesh.quantRange;
    const uint32_t mipCount = out.atlasMesh.mipCount;

    // Raw float chain first (pooling must run on un-encoded distances).
    std::vector<std::vector<float>> raw(mipCount);
    raw[0] = parsed.distances;
    std::array<uint32_t, 3> dimsPrev = h.resolution;
    for (uint32_t m = 1; m < mipCount; ++m)
    {
        const std::array<uint32_t, 3> dimsCur = atlasMipDims(h.resolution, m);
        raw[m] = scenePoolMinAbs(raw[m - 1], dimsPrev, dimsCur);
        dimsPrev = dimsCur;
    }

    out.mipFloats.resize(mipCount);
    out.mipFloats[0] = raw[0]; // fine: raw float distance (R16Float decode is identity for stored f32).
    out.stagingBytes = raw[0].size() * sizeof(float);
    for (uint32_t m = 1; m < mipCount; ++m)
    {
        std::vector<float>& stored = out.mipFloats[m];
        stored = std::move(raw[m]);
        for (float& v : stored)
            v = std::max(-1.f, std::min(1.f, v / R)); // coarse: code/127 * R decodes back to d.
        out.stagingBytes += stored.size() * sizeof(float);
    }
    out.gpuBytes = sceneVolumeGpuBytes(h, params.quality);
    return true;
}

// -------------------------------------------------------------------------------------
// The scene pipeline component (header-only class; all methods inline)
// -------------------------------------------------------------------------------------

/// Scene-level Mesh SDF orchestration: cache -> builder -> convert -> atlas -> instance
/// table, with the volume state machine, budget eviction and GDF interop described in
/// the file header. Deterministic; NOT thread-safe.
class LumenMeshSDFScene
{
public:
    /// \param cacheDirectory Disk cache root (defaults to Cache::defaultCacheDir()).
    /// \param atlasMemoryBudgetBytes Hard GPU-byte budget forwarded to the atlas; 0 = unlimited.
    /// \param atlasPageBudget Hard resident-page budget forwarded to the atlas; 0 = unlimited.
    /// \param minResidencyFrames Atlas min-residency window (LRU eviction shield).
    explicit LumenMeshSDFScene(
        std::filesystem::path cacheDirectory = Cache::defaultCacheDir(),
        uint64_t atlasMemoryBudgetBytes = 0,
        uint64_t atlasPageBudget = 0,
        uint32_t minResidencyFrames = kLumenMeshSDFAtlasDefaultMinResidencyFrames)
        : mTable(kLumenMeshSDFAtlasDefaultPagesPerSide, atlasMemoryBudgetBytes, atlasPageBudget, minResidencyFrames)
        , mCache(std::move(cacheDirectory))
        , mBuilder(&placeholderBuilder)
        , mConverter(&referenceConverter)
    {
    }

    // -- Configuration -------------------------------------------------------

    /// Override the builder (default: inline placeholder). The root pass can route to
    /// MeshSDFBuilder.exe here, or via setExternalBuilderPath().
    void setBuilder(VolumeBuilder builder) { mBuilder = std::move(builder); }

    /// Configure the external builder exe; installs externalExeBuilder() as the builder.
    void setExternalBuilderPath(const std::filesystem::path& exe)
    {
        mBuilderPath = exe;
        mBuilder = [exe](const LumenMeshSDFSceneMeshDesc& mesh, const std::filesystem::path& target, std::string& err) {
            return externalExeBuilder(exe, mesh, target, err);
        };
    }

    /// Override the volume converter (default: inline reference converter). The root
    /// pass wires Agent G's buildVolume() here for the production data path.
    void setConverter(VolumeConverter converter) { mConverter = std::move(converter); }

    /// Eviction distance anchor (budget policy). Default: world origin.
    void setCamera(const float worldPos[3])
    {
        mCamera[0] = worldPos[0];
        mCamera[1] = worldPos[1];
        mCamera[2] = worldPos[2];
    }

    // -- Cache key / state ---------------------------------------------------

    /// S6-A2 disk cache key for a mesh (16 lowercase hex chars, <key>.msdf).
    static std::string meshKey(const LumenMeshSDFSceneMeshDesc& mesh)
    {
        return Cache::cacheKey(mesh.meshContentHash, mesh.cacheParams);
    }

    /// Current volume state (does NOT query the disk; see probeMesh()).
    VolumeState meshState(const LumenMeshSDFSceneMeshDesc& mesh) const
    {
        const size_t idx = findMeshRecord(meshKey(mesh));
        return idx == kNoMesh ? VolumeState::Unknown : mMeshes[idx].state;
    }

    /// Current volume state by cache key.
    VolumeState meshState(const std::string& key) const
    {
        const size_t idx = findMeshRecord(key);
        return idx == kNoMesh ? VolumeState::Unknown : mMeshes[idx].state;
    }

    /// Query the disk cache and set the state to Cached (valid volume found) or
    /// NeedsBuild (missing / corrupt). Loaded volumes (Uploaded / Evicted) are not
    /// re-probed - their in-memory data is authoritative until reload().
    VolumeState probeMesh(const LumenMeshSDFSceneMeshDesc& mesh)
    {
        const size_t idx = findOrCreateMeshRecord(meshKey(mesh), mesh);
        return probe(mMeshes[idx]);
    }

    /// Run the builder (NeedsBuild -> Cached). Stores a validated volume into the cache.
    /// Returns false and sets `err` on failure (state becomes Error).
    bool buildMesh(const LumenMeshSDFSceneMeshDesc& mesh, std::string& err)
    {
        const size_t idx = findOrCreateMeshRecord(meshKey(mesh), mesh);
        return buildMesh(mMeshes[idx], err);
    }

    /// Full data flow (see the file header state machine): query cache -> build on miss
    /// -> convert -> atlas register + page upload. On success `atlasMeshID` receives the
    /// registered (deduplicated) atlas mesh ID and the state is Uploaded.
    bool ensureMesh(const LumenMeshSDFSceneMeshDesc& mesh, uint32_t& atlasMeshID, std::string& err)
    {
        const size_t idx = findOrCreateMeshRecord(meshKey(mesh), mesh);
        return ensureMesh(mMeshes[idx], atlasMeshID, err);
    }

    /// Header of a loaded (Uploaded / Evicted) volume. Returns false when the mesh is
    /// not loaded yet (probe-only state Cached does not retain the parse result).
    bool getMeshHeader(const LumenMeshSDFSceneMeshDesc& mesh, MSDFHeader& out) const
    {
        const size_t idx = findMeshRecord(meshKey(mesh));
        if (idx == kNoMesh)
            return false;
        const MeshRecord& rec = mMeshes[idx];
        if (rec.state != VolumeState::Uploaded && rec.state != VolumeState::Evicted)
            return false;
        out = rec.header;
        return true;
    }

    // -- Scene instance registration -----------------------------------------

    /// Register a scene instance: ensure the mesh is loaded (cache/builder/convert/
    /// atlas), place its pages and return a STABLE scene instance ID, or
    /// kLumenMeshSDFAtlasInvalidID on failure (`err` is set). The instance is marked
    /// non-resident when the atlas cannot place its pages (budget-bound) - stable
    /// degradation, never a crash.
    uint32_t addInstance(
        const LumenMeshSDFSceneMeshDesc& mesh,
        const LumenMeshSDFAtlasInstanceDesc& transform,
        uint32_t layerMask,
        std::string& err
    )
    {
        const std::string key = meshKey(mesh);
        const size_t idx = findOrCreateMeshRecord(key, mesh);
        MeshRecord& rec = mMeshes[idx];

        uint32_t atlasMeshID = kLumenMeshSDFAtlasInvalidID;
        if (!ensureMesh(rec, atlasMeshID, err))
            return kLumenMeshSDFAtlasInvalidID;

        LumenMeshSDFSceneInstanceDesc desc;
        desc.meshID = atlasMeshID;
        desc.transform = transform;
        desc.layerMask = layerMask;
        const uint32_t tableID = mTable.addInstance(desc);
        if (tableID == kLumenMeshSDFAtlasInvalidID)
            return kLumenMeshSDFAtlasInvalidID;

        // Pages are placed now; upload the held mip data (first upload during ensureMesh
        // was a deferred no-op because the mesh had no resident group yet).
        mTable.touchInstance(tableID);
        uploadMeshPages(rec);

        ++rec.refCount;
        rec.lastUsedFrame = mFrame;

        InstanceRecord r;
        r.active = true;
        r.meshKey = key;
        r.atlasMeshID = atlasMeshID;
        r.tableInstanceID = tableID;
        r.transform = transform;
        r.layerMask = layerMask;
        mInstances.push_back(r);
        return uint32_t(mInstances.size() - 1u);
    }

    /// Remove a scene instance: page refcounts drop; pages are freed when the last
    /// instance of a mesh goes away. Returns false for an invalid/removed handle.
    bool removeInstance(uint32_t sceneInstanceID)
    {
        if (sceneInstanceID >= mInstances.size())
            return false;
        InstanceRecord& r = mInstances[sceneInstanceID];
        if (!r.active)
            return false;
        if (!r.evicted)
            mTable.removeInstance(r.tableInstanceID);
        r.active = false;
        r.tableInstanceID = kLumenMeshSDFAtlasInvalidID;
        MeshRecord* rec = findMeshRecordByKey(r.meshKey);
        if (rec && rec->refCount > 0)
            --rec->refCount;
        return true;
    }

    /// Change an instance's transform (marks the atlas entry dirty for GPU upload).
    /// Returns false on invalid input (old transform is best-effort restored).
    bool setInstanceTransform(uint32_t sceneInstanceID, const LumenMeshSDFAtlasInstanceDesc& transform)
    {
        if (sceneInstanceID >= mInstances.size())
            return false;
        InstanceRecord& r = mInstances[sceneInstanceID];
        if (!r.active)
            return false;
        if (!r.evicted && !mTable.setInstanceTransform(r.tableInstanceID, transform))
            return false;
        r.transform = transform;
        return true;
    }

    /// Change an instance's layer mask (Static / Dynamic; marks dirty).
    bool setInstanceLayerMask(uint32_t sceneInstanceID, uint32_t layerMask)
    {
        if (sceneInstanceID >= mInstances.size())
            return false;
        InstanceRecord& r = mInstances[sceneInstanceID];
        if (!r.active)
            return false;
        if (!r.evicted && !mTable.setInstanceLayerMask(r.tableInstanceID, layerMask))
            return false;
        r.layerMask = layerMask;
        return true;
    }

    /// Re-resident an evicted instance: re-add to the atlas (generation bump) and
    /// re-upload the held page data. Returns false on invalid input or placement failure.
    bool restoreInstance(uint32_t sceneInstanceID, std::string& err)
    {
        if (sceneInstanceID >= mInstances.size())
            return false;
        InstanceRecord& r = mInstances[sceneInstanceID];
        if (!r.active)
            return false;
        if (!r.evicted)
            return true; // already resident
        MeshRecord* rec = findMeshRecordByKey(r.meshKey);
        if (rec == nullptr || rec->state != VolumeState::Uploaded)
        {
            err = "mesh not loaded for restore";
            return false;
        }
        LumenMeshSDFSceneInstanceDesc desc;
        desc.meshID = rec->atlasMeshID;
        desc.transform = r.transform;
        desc.layerMask = r.layerMask;
        const uint32_t tableID = mTable.addInstance(desc);
        if (tableID == kLumenMeshSDFAtlasInvalidID)
        {
            err = "atlas addInstance failed on restore";
            return false;
        }
        r.tableInstanceID = tableID;
        r.evicted = false;
        mTable.touchInstance(tableID); // re-place any mips the atlas could not place
        uploadMeshPages(*rec);
        ++mRestores;
        return true;
    }

    // -- Queries -------------------------------------------------------------

    /// True when ALL mips of the scene instance are resident in the atlas.
    bool isResident(uint32_t sceneInstanceID) const
    {
        if (sceneInstanceID >= mInstances.size())
            return false;
        const InstanceRecord& r = mInstances[sceneInstanceID];
        if (!r.active || r.evicted)
            return false;
        return mTable.isResident(r.tableInstanceID);
    }

    /// Current instance -> atlas mapping (atlas instance index, mesh, base page, bounds).
    LumenMeshSDFInstanceAtlasMapping instanceToAtlas(uint32_t sceneInstanceID) const
    {
        LumenMeshSDFInstanceAtlasMapping out;
        if (sceneInstanceID >= mInstances.size())
            return out;
        const InstanceRecord& r = mInstances[sceneInstanceID];
        if (!r.active || r.evicted)
            return out;
        return mTable.instanceToAtlas(r.tableInstanceID);
    }

    /// Continuous mip-0 voxel coords for a world position (CPU mirror of the atlas math).
    std::array<float, 3> worldToAtlasVoxel(uint32_t sceneInstanceID, const float worldPos[3]) const
    {
        if (sceneInstanceID >= mInstances.size())
            return {0.f, 0.f, 0.f};
        const InstanceRecord& r = mInstances[sceneInstanceID];
        if (!r.active || r.evicted)
            return {0.f, 0.f, 0.f};
        return mTable.worldToAtlasVoxel(r.tableInstanceID, worldPos);
    }

    /// Sample the atlas at a world position for a scene instance (CPU mirror of the
    /// GPU sampling path). Distances are OUTPUT units.
    LumenMeshSDFAtlasSampleResult worldToAtlasSample(uint32_t sceneInstanceID, const float worldPos[3], uint32_t mip = 0)
    {
        LumenMeshSDFAtlasSampleResult miss;
        if (sceneInstanceID >= mInstances.size())
        {
            miss.reason = AtlasMissReason::NoInstance;
            return miss;
        }
        const InstanceRecord& r = mInstances[sceneInstanceID];
        if (!r.active || r.evicted)
        {
            miss.reason = AtlasMissReason::NoInstance;
            return miss;
        }
        return mTable.worldToAtlasSample(r.tableInstanceID, worldPos, mip);
    }

    /// Active (non-removed, non-evicted) scene instances whose layer mask overlaps
    /// `layerMask` (for GDF compose / trace filtering).
    std::vector<uint32_t> instancesForLayer(uint32_t layerMask) const
    {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < mInstances.size(); ++i)
        {
            const InstanceRecord& r = mInstances[i];
            if (r.active && !r.evicted && (r.layerMask & layerMask) != 0)
                out.push_back(uint32_t(i));
        }
        return out;
    }

    /// Scene instance IDs whose atlas entry changed (add / remove / transform / layer).
    std::vector<uint32_t> dirtySceneInstanceIDs() const
    {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < mInstances.size(); ++i)
        {
            const InstanceRecord& r = mInstances[i];
            if (r.active && !r.evicted && mTable.isDirty(r.tableInstanceID))
                out.push_back(uint32_t(i));
        }
        return out;
    }

    /// GDF compose input: resident instances whose layer mask overlaps `gdfLayerMask`,
    /// emitted as LumenMeshSDFGDFInstance (LumenGDFData.slang mirror). The instance
    /// layerMask is passed through unchanged (bit-compatible with kLumenGDFLayer*).
    std::vector<LumenMeshSDFGDFInstance> buildGDFInstanceList(uint32_t gdfLayerMask) const
    {
        std::vector<LumenMeshSDFGDFInstance> out;
        for (size_t i = 0; i < mInstances.size(); ++i)
        {
            const InstanceRecord& r = mInstances[i];
            if (!r.active || r.evicted || (r.layerMask & gdfLayerMask) == 0)
                continue;
            if (!mTable.isResident(r.tableInstanceID))
                continue; // resident == 0 -> excluded from compose this frame.
            const LumenMeshSDFInstanceAtlasMapping m = mTable.instanceToAtlas(r.tableInstanceID);
            LumenMeshSDFGDFInstance e;
            e.atlasInstanceID = m.atlasInstanceID;
            e.resident = 1;
            e.layerMask = r.layerMask;
            e.boundsMin = m.worldBoundsMin;
            e.boundsMax = m.worldBoundsMax;
            out.push_back(e);
        }
        return out;
    }

    // -- Budget / eviction ---------------------------------------------------

    /// Atlas resident pages (GPU bytes).
    uint64_t residentBytes() const { return mTable.getStats().residentBytes; }

    /// "Atlas + volume" GPU estimate: resident pages + the standalone mip-chain bytes of
    /// every loaded volume (pages shared across instances counted once per mesh).
    uint64_t estimateGpuBytes() const
    {
        uint64_t total = residentBytes();
        for (const MeshRecord& rec : mMeshes)
            if (rec.state == VolumeState::Uploaded || rec.state == VolumeState::Evicted)
                total += rec.gpuBytes;
        return total;
    }

    /// True when estimateGpuBytes() fits the scene budget (0 = unlimited).
    bool isWithinBudget() const
    {
        return mBudgetBytes == 0 || estimateGpuBytes() <= mBudgetBytes;
    }

    /// Evict resident instances farthest from the camera until estimateGpuBytes() fits
    /// `budgetBytes`. Returns whether the estimate is now within budget.
    bool enforceBudget(uint64_t budgetBytes)
    {
        mBudgetBytes = budgetBytes;
        if (budgetBytes == 0)
            return true;
        for (uint32_t guard = 0; guard <= mInstances.size(); ++guard)
        {
            if (estimateGpuBytes() <= budgetBytes)
                return true;
            if (evictFarthestInstance() == kLumenMeshSDFAtlasInvalidID)
                return false;
        }
        return estimateGpuBytes() <= budgetBytes;
    }

    /// Evict the pages of the resident instance farthest from the camera (deterministic:
    /// squared distance to the instance world AABB center, ties broken toward the larger
    /// scene instance ID). The instance stays registered (state Evicted); it is excluded
    /// from sampling / GDF lists until restoreInstance(). Returns the evicted scene
    /// instance ID, or kLumenMeshSDFAtlasInvalidID when nothing is evictable.
    uint32_t evictFarthestInstance()
    {
        size_t best = kNoMesh;
        float bestDistSq = -1.f;
        for (size_t i = 0; i < mInstances.size(); ++i)
        {
            const InstanceRecord& r = mInstances[i];
            if (!r.active || r.evicted)
                continue;
            if (!mTable.isResident(r.tableInstanceID))
                continue;
            const LumenMeshSDFInstanceAtlasMapping m = mTable.instanceToAtlas(r.tableInstanceID);
            const float cx = 0.5f * (m.worldBoundsMin[0] + m.worldBoundsMax[0]) - mCamera[0];
            const float cy = 0.5f * (m.worldBoundsMin[1] + m.worldBoundsMax[1]) - mCamera[1];
            const float cz = 0.5f * (m.worldBoundsMin[2] + m.worldBoundsMax[2]) - mCamera[2];
            const float distSq = cx * cx + cy * cy + cz * cz;
            if (distSq > bestDistSq + 1e-12f)
            {
                bestDistSq = distSq;
                best = i;
            }
            else if (distSq > bestDistSq - 1e-12f && i > best)
            {
                bestDistSq = distSq; // equal distance: deterministic tie-break.
                best = i;
            }
        }
        if (best == kNoMesh)
            return kLumenMeshSDFAtlasInvalidID;
        mTable.removeInstance(mInstances[best].tableInstanceID);
        mInstances[best].tableInstanceID = kLumenMeshSDFAtlasInvalidID;
        mInstances[best].evicted = true;
        ++mEvictions;
        return uint32_t(best);
    }

    // -- Scene lifecycle -----------------------------------------------------

    /// Advance the frame counter (forwards to the atlas; drives LRU min-residency).
    void endFrame()
    {
        mTable.endFrame();
        ++mFrame;
    }

    /// Clear the atlas + instance table and every volume's runtime state, but keep the
    /// scene registration (mesh descriptors + instance transforms/layers). applyScene()
    /// re-materializes everything from the disk cache (a cache HIT after reload, never
    /// a rebuild) - the "scene reload" path.
    void reload()
    {
        mTable.reset();
        for (MeshRecord& rec : mMeshes)
        {
            rec.state = VolumeState::Unknown;
            rec.atlasMeshID = kLumenMeshSDFAtlasInvalidID;
            rec.mipFloats.clear();
            rec.stagingBytes = 0;
            rec.gpuBytes = 0;
        }
        for (InstanceRecord& r : mInstances)
        {
            r.tableInstanceID = kLumenMeshSDFAtlasInvalidID;
            r.evicted = false;
        }
    }

    /// Re-materialize the registered scene after reload(): re-ensure every referenced
    /// mesh (disk cache) and re-add every instance + re-upload page data. Returns false
    /// on any failure.
    bool applyScene(std::string& err)
    {
        for (InstanceRecord& r : mInstances)
        {
            if (!r.active)
                continue;
            MeshRecord* rec = findMeshRecordByKey(r.meshKey);
            if (rec == nullptr)
                continue;
            uint32_t atlasMeshID = kLumenMeshSDFAtlasInvalidID;
            if (!ensureMesh(*rec, atlasMeshID, err))
                return false;
            if (rec->state != VolumeState::Uploaded)
                continue;
            LumenMeshSDFSceneInstanceDesc desc;
            desc.meshID = rec->atlasMeshID;
            desc.transform = r.transform;
            desc.layerMask = r.layerMask;
            const uint32_t tableID = mTable.addInstance(desc);
            if (tableID == kLumenMeshSDFAtlasInvalidID)
            {
                err = "applyScene: atlas addInstance failed for a scene instance";
                return false;
            }
            r.tableInstanceID = tableID;
            r.evicted = false;
            mTable.touchInstance(tableID);
            uploadMeshPages(*rec);
        }
        return true;
    }

    /// Full teardown: atlas + table + all registration + counters.
    void reset()
    {
        mTable.reset();
        mMeshes.clear();
        mInstances.clear();
        mFrame = 0;
        mBudgetBytes = 0;
        mCacheLookups = 0;
        mCacheHits = 0;
        mCacheMisses = 0;
        mCorruptions = 0;
        mBuilds = 0;
        mConversions = 0;
        mEvictions = 0;
        mRestores = 0;
    }

    // -- Stats / access ------------------------------------------------------

    /// Counts of registered meshes / active scene instances.
    uint32_t meshCount() const { return static_cast<uint32_t>(mMeshes.size()); }
    uint32_t instanceCount() const
    {
        uint32_t n = 0;
        for (const InstanceRecord& r : mInstances)
            if (r.active)
                ++n;
        return n;
    }

    /// Snapshot of all counters and derived state.
    LumenMeshSDFSceneStats getStats() const
    {
        LumenMeshSDFSceneStats s;
        s.registeredMeshes = static_cast<uint32_t>(mMeshes.size());
        for (const MeshRecord& rec : mMeshes)
            ++s.stateCounts[static_cast<size_t>(rec.state)];
        for (const InstanceRecord& r : mInstances)
        {
            if (!r.active)
                continue;
            ++s.activeInstances;
            if (r.evicted)
            {
                ++s.evictedInstances;
            }
            else if (mTable.isResident(r.tableInstanceID))
            {
                ++s.residentInstances;
            }
        }
        s.cacheLookups = mCacheLookups;
        s.cacheHits = mCacheHits;
        s.cacheMisses = mCacheMisses;
        s.corruptionsDetected = mCorruptions;
        s.builds = mBuilds;
        s.conversions = mConversions;
        s.evictions = mEvictions;
        s.restores = mRestores;
        s.estimatedGpuBytes = estimateGpuBytes();
        s.residentBytes = residentBytes();
        s.budgetBytes = mBudgetBytes;
        return s;
    }

    /// The underlying instance table + atlas (page tables / instance table / volumes
    /// upload; dirtyInstanceIDs etc.).
    LumenMeshSDFInstanceTable& instanceTable() { return mTable; }
    const LumenMeshSDFInstanceTable& instanceTable() const { return mTable; }

    /// The configured disk cache directory.
    const std::filesystem::path& cacheDirectory() const { return mCache.directory(); }

private:
    static constexpr size_t kNoMesh = SIZE_MAX;

    struct MeshRecord
    {
        std::string key;                              ///< S6-A2 disk cache key.
        LumenMeshSDFSceneMeshDesc desc;               ///< Registration input (kept across reload).
        VolumeState state = VolumeState::Unknown;
        uint32_t atlasMeshID = kLumenMeshSDFAtlasInvalidID;
        MSDFHeader header;                            ///< Retained after conversion (Loaded/Evicted).
        size_t gpuBytes = 0;                          ///< Standalone-volume GPU estimate.
        size_t stagingBytes = 0;                      ///< CPU bytes of mipFloats.
        std::vector<std::vector<float>> mipFloats;    ///< Held page data (re-upload after eviction).
        uint32_t refCount = 0;                        ///< Scene instances referencing this mesh.
        uint64_t lastUsedFrame = 0;
    };

    struct InstanceRecord
    {
        bool active = false;
        bool evicted = false;
        std::string meshKey;
        uint32_t atlasMeshID = kLumenMeshSDFAtlasInvalidID;
        uint32_t tableInstanceID = kLumenMeshSDFAtlasInvalidID;
        LumenMeshSDFAtlasInstanceDesc transform;
        uint32_t layerMask = kLumenMeshSDFLayerMaskAll;
    };

    // -- Mesh record helpers -------------------------------------------------

    size_t findMeshRecord(const std::string& key) const
    {
        for (size_t i = 0; i < mMeshes.size(); ++i)
            if (mMeshes[i].key == key)
                return i;
        return kNoMesh;
    }

    MeshRecord* findMeshRecordByKey(const std::string& key)
    {
        const size_t idx = findMeshRecord(key);
        return idx == kNoMesh ? nullptr : &mMeshes[idx];
    }

    size_t findOrCreateMeshRecord(const std::string& key, const LumenMeshSDFSceneMeshDesc& desc)
    {
        const size_t idx = findMeshRecord(key);
        if (idx != kNoMesh)
            return idx;
        MeshRecord rec;
        rec.key = key;
        rec.desc = desc;
        mMeshes.push_back(std::move(rec));
        return mMeshes.size() - 1u;
    }

    /// Disk query for one mesh record. Loaded volumes (Uploaded / Evicted) are
    /// authoritative in memory and not re-queried. A corrupt entry (file exists but
    /// findCached failed) is counted and reported as NeedsBuild - never served.
    VolumeState probe(MeshRecord& rec)
    {
        if (rec.state == VolumeState::Uploaded || rec.state == VolumeState::Evicted)
            return rec.state;
        ++mCacheLookups;
        MSDFParseResult parsed;
        std::string err;
        if (mCache.findCached(rec.key, parsed, err))
        {
            ++mCacheHits;
            rec.state = VolumeState::Cached;
        }
        else
        {
            ++mCacheMisses;
            if (mCache.exists(rec.key))
                ++mCorruptions;
            rec.state = VolumeState::NeedsBuild;
        }
        return rec.state;
    }

    /// Run the builder, validate the produced ".msdf" and atomically store it into the
    /// disk cache (NeedsBuild -> Cached). State becomes Error on any failure.
    bool buildMesh(MeshRecord& rec, std::string& err)
    {
        if (rec.state == VolumeState::Cached || rec.state == VolumeState::Uploaded)
            return true;
        rec.state = VolumeState::Building;

        std::string derr;
        if (!mCache.ensureDirectory(derr))
        {
            rec.state = VolumeState::Error;
            err = "cannot create cache directory: " + derr;
            return false;
        }
        const std::filesystem::path tmp = mCache.directory() / (rec.key + ".build.tmp");
        if (!mBuilder(rec.desc, tmp, err))
        {
            rec.state = VolumeState::Error;
            return false;
        }
        MSDFParseResult parsed;
        std::string verr;
        if (!Cache::findCached(tmp, parsed, verr))
        {
            ++mCorruptions;
            rec.state = VolumeState::Error;
            err = "builder produced an invalid .msdf volume: " + verr;
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
        if (!mCache.storeVolume(rec.key, parsed.header, parsed.distances, parsed.warnings, err))
        {
            rec.state = VolumeState::Error;
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        ++mBuilds;
        rec.state = VolumeState::Cached;
        return true;
    }

    /// The full data flow for one mesh (see the file header state machine): query the
    /// cache, build on miss, convert, register in the atlas and upload the pages.
    bool ensureMesh(MeshRecord& rec, uint32_t& atlasMeshID, std::string& err)
    {
        if (rec.state == VolumeState::Uploaded)
        {
            atlasMeshID = rec.atlasMeshID;
            return true;
        }
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            if (rec.state != VolumeState::Cached)
            {
                if (rec.state != VolumeState::NeedsBuild)
                    probe(rec);
                if (rec.state == VolumeState::NeedsBuild && !buildMesh(rec, err))
                    return false;
            }
            if (rec.state != VolumeState::Cached)
            {
                err = "mesh " + rec.key + " is not ready (state does not allow loading)";
                return false;
            }

            MSDFParseResult parsed;
            if (!mCache.findCached(rec.key, parsed, err))
            {
                // Corrupted between probe and load: rebuild once (bounded below).
                ++mCorruptions;
                rec.state = VolumeState::NeedsBuild;
                continue;
            }

            BuildParams bp;
            bp.quality = rec.desc.cacheParams.quality;
            bp.pooling = rec.desc.cacheParams.pooling;
            bp.meshContentHash = rec.desc.meshContentHash;

            LumenMeshSDFConvertedVolume cv;
            if (!mConverter(parsed, bp, cv, err))
            {
                rec.state = VolumeState::Error;
                return false;
            }
            if (!atlasValidateMeshDesc(cv.atlasMesh))
            {
                rec.state = VolumeState::Error;
                err = "converter produced an invalid atlas mesh desc";
                return false;
            }
            const uint32_t meshID = mTable.registerMesh(cv.atlasMesh);
            if (meshID == kLumenMeshSDFAtlasInvalidID)
            {
                rec.state = VolumeState::Error;
                err = "atlas registerMesh failed for mesh " + rec.key;
                return false;
            }
            for (uint32_t m = 0; m < cv.mipFloats.size(); ++m)
            {
                if (cv.mipFloats[m].empty())
                    continue;
                if (!mTable.atlas().uploadVolumeFloats(meshID, m, cv.mipFloats[m]))
                {
                    rec.state = VolumeState::Error;
                    err = "atlas uploadVolumeFloats failed for mesh " + rec.key + " mip " + std::to_string(m);
                    return false;
                }
            }

            rec.atlasMeshID = meshID;
            rec.header = parsed.header;
            rec.gpuBytes = cv.gpuBytes;
            rec.stagingBytes = cv.stagingBytes;
            rec.mipFloats = std::move(cv.mipFloats);
            rec.lastUsedFrame = mFrame;
            ++mConversions;
            rec.state = VolumeState::Uploaded;
            atlasMeshID = meshID;
            return true;
        }
        rec.state = VolumeState::Error;
        err = "mesh " + rec.key + ": too many rebuild attempts";
        return false;
    }

    /// Re-apply the held page data to the atlas (idempotent; required after page
    /// re-allocation - eviction/restore bumps the page generation).
    void uploadMeshPages(MeshRecord& rec)
    {
        for (uint32_t m = 0; m < rec.mipFloats.size(); ++m)
        {
            if (rec.mipFloats[m].empty())
                continue;
            mTable.atlas().uploadVolumeFloats(rec.atlasMeshID, m, rec.mipFloats[m]);
        }
    }

    // -- State ---------------------------------------------------------------

    LumenMeshSDFInstanceTable mTable;
    Cache::LumenMeshSDFDiskCache mCache;
    VolumeBuilder mBuilder;
    VolumeConverter mConverter;
    std::filesystem::path mBuilderPath;
    std::array<float, 3> mCamera = {0.f, 0.f, 0.f};
    uint64_t mFrame = 0;
    uint64_t mBudgetBytes = 0;
    std::vector<MeshRecord> mMeshes;
    std::vector<InstanceRecord> mInstances;

    // Stats counters.
    uint64_t mCacheLookups = 0;
    uint64_t mCacheHits = 0;
    uint64_t mCacheMisses = 0;
    uint64_t mCorruptions = 0;
    uint64_t mBuilds = 0;
    uint64_t mConversions = 0;
    uint64_t mEvictions = 0;
    uint64_t mRestores = 0;
};

} // namespace Scene
} // namespace MeshSDF
} // namespace LumenGI
