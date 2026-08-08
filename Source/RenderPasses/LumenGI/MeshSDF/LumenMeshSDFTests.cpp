// =====================================================================================
//  LumenMeshSDFTests.cpp - self-contained CPU tests for the S6-B1 Mesh SDF runtime.
//  -------------------------------------------------------------------------------------
//  No test framework, no Falcor, no CMake target: plain asserts + a main() that
//  prints [PASS]/[FAIL] lines and returns a non-zero exit code on any failure.
//
//  BUILD (from this directory)
//  -------------------------------------------------------------------------------------
//  MSVC:   cl /nologo /std:c++17 /O2 /EHsc /W4 LumenMeshSDF.cpp LumenMeshSDFTests.cpp /Fe:LumenMeshSDFTests.exe
//  GCC:    g++ -std=c++17 -O2 LumenMeshSDF.cpp LumenMeshSDFTests.cpp -o LumenMeshSDFTests
//
//  COVERAGE (maps to S6-B1 + S6-C1/C2 integration points)
//  -------------------------------------------------------------------------------------
//  1. FNV-1a64 known vectors (byte-identical algorithm to Agent D's builder)
//  2. ".msdf" serialize -> parse round-trip: header fields, distances bit-exact,
//     warnings, byte-offset layout frozen table
//  3. Corruption detection: magic / version / dataCount / offsets / checksum / truncation
//  4. Frozen GPU descriptor layout (96 bytes, field offsets)
//  5. Volume layout: mip dims (pow2 and non-pow2), mip counts, exact memory budget
//  6. f32 <-> f16 round-trip: known bit vectors + RNE tie behavior + error bound
//  7. snorm8 encode/decode: exact zero, clamping, error bound R/127
//  8. Mip pooling: constant field, min-abs vs average on a mixed-sign cell
//  9. End-to-end buildVolume on an analytic sphere field: descriptor fields,
//     quantization error bounds, cache key determinism/sensitivity
//  10. World <-> voxel transform round-trips (CPU mirror of the sampling math)
//  11. Trilinear sampling mirror: voxel-center exactness, clamped border cells
// =====================================================================================

#include "LumenMeshSDF.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace LumenGI::MeshSDF;

struct Harness
{
    int pass = 0;
    int fail = 0;

    void expect(bool cond, const std::string& what)
    {
        std::cout << (cond ? "[PASS] " : "[FAIL] ") << what << "\n";
        std::cout.flush(); // keep output visible on hard crash
        if (cond)
            ++pass;
        else
            ++fail;
    }

    void expectEq(uint64_t a, uint64_t b, const std::string& what) { expect(a == b, what); }
    void expectEqI(int64_t a, int64_t b, const std::string& what) { expect(a == b, what); }
    void expectNear(float a, float b, float tol, const std::string& what)
    {
        expect(std::fabs(a - b) <= tol, what);
    }
    void expectTrue(bool v, const std::string& what) { expect(v, what); }

    int result() const
    {
        std::cout << "----\n" << pass << " passed, " << fail << " failed\n";
        return fail == 0 ? 0 : 1;
    }
};

// Deterministic pseudo-random source for reproducible error spreads.
struct LCG
{
    uint32_t s = 12345u;
    float next()
    {
        s = s * 1664525u + 1013904223u;
        return float(s & 0xFFFFFFu) / float(1u << 24);
    }
    float nextRange(float lo, float hi) { return lo + (hi - lo) * next(); }
};

// -------------------------------------------------------------------------------------
// Test 1: FNV-1a64 known vectors
// -------------------------------------------------------------------------------------

void testFNV1a(Harness& h)
{
    // Published FNV-1a 64-bit test vectors (offset basis + prime).
    const uint64_t empty = msdfFNV1a64("", 0);
    const uint64_t oneA = msdfFNV1a64("a", 1);
    const uint64_t oneB = msdfFNV1a64("b", 1);
    h.expectEq(empty, 0xcbf29ce484222325ULL, "fnv1a: empty input == offset basis");
    h.expectEq(oneA, 0xaf63dc4c8601ec8cULL, "fnv1a: 'a' == published vector");
    h.expectTrue(oneA != oneB, "fnv1a: single-byte change alters the hash");
    h.expectEq(msdfFNV1a64("foobar", 6), msdfFNV1a64("foobar", 6), "fnv1a: deterministic");
    h.expectTrue(msdfFNV1a64("foobar", 6) != msdfFNV1a64("foo", 3), "fnv1a: length-sensitive");
}

// -------------------------------------------------------------------------------------
// Synthetic volume builders
// -------------------------------------------------------------------------------------

