// =====================================================================================
//  LumenGI - S6-A2 Mesh SDF disk cache (CPU side, header-only)
//  -------------------------------------------------------------------------------------
//  A content-addressed, self-validating disk cache for builder output (".msdf"
//  volumes). The cache key is derived from the mesh geometry identity
//  (meshContentHash, Agent D's S6-A1 contract) plus the builder version, the
//  ".msdf" format version, the grid resolution and the compression knobs (quality
//  tier + mip pooling). Any change to those inputs changes the key, so a stale
//  builder or a re-tuned resolution never serves a stale volume.
//
//  SCOPE / CONSTRAINTS
//  -------------------------------------------------------------------------------------
//  * PURE C++17, header-only, standard library only. It includes LumenMeshSDF.h
//    (READ-ONLY) for the frozen ".msdf" constants / types (MSDFHeader,
//    MSDFParseResult, kMSDFMagic, kMSDFFormatVersion, kMSDFHeaderSize, Quality,
//    MipPooling) but does NOT call any symbol defined in LumenMeshSDF.cpp
//    (parseMSDFFile / msdfFNV1a64 / ...): the checksum + parse + serialize logic
//    below is self-contained and inline, so a TU that links only this header +
//    the standard library works. The inline FNV-1a64 is algorithm-identical to
//    Agent G's msdfFNV1a64 (distinct name avoids ODR collisions, same pattern as
//    LumenMeshSDFAtlas.h's atlasFNV1a64).
//  * No Falcor include, no CMake target (root pass integrates).
//  * Syntax check: cl /Zs /std:c++17 /EHsc LumenMeshSDFCache.h
//
//  CACHE KEY (frozen; see cacheKey() / the DISK CACHE KEY section below)
//  -------------------------------------------------------------------------------------
//  `cacheKey(meshHash, params)` hashes a canonical little-endian byte buffer with
//  FNV-1a64 and returns the lower-case 16-hex-digit string. The file name is
//  "<key>.msdf". builder version + format version are folded into the key, so:
//      * any change to the field-generation algorithm  -> bump
//        kLumenMeshSDFCacheBuilderVersion (invalidates everything).
//      * any change to the ".msdf" container format    -> bump kMSDFFormatVersion
//        (already in the key through kMSDFFormatVersion).
//
//  CORRUPTION DETECTION / REBUILD (S6-A2 contract)
//  -------------------------------------------------------------------------------------
//  findCached() parses + fully validates the file (magic / version / headerSize /
//  layout offsets / data count vs. resolution / FNV-1a64 checksum over
//  [0, checksumOffset)). A corrupted or missing entry returns false and the
//  caller re-builds and re-stores; a partially corrupt volume is never used.
//  The trailing checksum covers the whole file up to itself, so corruption in any
//  field (including magic / header) is detected.
//
//  ATOMIC STORE
//  -------------------------------------------------------------------------------------
//  store() writes to a temp file in the SAME directory as the target, flushes it,
//  then renames it over the target. On the same filesystem the rename is atomic:
//  readers never observe a partially written volume. std::filesystem::rename on
//  Windows fails when the destination already exists, so store() removes a stale
//  destination and retries the rename (a completed temp write always exists
//  before any removal, so the target is either the old or the new file - never a
//  partial write). The temp file is removed on failure.
//
//  LAYER / INSTANCE NOTES
//  -------------------------------------------------------------------------------------
//  The cache only stores .msdf bytes; atlas residency / instance mapping is the
//  job of LumenMeshSDFInstanceTable.h (S6-B2). The 96-byte volume descriptor is
//  rebuilt from the parsed header by Agent G's buildVolume() after a cache hit.
// =====================================================================================
#pragma once

