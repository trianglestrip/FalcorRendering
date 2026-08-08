// =====================================================================================
//  LumenGI - S6-A1 Mesh SDF Builder (standalone CPU tool)
//  -------------------------------------------------------------------------------------
//  Builds a signed distance field (SDF) volume from a static triangle mesh.
//
//  SCOPE / CONSTRAINTS
//  -------------------------------------------------------------------------------------
//  * This is an INDEPENDENT tool. It is NOT registered in any CMake target and NOT
//    referenced by the LumenGI render pass plugin. It links only against the C++
//    standard library (C++17). Do not add Falcor includes to this directory.
//  * It is the CPU reference path for task.md S6-A1. The binary output format
//    (".msdf", see below) is self-describing so that later stages (S6-B1 GPU
//    layout/compression, S6-C1 tests) can consume it without Falcor dependencies.
//
//  SIGN CONVENTION  (project-wide, keep in sync across all LumenGI stages)
//  -------------------------------------------------------------------------------------
//  Signed distances follow the mathematical convention:
//
//        distance > 0  => voxel center is OUTSIDE the mesh
//        distance < 0  => voxel center is INSIDE  the mesh
//        distance = 0  => voxel center lies on the surface
//
//  This is recorded as SignConvention::PositiveOutside in the output header
//  (signConvention == 0). Any later stage that reads .msdf must honor this byte.
//  For meshes that are not watertight (open / non-manifold), the sign is not well
//  defined; the builder still emits a signed field but marks it unreliable
//  (signReliable == 0) and reports warnings - it never silently "fixes" the mesh
//  and never silently falls back.
//
//  DISTANCE FIELD PARAMETERIZATION
//  -------------------------------------------------------------------------------------
//  * The grid covers the mesh bounding box expanded by `paddingWorld` on every side
//    (or a user-provided bounding box override). Voxel centers are sampled at
//    (i + 0.5) * voxelSize offsets; voxelSize is uniform per axis.
//  * `maxResolution` is the voxel count along the longest grid axis; shorter axes
//    get floor(extent / voxelSize) + 1 voxels (never more than maxResolution), so
//    coverage extends to within half a voxel of the padded grid max edge.
//  * Optionally (`normalize == true`, the default) the mesh is uniformly scaled so
//    that its largest extent becomes 1.0. All distances in the output are then in
//    normalized units; multiply by `normalizationScale` (stored in the header) to
//    get world-space distances. With normalize == false, distances are world-space
//    and normalizationScale == 1.
//
//  ALGORITHM (correctness first; naive reference is available)
//  -------------------------------------------------------------------------------------
//  * Unsigned distance: nearest-triangle distance per voxel center. Implemented two
//    ways, both producing bit-identical results:
//      - `useBruteForce == true` : naive O(V*T) loop over every triangle (reference).
//      - `useBruteForce == false`: exact median-split BVH with the same per-triangle
//        math (default; required for 128^3 grids with large meshes).
//  * Sign determination:
//      - Primary: parity ray casting along +X/+Y/+Z with deterministic micro-jitter
//        of ray origin/direction (seeded by build seed + voxel index). Majority of
//        the three axes decides inside/outside. Voxels where the axes disagree are
//        counted in `ambiguousVoxelCount`.
//      - Secondary (informational): nearest-triangle normal dot-product voting on
//        the 8 nearest triangles for voxels within the 4-voxel surface band; counts
//        of disagreement with parity are reported in `voteDisagreementCount` and do
//        NOT affect reliability (SDFs are winding-independent; parity is authoritative
//        for watertight meshes).
//  * Mesh soundness (see analyzeMesh): boundary-edge and non-manifold-edge counts,
//    degenerate triangle detection, connected-component count. Open or non-manifold
//    meshes produce an OpenBoundary / NonManifoldEdges warning and signReliable == 0.
//  * Thin meshes: for inside voxels, local thickness is estimated as the minimum of
//    (first-hit distance in +axis + first-hit distance in -axis) over the three axes.
//    If more than 1% of inside voxels are thinner than 2.5 voxels a ThinMesh warning
//    is emitted (e.g. hollow shells with walls of ~1 voxel).
//
//  CONTENT HASH (for later disk-cache keys, S6-A2)
//  -------------------------------------------------------------------------------------
//  `meshContentHash` is an FNV-1a 64-bit hash over a CANONICAL form of the mesh:
//  one hash per triangle over the 36 raw bytes of its three vertex positions
//  (vertex order within the triangle is significant), then a hash over the sorted
//  list of per-triangle hashes plus the vertex/triangle counts. The sort makes the
//  hash invariant to triangle order and vertex re-ordering (as long as index
//  references are updated accordingly) - i.e. it is a "geometry identity" hash
//  suitable for cache keys. FNV-1a is fast and stable but not collision-resistant;
//  S6-A2 may upgrade to a stronger hash without changing the key schema.
//
//  OUTPUT FORMAT ".msdf" (binary, little-endian, self-describing)
//  -------------------------------------------------------------------------------------
//  Layout (all integers little-endian; floats IEEE-754, target x86-64):
//
//    Offset  Size  Field
//    0       4     magic "MSDF" (0x4D 0x53 0x44 0x46)
//    4       4     formatVersion = 1 (u32)
//    8       4     headerSize = 88 (u32; byte offset of the warnings section)
//    12      12    resolution nx, ny, nz (u32 x3; x is the fastest axis)
//    24      24    gridBBoxMin xyz, gridBBoxMax xyz (f32 x6, OUTPUT space)
//    48      4     voxelSize (f32, output space)
//    52      4     normalizationScale (f32; world = normalized / scale)
//    56      4     paddingWorld (f32, world units)
//    60      1     signConvention (u8; 0 = positive outside, negative inside)
//    61      1     signReliable (u8; 0 = sign ambiguous, treat field as unsupported)
//    62      2     warningCount (u16)
//    64      8     dataOffset (u64; absolute file offset of the distance data)
//    72      8     dataCount (u64; == nx*ny*nz)
//    80      8     checksumOffset (u64; absolute offset of the trailing checksum)
//    88      ...   warnings: warningCount x (u16 length + UTF-8 bytes)
//    ...          distance data: nx*ny*nz x f32, x fastest (row-major xyz)
//    end          checksum: u64 FNV-1a64 over file bytes [0, checksumOffset)
//
//  The trailing checksum covers the whole file up to itself, so any corruption
//  (including the magic/header) is detected on read (S6-A2 cache corruption check).
//
//  OBJ INPUT CONSTRAINTS (parser written from scratch; see parseOBJ)
//  -------------------------------------------------------------------------------------
//  Supported: `v x y z`, `f` with v[/vt][/vn] and 1-based or negative (relative)
//  indices; faces with >= 3 vertices are triangulated as a fan. `vt`, `vn`, `o`,
//  `g`, `s`, `usemtl`, `mtllib`, `l`, `p` and comments are ignored. Vertex
//  normals are NOT used: normals are recomputed from triangle winding.
//  Unsupported (fail with an error, never silently mis-parse): malformed floats,
//  out-of-range face indices, faces with fewer than 3 vertices. Non-finite
//  coordinates are rejected.
// =====================================================================================
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace LumenGI
{
namespace MeshSDF
{

struct Vec3
{
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    float& operator[](int i) { return (&x)[i]; }
    const float& operator[](int i) const { return (&x)[i]; }

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o)
    {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    Vec3& operator-=(const Vec3& o)
    {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
};

inline float dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float lengthSq(const Vec3& a)
{
    return dot(a, a);
}

inline float length(const Vec3& a)
{
    return std::sqrt(lengthSq(a));
}

inline Vec3 normalize(const Vec3& a)
{
    return a / length(a);
}

/// One triangle; indices reference Mesh::positions. No guarantees about validity
/// until validateMesh / analyzeMesh / buildMeshSDF have run.
struct TriangleIndex
{
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
};

struct Mesh
{
    std::vector<Vec3> positions;
    std::vector<TriangleIndex> triangles;
};

enum class WarningCode : uint8_t
{
    Unknown = 0,           ///< Code not recoverable from file (only the message is persisted).
    DegenerateTriangles = 1, ///< Some triangles have (near) zero area; excluded from the field.
    NonManifoldEdges = 2,    ///< Edges shared by > 2 triangles (T-junctions, folded sheets).
    OpenBoundary = 3,        ///< Edges shared by exactly 1 triangle -> mesh is not watertight.
    ThinMesh = 4,            ///< Closed but locally thinner than ~2.5 voxels.
    SignAmbiguity = 5,       ///< Axis parity votes disagreed; sign is unreliable for these voxels.
    NoValidTriangles = 6,    ///< No usable (non-degenerate) triangles -> build refused.
};

/// Stable short name for WarningCode (used in the JSON report and CLI output).
const char* warningCodeName(WarningCode code);

struct Warning
{
    WarningCode code;
    std::string message;
};

/// Result of the topology / soundness analysis of a mesh.
struct MeshAnalysis
{
    size_t vertexCount = 0;
    size_t triangleCount = 0;
    size_t validTriangleCount = 0;    ///< Non-degenerate triangles usable for the field.
    size_t degenerateTriangleCount = 0;
    size_t boundaryEdgeCount = 0;     ///< Edges with exactly 1 incident triangle.
    size_t nonManifoldEdgeCount = 0;  ///< Edges with > 2 incident triangles.
    bool watertight = false;          ///< boundaryEdgeCount == 0 && nonManifoldEdgeCount == 0.
    size_t componentCount = 0;        ///< Connected components over valid triangles.
    std::vector<Warning> warnings;
};

/// Sign convention of the output field (see file header comment).
enum class SignConvention : uint8_t
{
    PositiveOutside = 0, ///< distance > 0 outside, < 0 inside.
};

/// In-memory mirror of the ".msdf" header (see file header comment for layout).
struct SDFHeader
{
    uint32_t formatVersion = 1;
    std::array<uint32_t, 3> resolution = {0, 0, 0};
    std::array<float, 3> bboxMin = {0.f, 0.f, 0.f};
    std::array<float, 3> bboxMax = {0.f, 0.f, 0.f};
    float voxelSize = 0.f;
    float normalizationScale = 1.f;
    float paddingWorld = 0.f;
    SignConvention signConvention = SignConvention::PositiveOutside;
    bool signReliable = false;
    uint64_t dataCount = 0;
};

struct SDFBuildParams
{
    /// Voxel count along the longest grid axis (min 2). Used when fixedResolution is all zero.
    uint32_t maxResolution = 128;
    /// Per-axis voxel counts; if any entry is non-zero, this overrides maxResolution
    /// (voxelSize = max_i(extent_i / (R_i - 1)), actual per-axis counts <= R_i).
    std::array<uint32_t, 3> fixedResolution = {0, 0, 0};
    /// Extra margin around the mesh bbox in world units. 0 = auto (0.1 * max extent).
    float paddingWorld = 0.f;
    /// World-space grid bounds override {minX,minY,minZ,maxX,maxY,maxZ}; padding ignored.
    std::optional<std::array<float, 6>> bboxOverride;
    /// Uniformly scale the mesh so its largest extent becomes 1 (see file header).
    bool normalize = true;
    /// Deterministic seed for ray-jitter. Same input + seed -> bit-identical output.
    uint64_t seed = 0;
    /// Use the naive O(V*T) reference distance pass instead of the BVH.
    bool useBruteForce = false;
};

struct SDFBuildResult
{
    SDFHeader header;
    /// nx*ny*nz signed distances, x fastest. Convention: PositiveOutside.
    std::vector<float> distances;
    MeshAnalysis analysis;
    uint64_t contentHash = 0;       ///< meshContentHash of the input mesh.
    std::string contentHashHex;     ///< Lower-case hex of contentHash.
    std::vector<Warning> warnings;  ///< analysis + grid warnings, deduplicated.
    size_t ambiguousVoxelCount = 0; ///< Voxels where the 3 axis parity votes disagreed.
    float ambiguousRatio = 0.f;     ///< ambiguousVoxelCount / totalVoxels.
    size_t voteDisagreementCount = 0; ///< Surface-band voxels where normal vote != parity sign.
    size_t thinVoxelCount = 0;        ///< Inside voxels thinner than 2.5 voxels.
    // Timings in milliseconds (filled by buildMeshSDF).
    double analysisMs = 0.0;
    double hashMs = 0.0;
    double distanceMs = 0.0;
    double totalMs = 0.0;
};

// -------------------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------------------

/// Minimal Wavefront OBJ parser (see file header for supported subset and errors).
/// On failure returns false and sets `err`; on success fills `mesh`.
bool parseOBJ(std::istream& in, Mesh& mesh, std::string& err);

/// Topology / soundness analysis: edge manifoldness, degenerate triangles,
/// watertightness, connected components. Cheap; call before building.
MeshAnalysis analyzeMesh(const Mesh& mesh);

/// FNV-1a 64-bit hash over raw bytes (offset basis by default).
uint64_t fnv1a64(const void* data, size_t size);

/// Canonical geometry-identity hash (see file header comment).
uint64_t meshContentHash(const Mesh& mesh);

/// Lower-case 16-digit hex of a 64-bit value.
std::string toHex(uint64_t value);

/// Build the signed distance field. On failure returns false and sets `err`.
/// `out.header`/`out.distances`/`out.warnings` are only valid on success.
bool buildMeshSDF(const Mesh& mesh, const SDFBuildParams& params, SDFBuildResult& out, std::string& err);

/// Serialize a successful build to ".msdf" (see file header for the layout).
bool writeMSDF(const std::filesystem::path& path, const SDFBuildResult& result, std::string& err);

/// Read back a ".msdf" file; verifies magic/version/checksum. On failure returns false.
bool readMSDF(
    const std::filesystem::path& path,
    SDFHeader& header,
    std::vector<Warning>& warnings,
    std::vector<float>& distances,
    std::string& err
);

/// Hand-rolled minimal JSON report (no external library). Deterministic field order.
bool writeJSONReport(const std::filesystem::path& path, const SDFBuildResult& result, std::string& err);

} // namespace MeshSDF
} // namespace LumenGI