MSDFHeader makeHeader(uint32_t n, float scale)
{
    MSDFHeader h;
    h.formatVersion = kMSDFFormatVersion;
    h.resolution = {n, n, n};
    h.bboxMin = {0.f, 0.f, 0.f};
    h.bboxMax = {1.f, 1.f, 1.f};
    h.voxelSize = 1.f / float(n - 1);
    h.normalizationScale = scale;
    h.paddingWorld = 0.1f;
    h.signConvention = kLumenMeshSDFSignConventionPositiveOutside;
    h.signReliable = 1;
    h.dataCount = uint64_t(n) * uint64_t(n) * uint64_t(n);
    return h;
}

/// Analytic sphere field (signed distance to a sphere surface, output space).
std::vector<float> makeSphereField(const MSDFHeader& h, float cx, float cy, float cz, float radius)
{
    const uint32_t nx = h.resolution[0], ny = h.resolution[1], nz = h.resolution[2];
    std::vector<float> d(h.dataCount);
    size_t i = 0;
    for (uint32_t z = 0; z < nz; ++z)
        for (uint32_t y = 0; y < ny; ++y)
            for (uint32_t x = 0; x < nx; ++x, ++i)
            {
                const float px = h.bboxMin[0] + (float(x) + 0.5f) * h.voxelSize;
                const float py = h.bboxMin[1] + (float(y) + 0.5f) * h.voxelSize;
                const float pz = h.bboxMin[2] + (float(z) + 0.5f) * h.voxelSize;
                d[i] = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy) + (pz - cz) * (pz - cz)) - radius;
            }
    return d;
}

float sphereMaxAbs(const std::vector<float>& d)
{
    float m = 0.f;
    for (float v : d)
        m = std::max(m, std::fabs(v));
    return m;
}

// -------------------------------------------------------------------------------------
// Test 2/3: ".msdf" round-trip and corruption detection
// -------------------------------------------------------------------------------------

