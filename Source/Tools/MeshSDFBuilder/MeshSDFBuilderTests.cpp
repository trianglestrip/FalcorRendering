// =====================================================================================
//  MeshSDFBuilderTests.cpp - self-contained CPU tests for the S6-A1 Mesh SDF builder.
//  -------------------------------------------------------------------------------------
//  No test framework, no Falcor, no CMake target: plain asserts + a main() that
//  prints [PASS]/[FAIL] lines and returns a non-zero exit code on any failure.
//
//  BUILD (from this directory)
//  -------------------------------------------------------------------------------------
//  MSVC:   cl /nologo /std:c++17 /O2 /EHsc /W4 MeshSDFBuilder.cpp MeshSDFBuilderTests.cpp /Fe:MeshSDFBuilderTests.exe
//  GCC:    g++ -std=c++17 -O2 MeshSDFBuilder.cpp MeshSDFBuilderTests.cpp -o MeshSDFBuilderTests
//
//  COVERAGE (maps to task.md S6-C1)
//  -------------------------------------------------------------------------------------
//  1. FNV-1a hash basics (offset basis, determinism, sensitivity)
//  2. Canonical mesh content hash (order invariance, perturbation sensitivity)
//  3. OBJ parser: quads, negative indices, vn/vt ignored, error cases
//  4. Cube: exact analytic signed distance, sign convention, watertight, no warnings
//  5. Sphere: analytic distance within tessellation error, signs, watertight
//  6. Open plane: detected as open -> warnings + signReliable == false
//  7. Thin closed shell: ThinMesh warning
//  8. Degenerate / empty input handling
//  9. BVH == brute-force reference (bit-identical)
//  10. Serialization round-trip + corruption detection
//  11. bbox override and resolution parameterization
//  12. Determinism across builds
// =====================================================================================

#include "MeshSDFBuilder.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
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
        if (cond)
        {
            ++pass;
            std::cout << "[PASS] " << what << "\n";
        }
        else
        {
            ++fail;
            std::cout << "[FAIL] " << what << "\n";
        }
    }

    void expectEq(uint64_t a, uint64_t b, const std::string& what) { expect(a == b, what); }
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

// -------------------------------------------------------------------------------------
// Test mesh generators
// -------------------------------------------------------------------------------------