#include "LumenMeshSDF.h" // READ-ONLY: MSDFHeader / MSDFParseResult / constants / enums

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace LumenGI
{
namespace MeshSDF
{
namespace Cache
{

// -------------------------------------------------------------------------------------
// Version constants (both fold into the cache key)
// -------------------------------------------------------------------------------------

/// Builder version: bump whenever the SDF field-generation algorithm changes in a
/// way that alters the produced ".msdf" bytes for identical inputs (e.g. the BVH
/// distance pass, parity micro-jitter, padding rules). Independent of
/// kMSDFFormatVersion (file container version). Frozen for this round: 1.
constexpr uint32_t kLumenMeshSDFCacheBuilderVersion = 1;

/// Default cache root when neither the caller nor the environment specifies one:
/// <current working directory>/LumenGI/MSDFCache. Overridable at construction.
inline std::filesystem::path defaultCacheDir()
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // std::getenv: MSVC deprecation; kept for portability (GCC/clang do not warn)
#endif
    const char* env = std::getenv("LUMEN_MSDF_CACHE_DIR");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (env != nullptr && env[0] != '\0')
        return std::filesystem::path(env);
    return std::filesystem::current_path() / "LumenGI" / "MSDFCache";
}

// -------------------------------------------------------------------------------------
// Cache key inputs
// -------------------------------------------------------------------------------------

/// Everything that changes the encoded ".msdf" bytes for a given mesh. meshHash
/// is passed separately to cacheKey().
struct LumenMeshSDFCacheParams
{
    std::array<uint32_t, 3> resolution = {0, 0, 0}; ///< mip0 voxel counts (x fastest).
    Quality quality = Quality::High;                ///< mip0 format tier (R16Float / R8Snorm).
    MipPooling pooling = MipPooling::MinAbs;        ///< coarse-mip pooling mode.
    /// Output-space grid bounds {minX,minY,minZ,maxX,maxY,maxZ}. All-zero / degenerate
    /// (any min >= max) means "not provided": the field-range guard byte is then hashed
    /// as 0. Provide this when the host varies padding or bbox overrides so that a grid
    /// change invalidates the entry (the geometry identity alone does not pin the grid).
    std::array<float, 6> gridBounds = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
};

// -------------------------------------------------------------------------------------
// Little-endian helpers (".msdf" is little-endian; inline, standard-library only)
// -------------------------------------------------------------------------------------

inline uint16_t msdfCacheReadLEU16(const uint8_t* p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline uint32_t msdfCacheReadLEU32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t msdfCacheReadLEU64(const uint8_t* p)
{
    return uint64_t(msdfCacheReadLEU32(p)) | (uint64_t(msdfCacheReadLEU32(p + 4)) << 32);
}

inline float msdfCacheReadLEF32(const uint8_t* p)
{
    uint32_t u = msdfCacheReadLEU32(p);
    float f = 0.f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline void msdfCacheWriteLEU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(uint8_t(v & 0xFFu));
    out.push_back(uint8_t((v >> 8) & 0xFFu));
}

inline void msdfCacheWriteLEU32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v & 0xFFu));
    out.push_back(uint8_t((v >> 8) & 0xFFu));
    out.push_back(uint8_t((v >> 16) & 0xFFu));
    out.push_back(uint8_t((v >> 24) & 0xFFu));
}

inline void msdfCacheWriteLEU64(std::vector<uint8_t>& out, uint64_t v)
{
    msdfCacheWriteLEU32(out, uint32_t(v & 0xFFFFFFFFull));
    msdfCacheWriteLEU32(out, uint32_t(v >> 32));
}

inline void msdfCacheWriteLEF32(std::vector<uint8_t>& out, float f)
{
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    msdfCacheWriteLEU32(out, u);
}

// -------------------------------------------------------------------------------------
// FNV-1a 64-bit (algorithm-identical to Agent G's msdfFNV1a64 / Agent D's fnv1a64;
// distinct name avoids ODR collisions when a TU also links LumenMeshSDF.cpp)
// -------------------------------------------------------------------------------------

inline uint64_t msdfCacheFNV1a64(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;
    constexpr uint64_t kPrime = 0x100000001b3ULL;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= kPrime;
    }
    return h;
}

/// Lower-case 16-digit hex of a 64-bit value (same style as the builder's
/// contentHashHex). Used as the cache file name stem.
inline std::string msdfCacheToHex(uint64_t value)
{
    constexpr char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i)
    {
        s[static_cast<size_t>(i)] = kDigits[value & 0xF];
        value >>= 4;
    }
    return s;
}