void testMSDFRoundTrip(Harness& h)
{
    const MSDFHeader header = makeHeader(16, 0.5f);
    std::vector<float> field = makeSphereField(header, 0.5f, 0.5f, 0.5f, 0.25f);
    field[0] += 0.125f; // perturb a corner so the field is not perfectly symmetric
    const std::vector<std::string> warnings = {"mesh is not watertight", "thickness below 2.5 voxels"};

    std::vector<uint8_t> bytes;
    std::string err;
    h.expectTrue(serializeMSDFBytes(header, field, warnings, bytes, err), "serialize: valid input accepted");

    // Frozen byte layout table.
    h.expectTrue(bytes.size() >= 88 + 8, "serialize: file >= header + checksum");
    h.expectTrue(std::memcmp(bytes.data(), "MSDF", 4) == 0, "layout: magic at offset 0");
    h.expectEq(uint32_t(bytes[4]) | uint32_t(bytes[5]) << 8, kMSDFFormatVersion, "layout: version at offset 4");
    h.expectEq(uint32_t(bytes[8]) | uint32_t(bytes[9]) << 8 | uint32_t(bytes[10]) << 16 | uint32_t(bytes[11]) << 24,
               kMSDFHeaderSize, "layout: headerSize at offset 8");
    // dataOffset = 88 + align8(warnings) ; each warning = 2 + len bytes.
    const uint64_t warnBytes = (2 + warnings[0].size()) + (2 + warnings[1].size());
    const uint64_t dataOffset = 88 + ((warnBytes + 7) & ~uint64_t(7));
    uint64_t storedDataOffset = 0;
    for (int i = 0; i < 8; ++i)
        storedDataOffset |= uint64_t(bytes[64 + i]) << (8 * i);
    uint64_t storedChecksumOffset = 0;
    for (int i = 0; i < 8; ++i)
        storedChecksumOffset |= uint64_t(bytes[80 + i]) << (8 * i);
    h.expectEq(storedDataOffset, dataOffset, "layout: dataOffset == 88 + align8(warnings)");
    h.expectEq(storedChecksumOffset, dataOffset + field.size() * 4, "layout: checksumOffset == data end");
    h.expectEq(bytes.size(), size_t(storedChecksumOffset + 8), "layout: trailing checksum present");

    MSDFParseResult parsed;
    h.expectTrue(parseMSDFBytes(bytes.data(), bytes.size(), parsed, err), "parse: round-trip accepted");
    h.expectEq(parsed.header.formatVersion, header.formatVersion, "parse: formatVersion");
    h.expectEq(parsed.header.resolution[0], header.resolution[0], "parse: resolution.x");
    h.expectEq(parsed.header.dataCount, header.dataCount, "parse: dataCount");
    h.expectNear(parsed.header.voxelSize, header.voxelSize, 0.f, "parse: voxelSize bit-exact");
    h.expectNear(parsed.header.normalizationScale, header.normalizationScale, 0.f, "parse: normalizationScale bit-exact");
    h.expectEq(parsed.header.signConvention, header.signConvention, "parse: signConvention");
    h.expectEq(parsed.header.signReliable, header.signReliable, "parse: signReliable");
    h.expectTrue(parsed.warnings.size() == warnings.size(), "parse: warning count");
    h.expectTrue(parsed.warnings[0] == warnings[0] && parsed.warnings[1] == warnings[1], "parse: warning texts");
    h.expectTrue(parsed.distances.size() == field.size(), "parse: distance count");
    h.expectTrue(std::memcmp(parsed.distances.data(), field.data(), field.size() * sizeof(float)) == 0,
                 "parse: distances bit-exact round-trip");

    // Corruption detection.
    auto expectCorrupt = [&](const std::vector<uint8_t>& b, const std::string& what) {
        MSDFParseResult p;
        std::string e;
        h.expectTrue(!parseMSDFBytes(b.data(), b.size(), p, e) && !e.empty(), what);
    };

    {
        std::vector<uint8_t> bad = bytes;
        bad[0] ^= 0xFF;
        expectCorrupt(bad, "corrupt: magic byte detected");
    }
    {
        std::vector<uint8_t> bad = bytes;
        bad[4] = 2;
        expectCorrupt(bad, "corrupt: unsupported version detected");
    }
    {
        std::vector<uint8_t> bad = bytes;
        for (int i = 0; i < 8; ++i)
            bad[72 + i] = 0; // dataCount := 0
        expectCorrupt(bad, "corrupt: dataCount != resolution detected");
    }
    {
        std::vector<uint8_t> bad = bytes;
        bad[64] = 87; // dataOffset < headerSize
        expectCorrupt(bad, "corrupt: dataOffset out of range detected");
    }
    {
        std::vector<uint8_t> bad = bytes;
        bad[80] ^= 0x01; // checksumOffset mismatch
        expectCorrupt(bad, "corrupt: checksumOffset mismatch detected");
    }
    {
        std::vector<uint8_t> bad = bytes;
        bad[size_t(storedChecksumOffset) + 2] ^= 0xFF; // flip a data byte
        expectCorrupt(bad, "corrupt: data corruption via checksum detected");
    }
    {
        std::vector<uint8_t> bad = bytes;
        bad.pop_back(); // truncate the checksum
        expectCorrupt(bad, "corrupt: truncated file detected");
    }
    {
        std::vector<uint8_t> bad = {0x4D, 0x53, 0x44, 0x46};
        expectCorrupt(bad, "corrupt: header-only stub rejected");
    }
}

// -------------------------------------------------------------------------------------
// Test 4: frozen GPU descriptor layout
// -------------------------------------------------------------------------------------

void testDescriptorLayout(Harness& h)
{
    h.expectEq(sizeof(LumenMeshSDFVolumeDescriptor), size_t(96), "descriptor: 96 bytes");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, resolution[0]), size_t(0), "descriptor: resolution @ 0");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, formatMip0), size_t(12), "descriptor: formatMip0 @ 12");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, formatCoarse), size_t(16), "descriptor: formatCoarse @ 16");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, mipCount), size_t(20), "descriptor: mipCount @ 20");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, bboxMin[0]), size_t(24), "descriptor: bboxMin @ 24");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, bboxMax[0]), size_t(36), "descriptor: bboxMax @ 36");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, voxelSize), size_t(48), "descriptor: voxelSize @ 48");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, invVoxelSize), size_t(52), "descriptor: invVoxelSize @ 52");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, normalizationScale), size_t(56),
               "descriptor: normalizationScale @ 56");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, invNormalizationScale), size_t(60),
               "descriptor: invNormalizationScale @ 60");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, quantRange), size_t(64), "descriptor: quantRange @ 64");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, signConvention), size_t(68), "descriptor: signConvention @ 68");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, signReliable), size_t(72), "descriptor: signReliable @ 72");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, paddingWorld), size_t(76), "descriptor: paddingWorld @ 76");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, atlasPage), size_t(80), "descriptor: atlasPage @ 80");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, contentHashLo), size_t(84), "descriptor: contentHashLo @ 84");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, contentHashHi), size_t(88), "descriptor: contentHashHi @ 88");
    h.expectEq(offsetof(LumenMeshSDFVolumeDescriptor, flags), size_t(92), "descriptor: flags @ 92");
}

