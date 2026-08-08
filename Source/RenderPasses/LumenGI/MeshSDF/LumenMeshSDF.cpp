// =====================================================================================
//  LumenMeshSDF.cpp - S6-B1 Mesh SDF runtime, CPU side (see LumenMeshSDF.h)
// =====================================================================================

#include "LumenMeshSDF.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace LumenGI
{
namespace MeshSDF
{

namespace
{

// -------------------------------------------------------------------------------------
// Little-endian helpers (".msdf" is little-endian)
// -------------------------------------------------------------------------------------

inline uint16_t readLEU16(const uint8_t* p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline uint32_t readLEU32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t readLEU64(const uint8_t* p)
{
    return uint64_t(readLEU32(p)) | (uint64_t(readLEU32(p + 4)) << 32);
}

inline float readLEF32(const uint8_t* p)
{
    uint32_t u = readLEU32(p);
    float f = 0.f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline void writeLEU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(uint8_t(v & 0xFFu));
    out.push_back(uint8_t((v >> 8) & 0xFFu));
}

inline void writeLEU32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v & 0xFFu));
    out.push_back(uint8_t((v >> 8) & 0xFFu));
    out.push_back(uint8_t((v >> 16) & 0xFFu));
    out.push_back(uint8_t((v >> 24) & 0xFFu));
}

inline void writeLEU64(std::vector<uint8_t>& out, uint64_t v)
{
    writeLEU32(out, uint32_t(v & 0xFFFFFFFFull));
    writeLEU32(out, uint32_t(v >> 32));
}

inline void writeLEF32(std::vector<uint8_t>& out, float f)
{
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    writeLEU32(out, u);
}

bool validGridHeader(const MSDFHeader& h)
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

size_t mipByteSize(const MipSpec& s)
{
    const size_t bpp =
        s.format == VolumeFormat::R16Float ? kBytesPerVoxelR16Float : kBytesPerVoxelR8Snorm;
    return s.voxelCount * bpp;
}

} // namespace

// -------------------------------------------------------------------------------------
// Mip pooling (public; shared with S6-B2)
// -------------------------------------------------------------------------------------

std::vector<float> poolMipLevel(
    const std::vector<float>& src,
    const std::array<uint32_t, 3>& dimsPrev,
    const std::array<uint32_t, 3>& dimsCur,
    MipPooling mode
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

                float acc = 0.f;
                float bestAbs = 3.4e38f;
                float bestVal = 0.f;
                size_t count = 0;
                for (uint32_t z = zLo; z <= zHi; ++z)
                    for (uint32_t y = yLo; y <= yHi; ++y)
                        for (uint32_t x = xLo; x <= xHi; ++x)
                        {
                            const float v = src[(size_t(z) * dimsPrev[1] + y) * dimsPrev[0] + x];
                            acc += v;
                            const float a = std::fabs(v);
                            if (a < bestAbs)
                            {
                                bestAbs = a;
                                bestVal = v;
                            }
                            ++count;
                        }
                out[(size_t(tz) * dimsCur[1] + ty) * dimsCur[0] + tx] =
                    (mode == MipPooling::MinAbs) ? bestVal : acc / float(count);
            }
        }
    }
    return out;
}

void quantizeMip(std::vector<uint8_t>& bytes, const std::vector<float>& data, VolumeFormat format, float range)
{
    bytes.clear();
    bytes.reserve(data.size() * (format == VolumeFormat::R16Float ? 2u : 1u));
    for (float v : data)
    {
        if (format == VolumeFormat::R16Float)
        {
            writeLEU16(bytes, f32ToF16Bits(v));
        }
        else
        {
            bytes.push_back(uint8_t(int8_t(encodeSnorm8(v, range))));
        }
    }
}

QuantizationError estimateMipError(const std::vector<float>& data, VolumeFormat format, float range, float voxelSize)
{
    QuantizationError e;
    if (data.empty())
        return e;
    double sumAbs = 0.0;
    double sumSq = 0.0;
    for (float v : data)
    {
        const float decoded = (format == VolumeFormat::R16Float)
            ? f16BitsToF32(f32ToF16Bits(v))
            : decodeSnorm8(encodeSnorm8(v, range), range);
        const float err = std::fabs(decoded - v);
        e.maxAbsError = std::max(e.maxAbsError, err);
        sumAbs += err;
        sumSq += double(err) * double(err);
    }
    e.meanAbsError = float(sumAbs / double(data.size()));
    e.rmsError = float(std::sqrt(sumSq / double(data.size())));
    e.maxErrorInVoxels = e.maxAbsError / voxelSize;
    return e;
}