// -------------------------------------------------------------------------------------
// DISK CACHE KEY (frozen layout; LumenMeshSDFCacheTests pins it byte-for-byte)
// -------------------------------------------------------------------------------------
//    offset  size  field
//    0       8     meshContentHash           (u64 LE)
//    8       4     kLumenMeshSDFCacheBuilderVersion (u32 LE)
//    12      4     kMSDFFormatVersion        (u32 LE)
//    16      12    resolution[3]             (u32 LE each)
//    28      1     quality                   (u8; Quality::High=0 / Low=1)
//    29      1     pooling                   (u8; MipPooling::MinAbs=0 / Average=1)
//    30      1     mip0 format               (u8; VolumeFormat::R16Float=0 / R8Snorm=1)
//    31      4     rangeBound                (f32 LE; max(0.5*halfGridDiagonal, 1e-6);
//                                               0 when gridBounds are not provided)
//    ---------------------------------------------------------------
//    total 35 bytes -> FNV-1a64 -> 16 lowercase hex chars.
//
//  meshHash is the geometry identity (Agent D's meshContentHash). resolution +
//  quality + pooling are the "resolution / compression parameters" of the S6-A2
//  task card; rangeBound additionally guards against grid (bbox/padding) changes
//  that a geometry-identity hash alone would not pin.
inline std::string cacheKey(uint64_t meshHash, const LumenMeshSDFCacheParams& params)
{
    std::vector<uint8_t> buf;
    for (int i = 0; i < 8; ++i)
        buf.push_back(uint8_t((meshHash >> (8 * i)) & 0xFFull));
    msdfCacheWriteLEU32(buf, kLumenMeshSDFCacheBuilderVersion);
    msdfCacheWriteLEU32(buf, kMSDFFormatVersion);
    for (int i = 0; i < 3; ++i)
        msdfCacheWriteLEU32(buf, params.resolution[i]);
    buf.push_back(uint8_t(params.quality));
    buf.push_back(uint8_t(params.pooling));
    buf.push_back(uint8_t(params.quality == Quality::High ? uint32_t(VolumeFormat::R16Float)
                                                          : uint32_t(VolumeFormat::R8Snorm)));
    float rangeBound = 0.f;
    const bool boundsProvided = params.gridBounds[0] < params.gridBounds[3] &&
                                params.gridBounds[1] < params.gridBounds[4] &&
                                params.gridBounds[2] < params.gridBounds[5];
    if (boundsProvided)
    {
        const float dx = params.gridBounds[3] - params.gridBounds[0];
        const float dy = params.gridBounds[4] - params.gridBounds[1];
        const float dz = params.gridBounds[5] - params.gridBounds[2];
        rangeBound = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 1e-6f);
    }
    msdfCacheWriteLEF32(buf, rangeBound);
    return msdfCacheToHex(msdfCacheFNV1a64(buf.data(), buf.size()));
}

// -------------------------------------------------------------------------------------
// Grid validation (mirrors LumenMeshSDF.cpp's validGridHeader, inline here)
// -------------------------------------------------------------------------------------

inline bool msdfCacheValidGridHeader(const MSDFHeader& h)
{
    if (h.formatVersion != kMSDFFormatVersion)
        return false;
    for (int i = 0; i < 3; ++i)
    {
        if (h.resolution[i] < 2)
            return false;
        if (!(h.bboxMin[i] < h.bboxMax[i]) || !std::isfinite(h.bboxMin[i]) || !std::isfinite(h.bboxMax[i]))
            return false;
    }
    return h.voxelSize > 0.f && std::isfinite(h.voxelSize) && h.normalizationScale > 0.f &&
           std::isfinite(h.normalizationScale);
}

// -------------------------------------------------------------------------------------
// ".msdf" parse + checksum validation (inline mirror of LumenMeshSDF.cpp)
// -------------------------------------------------------------------------------------