Mesh makeCube(float half)
{
    Mesh m;
    const float h = half;
    const Vec3 v[8] = {
        {-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
        {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h},
    };
    for (const Vec3& p : v)
        m.positions.push_back(p);
    const uint32_t f[12][3] = {
        {0, 1, 2}, {0, 2, 3}, // -z
        {5, 4, 7}, {5, 7, 6}, // +z
        {4, 0, 3}, {4, 3, 7}, // -x
        {1, 5, 6}, {1, 6, 2}, // +x
        {3, 2, 6}, {3, 6, 7}, // +y
        {4, 5, 1}, {4, 1, 0}, // -y
    };
    for (const auto& t : f)
        m.triangles.push_back({t[0], t[1], t[2]});
    return m;
}

// UV sphere with apex poles (no degenerate triangles), y-up.
Mesh makeSphere(float radius, int stacks, int slices)
{
    Mesh m;
    const uint32_t top = uint32_t(m.positions.size());
    m.positions.push_back({0.f, radius, 0.f});
    const uint32_t bot = uint32_t(m.positions.size());
    m.positions.push_back({0.f, -radius, 0.f});

    constexpr float kPi = 3.14159265358979323846f;
    for (int s = 1; s < stacks; ++s)
    {
        const float phi = kPi * float(s) / float(stacks);
        const float sy = std::cos(phi);
        const float sr = std::sin(phi);
        for (int t = 0; t < slices; ++t)
        {
            const float th = 2.f * kPi * float(t) / float(slices);
            m.positions.push_back({radius * sr * std::cos(th), radius * sy, radius * sr * std::sin(th)});
        }
    }
    const uint32_t ring0 = 2; // first ring index

    const auto ring = [&](int s, int t) -> uint32_t { return ring0 + uint32_t(s) * uint32_t(slices) + uint32_t(t); };
    const auto next = [&](int t) -> int { return (t + 1) % slices; };

    for (int t = 0; t < slices; ++t)
    {
        m.triangles.push_back({top, ring(0, t), ring(0, next(t))});
    }
    for (int s = 0; s < stacks - 2; ++s)
    {
        for (int t = 0; t < slices; ++t)
        {
            const uint32_t a = ring(s, t);
            const uint32_t b = ring(s + 1, t);
            const uint32_t c = ring(s + 1, next(t));
            const uint32_t d = ring(s, next(t));
            m.triangles.push_back({a, b, c});
            m.triangles.push_back({a, c, d});
        }
    }
    for (int t = 0; t < slices; ++t)
    {
        m.triangles.push_back({bot, ring(stacks - 2, next(t)), ring(stacks - 2, t)});
    }
    return m;
}

Mesh makeOpenPlane()
{
    Mesh m;
    m.positions.push_back({-1.f, -1.f, 0.f});
    m.positions.push_back({1.f, -1.f, 0.f});
    m.positions.push_back({1.f, 1.f, 0.f});
    m.positions.push_back({-1.f, 1.f, 0.f});
    m.triangles.push_back({0, 1, 2});
    m.triangles.push_back({0, 2, 3});
    return m;
}

// Closed box with thin walls: outer box [-1,1]^3, inner box [-0.95,0.95]^3.
Mesh makeThinShell(float outer, float inner)
{
    Mesh outMesh;
    auto addBox = [&](float h) {
        const uint32_t base = uint32_t(outMesh.positions.size());
        const Vec3 v[8] = {
            {-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
            {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h},
        };
        for (const Vec3& p : v)
            outMesh.positions.push_back(p);
        const uint32_t f[12][3] = {
            {0, 1, 2}, {0, 2, 3}, {5, 4, 7}, {5, 7, 6}, {4, 0, 3}, {4, 3, 7},
            {1, 5, 6}, {1, 6, 2}, {3, 2, 6}, {3, 6, 7}, {4, 5, 1}, {4, 1, 0},
        };
        for (const auto& t : f)
            outMesh.triangles.push_back({base + t[0], base + t[1], base + t[2]});
    };
    addBox(outer);
    addBox(inner);
    return outMesh;
}

// Deterministic LCG "random" mesh (no exact degenerate triangles).
Mesh makeRandomMesh(uint32_t seed, uint32_t triCount)
{
    Mesh m;
    uint64_t st = seed;
    const auto unit = [&]() -> float {
        st = st * 6364136223846793005ULL + 1442695040888963407ULL;
        return float(st >> 40) * (1.0f / 16777216.0f);
    };
    const uint32_t vertexCount = triCount + 8;
    for (uint32_t i = 0; i < vertexCount; ++i)
        m.positions.push_back({unit(), unit(), unit()});
    for (uint32_t i = 0; i < triCount; ++i)
    {
        const uint32_t a = uint32_t(unit() * float(vertexCount)) % vertexCount;
        const uint32_t b = uint32_t(unit() * float(vertexCount)) % vertexCount;
        const uint32_t c = uint32_t(unit() * float(vertexCount)) % vertexCount;
        m.triangles.push_back({a, b, c});
    }
    return m;
}

// Exact signed distance to an axis-aligned box (solid).
float boxSdf(const Vec3& p, const Vec3& h)
{
    const float dx = std::fabs(p.x) - h.x;
    const float dy = std::fabs(p.y) - h.y;
    const float dz = std::fabs(p.z) - h.z;
    const float m = std::max(dx, std::max(dy, dz));
    const float outside = std::sqrt(
        std::max(dx, 0.f) * std::max(dx, 0.f) + std::max(dy, 0.f) * std::max(dy, 0.f) +
        std::max(dz, 0.f) * std::max(dz, 0.f)
    );
    return outside + std::min(m, 0.f);
}

bool hasWarning(const std::vector<Warning>& warnings, WarningCode code)
{
    for (const Warning& w : warnings)
    {
        if (w.code == code)
            return true;
    }
    return false;
}

// -------------------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------------------

void test_fnv1a_basics(Harness& h)
{
    uint64_t empty = fnv1a64("", 0);
    h.expectEq(empty, 0xcbf29ce484222325ULL, "fnv1a: empty input == offset basis");
    h.expect(fnv1a64("a", 1) != fnv1a64("b", 1), "fnv1a: single-byte change alters the hash");
    const char* s1 = "The quick brown fox";
    h.expect(fnv1a64(s1, std::strlen(s1)) == fnv1a64(s1, std::strlen(s1)), "fnv1a: deterministic");
}

void test_mesh_content_hash(Harness& h)
{
    Mesh cube = makeCube(1.f);
    const uint64_t base = meshContentHash(cube);
    h.expectEq(base, meshContentHash(cube), "hash: stable across calls");

    Mesh permuted = cube;
    // Reorder triangles.
    std::vector<TriangleIndex> tris = permuted.triangles;
    for (size_t i = 0; i < tris.size() / 2; ++i)
        std::swap(tris[i], tris[tris.size() - 1 - i]);
    permuted.triangles = tris;
    h.expectEq(base, meshContentHash(permuted), "hash: invariant to triangle order");

    Mesh remapped = cube;
    // Swap vertices 0 and 1 and fix up indices: content hash must not change.
    std::swap(remapped.positions[0], remapped.positions[1]);
    for (TriangleIndex& t : remapped.triangles)
    {
        if (t.a == 0)
            t.a = 1;
        else if (t.a == 1)
            t.a = 0;
        if (t.b == 0)
            t.b = 1;
        else if (t.b == 1)
            t.b = 0;
        if (t.c == 0)
            t.c = 1;
        else if (t.c == 1)
            t.c = 0;
    }
    h.expectEq(base, meshContentHash(remapped), "hash: invariant to vertex re-ordering");

    Mesh perturbed = cube;
    perturbed.positions[3].x += 0.001f;
    h.expect(base != meshContentHash(perturbed), "hash: sensitive to geometry changes");

    Mesh extra = cube;
    extra.positions.push_back({5.f, 5.f, 5.f});
    h.expect(base != meshContentHash(extra), "hash: sensitive to vertex count");
}

void test_obj_parser(Harness& h)
{
    // Quad face -> 2 triangles, fan triangulation.
    {
        std::istringstream in("# comment\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n");
        Mesh m;
        std::string err;
        h.expect(parseOBJ(in, m, err), "obj: quad parses");
        h.expectEq(uint64_t(m.positions.size()), 4, "obj: 4 vertices");
        h.expectEq(uint64_t(m.triangles.size()), 2, "obj: quad -> 2 triangles");
    }
    // Negative indices and vn/vt.
    {
        std::istringstream in("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0.5 0.5\nvn 0 0 1\nf -3/-1/-1 -2 -1\n");
        Mesh m;
        std::string err;
        h.expect(parseOBJ(in, m, err), "obj: negative indices + vt/vn parse");
        h.expectEq(uint64_t(m.triangles.size()), 1, "obj: one triangle");
        h.expect(m.triangles[0].a == 0 && m.triangles[0].b == 1 && m.triangles[0].c == 2, "obj: resolved indices");
    }
    // Out-of-range index is a hard error.
    {
        std::istringstream in("v 0 0 0\nv 1 0 0\nf 1 2 99\n");
        Mesh m;
        std::string err;
        h.expect(!parseOBJ(in, m, err), "obj: out-of-range index rejected");
    }
    // Malformed float.
    {
        std::istringstream in("v 0 0\n");
        Mesh m;
        std::string err;
        h.expect(!parseOBJ(in, m, err), "obj: malformed vertex rejected");
    }
    // Vertices without faces.
    {
        std::istringstream in("v 0 0 0\nv 1 0 0\nv 0 1 0\n");
        Mesh m;
        std::string err;
        h.expect(!parseOBJ(in, m, err), "obj: no faces rejected");
    }
    // Non-finite coordinate.
    {
        std::istringstream in("v nan 0 0\nf 1 1 1\n");
        Mesh m;
        std::string err;
        h.expect(!parseOBJ(in, m, err), "obj: non-finite vertex rejected");
    }
}

void test_cube_analytic(Harness& h)
{
    Mesh cube = makeCube(1.f);
    SDFBuildParams p;
    p.maxResolution = 33;
    p.paddingWorld = 0.125f;
    p.normalize = false;
    p.seed = 1234;

    SDFBuildResult r;
    std::string err;
    h.expect(buildMeshSDF(cube, p, r, err), "cube: build succeeds");
    if (!r.distances.empty())
    {
        h.expect(r.analysis.watertight, "cube: watertight");
        h.expectEq(uint64_t(r.analysis.boundaryEdgeCount), 0, "cube: no boundary edges");
        h.expect(r.header.signReliable, "cube: sign reliable");
        h.expectEq(uint64_t(r.ambiguousVoxelCount), 0, "cube: no ambiguous voxels");
        h.expect(r.warnings.empty(), "cube: no warnings");
        h.expectEq(r.header.resolution[0], 33, "cube: max axis resolution == 33");
        h.expectNear(r.header.voxelSize, 2.25f / 32.f, 1e-6f, "cube: voxel size");
        h.expectNear(r.header.normalizationScale, 1.f, 0.f, "cube: no normalization");

        const Vec3 gmin = {r.header.bboxMin[0], r.header.bboxMin[1], r.header.bboxMin[2]};
        const float vs = r.header.voxelSize;
        const Vec3 hbox = {1.f, 1.f, 1.f};
        size_t outside = 0;
        size_t inside = 0;
        float maxErr = 0.f;
        for (uint32_t z = 0; z < r.header.resolution[2]; ++z)
        {
            for (uint32_t y = 0; y < r.header.resolution[1]; ++y)
            {
                for (uint32_t x = 0; x < r.header.resolution[0]; ++x)
                {
                    const Vec3 c = {
                        gmin.x + (float(x) + 0.5f) * vs,
                        gmin.y + (float(y) + 0.5f) * vs,
                        gmin.z + (float(z) + 0.5f) * vs,
                    };
                    const float analytic = boxSdf(c, hbox);
                    const size_t idx = size_t(z) * r.header.resolution[1] * r.header.resolution[0] +
                        size_t(y) * r.header.resolution[0] + size_t(x);
                    const float got = r.distances[idx];
                    maxErr = std::max(maxErr, std::fabs(got - analytic));
                    if (analytic > 0.f)
                    {
                        ++outside;
                        h.expect(got > -1e-4f, "cube: outside voxel is positive");
                    }
                    else
                    {
                        ++inside;
                        h.expect(got < 1e-4f, "cube: inside voxel is negative");
                    }
                }
            }
        }
        h.expect(outside > 0 && inside > 0, "cube: both inside and outside voxels exist");
        h.expect(maxErr < 1e-4f, "cube: max |computed - analytic| < 1e-4");
    }
}

void test_sphere(Harness& h)
{
    Mesh sphere = makeSphere(1.f, 12, 24);
    SDFBuildParams p;
    p.maxResolution = 24;
    p.paddingWorld = 0.15f;
    p.normalize = false;
    p.seed = 7;

    SDFBuildResult r;
    std::string err;
    h.expect(buildMeshSDF(sphere, p, r, err), "sphere: build succeeds");
    if (!r.distances.empty())
    {
        h.expect(r.analysis.watertight, "sphere: watertight");
        h.expect(r.ambiguousVoxelCount < 8, "sphere: (near) no ambiguous voxels");
        h.expect(!hasWarning(r.warnings, WarningCode::OpenBoundary), "sphere: no open-boundary warning");
        h.expect(!hasWarning(r.warnings, WarningCode::ThinMesh), "sphere: no thin-mesh warning");

        const Vec3 gmin = {r.header.bboxMin[0], r.header.bboxMin[1], r.header.bboxMin[2]};
        const float vs = r.header.voxelSize;
        float maxErr = 0.f;
        size_t outside = 0;
        size_t inside = 0;
        for (uint32_t z = 0; z < r.header.resolution[2]; ++z)
        {
            for (uint32_t y = 0; y < r.header.resolution[1]; ++y)
            {
                for (uint32_t x = 0; x < r.header.resolution[0]; ++x)
                {
                    const Vec3 c = {
                        gmin.x + (float(x) + 0.5f) * vs,
                        gmin.y + (float(y) + 0.5f) * vs,
                        gmin.z + (float(z) + 0.5f) * vs,
                    };
                    const float radius = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
                    const float analytic = radius - 1.f;
                    const size_t idx = size_t(z) * r.header.resolution[1] * r.header.resolution[0] +
                        size_t(y) * r.header.resolution[0] + size_t(x);
                    const float got = r.distances[idx];
                    maxErr = std::max(maxErr, std::fabs(got - analytic));
                    if (radius > 1.2f)
                    {
                        ++outside;
                        h.expect(got > 0.f, "sphere: clearly-outside voxel is positive");
                    }
                    else if (radius < 0.8f)
                    {
                        ++inside;
                        h.expect(got < 0.f, "sphere: clearly-inside voxel is negative");
                    }
                }
            }
        }
        h.expect(outside > 0 && inside > 0, "sphere: both inside and outside voxels exist");
        h.expect(maxErr < 0.02f, "sphere: max distance error < 0.02 (tessellation + voxelization)");
    }
}

void test_open_plane(Harness& h)
{
    Mesh plane = makeOpenPlane();
    MeshAnalysis a = analyzeMesh(plane);
    h.expect(!a.watertight, "plane: not watertight");
    h.expectEq(uint64_t(a.boundaryEdgeCount), 4, "plane: 4 boundary edges");
    h.expect(hasWarning(a.warnings, WarningCode::OpenBoundary), "plane: OpenBoundary warning");

    SDFBuildParams p;
    p.maxResolution = 16;
    p.paddingWorld = 0.2f;
    p.normalize = false;
    p.seed = 42;
    SDFBuildResult r;
    std::string err;
    h.expect(buildMeshSDF(plane, p, r, err), "plane: build succeeds (flagged, not silent)");
    h.expect(!r.header.signReliable, "plane: sign marked unreliable");
    h.expect(!r.distances.empty(), "plane: field still produced for inspection");
}

void test_thin_shell(Harness& h)
{
    // Wall thickness 0.3 world units vs voxelSize 0.15 (res 16, pad 0.125) => ~2 voxels.
    Mesh shell = makeThinShell(1.f, 0.7f);
    MeshAnalysis a = analyzeMesh(shell);
    h.expect(a.watertight, "shell: watertight");

    SDFBuildParams p;
    p.maxResolution = 16;
    p.paddingWorld = 0.125f;
    p.normalize = false;
    p.seed = 99;
    SDFBuildResult r;
    std::string err;
    h.expect(buildMeshSDF(shell, p, r, err), "shell: build succeeds");
    h.expect(hasWarning(r.warnings, WarningCode::ThinMesh), "shell: ThinMesh warning");
    h.expect(r.thinVoxelCount > 0, "shell: thin voxels detected");
}

void test_degenerate_and_empty(Harness& h)
{
    // Single degenerate triangle (repeated vertex).
    {
        Mesh m;
        m.positions.push_back({0.f, 0.f, 0.f});
        m.positions.push_back({1.f, 0.f, 0.f});
        m.triangles.push_back({0, 0, 1});
        MeshAnalysis a = analyzeMesh(m);
        h.expectEq(uint64_t(a.degenerateTriangleCount), 1, "degenerate: detected");
        SDFBuildResult r;
        std::string err;
        h.expect(!buildMeshSDF(m, SDFBuildParams{}, r, err), "degenerate: build refused (no valid triangles)");
    }
    // Empty mesh.
    {
        Mesh m;
        SDFBuildResult r;
        std::string err;
        h.expect(!buildMeshSDF(m, SDFBuildParams{}, r, err), "empty: build refused");
    }
    // Triangle with out-of-range index.
    {
        Mesh m;
        m.positions.push_back({0.f, 0.f, 0.f});
        m.triangles.push_back({0, 1, 2});
        SDFBuildResult r;
        std::string err;
        h.expect(!buildMeshSDF(m, SDFBuildParams{}, r, err), "bad index: build refused");
    }
}

void test_bvh_equals_brute_force(Harness& h)
{
    Mesh m = makeRandomMesh(20260809u, 200);
    SDFBuildParams p;
    p.maxResolution = 12;
    p.paddingWorld = 0.05f;
    p.normalize = false;
    p.seed = 5;

    SDFBuildResult ref;
    SDFBuildResult acc;
    std::string err;
    h.expect(buildMeshSDF(m, p, ref, err), "bvh-vs-brute: reference build");
    p.useBruteForce = true;
    h.expect(buildMeshSDF(m, p, acc, err), "bvh-vs-brute: brute-force build");
    h.expect(ref.distances.size() == acc.distances.size(), "bvh-vs-brute: same voxel count");
    bool identical = ref.distances.size() == acc.distances.size() &&
        std::memcmp(ref.distances.data(), acc.distances.data(), ref.distances.size() * sizeof(float)) == 0;
    h.expect(identical, "bvh-vs-brute: bit-identical distances");
    h.expectEq(ref.ambiguousVoxelCount, acc.ambiguousVoxelCount, "bvh-vs-brute: identical ambiguity stats");
}

void test_serialization(Harness& h)
{
    Mesh cube = makeCube(1.f);
    SDFBuildParams p;
    p.maxResolution = 16;
    p.paddingWorld = 0.125f;
    p.normalize = false;
    p.seed = 3;
    SDFBuildResult r;
    std::string err;
    h.expect(buildMeshSDF(cube, p, r, err), "serialize: build succeeds");

    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path file = dir / "msdf_test_roundtrip.msdf";
    const std::filesystem::path json = dir / "msdf_test_roundtrip.json";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::filesystem::remove(json, ec);

    h.expect(writeMSDF(file, r, err), "serialize: write .msdf");
    h.expect(writeJSONReport(json, r, err), "serialize: write .json");

    SDFHeader hdr;
    std::vector<Warning> warnings;
    std::vector<float> data;
    h.expect(readMSDF(file, hdr, warnings, data, err), "serialize: read back");
    if (!data.empty())
    {
        h.expect(hdr.resolution == r.header.resolution, "serialize: resolution round-trips");
        h.expect(hdr.signReliable == r.header.signReliable, "serialize: signReliable round-trips");
        h.expect(hdr.voxelSize == r.header.voxelSize, "serialize: voxelSize round-trips");
        h.expect(data.size() == r.distances.size(), "serialize: data count round-trips");
        const bool identical = data.size() == r.distances.size() &&
            std::memcmp(data.data(), r.distances.data(), data.size() * sizeof(float)) == 0;
        h.expect(identical, "serialize: distance data bit-identical");
        h.expectEq(uint64_t(warnings.size()), uint64_t(r.warnings.size()), "serialize: warnings round-trip");
    }

    // Corruption detection: flip one byte in the data region.
    {
        std::ifstream in(file, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        h.expect(!bytes.empty(), "serialize: file is non-empty");
        if (!bytes.empty())
        {
            const size_t dataOffset = bytes.size() > 200 ? 100 : 0; // inside header/data area, before checksum
            bytes[dataOffset] ^= 0xFF;
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.close();
            SDFHeader badHdr;
            std::vector<Warning> badWarnings;
            std::vector<float> badData;
            std::string badErr;
            h.expect(!readMSDF(file, badHdr, badWarnings, badData, badErr), "serialize: corrupted file rejected");
        }
    }

    // Corrupt magic.
    {
        std::ifstream in(file, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!bytes.empty())
        {
            bytes[0] = 'X';
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.close();
            SDFHeader badHdr;
            std::vector<Warning> badWarnings;
            std::vector<float> badData;
            std::string badErr;
            h.expect(!readMSDF(file, badHdr, badWarnings, badData, badErr), "serialize: bad magic rejected");
        }
    }

    std::filesystem::remove(file, ec);
    std::filesystem::remove(json, ec);
}

void test_bbox_override_and_resolution(Harness& h)
{
    Mesh cube = makeCube(0.5f); // small mesh, big grid
    SDFBuildParams p;
    p.fixedResolution = {10, 10, 10};
    p.bboxOverride = std::array<float, 6>{-2.f, -2.f, -2.f, 2.f, 2.f, 2.f};
    p.normalize = false;
    SDFBuildResult r;
    std::string err;
    h.expect(buildMeshSDF(cube, p, r, err), "bbox: build succeeds");
    h.expectEq(r.header.resolution[0], 10, "bbox: fixed resolution honored (x)");
    h.expectEq(r.header.resolution[1], 10, "bbox: fixed resolution honored (y)");
    h.expectEq(r.header.resolution[2], 10, "bbox: fixed resolution honored (z)");
    h.expectNear(r.header.voxelSize, 4.f / 9.f, 1e-6f, "bbox: voxel size from override bounds");
    h.expectNear(r.header.bboxMin[0], -2.f, 0.f, "bbox: grid min from override");
    h.expectNear(r.header.bboxMax[2], 2.f, 0.f, "bbox: grid max from override");

    // Default (single-N) parameterization: longest axis uses N voxels.
    SDFBuildParams p2;
    p2.maxResolution = 31;
    p2.paddingWorld = 0.f; // auto = 0.1 * maxExtent
    SDFBuildResult r2;
    h.expect(buildMeshSDF(cube, p2, r2, err), "bbox: default parameterization builds");
    const uint32_t maxAxis = std::max(r2.header.resolution[0], std::max(r2.header.resolution[1], r2.header.resolution[2]));
    h.expectEq(maxAxis, 31, "bbox: longest axis uses maxResolution");
}

void test_determinism(Harness& h)
{
    Mesh sphere = makeSphere(1.f, 10, 20);
    SDFBuildParams p;
    p.maxResolution = 20;
    p.paddingWorld = 0.1f;
    p.normalize = false;
    p.seed = 11;
    SDFBuildResult a, b;
    std::string err;
    h.expect(buildMeshSDF(sphere, p, a, err), "determinism: first build");
    h.expect(buildMeshSDF(sphere, p, b, err), "determinism: second build");
    h.expect(a.distances == b.distances, "determinism: bit-identical distances");
    h.expectEq(a.contentHash, b.contentHash, "determinism: identical content hash");
    h.expectEq(a.ambiguousVoxelCount, b.ambiguousVoxelCount, "determinism: identical ambiguity stats");
}

void test_json_report(Harness& h)
{
    Mesh cube = makeCube(1.f);
    SDFBuildParams p;
    p.maxResolution = 12;
    p.paddingWorld = 0.1f;
    p.normalize = false;
    SDFBuildResult r;
    std::string err;
    h.expect(buildMeshSDF(cube, p, r, err), "json: build succeeds");
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "msdf_test_report.json";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    h.expect(writeJSONReport(file, r, err), "json: report written");
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();
    h.expect(content.find("\"tool\": \"MeshSDFBuilder\"") != std::string::npos, "json: tool field");
    h.expect(content.find("\"contentHash\"") != std::string::npos, "json: contentHash field");
    h.expect(content.find("\"signReliable\"") != std::string::npos, "json: signReliable field");
    std::filesystem::remove(file, ec);
}

} // namespace

int main()
{
    Harness h;
    test_fnv1a_basics(h);
    test_mesh_content_hash(h);
    test_obj_parser(h);
    test_cube_analytic(h);
    test_sphere(h);
    test_open_plane(h);
    test_thin_shell(h);
    test_degenerate_and_empty(h);
    test_bvh_equals_brute_force(h);
    test_serialization(h);
    test_bbox_override_and_resolution(h);
    test_determinism(h);
    test_json_report(h);
    return h.result();
}