// -------------------------------------------------------------------------------------
// FNV-1a 64-bit (algorithm-identical to Agent D's MeshSDFBuilder::fnv1a64)
// -------------------------------------------------------------------------------------

uint64_t msdfFNV1a64(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// -------------------------------------------------------------------------------------
// ".msdf" parser (mirrors Agent D's readMSDF validation, see file header for layout)
// -------------------------------------------------------------------------------------

bool parseMSDFBytes(const uint8_t* data, size_t size, MSDFParseResult& out, std::string& err)
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
    const uint32_t version = readLEU32(data + 4);
    const uint32_t headerSize = readLEU32(data + 8);
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
        h.resolution[i] = readLEU32(data + 12 + 4u * uint32_t(i));
    for (int i = 0; i < 3; ++i)
        h.bboxMin[i] = readLEF32(data + 24 + 4u * uint32_t(i));
    for (int i = 0; i < 3; ++i)
        h.bboxMax[i] = readLEF32(data + 36 + 4u * uint32_t(i));
    h.voxelSize = readLEF32(data + 48);
    h.normalizationScale = readLEF32(data + 52);
    h.paddingWorld = readLEF32(data + 56);
    h.signConvention = data[60];
    h.signReliable = data[61] != 0;
    const uint16_t warningCount = readLEU16(data + 62);
    const uint64_t dataOffset = readLEU64(data + 64);
    const uint64_t dataCount = readLEU64(data + 72);
    const uint64_t checksumOffset = readLEU64(data + 80);
    h.dataCount = dataCount;

    if (!validGridHeader(h))
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
    const uint64_t storedChecksum = readLEU64(data + checksumOffset);
    const uint64_t computedChecksum = msdfFNV1a64(data, size_t(checksumOffset));
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
        const uint16_t len = readLEU16(data + pos);
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

bool parseMSDFFile(const std::filesystem::path& path, MSDFParseResult& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err = "cannot open input file: " + path.string();
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
    in.read(reinterpret_cast<char*>(buf.data()), fileSize);
    if (!in)
    {
        err = "failed reading input file: " + path.string();
        return false;
    }
    return parseMSDFBytes(buf.data(), buf.size(), out, err);
}

bool serializeMSDFBytes(
    const MSDFHeader& header,
    const std::vector<float>& distances,
    const std::vector<std::string>& warnings,
    std::vector<uint8_t>& outBytes,
    std::string& err
)
{
    outBytes.clear();
    if (!validGridHeader(header))
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
    writeLEU32(outBytes, kMSDFFormatVersion);
    writeLEU32(outBytes, kMSDFHeaderSize);
    for (int i = 0; i < 3; ++i)
        writeLEU32(outBytes, header.resolution[i]);
    for (int i = 0; i < 3; ++i)
        writeLEF32(outBytes, header.bboxMin[i]);
    for (int i = 0; i < 3; ++i)
        writeLEF32(outBytes, header.bboxMax[i]);
    writeLEF32(outBytes, header.voxelSize);
    writeLEF32(outBytes, header.normalizationScale);
    writeLEF32(outBytes, header.paddingWorld);
    outBytes.push_back(uint8_t(header.signConvention));
    outBytes.push_back(header.signReliable ? 1 : 0);
    writeLEU16(outBytes, uint16_t(warnings.size()));

    uint64_t warnBytes = 0;
    for (const std::string& w : warnings)
        warnBytes += 2 + w.size();
    const uint64_t dataOffset = uint64_t(kMSDFHeaderSize) + ((warnBytes + 7) & ~uint64_t(7));
    const uint64_t dataCount = uint64_t(distances.size());
    const uint64_t checksumOffset = dataOffset + dataCount * 4;

    writeLEU64(outBytes, dataOffset);
    writeLEU64(outBytes, dataCount);
    writeLEU64(outBytes, checksumOffset);

    for (const std::string& w : warnings)
    {
        writeLEU16(outBytes, uint16_t(w.size()));
        outBytes.insert(outBytes.end(), w.begin(), w.end());
    }
    while (outBytes.size() < size_t(dataOffset))
        outBytes.push_back(0);
    for (float d : distances)
        writeLEF32(outBytes, d);

    writeLEU64(outBytes, msdfFNV1a64(outBytes.data(), outBytes.size()));
    return true;
}

// -------------------------------------------------------------------------------------
// Volume layout (frozen formulas)
// -------------------------------------------------------------------------------------

VolumeLayout VolumeLayout::compute(const MSDFHeader& header, VolumeFormat mip0Format)
{
    VolumeLayout l;
    std::array<uint32_t, 3> dims = header.resolution;
    const uint32_t maxDim = std::max(dims[0], std::max(dims[1], dims[2]));
    uint32_t count = 1;
    for (uint32_t d = maxDim; d > 1; d = (d + 1) / 2)
        ++count;
    l.mipCount = std::min(count, kLumenMeshSDFMaxMipCount);
    for (uint32_t m = 0; m < l.mipCount; ++m)
    {
        MipSpec& s = l.mips[m];
        s.dims = dims;
        s.format = (m == 0) ? mip0Format : VolumeFormat::R8Snorm;
        s.voxelCount = size_t(dims[0]) * size_t(dims[1]) * size_t(dims[2]);
        s.byteSize = mipByteSize(s);
        if (m + 1 < l.mipCount)
        {
            for (int a = 0; a < 3; ++a)
                dims[a] = std::max(1u, (dims[a] + 1) / 2);
        }
    }
    return l;
}

// -------------------------------------------------------------------------------------
// Quantization primitives
// -------------------------------------------------------------------------------------

uint16_t f32ToF16Bits(float value)
{
    uint32_t u = 0;
    std::memcpy(&u, &value, sizeof(u));
    const uint32_t sign = (u >> 16) & 0x8000u;
    const uint32_t e = (u >> 23) & 0xFFu;
    const uint32_t m = u & 0x7FFFFFu;

    if (e == 0xFFu) // Inf / NaN
        return uint16_t(sign | 0x7C00u | (m != 0u ? 0x0200u : 0u));
    if (e == 0u) // f32 zero / subnormal: flush to signed zero
        return uint16_t(sign);

    // Normal f32: f16 exponent field = e - 112 (f16 bias 15, f32 bias 127).
    int32_t exp = int32_t(e) - 112;

    if (exp >= 31)
        return uint16_t(sign | 0x7C00u); // overflow -> Inf

    if (exp > 0)
    {
        // Normal f16: 10 mantissa bits, round-to-nearest-even on the 13 dropped.
        uint32_t halfMant = m >> 13;
        const uint32_t rem = m & 0x1FFFu;
        if (rem > 0x1000u || (rem == 0x1000u && (halfMant & 1u) != 0u))
            ++halfMant;
        if (halfMant == 0x400u)
        {
            halfMant = 0;
            ++exp;
            if (exp >= 31)
                return uint16_t(sign | 0x7C00u);
        }
        return uint16_t(sign | (uint32_t(exp) << 10) | halfMant);
    }

    // Subnormal f16: value = (2^23 + m) * 2^(e-150) = s * 2^-24 with
    // s = (2^23 + m) >> (14 - exp); round-to-nearest-even on the dropped bits.
    const uint32_t mant = m | 0x800000u;
    const uint32_t shift = uint32_t(14 - exp); // exp <= 0 -> shift >= 14
    if (shift > 24)
    {
        return uint16_t(sign); // far below 2^-24: rounds to zero (half < mant threshold)
    }
    if (shift == 24)
    {
        // halfMant = mant >> 24 == 0; round up only when mant > 2^23.
        return uint16_t(sign | (mant > 0x800000u ? 1u : 0u));
    }
    const uint32_t half = 1u << (shift - 1u);
    const uint32_t dropped = mant & ((1u << shift) - 1u);
    uint32_t halfMant = mant >> shift;
    if (dropped > half || (dropped == half && (halfMant & 1u) != 0u))
        ++halfMant;
    if (halfMant == 0x400u)
        return uint16_t(sign | 0x0400u); // rounded up into the smallest normal
    return uint16_t(sign | halfMant);
}

float f16BitsToF32(uint16_t bits)
{
    const uint32_t sign = (uint32_t(bits) & 0x8000u) << 16;
    const uint32_t e = (bits >> 10) & 0x1Fu;
    const uint32_t m = bits & 0x3FFu;
    uint32_t out = 0;
    if (e == 0x1Fu)
    {
        out = sign | 0x7F800000u | (m != 0u ? 0x400000u : 0u); // Inf / NaN
    }
    else if (e == 0u)
    {
        if (m == 0u)
        {
            out = sign; // signed zero
        }
        else
        {
            // Subnormal f16 = m * 2^-24; every such value is a NORMAL f32 (>= 2^-24
            // far above the f32 subnormal range), so find its f32 exponent/mantissa.
            uint32_t m32 = m;
            uint32_t k = 0;
            while (m32 > 1)
            {
                m32 >>= 1;
                ++k;
            }
            out = sign | ((103u + k) << 23) | ((m << (23u - k)) & 0x7FFFFFu);
        }
    }
    else
    {
        out = sign | ((e + 112u) << 23) | (m << 13);
    }
    float f = 0.f;
    std::memcpy(&f, &out, sizeof(f));
    return f;
}

int8_t encodeSnorm8(float value, float range)
{
    const float scaled = value / range * 127.0f;
    int c = int(std::lround(scaled));
    c = std::max(-127, std::min(127, c));
    return int8_t(c);
}

float decodeSnorm8(int8_t code, float range)
{
    const float q = std::max(float(code) / 127.0f, -1.0f);
    return q * range;
}

// -------------------------------------------------------------------------------------
// Volume conversion
// -------------------------------------------------------------------------------------

bool buildVolume(
    const MSDFHeader& header,
    const std::vector<float>& distances,
    const BuildParams& params,
    BuildResult& out,
    std::string& err
)
{
    out = BuildResult{};
    if (!validGridHeader(header))
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

    float quantRange = 0.f;
    for (float d : distances)
    {
        if (!std::isfinite(d))
        {
            err = "non-finite distance in field (corrupt volume)";
            return false;
        }
        quantRange = std::max(quantRange, std::fabs(d));
    }
    if (quantRange < 1e-30f)
        quantRange = 1.f; // degenerate all-zero field: keep encoding well-defined

    // Volume descriptor (96 bytes, mirrors LumenMeshSDFData.slang).
    LumenMeshSDFVolumeDescriptor& desc = out.desc;
    for (int i = 0; i < 3; ++i)
    {
        desc.resolution[i] = header.resolution[i];
        desc.bboxMin[i] = header.bboxMin[i];
        desc.bboxMax[i] = header.bboxMax[i];
    }
    const VolumeFormat mip0Format = params.quality == Quality::High ? VolumeFormat::R16Float : VolumeFormat::R8Snorm;
    desc.formatMip0 = uint32_t(mip0Format);
    desc.formatCoarse = uint32_t(VolumeFormat::R8Snorm);
    desc.mipCount = 0; // filled below from the layout
    desc.voxelSize = header.voxelSize;
    desc.invVoxelSize = 1.f / header.voxelSize;
    desc.normalizationScale = header.normalizationScale;
    desc.invNormalizationScale = 1.f / header.normalizationScale;
    desc.quantRange = quantRange;
    desc.signConvention = header.signConvention;
    desc.signReliable = header.signReliable;
    desc.paddingWorld = header.paddingWorld;
    desc.atlasPage = kLumenMeshSDFNotResident;
    desc.contentHashLo = uint32_t(params.meshContentHash & 0xFFFFFFFFull);
    desc.contentHashHi = uint32_t(params.meshContentHash >> 32);
    desc.flags = 0;
    if (header.signReliable)
        desc.flags |= kLumenMeshSDFFlagSignReliable;
    if (params.quality == Quality::High)
        desc.flags |= kLumenMeshSDFFlagQualityHigh;
    if (params.pooling == MipPooling::MinAbs)
        desc.flags |= kLumenMeshSDFFlagPoolMinAbs;

    out.layout = VolumeLayout::compute(header, mip0Format);
    desc.mipCount = out.layout.mipCount;

    // Build the float mip chain, then quantize each level and estimate errors.
    std::vector<std::vector<float>> floats(out.layout.mipCount);
    floats[0] = distances;
    for (uint32_t m = 1; m < out.layout.mipCount; ++m)
    {
        floats[m] = poolMipLevel(floats[m - 1], out.layout.mips[m - 1].dims, out.layout.mips[m].dims, params.pooling);
    }

    out.mipData.resize(out.layout.mipCount);
    out.errors.resize(out.layout.mipCount);
    for (uint32_t m = 0; m < out.layout.mipCount; ++m)
    {
        const VolumeFormat format = out.layout.mips[m].format;
        quantizeMip(out.mipData[m], floats[m], format, quantRange);
        out.errors[m] = estimateMipError(floats[m], format, quantRange, header.voxelSize);
    }
    return true;
}

size_t estimateMemoryBytes(const MSDFHeader& header, Quality quality)
{
    const VolumeFormat mip0 = quality == Quality::High ? VolumeFormat::R16Float : VolumeFormat::R8Snorm;
    const VolumeLayout l = VolumeLayout::compute(header, mip0);
    size_t total = 0;
    for (uint32_t m = 0; m < l.mipCount; ++m)
        total += l.mips[m].byteSize;
    return total;
}

uint64_t computeCacheKey(const MSDFHeader& header, const BuildParams& params)
{
    // Components (any change to these must invalidate the disk cache):
    //   { meshContentHash(8), formatVersion(4), resolution(12),
    //     quality(1), pooling(1), mip0Format(1),
    //     quantRange bound (4, half grid diagonal in output space) }
    // The actual quantRange R = max |d| is a deterministic function of the
    // geometry + resolution, so it is already pinned by meshContentHash; the
    // header-derived bound component guards against field-level changes that
    // somehow share a content hash.
    std::vector<uint8_t> buf;
    const uint64_t contentHash = params.meshContentHash;
    for (int i = 0; i < 8; ++i)
        buf.push_back(uint8_t((contentHash >> (8 * i)) & 0xFFull));
    writeLEU32(buf, header.formatVersion);
    for (int i = 0; i < 3; ++i)
        writeLEU32(buf, header.resolution[i]);
    buf.push_back(uint8_t(params.quality));
    buf.push_back(uint8_t(params.pooling));
    buf.push_back(uint8_t(params.quality == Quality::High ? uint32_t(VolumeFormat::R16Float)
                                                          : uint32_t(VolumeFormat::R8Snorm)));
    const float dx = header.bboxMax[0] - header.bboxMin[0];
    const float dy = header.bboxMax[1] - header.bboxMin[1];
    const float dz = header.bboxMax[2] - header.bboxMin[2];
    const float rangeBound = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 1e-6f);
    writeLEF32(buf, rangeBound);
    return msdfFNV1a64(buf.data(), buf.size());
}