// -------------------------------------------------------------------------------------
// Test 5: volume layout + memory budget
// -------------------------------------------------------------------------------------

void testVolumeLayout(Harness& h)
{
    {
        const MSDFHeader hdr = makeHeader(128, 1.f);
        const VolumeLayout l = VolumeLayout::compute(hdr, VolumeFormat::R16Float);
        h.expectEq(l.mipCount, 8u, "layout: 128^3 -> 8 mips");
        const std::array<uint32_t, 3> expected[] = {
            {128, 128, 128}, {64, 64, 64}, {32, 32, 32}, {16, 16, 16},
            {8, 8, 8},       {4, 4, 4},    {2, 2, 2},    {1, 1, 1},
        };
        bool dimsOk = true;
        for (uint32_t m = 0; m < l.mipCount; ++m)
            dimsOk = dimsOk && l.mips[m].dims == expected[m];
        h.expectTrue(dimsOk, "layout: pow2 mip dims chain exact");
        h.expectEq(uint32_t(l.mips[0].format), uint32_t(VolumeFormat::R16Float),
                   "layout: mip0 format R16Float (High)");
        h.expectEq(uint32_t(l.mips[1].format), uint32_t(VolumeFormat::R8Snorm),
                   "layout: coarse format R8Snorm");
        h.expectEq(l.mips[0].byteSize, size_t(128 * 128 * 128 * 2), "layout: mip0 bytes 2/voxel");
        h.expectEq(l.mips[1].byteSize, size_t(64 * 64 * 64), "layout: mip1 bytes 1/voxel");
    }
    {
        const MSDFHeader hdr = makeHeader(127, 1.f); // non-pow2
        const VolumeLayout l = VolumeLayout::compute(hdr, VolumeFormat::R8Snorm);
        h.expectEq(l.mipCount, 8u, "layout: 127^3 -> 8 mips (ceil halving)");
        h.expectEq(l.mips[1].dims[0], 64u, "layout: 127 -> 64");
        h.expectEq(l.mips[4].dims[0], 8u, "layout: 127 -> ... -> 8");
        h.expectEq(l.mips[7].dims[0], 1u, "layout: last mip dim == 1");
    }
    {
        const MSDFHeader hdr = makeHeader(2, 1.f);
        h.expectEq(VolumeLayout::compute(hdr, VolumeFormat::R8Snorm).mipCount, 2u, "layout: 2^3 -> 2 mips");
    }

    // Memory budget (frozen numbers, see S6-B1 report).
    const MSDFHeader hdr128 = makeHeader(128, 1.f);
    const size_t coarseBytes = size_t(64 * 64 * 64 + 32 * 32 * 32 + 16 * 16 * 16 + 8 * 8 * 8 + 4 * 4 * 4 + 2 * 2 * 2 + 1);
    h.expectEq(estimateMemoryBytes(hdr128, Quality::High), size_t(128 * 128 * 128 * 2) + coarseBytes,
               "memory: 128^3 High exact budget");
    h.expectEq(estimateMemoryBytes(hdr128, Quality::Low), size_t(128 * 128 * 128) + coarseBytes,
               "memory: 128^3 Low exact budget");
    h.expectTrue(estimateMemoryBytes(hdr128, Quality::High) < size_t(5 * 1024 * 1024),
                 "memory: 128^3 High < 5 MB");
    h.expectTrue(estimateMemoryBytes(hdr128, Quality::Low) < size_t(3 * 1024 * 1024),
                 "memory: 128^3 Low < 3 MB");
    h.expectTrue(estimateMemoryBytes(hdr128, Quality::High) == estimateMemoryBytes(hdr128, Quality::High),
                 "memory: deterministic");
}

// -------------------------------------------------------------------------------------
// Test 6: f32 <-> f16 round-trip
// -------------------------------------------------------------------------------------

