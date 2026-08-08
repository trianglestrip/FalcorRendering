// =====================================================================================
//  LumenGI - S6-B1 Mesh SDF runtime (CPU side)
//  -------------------------------------------------------------------------------------
//  Consumes the ".msdf" files produced by the S6-A1 Mesh SDF builder
//  (Source/Tools/MeshSDFBuilder, Agent D) and produces:
//    * GPU creation parameters for the volume (formats, dims, mip count, bytes)
//    * CPU-encoded per-mip data ready to upload into Texture3D resources
//    * quantization error statistics (max / mean / RMS)
//    * the disk-cache key seed for S6-A2
//
//  SCOPE / CONSTRAINTS
//  -------------------------------------------------------------------------------------
//  * PURE C++17, no Falcor includes, no CMake target (root pass integrates).
//    Syntax check: cl /Zs /std:c++17 /EHsc LumenMeshSDF.cpp
//  * The GPU-side contract lives in LumenMeshSDFData.slang / LumenMeshSDFSampling.slang
//    (same directory). LumenMeshSDFVolumeDescriptor below is the byte-identical
//    mirror of the Slang struct LumenMeshSDFVolumeDescriptor (96 bytes); the root
//    pass memcpys it into a constant buffer.
//
//  THE ".msdf" FILE LAYOUT (Agent D contract; offsets are frozen, little-endian)
//  -------------------------------------------------------------------------------------
//    Offset  Size  Field
//    0       4     magic "MSDF"
//    4       4     formatVersion = 1 (u32)
//    8       4     headerSize = 88 (u32)
//    12      12    resolution nx, ny, nz (u32 x3; x fastest)
//    24      24    gridBBoxMin xyz, gridBBoxMax xyz (f32 x6, OUTPUT space)
//    48      4     voxelSize (f32, output space)
//    52      4     normalizationScale (f32; world = output / scale)
//    56      4     paddingWorld (f32, world units)
//    60      1     signConvention (u8; 0 = positive outside)
//    61      1     signReliable (u8)
//    62      2     warningCount (u16)
//    64      8     dataOffset (u64)
//    72      8     dataCount (u64; == nx*ny*nz)
//    80      8     checksumOffset (u64)
//    88      ...   warnings: warningCount x (u16 length + UTF-8 bytes)
//    ...          distance data: dataCount x f32, x fastest
//    end          checksum: u64 FNV-1a64 over file bytes [0, checksumOffset)
//
//  Voxel centers are at bboxMin + (i + 0.5) * voxelSize (output space).
//  World <-> output: p_out = p_world * normalizationScale; distances scale the
//  same way, i.e. d_world = d_out / normalizationScale (the builder writes
//  normalized output when normalize == true, so scale is usually < 1).
//
//  FROZEN GPU LAYOUT (S6-B1; see LumenMeshSDFData.slang for the full argument)
//  -------------------------------------------------------------------------------------
//  * mip 0 (finest): R16Float (High quality) or R8Snorm (Low quality).
//    R16Float: CPU encodes f32 -> f16 (round-to-nearest-even); GPU Load converts
//    back, relative error <= 2^-11 ~ 4.88e-4.
//  * mips 1..mipCount-1: R8Snorm with per-volume scalar scale R (= quantRange,
//    max |d| over the mip0 field). Encode code = clamp(round(d / R * 127), -127, 127);
//    decode d = max(int8(code) / 127, -1) * R. Absolute error <= R / 127.
//  * Mip dims: mip[0] = resolution; mip[m] = max(1, ceil(mip[m-1] / 2)).
//  * Mip data is generated on the CPU with min-abs pooling (frozen default:
//    pooled |d| is a lower bound on every child |d| at voxel centers, i.e. a
//    conservative step bound) or average pooling (quality mode).
//  * Data is stored x-fastest (row-major xyz), texel (x,y,z) == voxel (x,y,z),
//    ready for Texture::createTexture3D + uploadTextureData (1 level per texture,
//    one texture object per mip because the formats differ across mips).
//
//  HOOKS FOR LATER STAGES
//  -------------------------------------------------------------------------------------
//  * S6-A2 disk cache: the .msdf file does NOT contain meshContentHash (it lives
//    in the builder's JSON report). The host supplies it via BuildParams; the
//    cache key seed is computeCacheKey(header, params) - FNV-1a64 over
//    { contentHash, formatVersion, resolution, quality, pooling, quantRange }.
//  * S6-B2 atlas: descriptor.atlasPage + kLumenMeshSDFFlagAtlasResident are
//    consumed by the atlas host. World<->voxel math is unchanged by the atlas.
// =====================================================================================
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace LumenGI
{
namespace MeshSDF
{

// -------------------------------------------------------------------------------------
// Frozen constants (mirror LumenMeshSDFData.slang; keep both in sync)
// -------------------------------------------------------------------------------------

/// ".msdf" container constants (Agent D contract).
constexpr uint8_t kMSDFMagic[4] = {0x4Du, 0x53u, 0x44u, 0x46u}; ///< "MSDF"
constexpr uint32_t kMSDFFormatVersion = 1;
constexpr uint32_t kMSDFHeaderSize = 88;

/// Per-mip volume formats (values frozen; mirrored in Slang).
enum class VolumeFormat : uint8_t
{
    R16Float = 0, ///< mip 0 in High quality; CPU-encoded f16, relative error <= 2^-11.
    R8Snorm = 1,  ///< mips >= 1 (and mip 0 in Low quality); scaled by quantRange.
};
constexpr size_t kBytesPerVoxelR16Float = 2;
constexpr size_t kBytesPerVoxelR8Snorm = 1;

/// Mip pooling modes (values frozen; mirrored in Slang).
enum class MipPooling : uint8_t
{
    MinAbs = 0,  ///< Frozen default: conservative lower-bound pooling.
    Average = 1, ///< Quality mode: signed average.
};

/// Quality tier (frozen; mirrored in Slang flag kLumenMeshSDFFlagQualityHigh).
enum class Quality : uint8_t
{
    High = 0, ///< mip 0 = R16Float (2 B/voxel).
    Low = 1,  ///< mip 0 = R8Snorm (1 B/voxel).
};

constexpr uint32_t kLumenMeshSDFMaxMipCount = 12; ///< Covers grids up to 2048^3.
constexpr uint32_t kLumenMeshSDFNotResident = 0xFFFFFFFFu;
constexpr uint32_t kLumenMeshSDFSignConventionPositiveOutside = 0;

/// Descriptor flag bits (descriptor.flags; mirror Slang kLumenMeshSDFFlag*).
constexpr uint32_t kLumenMeshSDFFlagSignReliable = 1u << 0;
constexpr uint32_t kLumenMeshSDFFlagQualityHigh = 1u << 1;
constexpr uint32_t kLumenMeshSDFFlagPoolMinAbs = 1u << 2;
constexpr uint32_t kLumenMeshSDFFlagAtlasResident = 1u << 3;

// -------------------------------------------------------------------------------------
// ".msdf" parser types
// -------------------------------------------------------------------------------------

/// In-memory mirror of the ".msdf" header (layout above).
struct MSDFHeader
{
    uint32_t formatVersion = 0;
    std::array<uint32_t, 3> resolution = {0, 0, 0};
    std::array<float, 3> bboxMin = {0.f, 0.f, 0.f};
    std::array<float, 3> bboxMax = {0.f, 0.f, 0.f};
    float voxelSize = 0.f;
    float normalizationScale = 1.f;
    float paddingWorld = 0.f;
    uint32_t signConvention = kLumenMeshSDFSignConventionPositiveOutside;
    uint32_t signReliable = 0;
    uint64_t dataCount = 0;
};

struct MSDFParseResult
{
    MSDFHeader header;
    std::vector<float> distances;     ///< nx*ny*nz f32, x fastest.
    std::vector<std::string> warnings; ///< Warning texts (codes are not persisted).
};

/// FNV-1a 64-bit over raw bytes. ALGORITHM-IDENTICAL to Agent D's
/// MeshSDFBuilder::fnv1a64 (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
/// Named differently on purpose: if a test links this file together with
/// MeshSDFBuilder.cpp, the two fnv1a64 symbols would collide (ODR).
uint64_t msdfFNV1a64(const void* data, size_t size);

/// Parse + fully validate a ".msdf" file (magic / version / headerSize / layout
/// offsets / FNV-1a64 checksum over [0, checksumOffset) / data count vs. resolution).
/// Returns false and sets `err` on any corruption; a corrupted cache entry must
/// be rebuilt, never partially used (S6-A2 contract).
bool parseMSDFFile(const std::filesystem::path& path, MSDFParseResult& out, std::string& err);

/// In-memory variant (used by tests and cache validators).
bool parseMSDFBytes(const uint8_t* data, size_t size, MSDFParseResult& out, std::string& err);

/// Serialize header + distances + warnings into ".msdf" bytes, byte-identical to
/// MeshSDFBuilder::writeMSDF (used by tests to synthesize files and by cache
/// verification round-trips). Returns false and sets `err` on invalid input.
bool serializeMSDFBytes(
    const MSDFHeader& header,
    const std::vector<float>& distances,
    const std::vector<std::string>& warnings,
    std::vector<uint8_t>& outBytes,
    std::string& err
);

// -------------------------------------------------------------------------------------
// GPU descriptor + layout (the S6-B1 core contract)
// -------------------------------------------------------------------------------------

/// Byte-identical to `LumenMeshSDFVolumeDescriptor` in LumenMeshSDFData.slang
/// (96 bytes, packed; Slang scalar layout == C layout for these members).
/// The root pass memcpys this into a constant buffer and creates the textures
/// from VolumeLayout.
struct LumenMeshSDFVolumeDescriptor
{
    uint32_t resolution[3];              // +0  mip0 voxel counts (x fastest)
    uint32_t formatMip0;                 // +12 kLumenMeshSDFFormatR16Float / R8Snorm
    uint32_t formatCoarse;               // +16 kLumenMeshSDFFormatR8Snorm (frozen)
    uint32_t mipCount;                   // +20 1..kLumenMeshSDFMaxMipCount
    float bboxMin[3];                    // +24 output-space grid min
    float bboxMax[3];                    // +36 output-space grid max
    float voxelSize;                     // +48 output-space voxel size (uniform)
    float invVoxelSize;                  // +52 1 / voxelSize
    float normalizationScale;            // +56 world = output / normalizationScale
    float invNormalizationScale;         // +60 1 / normalizationScale
    float quantRange;                    // +64 R: snorm scale (max |d| over mip0 data)
    uint32_t signConvention;             // +68 from .msdf (0 = positive outside)
    uint32_t signReliable;               // +72 from .msdf (0 = unsupported field)
    float paddingWorld;                  // +76 informational (world units)
    uint32_t atlasPage;                  // +80 S6-B2 atlas page or kLumenMeshSDFNotResident
    uint32_t contentHashLo;              // +84 S6-A2 meshContentHash low 32
    uint32_t contentHashHi;              // +88 S6-A2 meshContentHash high 32
    uint32_t flags;                      // +92 kLumenMeshSDFFlag* bitfield
};
static_assert(sizeof(LumenMeshSDFVolumeDescriptor) == 96, "GPU descriptor must be 96 bytes");
static_assert(offsetof(LumenMeshSDFVolumeDescriptor, flags) == 92, "flags offset frozen");

/// One GPU texture's creation parameters.
struct MipSpec
{
    std::array<uint32_t, 3> dims = {0, 0, 0}; ///< max(1, ceil(prevDims / 2)); [0] = resolution.
    VolumeFormat format = VolumeFormat::R8Snorm;
    size_t voxelCount = 0;   ///< dims[0]*dims[1]*dims[2]
    size_t byteSize = 0;     ///< voxelCount * bytesPerVoxel(format)
};

/// CPU-side volume layout: per-mip GPU creation parameters + total memory.
struct VolumeLayout
{
    uint32_t mipCount = 0;
    std::array<MipSpec, kLumenMeshSDFMaxMipCount> mips;

    /// Pure layout computation from the ".msdf" header (frozen formulas).
    /// \param mip0Format R16Float (High) or R8Snorm (Low).
    static VolumeLayout compute(const MSDFHeader& header, VolumeFormat mip0Format);
};

/// Per-mip quantization error statistics (output-space units).
struct QuantizationError
{
    float maxAbsError = 0.f;      ///< max |decoded - original|
    float meanAbsError = 0.f;     ///< mean |decoded - original|
    float rmsError = 0.f;         ///< sqrt(mean((decoded - original)^2))
    float maxErrorInVoxels = 0.f; ///< maxAbsError / header.voxelSize
};

/// Build options. All fields participate in the S6-A2 cache key.
struct BuildParams
{
    Quality quality = Quality::High; ///< mip0 format selection.
    MipPooling pooling = MipPooling::MinAbs; ///< Frozen default: conservative.
    uint64_t meshContentHash = 0;    ///< From the builder's JSON report (S6-A2).
};

/// Full runtime conversion result.
struct BuildResult
{
    LumenMeshSDFVolumeDescriptor desc;
    VolumeLayout layout;
    /// Encoded mip data, ready to upload: mipData[m].size() == layout.mips[m].byteSize.
    /// R16Float mips are little-endian f16 bits; R8Snorm mips are int8 codes.
    std::vector<std::vector<uint8_t>> mipData;
    /// Per-mip quantization errors (mip0 is the quality-critical one).
    std::vector<QuantizationError> errors;
};

// -------------------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------------------

/// Convert a parsed ".msdf" volume into the GPU layout:
///  1. validate finite distances
///  2. compute the volume descriptor (formats / dims / mip count / quantRange)
///  3. generate mip levels (min-abs or average pooling)
///  4. quantize every mip (f16 for R16Float mips, snorm8 for R8Snorm mips)
///  5. estimate per-mip quantization errors
/// On failure returns false and sets `err`.
bool buildVolume(
    const MSDFHeader& header,
    const std::vector<float>& distances,
    const BuildParams& params,
    BuildResult& out,
    std::string& err
);

/// Pure memory estimate for a volume in the given quality tier (bytes).
size_t estimateMemoryBytes(const MSDFHeader& header, Quality quality);

/// S6-A2 cache key seed: FNV-1a64 over the geometry identity + build parameters
/// that affect the encoded bytes. Any change to these inputs must invalidate.
uint64_t computeCacheKey(const MSDFHeader& header, const BuildParams& params);

// -------------------------------------------------------------------------------------
// Quantization primitives (exposed for tests; formulas mirror the Slang side)
// -------------------------------------------------------------------------------------

/// f32 -> f16 bits, round-to-nearest-even (ties to even). NaN/Inf pass through.
uint16_t f32ToF16Bits(float value);
float f16BitsToF32(uint16_t bits);

/// code = clamp(round(v / range * 127), -127, 127) as int8.
int8_t encodeSnorm8(float value, float range);
/// decoded = max(int8(code) / 127, -1) * range  (DXGI R8_SNORM semantics).
float decodeSnorm8(int8_t code, float range);

/// CPU mirror of lumenMeshSDFSampleStored in LumenMeshSDFSampling.slang:
/// manual trilinear with clamped cells over a decoded float mip image
/// (kept in sync with the Slang file; used by tests and S6-B2 validation).
float sampleTrilinearCPU(
    const std::array<uint32_t, 3>& dims,
    const std::vector<float>& mipData, ///< decoded floats, x fastest
    const float voxel[3]               ///< continuous voxel coords on this mip
);

/// CPU mirror of lumenMeshSDFWorldToVoxel (mip0 continuous coords).
std::array<float, 3> worldToVoxelCPU(
    const LumenMeshSDFVolumeDescriptor& desc,
    const float worldPos[3]
);

/// Mip pooling (also used by S6-B2 for atlas-side validation): pools one mip
/// level from the previous one. Child cell of mip texel t is [2t, 2t+1] clipped
/// to the parent dims (ceil-halving; only existing children are pooled).
/// MinAbs: signed value of the child with min |d|. Average: signed mean.
std::vector<float> poolMipLevel(
    const std::vector<float>& src,
    const std::array<uint32_t, 3>& dimsPrev,
    const std::array<uint32_t, 3>& dimsCur,
    MipPooling mode
);

} // namespace MeshSDF
} // namespace LumenGI