/// Parse + fully validate ".msdf" bytes: magic / version / headerSize / layout
/// offsets / FNV-1a64 checksum over [0, checksumOffset) / data count vs.
/// resolution. Returns false and sets `err` on any corruption; a corrupted cache
/// entry must be rebuilt, never partially used (S6-A2 contract).
inline bool parseMSDFBytes(const uint8_t* data, size_t size, MSDFParseResult& out, std::string& err)
{
    out = MSDFParseResult{};

    if (size < kMSDFHeaderSize + 8)
    {
        err = "file too small to be a .msdf volume";
        return false;
    }
    if (std::memcmp(data, kMSDFMagic, 4) != 0)
    {
        err = "bad magic: not a .msdf file";
        return false;
    }
    const uint32_t version = msdfCacheReadLEU32(data + 4);
    const uint32_t headerSize = msdfCacheReadLEU32(data + 8);
    if (version != kMSDFFormatVersion)
    {
        err = "unsupported format version " + std::to_string(version);
        return false;
    }
    if (headerSize != kMSDFHeaderSize)
    {
        err = "unexpected header size " + std::to_string(headerSize);
        return false;
    }

    MSDFHeader& h = out.header;
    h.formatVersion = version;
    for (int i = 0; i < 3; ++i)
        h.resolution[i] = msdfCacheReadLEU32(data + 12 + 4u * uint32_t(i));
    for (int i = 0; i < 3; ++i)
        h.bboxMin[i] = msdfCacheReadLEF32(data + 24 + 4u * uint32_t(i));
    for (int i = 0; i < 3; ++i)
        h.bboxMax[i] = msdfCacheReadLEF32(data + 36 + 4u * uint32_t(i));
    h.voxelSize = msdfCacheReadLEF32(data + 48);
    h.normalizationScale = msdfCacheReadLEF32(data + 52);
    h.paddingWorld = msdfCacheReadLEF32(data + 56);
    h.signConvention = data[60]; // u8 in the file layout (offset 60), stored as u32 in MSDFHeader.
    h.signReliable = data[61] != 0;
    const uint16_t warningCount = msdfCacheReadLEU16(data + 62);
    const uint64_t dataOffset = msdfCacheReadLEU64(data + 64);
    const uint64_t dataCount = msdfCacheReadLEU64(data + 72);
    const uint64_t checksumOffset = msdfCacheReadLEU64(data + 80);
    h.dataCount = dataCount;

    if (!msdfCacheValidGridHeader(h))
    {
        err = "corrupt grid parameters in header";
        return false;
    }
    const uint64_t expectedCount = uint64_t(h.resolution[0]) * uint64_t(h.resolution[1]) * uint64_t(h.resolution[2]);
    if (dataCount != expectedCount)
    {
        err = "data count does not match resolution";
        return false;
    }
    if (dataOffset < kMSDFHeaderSize || checksumOffset != dataOffset + dataCount * 4 ||
        checksumOffset + 8 != uint64_t(size))
    {
        err = "corrupt file layout (offsets do not line up)";
        return false;
    }
    const uint64_t storedChecksum = msdfCacheReadLEU64(data + checksumOffset);
    const uint64_t computedChecksum = msdfCacheFNV1a64(data, size_t(checksumOffset));
    if (storedChecksum != computedChecksum)
    {
        err = "checksum mismatch: file is corrupted";
        return false;
    }

    out.warnings.clear();
    size_t pos = size_t(kMSDFHeaderSize);
    for (uint16_t i = 0; i < warningCount; ++i)
    {
        if (pos + 2 > size_t(dataOffset))
        {
            err = "corrupt warnings section";
            return false;
        }
        const uint16_t len = msdfCacheReadLEU16(data + pos);
        pos += 2;
        if (pos + len > size_t(dataOffset))
        {
            err = "corrupt warnings section";
            return false;
        }
        out.warnings.emplace_back(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
    }

    out.distances.resize(size_t(dataCount));
    std::memcpy(out.distances.data(), data + size_t(dataOffset), out.distances.size() * sizeof(float));
    return true;
}

/// Serialize header + distances + warnings into ".msdf" bytes, byte-identical to
/// Agent D's writeMSDF / Agent G's serializeMSDFBytes (offset table in
/// LumenMeshSDF.h). Used by storeVolume() and by tests to synthesize files.
inline bool serializeMSDFBytes(
    const MSDFHeader& header,
    const std::vector<float>& distances,
    const std::vector<std::string>& warnings,
    std::vector<uint8_t>& outBytes,
    std::string& err
)
{
    outBytes.clear();
    if (!msdfCacheValidGridHeader(header))
    {
        err = "invalid header (bad version/resolution/bbox/voxelSize)";
        return false;
    }
    const uint64_t expectedCount = uint64_t(header.resolution[0]) * uint64_t(header.resolution[1]) *
                                   uint64_t(header.resolution[2]);
    if (distances.size() != size_t(expectedCount))
    {
        err = "distance count does not match resolution";
        return false;
    }
    if (warnings.size() > 65535)
    {
        err = "too many warnings to serialize";
        return false;
    }
    for (const std::string& w : warnings)
    {
        if (w.size() > 65535)
        {
            err = "warning message too long to serialize";
            return false;
        }
    }

    outBytes.push_back(uint8_t(kMSDFMagic[0]));
    outBytes.push_back(uint8_t(kMSDFMagic[1]));
    outBytes.push_back(uint8_t(kMSDFMagic[2]));
    outBytes.push_back(uint8_t(kMSDFMagic[3]));
    msdfCacheWriteLEU32(outBytes, kMSDFFormatVersion);
    msdfCacheWriteLEU32(outBytes, kMSDFHeaderSize);
    for (int i = 0; i < 3; ++i)
        msdfCacheWriteLEU32(outBytes, header.resolution[i]);
    for (int i = 0; i < 3; ++i)
        msdfCacheWriteLEF32(outBytes, header.bboxMin[i]);
    for (int i = 0; i < 3; ++i)
        msdfCacheWriteLEF32(outBytes, header.bboxMax[i]);
    msdfCacheWriteLEF32(outBytes, header.voxelSize);
    msdfCacheWriteLEF32(outBytes, header.normalizationScale);
    msdfCacheWriteLEF32(outBytes, header.paddingWorld);
    outBytes.push_back(uint8_t(header.signConvention));
    outBytes.push_back(header.signReliable ? 1 : 0);
    msdfCacheWriteLEU16(outBytes, uint16_t(warnings.size()));

    uint64_t warnBytes = 0;
    for (const std::string& w : warnings)
        warnBytes += 2 + w.size();
    const uint64_t dataOffset = uint64_t(kMSDFHeaderSize) + ((warnBytes + 7) & ~uint64_t(7));
    const uint64_t dataCount = uint64_t(distances.size());
    const uint64_t checksumOffset = dataOffset + dataCount * 4;

    msdfCacheWriteLEU64(outBytes, dataOffset);
    msdfCacheWriteLEU64(outBytes, dataCount);
    msdfCacheWriteLEU64(outBytes, checksumOffset);

    for (const std::string& w : warnings)
    {
        msdfCacheWriteLEU16(outBytes, uint16_t(w.size()));
        outBytes.insert(outBytes.end(), w.begin(), w.end());
    }
    while (outBytes.size() < size_t(dataOffset))
        outBytes.push_back(0);
    for (float d : distances)
        msdfCacheWriteLEF32(outBytes, d);

    msdfCacheWriteLEU64(outBytes, msdfCacheFNV1a64(outBytes.data(), outBytes.size()));
    return true;
}

// -------------------------------------------------------------------------------------
// Free-function API (task card: findCached(path, out) / store(path, bytes))
// -------------------------------------------------------------------------------------

/// Read + fully validate a cached ".msdf" file. Returns false (and sets `err`)
/// on a missing file OR any corruption; the caller must rebuild and re-store.
inline bool findCached(const std::filesystem::path& path, MSDFParseResult& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err = "cache miss: cannot open " + path.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff fileSize = in.tellg();
    in.seekg(0, std::ios::beg);
    if (fileSize < 0)
    {
        err = "cannot determine file size: " + path.string();
        return false;
    }
    std::vector<uint8_t> buf;
    buf.resize(size_t(fileSize));
    if (fileSize > 0)
        in.read(reinterpret_cast<char*>(buf.data()), fileSize);
    if (!in)
    {
        err = "failed reading file: " + path.string();
        return false;
    }
    return Cache::parseMSDFBytes(buf.data(), buf.size(), out, err);
}

/// Atomic store of serialized ".msdf" bytes (see file header for the write
/// protocol). Creates the parent directory if needed. Returns false and sets
/// `err` on failure (the target file, if any, is left untouched on failure).
inline bool store(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string& err)
{
    const std::filesystem::path dir = path.parent_path();
    if (!dir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            err = "cannot create cache directory: " + dir.string();
            return false;
        }
    }

    // Temp file in the SAME directory as the target so rename stays on one volume.
    static std::atomic<uint64_t> sTempCounter{0};
    const std::string tmpName =
        path.filename().string() + ".tmp" + std::to_string(++sTempCounter);
    const std::filesystem::path tmp = dir / tmpName;

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err = "cannot open temp file: " + tmp.string();
            return false;
        }
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.flush();
        if (!out)
        {
            err = "failed writing temp file: " + tmp.string();
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        // Windows: rename fails when the destination exists; remove a stale target
        // and retry. The temp file is fully written before any removal, so the
        // target is always either the old or the new volume - never a partial write.
        ec.clear();
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec)
    {
        err = "rename failed: " + path.string();
        std::error_code ignore;
        std::filesystem::remove(tmp, ignore);
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------------------
// Directory-managing wrapper (default path parameterized; LUMEN_MSDF_CACHE_DIR or
// <cwd>/LumenGI/MSDFCache). Not thread-safe, single render-loop thread.
// -------------------------------------------------------------------------------------

class LumenMeshSDFDiskCache
{
public:
    explicit LumenMeshSDFDiskCache(std::filesystem::path directory = defaultCacheDir())
        : mDirectory(std::move(directory))
    {
    }

    /// The configured cache directory.
    const std::filesystem::path& directory() const { return mDirectory; }

    /// Default directory: $LUMEN_MSDF_CACHE_DIR, else <cwd>/LumenGI/MSDFCache.
    static std::filesystem::path defaultCacheDirectory() { return defaultCacheDir(); }

    /// Create the cache directory (and parents). Returns false on failure.
    bool ensureDirectory(std::string& err)
    {
        std::error_code ec;
        std::filesystem::create_directories(mDirectory, ec);
        if (ec)
        {
            err = "cannot create cache directory: " + mDirectory.string();
            return false;
        }
        return true;
    }

    /// Cache key for a mesh (== free function cacheKey).
    static std::string keyFor(uint64_t meshHash, const LumenMeshSDFCacheParams& params)
    {
        return cacheKey(meshHash, params);
    }

    /// Absolute path of the file backing `key`: <dir>/<key>.msdf.
    std::filesystem::path pathFor(const std::string& key) const { return mDirectory / (key + ".msdf"); }

    /// Absolute path for a mesh (convenience: keyFor + pathFor).
    std::filesystem::path pathFor(uint64_t meshHash, const LumenMeshSDFCacheParams& params) const
    {
        return pathFor(cacheKey(meshHash, params));
    }

    /// Presence check only (does NOT validate contents; use findCached for that).
    bool exists(const std::string& key) const
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(pathFor(key), ec);
    }

    /// Load + validate a cached volume. False on miss OR corruption -> rebuild.
    bool findCached(const std::string& key, MSDFParseResult& out, std::string& err) const
    {
        return Cache::findCached(pathFor(key), out, err);
    }

    /// Atomic store of already-serialized ".msdf" bytes under `key`.
    bool store(const std::string& key, const std::vector<uint8_t>& bytes, std::string& err)
    {
        return Cache::store(pathFor(key), bytes, err);
    }

    /// Serialize + atomically store a volume (header + distances + warnings).
    bool storeVolume(
        const std::string& key,
        const MSDFHeader& header,
        const std::vector<float>& distances,
        const std::vector<std::string>& warnings,
        std::string& err
    )
    {
        std::vector<uint8_t> bytes;
        if (!Cache::serializeMSDFBytes(header, distances, warnings, bytes, err))
            return false;
        return Cache::store(pathFor(key), bytes, err);
    }

    /// Remove a cached entry. Returns false when the key was not cached.
    bool remove(const std::string& key, std::string& err)
    {
        std::error_code ec;
        const bool removed = std::filesystem::remove(pathFor(key), ec);
        if (ec)
        {
            err = "remove failed: " + pathFor(key).string();
            return false;
        }
        if (!removed)
        {
            err = "cache entry not found: " + pathFor(key).string();
            return false;
        }
        return true;
    }

private:
    std::filesystem::path mDirectory;
};

} // namespace Cache
} // namespace MeshSDF
} // namespace LumenGI