void testF16(Harness& h)
{
    struct Vec
    {
        float v;
        uint16_t bits;
    };
    const Vec known[] = {
        {0.f, 0x0000}, {-0.f, 0x8000}, {1.f, 0x3C00}, {-1.f, 0xBC00}, {2.f, 0x4000}, {-2.f, 0xC000},
        {0.5f, 0x3800}, {3.f, 0x4200}, {65504.f, 0x7BFF}, {1e30f, 0x7C00}, {-1e30f, 0xFC00},
        {6.103515625e-05f, 0x0400},           // 2^-14: smallest normal
        {5.960464477539063e-08f, 0x0001},     // 2^-24: smallest subnormal
        {1.f + 1.f / 2048.f, 0x3C00},         // RNE: 1 + 2^-11 ties to even (down)
        {1.f + 1.f / 1024.f + 1.f / 2048.f, 0x3C02}, // RNE: 1 + 3*2^-11 rounds up
        {1.f + 1.f / 1024.f, 0x3C01},         // RNE: 1 + 2^-10 exactly representable
    };
    for (const Vec& v : known)
    {
        h.expectEq(f32ToF16Bits(v.v), v.bits, "f16: bit-exact encode " + std::to_string(double(v.v)));
    }
    // Decode table: canonical f16 value of each bit pattern (independent of the
    // source f32 above, so RNE-down cases like 1 + 2^-11 -> 0x3C00 are checked
    // against the actual f16 value, not the rounded-away original).
    struct DVec
    {
        uint16_t bits;
        float v;
    };
    const DVec decoded[] = {
        {0x0000, 0.f}, {0x8000, -0.f}, {0x3C00, 1.f}, {0xBC00, -1.f}, {0x4000, 2.f}, {0xC000, -2.f},
        {0x3800, 0.5f}, {0x4200, 3.f}, {0x7BFF, 65504.f}, {0x7C00, 1e30f}, {0xFC00, -1e30f},
        {0x0400, 6.103515625e-05f}, {0x0001, 5.960464477539063e-08f},
        {0x3C01, 1.0009765625f}, {0x3C02, 1.001953125f},
    };
    for (const DVec& d : decoded)
    {
        const float back = f16BitsToF32(d.bits);
        const bool isInfBits = (d.bits & 0x7FFFu) == 0x7C00u;
        if (isInfBits)
            h.expectTrue(std::isinf(back) && std::signbit(back) == std::signbit(d.v),
                         "f16: inf pattern decodes to inf with sign");
        else
            h.expectNear(back, d.v, 0.f, "f16: bit-exact decode 0x" + std::to_string(d.bits));
    }
    h.expectNear(f16BitsToF32(0x0001), 5.960464477539063e-08f, 1e-12f,
                 "f16: smallest subnormal value check");

    // Error bound: |roundtrip(x) - x| <= 2^-11 * |x| over a spread of magnitudes.
    LCG rng;
    float maxRel = 0.f;
    for (int i = 0; i < 20000; ++i)
    {
        const float x = rng.nextRange(-100.f, 100.f);
        const float rt = f16BitsToF32(f32ToF16Bits(x));
        maxRel = std::max(maxRel, std::fabs(rt - x) / std::max(std::fabs(x), 6e-5f));
    }
    h.expectTrue(maxRel <= 0.000977f, "f16: relative error <= 2^-11 + margin over 20k samples");
}

// -------------------------------------------------------------------------------------
// Test 7: snorm8 encode/decode
// -------------------------------------------------------------------------------------

void testSnorm8(Harness& h)
{
    h.expectEqI(int64_t(encodeSnorm8(0.f, 1.f)), 0, "snorm8: zero -> code 0");
    h.expectNear(decodeSnorm8(0, 1.f), 0.f, 0.f, "snorm8: code 0 -> exact zero");
    h.expectEqI(int64_t(encodeSnorm8(1.f, 1.f)), 127, "snorm8: +range -> +127");
    h.expectEqI(int64_t(encodeSnorm8(-1.f, 1.f)), -127, "snorm8: -range -> -127");
    h.expectEqI(int64_t(encodeSnorm8(5.f, 1.f)), 127, "snorm8: positive clamp");
    h.expectEqI(int64_t(encodeSnorm8(-5.f, 1.f)), -127, "snorm8: negative clamp");
    h.expectNear(decodeSnorm8(127, 1.f), 1.f, 0.f, "snorm8: +127 -> exact +1");
    h.expectNear(decodeSnorm8(-127, 1.f), -1.f, 0.f, "snorm8: -127 -> exact -1");
    h.expectEqI(int64_t(encodeSnorm8(0.5f, 1.f)), 64, "snorm8: 0.5 -> round(63.5) == 64");

    // Round-trip error <= R/127 (uniform absolute quantization).
    LCG rng;
    const float R = 0.75f;
    float maxErr = 0.f;
    for (int i = 0; i < 20000; ++i)
    {
        const float x = rng.nextRange(-R, R);
        maxErr = std::max(maxErr, std::fabs(decodeSnorm8(encodeSnorm8(x, R), R) - x));
    }
    h.expectTrue(maxErr <= R / 127.f + 1e-7f, "snorm8: round-trip error <= R/127 over 20k samples");
}

// -------------------------------------------------------------------------------------
// Test 8: mip pooling
// -------------------------------------------------------------------------------------