// -------------------------------------------------------------------------------------
// CPU mirrors of the sampling math (keep in sync with LumenMeshSDFSampling.slang)
// -------------------------------------------------------------------------------------

float sampleTrilinearCPU(
    const std::array<uint32_t, 3>& dims,
    const std::vector<float>& mipData,
    const float voxel[3]
)
{
    const auto idx = [&](uint32_t x, uint32_t y, uint32_t z) {
        return (size_t(z) * dims[1] + y) * dims[0] + x;
    };

    // dims-safe clamping (also for 1-texel mips): every corner index stays in
    // [0, dims-1] - mirrors lumenMeshSDFSampleStored in LumenMeshSDFSampling.slang.
    float cell[3], f[3];
    uint32_t base[3], bx[3];
    for (int a = 0; a < 3; ++a)
    {
        const float maxCoord = float(dims[a] - 1u);
        cell[a] = std::max(0.f, std::min(voxel[a], maxCoord));
        base[a] = uint32_t(std::min(std::floor(cell[a]), maxCoord));
        bx[a] = base[a] < dims[a] - 1u ? 1u : 0u;
        f[a] = cell[a] - float(base[a]);
    }

    const float v000 = mipData[idx(base[0], base[1], base[2])];
    const float v100 = mipData[idx(base[0] + bx[0], base[1], base[2])];
    const float v010 = mipData[idx(base[0], base[1] + bx[1], base[2])];
    const float v110 = mipData[idx(base[0] + bx[0], base[1] + bx[1], base[2])];
    const float v001 = mipData[idx(base[0], base[1], base[2] + bx[2])];
    const float v101 = mipData[idx(base[0] + bx[0], base[1], base[2] + bx[2])];
    const float v011 = mipData[idx(base[0], base[1] + bx[1], base[2] + bx[2])];
    const float v111 = mipData[idx(base[0] + bx[0], base[1] + bx[1], base[2] + bx[2])];

    const float c0 = v000 * (1.f - f[0]) + v100 * f[0];
    const float c1 = v010 * (1.f - f[0]) + v110 * f[0];
    const float c2 = v001 * (1.f - f[0]) + v101 * f[0];
    const float c3 = v011 * (1.f - f[0]) + v111 * f[0];
    const float c01 = c0 * (1.f - f[1]) + c1 * f[1];
    const float c23 = c2 * (1.f - f[1]) + c3 * f[1];
    return c01 * (1.f - f[2]) + c23 * f[2];
}

std::array<float, 3> worldToVoxelCPU(const LumenMeshSDFVolumeDescriptor& desc, const float worldPos[3])
{
    std::array<float, 3> u;
    for (int a = 0; a < 3; ++a)
    {
        const float pOut = worldPos[a] * desc.normalizationScale;
        u[a] = (pOut - desc.bboxMin[a]) * desc.invVoxelSize - 0.5f;
    }
    return u;
}

} // namespace MeshSDF
} // namespace LumenGI