void testPooling(Harness& h)
{
    // Constant field: every mip level reproduces the constant exactly.
    const MSDFHeader hdr = makeHeader(16, 1.f);
    std::vector<float> cfield(hdr.dataCount, 0.375f);
    BuildResult out;
    std::string err;
    h.expectTrue(buildVolume(hdr, cfield, BuildParams{}, out, err), "pooling: constant field builds");
    bool constOk = true;
    for (uint32_t m = 0; m < out.layout.mipCount; ++m)
    {
        const MipSpec& spec = out.layout.mips[m];
        std::vector<float> decoded(spec.voxelCount);
        for (size_t i = 0; i < spec.voxelCount; ++i)
        {
            decoded[i] = (spec.format == VolumeFormat::R16Float)
                ? f16BitsToF32(uint16_t(out.mipData[m][2 * i]) | (uint16_t(out.mipData[m][2 * i + 1]) << 8))
                : decodeSnorm8(int8_t(out.mipData[m][i]), out.desc.quantRange);
            constOk = constOk && std::fabs(decoded[i] - 0.375f) <= 2e-3f;
        }
    }
    h.expectTrue(constOk, "pooling: constant field constant across all mips (within quant error)");

    // Mixed-sign 2x2x2 cell: min-abs keeps the child with min |d| (sign preserved),
    // average takes the signed mean.
    std::vector<float> tiny = {3.f, -3.f, 3.f, -3.f, 3.f, -3.f, 3.f, -1.f};
    std::array<uint32_t, 3> dimsPrev = {2, 2, 2};
    std::array<uint32_t, 3> dimsCur = {1, 1, 1};
    const std::vector<float> minAbs = poolMipLevel(tiny, dimsPrev, dimsCur, MipPooling::MinAbs);
    const std::vector<float> avg = poolMipLevel(tiny, dimsPrev, dimsCur, MipPooling::Average);
    h.expectNear(minAbs[0], -1.f, 0.f, "pooling: min-abs keeps signed child with min |d|");
    h.expectNear(avg[0], 0.25f, 0.f, "pooling: average == signed mean of children");
}

// -------------------------------------------------------------------------------------
// Test 9: end-to-end buildVolume on an analytic sphere field
// -------------------------------------------------------------------------------------

void testBuildVolume(Harness& h)
{
    const MSDFHeader hdr = makeHeader(24, 0.5f);
    const std::vector<float> field = makeSphereField(hdr, 0.5f, 0.5f, 0.5f, 0.25f);
    const float R = sphereMaxAbs(field);
    h.expectTrue(R > 0.5f, "sphere: field range is non-trivial");

    BuildParams params;
    params.quality = Quality::High;
    params.pooling = MipPooling::MinAbs;
    params.meshContentHash = 0x123456789ABCDEF0ull;

    BuildResult out;
    std::string err;
    h.expectTrue(buildVolume(hdr, field, params, out, err), "build: sphere field accepted");

    // Descriptor invariants.
    const LumenMeshSDFVolumeDescriptor& d = out.desc;
    h.expectEq(d.resolution[0], 24u, "build: resolution propagated");
    h.expectEq(d.formatMip0, uint32_t(VolumeFormat::R16Float), "build: High -> R16Float mip0");
    h.expectEq(d.formatCoarse, uint32_t(VolumeFormat::R8Snorm), "build: coarse format frozen R8Snorm");
    h.expectEq(d.mipCount, 6u, "build: 24^3 -> 6 mips");
    h.expectNear(d.quantRange, R, 0.f, "build: quantRange == max |d| over mip0");
    h.expectNear(d.invVoxelSize, 1.f / d.voxelSize, 1e-6f, "build: invVoxelSize consistent");
    h.expectEq(d.signReliable, 1u, "build: signReliable propagated");
    h.expectEq(d.signConvention, kLumenMeshSDFSignConventionPositiveOutside, "build: signConvention propagated");
    h.expectEq(d.atlasPage, kLumenMeshSDFNotResident, "build: atlasPage defaults to not resident");
    h.expectEq(d.contentHashLo, uint32_t(0x123456789ABCDEF0ull & 0xFFFFFFFFull), "build: contentHashLo");
    h.expectEq(d.contentHashHi, uint32_t(0x123456789ABCDEF0ull >> 32), "build: contentHashHi");
    h.expectTrue((d.flags & kLumenMeshSDFFlagSignReliable) != 0u, "build: signReliable flag set");
    h.expectTrue((d.flags & kLumenMeshSDFFlagQualityHigh) != 0u, "build: quality-high flag set");
    h.expectTrue((d.flags & kLumenMeshSDFFlagPoolMinAbs) != 0u, "build: min-abs pooling flag set");

    // Per-mip data sizes match the layout; per-mip error bounds hold.
    bool sizesOk = true;
    for (uint32_t m = 0; m < out.layout.mipCount; ++m)
        sizesOk = sizesOk && out.mipData[m].size() == out.layout.mips[m].byteSize;
    h.expectTrue(sizesOk, "build: encoded byte sizes match layout");

    h.expectTrue(out.errors[0].maxAbsError <= 2e-3f, "build: mip0 (f16) max abs error <= 2e-3");
    h.expectTrue(out.errors[0].maxErrorInVoxels < 1.f, "build: mip0 error < 1 voxel");
    bool coarseOk = true;
    for (uint32_t m = 1; m < out.layout.mipCount; ++m)
        coarseOk = coarseOk && out.errors[m].maxAbsError <= R / 127.f * 1.001f;
    h.expectTrue(coarseOk, "build: coarse (snorm8) error <= R/127 per mip");
    h.expectTrue(out.errors[0].meanAbsError <= out.errors[0].maxAbsError, "build: mean <= max");
    h.expectTrue(out.errors[0].rmsError <= out.errors[0].maxAbsError, "build: rms <= max");

    // Decode mip0 bytes and verify the full round-trip against the original field.
    bool roundtripOk = true;
    float maxRtErr = 0.f;
    for (size_t i = 0; i < field.size(); ++i)
    {
        const uint16_t bits = uint16_t(out.mipData[0][2 * i]) | (uint16_t(out.mipData[0][2 * i + 1]) << 8);
        maxRtErr = std::max(maxRtErr, std::fabs(f16BitsToF32(bits) - field[i]));
    }
    roundtripOk = maxRtErr <= 2e-3f;
    h.expectTrue(roundtripOk, "build: decode(mip0) approximates the original field within 2e-3");

    // Low quality: mip0 becomes R8Snorm with the same quantRange.
    BuildParams low = params;
    low.quality = Quality::Low;
    BuildResult outLow;
    h.expectTrue(buildVolume(hdr, field, low, outLow, err), "build: Low quality builds");
    h.expectEq(outLow.desc.formatMip0, uint32_t(VolumeFormat::R8Snorm), "build: Low -> R8Snorm mip0");
    h.expectTrue((outLow.desc.flags & kLumenMeshSDFFlagQualityHigh) == 0u, "build: Low clears quality flag");
    h.expectTrue(outLow.errors[0].maxAbsError <= R / 127.f * 1.001f, "build: Low mip0 error <= R/127");

    // Non-finite field rejected.
    std::vector<float> badField = field;
    badField[badField.size() / 2] = std::nanf("");
    BuildResult outBad;
    h.expectTrue(!buildVolume(hdr, badField, params, outBad, err) && !err.empty(),
                 "build: non-finite distance rejected");

    // Cache key: deterministic, sensitive to every cache-relevant knob.
    const uint64_t k1 = computeCacheKey(hdr, params);
    h.expectEq(k1, computeCacheKey(hdr, params), "cachekey: deterministic");
    BuildParams other = params;
    other.quality = Quality::Low;
    h.expectTrue(computeCacheKey(hdr, other) != k1, "cachekey: quality changes the key");
    other = params;
    other.pooling = MipPooling::Average;
    h.expectTrue(computeCacheKey(hdr, other) != k1, "cachekey: pooling changes the key");
    other = params;
    other.meshContentHash = 0xDEADBEEFCAFEBABEull;
    h.expectTrue(computeCacheKey(hdr, other) != k1, "cachekey: content hash changes the key");
    MSDFHeader otherHdr = hdr;
    otherHdr.resolution[0] = 25;
    h.expectTrue(computeCacheKey(otherHdr, params) != k1, "cachekey: resolution changes the key");
}

// -------------------------------------------------------------------------------------
// Test 10/11: world <-> voxel transforms and trilinear mirror
// -------------------------------------------------------------------------------------

void testTransforms(Harness& h)
{
    const MSDFHeader hdr = makeHeader(24, 0.5f);
    const std::vector<float> field = makeSphereField(hdr, 0.5f, 0.5f, 0.5f, 0.25f);
    BuildResult out;
    std::string err;
    h.expectTrue(buildVolume(hdr, field, BuildParams{}, out, err), "transforms: build");

    const LumenMeshSDFVolumeDescriptor& d = out.desc;
    const uint32_t n = 24;

    // Every voxel center in world space maps back to its integer voxel index.
    bool centersOk = true;
    for (uint32_t z = 0; z < n && centersOk; ++z)
        for (uint32_t y = 0; y < n && centersOk; ++y)
            for (uint32_t x = 0; x < n && centersOk; ++x)
            {
                const float pOut[3] = {
                    d.bboxMin[0] + (float(x) + 0.5f) * d.voxelSize,
                    d.bboxMin[1] + (float(y) + 0.5f) * d.voxelSize,
                    d.bboxMin[2] + (float(z) + 0.5f) * d.voxelSize,
                };
                const float world[3] = {
                    pOut[0] * d.invNormalizationScale,
                    pOut[1] * d.invNormalizationScale,
                    pOut[2] * d.invNormalizationScale,
                };
                const std::array<float, 3> u = worldToVoxelCPU(d, world);
                const float tol = 1e-4f;
                centersOk = centersOk && std::fabs(u[0] - float(x)) <= tol && std::fabs(u[1] - float(y)) <= tol &&
                             std::fabs(u[2] - float(z)) <= tol;
            }
    h.expectTrue(centersOk, "transforms: voxel centers round-trip to integer coords (scale != 1)");

    // World->voxel->world round-trip.
    const float wp[3] = {0.3f, 0.7f, 0.2f};
    const std::array<float, 3> u = worldToVoxelCPU(d, wp);
    float back[3];
    for (int a = 0; a < 3; ++a)
        back[a] = (d.bboxMin[a] + (u[a] + 0.5f) * d.voxelSize) * d.invNormalizationScale;
    h.expectNear(back[0], wp[0], 1e-5f, "transforms: world->voxel->world round-trip x");
    h.expectNear(back[1], wp[1], 1e-5f, "transforms: world->voxel->world round-trip y");
    h.expectNear(back[2], wp[2], 1e-5f, "transforms: world->voxel->world round-trip z");

    // Trilinear mirror: sampling at a voxel center returns the stored value
    // exactly; border clamping never reads out of bounds and saturates.
    std::vector<float> decoded(n * n * n);
    for (size_t i = 0; i < decoded.size(); ++i)
        decoded[i] = f16BitsToF32(uint16_t(out.mipData[0][2 * i]) | (uint16_t(out.mipData[0][2 * i + 1]) << 8));
    const std::array<uint32_t, 3> dims = {n, n, n};

    const float center[3] = {7.f, 11.f, 3.f};
    h.expectNear(sampleTrilinearCPU(dims, decoded, center),
                 decoded[(size_t(3) * n + 11) * n + 7], 1e-6f, "trilinear: voxel-center sample exact");

    float clamped[3];
    const float below[3] = {-5.f, -5.f, -5.f};
    clamped[0] = sampleTrilinearCPU(dims, decoded, below);
    h.expectNear(clamped[0], decoded[0], 1e-6f, "trilinear: clamped below == corner value");
    const float above[3] = {1000.f, 1000.f, 1000.f};
    clamped[0] = sampleTrilinearCPU(dims, decoded, above);
    h.expectNear(clamped[0], decoded.back(), 1e-6f, "trilinear: clamped above == last value");

    // Linear ramp: interpolation is exact on a linear field.
    std::vector<float> ramp(n * n * n);
    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = float(i % n);
    const float mid[3] = {2.5f, 0.f, 0.f};
    h.expectNear(sampleTrilinearCPU(dims, ramp, mid), 2.5f, 1e-6f, "trilinear: linear field exact");
    const float far[3] = {float(n) - 0.25f, 0.f, 0.f};
    h.expectNear(sampleTrilinearCPU(dims, ramp, far), float(n) - 1.f, 1e-6f, "trilinear: last half-cell saturates");

    // 1-texel mip (last level of the chain): any coordinate returns the single value.
    const uint32_t last = out.layout.mipCount - 1u;
    const std::array<uint32_t, 3> dims1 = {1, 1, 1};
    const std::vector<float> single = {0.125f};
    const float p1[3] = {0.f, 0.f, 0.f};
    const float p2[3] = {100.f, -5.f, 0.5f};
    h.expectNear(sampleTrilinearCPU(dims1, single, p1), 0.125f, 0.f, "trilinear: 1-texel mip at origin");
    h.expectNear(sampleTrilinearCPU(dims1, single, p2), 0.125f, 0.f, "trilinear: 1-texel mip clamped coords");
    h.expectTrue(last >= 1u && out.layout.mips[last].dims[0] == 1u, "trilinear: last mip is 1x1x1");
}

} // namespace

int main()
{
    Harness h;
    testFNV1a(h);
    testMSDFRoundTrip(h);
    testDescriptorLayout(h);
    testVolumeLayout(h);
    testF16(h);
    testSnorm8(h);
    testPooling(h);
    testBuildVolume(h);
    testTransforms(h);
    return h.result();
}
