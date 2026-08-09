// MeshSDFBuilder.cpp - implementation of the S6-A1 Mesh SDF builder.
// See MeshSDFBuilder.h for the full contract, sign convention, output format
// and algorithm documentation.
#include "MeshSDFBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace LumenGI
{
namespace MeshSDF
{
namespace
{

constexpr char kMagic[4] = {'M', 'S', 'D', 'F'};
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kHeaderSize = 88;
constexpr uint32_t kInvalidNode = 0xFFFFFFFFu;
constexpr float kDegenerateEpsRel = 1e-14f; // (area / maxEdgeLen)^2 below this => degenerate
constexpr float kSurfaceBandVoxels = 4.f;   // sign-vote band width, in voxels
constexpr float kVoteRadiusVoxels = 6.f;    // candidate collection radius, in voxels
constexpr float kVoteMaxTriangles = 8;      // nearest triangles used by the normal vote
constexpr float kThinThresholdVoxels = 2.5f;
constexpr float kThinFractionThreshold = 0.01f;
constexpr float kJitterScale = 1e-3f;       // ray origin/direction jitter, in voxels / radians

const Vec3 kAxisDir[3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
const Vec3 kAxisPerpU[3] = {{0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {1.f, 0.f, 0.f}};
const Vec3 kAxisPerpV[3] = {{0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}};

// FNV-1a 64 with an explicit seed (internal helper).
inline uint64_t fnv1a64Seed(const void* data, size_t size, uint64_t h)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Deterministic pseudo-random splitmix64 (state is mutated).
inline uint64_t splitmix64(uint64_t& state)
{
    state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

inline float randUnit(uint64_t& state)
{
    return float(splitmix64(state) >> 40) * (1.0f / 16777216.0f); // [0,1)
}

bool isFinite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// -------------------------------------------------------------------------------------
// Triangle math
// -------------------------------------------------------------------------------------

// Closest point on triangle abc to point p (Ericson, Real-Time Collision Detection).
// Returns squared distance. Requires a non-degenerate triangle.
float distPointTriangleSq(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, Vec3& q)
{
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f)
    {
        q = a;
        return dot(ap, ap);
    }
    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3)
    {
        q = b;
        return dot(bp, bp);
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
    {
        const float v = d1 / (d1 - d3);
        q = a + ab * v;
        const Vec3 vp = p - q;
        return dot(vp, vp);
    }
    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6)
    {
        q = c;
        return dot(cp, cp);
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
    {
        const float w = d2 / (d2 - d6);
        q = a + ac * w;
        const Vec3 vp = p - q;
        return dot(vp, vp);
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        q = b + (c - b) * w;
        const Vec3 vp = p - q;
        return dot(vp, vp);
    }
    const float denom = va + vb + vc;
    if (denom <= 0.f)
    {
        q = a;
        return dot(ap, ap);
    }
    const float inv = 1.f / denom;
    const float v = vb * inv;
    const float w = vc * inv;
    q = a + ab * v + ac * w;
    const Vec3 vp = p - q;
    return dot(vp, vp);
}

// Moller-Trumbore, double-sided, parametric range (tMin, tMax].
bool rayTriangleMT(
    const Vec3& origin,
    const Vec3& dir,
    const Vec3& a,
    const Vec3& b,
    const Vec3& c,
    float tMin,
    float tMax,
    float& tOut
)
{
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 pv = cross(dir, e2);
    const float det = dot(e1, pv);
    if (std::abs(det) < 1e-12f)
        return false;
    const float inv = 1.f / det;
    const Vec3 tv = origin - a;
    const float u = dot(tv, pv) * inv;
    if (u < 0.f || u > 1.f)
        return false;
    const Vec3 qv = cross(tv, e1);
    const float v = dot(dir, qv) * inv;
    if (v < 0.f || u + v > 1.f)
        return false;
    const float t = dot(e2, qv) * inv;
    if (t <= tMin || t > tMax)
        return false;
    tOut = t;
    return true;
}

// Slab test; true if the ray hits the AABB at some t in [0, tMax].
bool rayAABB(const Vec3& origin, const Vec3& dir, const Vec3& lo, const Vec3& hi, float tMax)
{
    float tMin = 0.f;
    for (int i = 0; i < 3; ++i)
    {
        const float oi = origin[i];
        const float di = dir[i];
        if (std::abs(di) < 1e-30f)
        {
            if (oi < lo[i] || oi > hi[i])
                return false;
        }
        else
        {
            const float inv = 1.f / di;
            float t1 = (lo[i] - oi) * inv;
            float t2 = (hi[i] - oi) * inv;
            if (t1 > t2)
                std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax)
                return false;
        }
    }
    return true;
}

float aabbDistSq(const Vec3& p, const Vec3& lo, const Vec3& hi)
{
    float dx = 0.f;
    float dy = 0.f;
    float dz = 0.f;
    if (p.x < lo.x)
        dx = lo.x - p.x;
    else if (p.x > hi.x)
        dx = p.x - hi.x;
    if (p.y < lo.y)
        dy = lo.y - p.y;
    else if (p.y > hi.y)
        dy = p.y - hi.y;
    if (p.z < lo.z)
        dz = lo.z - p.z;
    else if (p.z > hi.z)
        dz = p.z - hi.z;
    return dx * dx + dy * dy + dz * dz;
}

// -------------------------------------------------------------------------------------
// BVH over valid (non-degenerate) triangles. Exact queries only: results are
// bit-identical to the brute-force reference because the same per-triangle math
// is used and the nearest triangle is the same.
// -------------------------------------------------------------------------------------

class Bvh
{
public:
    Bvh(const Mesh& mesh, const std::vector<uint32_t>& validTris) : m_mesh(mesh), m_tris(validTris)
    {
        m_order.resize(validTris.size());
        m_centroids.resize(validTris.size());
        for (size_t i = 0; i < validTris.size(); ++i)
        {
            m_order[i] = uint32_t(i);
            const TriangleIndex& t = mesh.triangles[validTris[i]];
            m_centroids[i] = (mesh.positions[t.a] + mesh.positions[t.b] + mesh.positions[t.c]) * (1.f / 3.f);
        }
        if (!m_tris.empty())
            build(0, uint32_t(m_tris.size()));
    }

    bool empty() const { return m_tris.empty(); }

    /// Squared distance to the nearest triangle; outTri receives the index into Mesh::triangles.
    float nearestSq(const Vec3& p, uint32_t& outTri) const
    {
        float best = std::numeric_limits<float>::infinity();
        uint32_t bestTri = 0;
        std::vector<uint32_t> stack;
        stack.reserve(64);
        stack.push_back(0);
        while (!stack.empty())
        {
            const uint32_t ni = stack.back();
            stack.pop_back();
            const Node& n = m_nodes[ni];
            if (aabbDistSq(p, n.lo, n.hi) >= best)
                continue;
            if (n.count != 0)
            {
                for (uint32_t k = n.first; k < n.first + n.count; ++k)
                {
                    const TriangleIndex& t = m_mesh.triangles[m_tris[m_order[k]]];
                    Vec3 q;
                    const float dsq = distPointTriangleSq(
                        p, m_mesh.positions[t.a], m_mesh.positions[t.b], m_mesh.positions[t.c], q
                    );
                    if (dsq < best)
                    {
                        best = dsq;
                        bestTri = m_tris[m_order[k]];
                    }
                }
            }
            else
            {
                stack.push_back(n.left);
                stack.push_back(n.right);
            }
        }
        outTri = bestTri;
        return best;
    }

    /// Collect indices (into Mesh::triangles) of all triangles within `radius` of p.
    void collectWithin(const Vec3& p, float radius, std::vector<uint32_t>& out) const
    {
        const float rSq = radius * radius;
        std::vector<uint32_t> stack;
        stack.reserve(64);
        stack.push_back(0);
        while (!stack.empty())
        {
            const uint32_t ni = stack.back();
            stack.pop_back();
            const Node& n = m_nodes[ni];
            if (aabbDistSq(p, n.lo, n.hi) > rSq)
                continue;
            if (n.count != 0)
            {
                for (uint32_t k = n.first; k < n.first + n.count; ++k)
                {
                    const TriangleIndex& t = m_mesh.triangles[m_tris[m_order[k]]];
                    Vec3 q;
                    const float dsq = distPointTriangleSq(
                        p, m_mesh.positions[t.a], m_mesh.positions[t.b], m_mesh.positions[t.c], q
                    );
                    if (dsq <= rSq)
                        out.push_back(m_tris[m_order[k]]);
                }
            }
            else
            {
                stack.push_back(n.left);
                stack.push_back(n.right);
            }
        }
    }

    /// Count triangles pierced by the ray with t in (tMin, tMax].
    uint32_t countHits(const Vec3& origin, const Vec3& dir, float tMin, float tMax) const
    {
        uint32_t hits = 0;
        std::vector<uint32_t> stack;
        stack.reserve(64);
        stack.push_back(0);
        while (!stack.empty())
        {
            const uint32_t ni = stack.back();
            stack.pop_back();
            const Node& n = m_nodes[ni];
            if (!rayAABB(origin, dir, n.lo, n.hi, tMax))
                continue;
            if (n.count != 0)
            {
                for (uint32_t k = n.first; k < n.first + n.count; ++k)
                {
                    const TriangleIndex& t = m_mesh.triangles[m_tris[m_order[k]]];
                    float tHit = 0.f;
                    if (rayTriangleMT(
                            origin,
                            dir,
                            m_mesh.positions[t.a],
                            m_mesh.positions[t.b],
                            m_mesh.positions[t.c],
                            tMin,
                            tMax,
                            tHit
                        ))
                        ++hits;
                }
            }
            else
            {
                stack.push_back(n.left);
                stack.push_back(n.right);
            }
        }
        return hits;
    }

    /// Distance to the first triangle pierced by the ray, or +inf if none.
    float firstHit(const Vec3& origin, const Vec3& dir, float tMin, float tMax) const
    {
        float best = tMax;
        std::vector<uint32_t> stack;
        stack.reserve(64);
        stack.push_back(0);
        while (!stack.empty())
        {
            const uint32_t ni = stack.back();
            stack.pop_back();
            const Node& n = m_nodes[ni];
            if (!rayAABB(origin, dir, n.lo, n.hi, best))
                continue;
            if (n.count != 0)
            {
                for (uint32_t k = n.first; k < n.first + n.count; ++k)
                {
                    const TriangleIndex& t = m_mesh.triangles[m_tris[m_order[k]]];
                    float tHit = 0.f;
                    if (rayTriangleMT(
                            origin,
                            dir,
                            m_mesh.positions[t.a],
                            m_mesh.positions[t.b],
                            m_mesh.positions[t.c],
                            tMin,
                            best,
                            tHit
                        ))
                        best = tHit;
                }
            }
            else
            {
                stack.push_back(n.left);
                stack.push_back(n.right);
            }
        }
        return best;
    }

private:
    struct Node
    {
        Vec3 lo;
        Vec3 hi;
        uint32_t left = kInvalidNode;
        uint32_t right = kInvalidNode;
        uint32_t first = 0;
        uint32_t count = 0;
    };

    uint32_t build(uint32_t begin, uint32_t end)
    {
        // IMPORTANT: never hold a `Node&` reference across the recursive build() calls below -
        // each recursive push can reallocate m_nodes and invalidate any reference taken before it
        // (the node must be re-fetched by index after the recursion).
        const uint32_t nodeIdx = uint32_t(m_nodes.size());
        m_nodes.push_back(Node{});

        // Node AABB from the ACTUAL triangle bounds (not centroids). A centroid-only AABB prunes
        // rays that hit a triangle far from its centroid, which breaks the ray/triangle counting
        // (parity sign) and the nearest-distance query. Per level this scans the node's range, so
        // the total build cost stays O(n log n).
        const TriangleIndex& t0 = m_mesh.triangles[m_tris[m_order[begin]]];
        Vec3 lo = m_mesh.positions[t0.a];
        Vec3 hi = m_mesh.positions[t0.a];
        auto grow = [&](const Vec3& p)
        {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        };
        for (uint32_t i = begin; i < end; ++i)
        {
            const TriangleIndex& t = m_mesh.triangles[m_tris[m_order[i]]];
            grow(m_mesh.positions[t.a]);
            grow(m_mesh.positions[t.b]);
            grow(m_mesh.positions[t.c]);
        }
        m_nodes[nodeIdx].lo = lo;
        m_nodes[nodeIdx].hi = hi;

        if (end - begin <= 4)
        {
            m_nodes[nodeIdx].first = begin;
            m_nodes[nodeIdx].count = end - begin;
            return nodeIdx;
        }

        const Vec3 extent = hi - lo;
        const int axis = (extent.x >= extent.y && extent.x >= extent.z) ? 0 : (extent.y >= extent.z ? 1 : 2);
        const uint32_t mid = begin + (end - begin) / 2;
        std::nth_element(
            m_order.begin() + begin,
            m_order.begin() + mid,
            m_order.begin() + end,
            [this, axis](uint32_t a, uint32_t b) { return m_centroids[a][axis] < m_centroids[b][axis]; }
        );
        m_nodes[nodeIdx].left = build(begin, mid);
        m_nodes[nodeIdx].right = build(mid, end);
        return nodeIdx;
    }

    const Mesh& m_mesh;
    const std::vector<uint32_t>& m_tris;
    std::vector<Vec3> m_centroids;
    std::vector<uint32_t> m_order;
    std::vector<Node> m_nodes;
};

// -------------------------------------------------------------------------------------
// Grid description (computed from mesh bbox / override / padding / resolution)
// -------------------------------------------------------------------------------------

struct GridDesc
{
    std::array<uint32_t, 3> res = {0, 0, 0};
    Vec3 bboxMin;
    Vec3 bboxMax;
    float voxelSize = 0.f;
    float normalizationScale = 1.f;
    float paddingWorld = 0.f;
    uint64_t totalVoxels = 0;
};

bool makeGridDesc(const Mesh& mesh, const SDFBuildParams& params, GridDesc& out, std::string& err)
{
    if (mesh.positions.empty())
    {
        err = "mesh has no vertices";
        return false;
    }

    Vec3 meshMin = mesh.positions[0];
    Vec3 meshMax = mesh.positions[0];
    for (const Vec3& p : mesh.positions)
    {
        meshMin = {std::min(meshMin.x, p.x), std::min(meshMin.y, p.y), std::min(meshMin.z, p.z)};
        meshMax = {std::max(meshMax.x, p.x), std::max(meshMax.y, p.y), std::max(meshMax.z, p.z)};
    }
    const Vec3 meshExtent = meshMax - meshMin;
    const float maxExtent = std::max(meshExtent.x, std::max(meshExtent.y, meshExtent.z));
    if (!(maxExtent > 0.f) || !std::isfinite(maxExtent))
    {
        err = "mesh bounding box is empty or non-finite";
        return false;
    }

    // World-space grid bounds.
    float pad = params.paddingWorld;
    if (pad == 0.f)
        pad = 0.1f * maxExtent;
    out.paddingWorld = pad;

    Vec3 gridMinW;
    Vec3 gridMaxW;
    if (params.bboxOverride)
    {
        const std::array<float, 6>& b = *params.bboxOverride;
        for (int i = 0; i < 3; ++i)
        {
            if (!(b[i] < b[i + 3]))
            {
                err = "bounding box override is degenerate (min >= max)";
                return false;
            }
        }
        gridMinW = {b[0], b[1], b[2]};
        gridMaxW = {b[3], b[4], b[5]};
    }
    else
    {
        gridMinW = meshMin - Vec3{pad, pad, pad};
        gridMaxW = meshMax + Vec3{pad, pad, pad};
    }
    const Vec3 gridExtent = gridMaxW - gridMinW;

    // Normalization: world -> output space.
    out.normalizationScale = params.normalize ? (1.f / maxExtent) : 1.f;
    out.bboxMin = gridMinW * out.normalizationScale;
    out.bboxMax = gridMaxW * out.normalizationScale;

    // Voxel size and per-axis resolution.
    const float ex = (out.bboxMax.x - out.bboxMin.x);
    const float ey = (out.bboxMax.y - out.bboxMin.y);
    const float ez = (out.bboxMax.z - out.bboxMin.z);
    const float extents[3] = {ex, ey, ez};

    bool useFixed = params.fixedResolution[0] != 0 || params.fixedResolution[1] != 0 || params.fixedResolution[2] != 0;
    if (useFixed)
    {
        float voxelSize = 0.f;
        for (int i = 0; i < 3; ++i)
        {
            if (params.fixedResolution[i] < 2)
            {
                err = "fixed resolution must be >= 2 on every axis";
                return false;
            }
            voxelSize = std::max(voxelSize, extents[i] / float(params.fixedResolution[i] - 1));
        }
        out.voxelSize = voxelSize;
        for (int i = 0; i < 3; ++i)
            out.res[i] = uint32_t(std::floor(extents[i] / voxelSize)) + 1;
    }
    else
    {
        if (params.maxResolution < 2)
        {
            err = "maxResolution must be >= 2";
            return false;
        }
        const float maxExtentOut = std::max(ex, std::max(ey, ez));
        out.voxelSize = maxExtentOut / float(params.maxResolution - 1);
        for (int i = 0; i < 3; ++i)
            out.res[i] = uint32_t(std::floor(extents[i] / out.voxelSize)) + 1;
    }

    for (int i = 0; i < 3; ++i)
        out.res[i] = std::max(uint32_t(2), out.res[i]);
    out.totalVoxels = uint64_t(out.res[0]) * uint64_t(out.res[1]) * uint64_t(out.res[2]);
    return true;
}

// -------------------------------------------------------------------------------------
// Sign helpers
// -------------------------------------------------------------------------------------

Vec3 voxelCenter(const GridDesc& g, uint32_t x, uint32_t y, uint32_t z)
{
    return Vec3{
        g.bboxMin.x + (float(x) + 0.5f) * g.voxelSize,
        g.bboxMin.y + (float(y) + 0.5f) * g.voxelSize,
        g.bboxMin.z + (float(z) + 0.5f) * g.voxelSize,
    };
}

// Parity vote along `axis` for one voxel; returns 1 if odd (inside) else 0.
// Deterministic jitter seeded by (seed, axis, voxelIndex).
int parityAlongAxis(
    const Bvh& bvh,
    const Vec3& center,
    int axis,
    float voxelSize,
    float tFar,
    uint64_t seed,
    uint64_t voxelIndex
)
{
    uint64_t st = seed ^ (uint64_t(axis) * 0x9e3779b97f4a7c15ULL) ^ splitmix64(voxelIndex);
    const Vec3 u = kAxisPerpU[axis];
    const Vec3 v = kAxisPerpV[axis];
    const float ja = 2.f * randUnit(st) - 1.f;
    const float jb = 2.f * randUnit(st) - 1.f;
    Vec3 dir = kAxisDir[axis] + (u * ja + v * jb) * kJitterScale;
    dir = normalize(dir);
    const Vec3 jitter = Vec3{2.f * randUnit(st) - 1.f, 2.f * randUnit(st) - 1.f, 2.f * randUnit(st) - 1.f}
        * (voxelSize * kJitterScale);
    const Vec3 origin = center + jitter;
    const uint32_t hits = bvh.countHits(origin, dir, 1e-6f, tFar);
    return int(hits & 1u);
}

// Normal dot-product vote over the k nearest triangles in the surface band.
// Returns the voted sign (+1 outside / -1 inside) or 0 when no candidate found.
int normalVote(
    const Bvh& bvh,
    const Mesh& mesh,
    const Vec3& p,
    float radius,
    std::vector<uint32_t>& scratchCandidates,
    size_t& candidateCount
)
{
    scratchCandidates.clear();
    bvh.collectWithin(p, radius, scratchCandidates);
    if (scratchCandidates.empty())
    {
        candidateCount = 0;
        return 0;
    }
    struct Rec
    {
        float d;
        uint32_t tri;
    };
    std::vector<Rec> recs;
    recs.reserve(scratchCandidates.size());
    for (uint32_t tri : scratchCandidates)
    {
        const TriangleIndex& t = mesh.triangles[tri];
        Vec3 q;
        const float dsq = distPointTriangleSq(
            p, mesh.positions[t.a], mesh.positions[t.b], mesh.positions[t.c], q
        );
        recs.push_back({dsq, tri});
    }
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        return a.d < b.d || (a.d == b.d && a.tri < b.tri);
    });

    int sum = 0;
    size_t used = 0;
    for (const Rec& r : recs)
    {
        if (used >= kVoteMaxTriangles)
            break;
        const TriangleIndex& t = mesh.triangles[r.tri];
        const Vec3 n = normalize(cross(mesh.positions[t.b] - mesh.positions[t.a], mesh.positions[t.c] - mesh.positions[t.a]));
        Vec3 q;
        distPointTriangleSq(p, mesh.positions[t.a], mesh.positions[t.b], mesh.positions[t.c], q);
        sum += dot(p - q, n) > 0.f ? 1 : -1;
        ++used;
    }
    candidateCount = used;
    if (used == 0)
        return 0;
    return sum >= 0 ? 1 : -1;
}

} // namespace

// -------------------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------------------

const char* warningCodeName(WarningCode code)
{
    switch (code)
    {
    case WarningCode::DegenerateTriangles:
        return "DegenerateTriangles";
    case WarningCode::NonManifoldEdges:
        return "NonManifoldEdges";
    case WarningCode::OpenBoundary:
        return "OpenBoundary";
    case WarningCode::ThinMesh:
        return "ThinMesh";
    case WarningCode::SignAmbiguity:
        return "SignAmbiguity";
    case WarningCode::NoValidTriangles:
        return "NoValidTriangles";
    }
    return "Unknown";
}

bool parseOBJ(std::istream& in, Mesh& mesh, std::string& err)
{
    mesh.positions.clear();
    mesh.triangles.clear();

    std::string line;
    size_t lineNo = 0;
    bool sawVertices = false;
    bool sawFaces = false;
    while (std::getline(in, line))
    {
        ++lineNo;
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#')
            continue;
        std::istringstream ls(line.substr(start));
        std::string tok;
        ls >> tok;
        if (tok == "v")
        {
            float x, y, z;
            if (!(ls >> x >> y >> z))
            {
                err = "line " + std::to_string(lineNo) + ": malformed vertex ('v')";
                return false;
            }
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            {
                err = "line " + std::to_string(lineNo) + ": non-finite vertex coordinate";
                return false;
            }
            mesh.positions.push_back({x, y, z});
            sawVertices = true;
        }
        else if (tok == "f")
        {
            std::vector<int64_t> idxs;
            std::string item;
            while (ls >> item)
            {
                const size_t slash = item.find('/');
                const std::string num = slash == std::string::npos ? item : item.substr(0, slash);
                char* end = nullptr;
                const long long v = std::strtoll(num.c_str(), &end, 10);
                if (end == num.c_str() || *end != '\0')
                {
                    err = "line " + std::to_string(lineNo) + ": malformed face vertex '" + item + "'";
                    return false;
                }
                idxs.push_back(int64_t(v));
            }
            if (idxs.size() < 3)
            {
                err = "line " + std::to_string(lineNo) + ": face with fewer than 3 vertices";
                return false;
            }
            const int64_t base = int64_t(mesh.positions.size());
            auto resolve = [&](int64_t v) -> bool {
                const int64_t resolved = v > 0 ? v - 1 : base + v;
                if (resolved < 0 || resolved >= base)
                {
                    err = "line " + std::to_string(lineNo) + ": face index out of range (" + std::to_string(v) + ")";
                    return false;
                }
                return true;
            };
            for (int64_t v : idxs)
            {
                if (!resolve(v))
                    return false;
            }
            const auto toIndex = [&](int64_t v) -> uint32_t { return uint32_t(v > 0 ? v - 1 : base + v); };
            const uint32_t a = toIndex(idxs[0]);
            for (size_t i = 1; i + 1 < idxs.size(); ++i)
            {
                const uint32_t b = toIndex(idxs[i]);
                const uint32_t c = toIndex(idxs[i + 1]);
                if (a != b && b != c && c != a)
                    mesh.triangles.push_back({a, b, c});
            }
            sawFaces = true;
        }
        // vt, vn, o, g, s, usemtl, mtllib, l, p, vp and any other directive: ignored.
    }

    if (!sawVertices)
    {
        err = "no vertices found (empty OBJ?)";
        return false;
    }
    if (!sawFaces)
    {
        err = "no faces found; mesh has vertices but no triangles";
        return false;
    }
    return true;
}

MeshAnalysis analyzeMesh(const Mesh& mesh)
{
    MeshAnalysis a;
    a.vertexCount = mesh.positions.size();
    a.triangleCount = mesh.triangles.size();

    // Per-triangle validity (index range + zero area).
    std::vector<uint8_t> valid(mesh.triangles.size(), 0);
    for (size_t i = 0; i < mesh.triangles.size(); ++i)
    {
        const TriangleIndex& t = mesh.triangles[i];
        if (t.a >= mesh.positions.size() || t.b >= mesh.positions.size() || t.c >= mesh.positions.size())
            continue;
        const Vec3 e1 = mesh.positions[t.b] - mesh.positions[t.a];
        const Vec3 e2 = mesh.positions[t.c] - mesh.positions[t.a];
        const float maxLenSq = std::max(lengthSq(e1), std::max(lengthSq(e2), lengthSq(e1 - e2)));
        const float crossSq = lengthSq(cross(e1, e2));
        if (crossSq <= kDegenerateEpsRel * maxLenSq)
            continue;
        valid[i] = 1;
        ++a.validTriangleCount;
    }
    a.degenerateTriangleCount = a.triangleCount - a.validTriangleCount;

    // Edge manifoldness over valid triangles.
    struct EdgeInfo
    {
        uint32_t firstTri = 0;
        uint32_t count = 0;
    };
    std::unordered_map<uint64_t, EdgeInfo> edgeMap;
    edgeMap.reserve(size_t(a.validTriangleCount) * 3);
    for (size_t i = 0; i < mesh.triangles.size(); ++i)
    {
        if (!valid[i])
            continue;
        const TriangleIndex& t = mesh.triangles[i];
        const uint32_t edges[3][2] = {{t.a, t.b}, {t.b, t.c}, {t.c, t.a}};
        for (int e = 0; e < 3; ++e)
        {
            const uint64_t lo = std::min(edges[e][0], edges[e][1]);
            const uint64_t hi = std::max(edges[e][0], edges[e][1]);
            const uint64_t key = lo | (hi << 32);
            EdgeInfo& info = edgeMap[key];
            if (info.count == 0)
                info.firstTri = uint32_t(i);
            ++info.count;
        }
    }
    for (const auto& kv : edgeMap)
    {
        if (kv.second.count == 1)
            ++a.boundaryEdgeCount;
        else if (kv.second.count > 2)
            ++a.nonManifoldEdgeCount;
    }

    // Connected components over valid triangles (DSU via shared edges).
    std::vector<uint32_t> dsuPos(mesh.triangles.size(), kInvalidNode);
    uint32_t dsuNext = 0;
    for (size_t i = 0; i < mesh.triangles.size(); ++i)
    {
        if (valid[i])
            dsuPos[i] = dsuNext++;
    }
    std::vector<uint32_t> parent(dsuNext);
    for (uint32_t i = 0; i < dsuNext; ++i)
        parent[i] = i;
    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) -> uint32_t {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](uint32_t x, uint32_t y) {
        const uint32_t rx = find(x);
        const uint32_t ry = find(y);
        if (rx != ry)
            parent[rx] = ry;
    };
    for (size_t i = 0; i < mesh.triangles.size(); ++i)
    {
        if (!valid[i])
            continue;
        const TriangleIndex& t = mesh.triangles[i];
        const uint32_t edges[3][2] = {{t.a, t.b}, {t.b, t.c}, {t.c, t.a}};
        for (int e = 0; e < 3; ++e)
        {
            const uint64_t lo = std::min(edges[e][0], edges[e][1]);
            const uint64_t hi = std::max(edges[e][0], edges[e][1]);
            const uint64_t key = lo | (hi << 32);
            const auto it = edgeMap.find(key);
            if (it != edgeMap.end() && it->second.count > 1 && it->second.firstTri != i)
                unite(dsuPos[i], dsuPos[it->second.firstTri]);
        }
    }
    a.componentCount = 0;
    for (size_t i = 0; i < parent.size(); ++i)
    {
        if (find(uint32_t(i)) == uint32_t(i))
            ++a.componentCount;
    }

    a.watertight = a.boundaryEdgeCount == 0 && a.nonManifoldEdgeCount == 0 && a.validTriangleCount > 0;

    if (a.degenerateTriangleCount > 0)
    {
        a.warnings.push_back(
            {WarningCode::DegenerateTriangles,
             std::to_string(a.degenerateTriangleCount) + " degenerate triangle(s) excluded from the field"}
        );
    }
    if (a.nonManifoldEdgeCount > 0)
    {
        a.warnings.push_back(
            {WarningCode::NonManifoldEdges,
             std::to_string(a.nonManifoldEdgeCount) + " edge(s) shared by more than two triangles"}
        );
    }
    if (a.boundaryEdgeCount > 0)
    {
        a.warnings.push_back(
            {WarningCode::OpenBoundary,
             "mesh is not watertight (" + std::to_string(a.boundaryEdgeCount) +
                 " boundary edge(s)); signed distances are ambiguous for open/thin meshes"}
        );
    }
    return a;
}

uint64_t fnv1a64(const void* data, size_t size)
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

uint64_t meshContentHash(const Mesh& mesh)
{
    std::vector<uint64_t> triHashes;
    triHashes.reserve(mesh.triangles.size());
    for (const TriangleIndex& t : mesh.triangles)
    {
        uint8_t buf[36];
        std::memcpy(buf, &mesh.positions[t.a], 12);
        std::memcpy(buf + 12, &mesh.positions[t.b], 12);
        std::memcpy(buf + 24, &mesh.positions[t.c], 12);
        triHashes.push_back(fnv1a64(buf, sizeof(buf)));
    }
    std::sort(triHashes.begin(), triHashes.end());
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint64_t counts[2] = {uint64_t(mesh.positions.size()), uint64_t(mesh.triangles.size())};
    h = fnv1a64Seed(&counts, sizeof(counts), h);
    if (!triHashes.empty())
        h = fnv1a64Seed(triHashes.data(), triHashes.size() * sizeof(uint64_t), h);
    return h;
}

std::string toHex(uint64_t value)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(16) << value;
    return os.str();
}

bool buildMeshSDF(const Mesh& mesh, const SDFBuildParams& params, SDFBuildResult& out, std::string& err)
{
    out = SDFBuildResult{};
    const auto tStart = std::chrono::steady_clock::now();

    const auto tAnalysis = std::chrono::steady_clock::now();
    out.analysis = analyzeMesh(mesh);
    out.analysisMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tAnalysis).count();

    if (out.analysis.triangleCount == 0)
    {
        err = "mesh has no triangles";
        return false;
    }
    if (out.analysis.validTriangleCount == 0)
    {
        err = "mesh has no valid (non-degenerate) triangles";
        out.warnings.push_back({WarningCode::NoValidTriangles, err});
        return false;
    }

    const auto tHash = std::chrono::steady_clock::now();
    out.contentHash = meshContentHash(mesh);
    out.contentHashHex = toHex(out.contentHash);
    out.hashMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tHash).count();

    GridDesc grid;
    if (!makeGridDesc(mesh, params, grid, err))
        return false;

    // Valid-triangle index list for the BVH / brute-force pass.
    std::vector<uint32_t> validTris;
    validTris.reserve(out.analysis.validTriangleCount);
    for (size_t i = 0; i < mesh.triangles.size(); ++i)
    {
        const TriangleIndex& t = mesh.triangles[i];
        if (t.a >= mesh.positions.size() || t.b >= mesh.positions.size() || t.c >= mesh.positions.size())
            continue;
        const Vec3 e1 = mesh.positions[t.b] - mesh.positions[t.a];
        const Vec3 e2 = mesh.positions[t.c] - mesh.positions[t.a];
        const float maxLenSq = std::max(lengthSq(e1), std::max(lengthSq(e2), lengthSq(e1 - e2)));
        if (lengthSq(cross(e1, e2)) > kDegenerateEpsRel * maxLenSq)
            validTris.push_back(uint32_t(i));
    }

    Bvh bvh(mesh, validTris);

    // Grid setup.
    out.header.formatVersion = kFormatVersion;
    out.header.resolution = grid.res;
    out.header.bboxMin = {grid.bboxMin.x, grid.bboxMin.y, grid.bboxMin.z};
    out.header.bboxMax = {grid.bboxMax.x, grid.bboxMax.y, grid.bboxMax.z};
    out.header.voxelSize = grid.voxelSize;
    out.header.normalizationScale = grid.normalizationScale;
    out.header.paddingWorld = grid.paddingWorld;
    out.header.signConvention = SignConvention::PositiveOutside;
    out.header.dataCount = grid.totalVoxels;

    const float tFar = 2.f * length(grid.bboxMax - grid.bboxMin) + 4.f * grid.voxelSize;
    const float bandRadius = kSurfaceBandVoxels * grid.voxelSize;
    const float voteRadius = kVoteRadiusVoxels * grid.voxelSize;

    out.distances.resize(size_t(grid.totalVoxels));
    std::vector<uint32_t> scratchCandidates;

    const auto tDist = std::chrono::steady_clock::now();
    size_t ambiguous = 0;
    size_t voteDisagree = 0;
    size_t insideVoxels = 0;
    std::vector<int8_t> insideMask(grid.totalVoxels);

    uint64_t voxelIndex = 0;
    for (uint32_t z = 0; z < grid.res[2]; ++z)
    {
        for (uint32_t y = 0; y < grid.res[1]; ++y)
        {
            for (uint32_t x = 0; x < grid.res[0]; ++x, ++voxelIndex)
            {
                const Vec3 center = voxelCenter(grid, x, y, z);

                // Unsigned distance to the nearest triangle.
                float d = 0.f;
                if (params.useBruteForce)
                {
                    d = std::numeric_limits<float>::infinity();
                    for (uint32_t tri : validTris)
                    {
                        const TriangleIndex& t = mesh.triangles[tri];
                        Vec3 q;
                        const float dsq = distPointTriangleSq(
                            center, mesh.positions[t.a], mesh.positions[t.b], mesh.positions[t.c], q
                        );
                        d = std::min(d, std::sqrt(dsq));
                    }
                }
                else
                {
                    uint32_t bestTri = 0;
                    d = std::sqrt(bvh.nearestSq(center, bestTri));
                }

                // Sign: 3-axis parity majority.
                int insideCount = 0;
                int parity[3];
                for (int axis = 0; axis < 3; ++axis)
                {
                    parity[axis] = parityAlongAxis(bvh, center, axis, grid.voxelSize, tFar, params.seed, voxelIndex);
                    insideCount += parity[axis];
                }
                const bool inside = insideCount > 1;
                if (inside)
                    ++insideVoxels;

                const float sign = inside ? -1.f : 1.f;
                out.distances[size_t(voxelIndex)] = d * sign;
                insideMask[size_t(voxelIndex)] = inside ? int8_t(1) : int8_t(0);

                const bool axesAgree = (parity[0] == parity[1]) && (parity[1] == parity[2]);
                if (!axesAgree)
                    ++ambiguous;

                // Informational normal-vote cross-check in the surface band.
                if (d < bandRadius)
                {
                    size_t used = 0;
                    const int vote = normalVote(bvh, mesh, center, voteRadius, scratchCandidates, used);
                    if (used > 0 && ((vote > 0) != !inside))
                        ++voteDisagree;
                }
            }
        }
    }

    // Thin-mesh detection over inside voxels.
    size_t thinVoxels = 0;
    if (insideVoxels > 0)
    {
        voxelIndex = 0;
        for (uint32_t z = 0; z < grid.res[2]; ++z)
        {
            for (uint32_t y = 0; y < grid.res[1]; ++y)
            {
                for (uint32_t x = 0; x < grid.res[0]; ++x, ++voxelIndex)
                {
                    if (!insideMask[size_t(voxelIndex)])
                        continue;
                    const Vec3 center = voxelCenter(grid, x, y, z);
                    float thickness = tFar;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const float tp = bvh.firstHit(center, kAxisDir[axis], 1e-6f, tFar);
                        const float tm = bvh.firstHit(center, kAxisDir[axis] * (-1.f), 1e-6f, tFar);
                        thickness = std::min(thickness, tp + tm);
                    }
                    if (thickness < kThinThresholdVoxels * grid.voxelSize)
                        ++thinVoxels;
                }
            }
        }
    }
    out.thinVoxelCount = thinVoxels;

    out.distanceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tDist).count();

    // Reliability and warnings.
    const size_t totalVoxels = size_t(grid.totalVoxels);
    out.ambiguousVoxelCount = ambiguous;
    out.ambiguousRatio = totalVoxels ? float(double(ambiguous) / double(totalVoxels)) : 0.f;
    out.voteDisagreementCount = voteDisagree;
    out.header.signReliable = out.analysis.watertight && ambiguous == 0;

    out.warnings = out.analysis.warnings;
    if (ambiguous > 0)
    {
        out.warnings.push_back(
            {WarningCode::SignAmbiguity,
             std::to_string(ambiguous) + " voxel(s) had disagreeing axis parity votes; sign unreliable there"}
        );
    }
    if (thinVoxels > 0 && (insideVoxels == 0 || double(thinVoxels) / double(insideVoxels) > kThinFractionThreshold))
    {
        out.warnings.push_back(
            {WarningCode::ThinMesh,
             std::to_string(thinVoxels) + " inside voxel(s) are thinner than 2.5 voxels; sphere tracing may step through"}
        );
    }

    out.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
    return true;
}

// -------------------------------------------------------------------------------------
// Serialization
// -------------------------------------------------------------------------------------

class ByteWriter
{
public:
    void u8(uint8_t v) { m_buf.push_back(v); }
    void u16(uint16_t v)
    {
        m_buf.push_back(uint8_t(v & 0xFF));
        m_buf.push_back(uint8_t(v >> 8));
    }
    void u32(uint32_t v)
    {
        m_buf.push_back(uint8_t(v & 0xFF));
        m_buf.push_back(uint8_t((v >> 8) & 0xFF));
        m_buf.push_back(uint8_t((v >> 16) & 0xFF));
        m_buf.push_back(uint8_t(v >> 24));
    }
    void u64(uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            m_buf.push_back(uint8_t(v >> (8 * i)));
    }
    void f32(float f)
    {
        static_assert(std::numeric_limits<float>::is_iec559, "IEEE-754 float required");
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        u32(bits);
    }
    void bytes(const void* p, size_t n)
    {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        m_buf.insert(m_buf.end(), b, b + n);
    }
    const std::vector<uint8_t>& buf() const { return m_buf; }

private:
    std::vector<uint8_t> m_buf;
};

bool readLEU16(const uint8_t* p, uint16_t& out)
{
    out = uint16_t(p[0]) | (uint16_t(p[1]) << 8);
    return true;
}

bool readLEU32(const uint8_t* p, uint32_t& out)
{
    out = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    return true;
}

bool readLEU64(const uint8_t* p, uint64_t& out)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    out = v;
    return true;
}

bool readLEF32(const uint8_t* p, float& out)
{
    uint32_t bits;
    readLEU32(p, bits);
    std::memcpy(&out, &bits, 4);
    return true;
}

bool writeMSDF(const std::filesystem::path& path, const SDFBuildResult& result, std::string& err)
{
    if (result.warnings.size() > 65535)
    {
        err = "too many warnings to serialize";
        return false;
    }
    ByteWriter w;
    w.bytes(kMagic, 4);
    w.u32(kFormatVersion);
    w.u32(kHeaderSize);

    const SDFHeader& h = result.header;
    for (int i = 0; i < 3; ++i)
        w.u32(h.resolution[i]);
    for (int i = 0; i < 3; ++i)
        w.f32(h.bboxMin[i]);
    for (int i = 0; i < 3; ++i)
        w.f32(h.bboxMax[i]);
    w.f32(h.voxelSize);
    w.f32(h.normalizationScale);
    w.f32(h.paddingWorld);
    w.u8(uint8_t(h.signConvention));
    w.u8(h.signReliable ? 1 : 0);
    w.u16(uint16_t(result.warnings.size()));

    const uint64_t dataOffset = 88 + [&]() {
        uint64_t n = 0;
        for (const Warning& warn : result.warnings)
            n += 2 + warn.message.size();
        n = (n + 7) & ~uint64_t(7);
        return n;
    }();
    const uint64_t dataCount = result.distances.size();
    const uint64_t checksumOffset = dataOffset + dataCount * 4;

    w.u64(dataOffset);
    w.u64(uint64_t(dataCount));
    w.u64(checksumOffset);

    for (const Warning& warn : result.warnings)
    {
        if (warn.message.size() > 65535)
        {
            err = "warning message too long to serialize";
            return false;
        }
        w.u16(uint16_t(warn.message.size()));
        w.bytes(warn.message.data(), warn.message.size());
    }
    while (w.buf().size() < dataOffset)
        w.u8(0);

    w.bytes(result.distances.data(), result.distances.size() * sizeof(float));
    // Trailing checksum over everything written so far.
    const uint64_t checksum = fnv1a64(w.buf().data(), w.buf().size());
    w.u64(checksum);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        err = "cannot open output file: " + path.string();
        return false;
    }
    out.write(reinterpret_cast<const char*>(w.buf().data()), std::streamsize(w.buf().size()));
    if (!out)
    {
        err = "failed writing output file: " + path.string();
        return false;
    }
    return true;
}

bool readMSDF(
    const std::filesystem::path& path,
    SDFHeader& header,
    std::vector<Warning>& warnings,
    std::vector<float>& distances,
    std::string& err
)
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
    if (fileSize < std::streamoff(kHeaderSize + 8))
    {
        err = "file too small to be a .msdf volume";
        return false;
    }
    std::vector<uint8_t> buf((size_t(fileSize)));
    in.read(reinterpret_cast<char*>(buf.data()), fileSize);
    if (!in)
    {
        err = "failed reading input file: " + path.string();
        return false;
    }

    if (std::memcmp(buf.data(), kMagic, 4) != 0)
    {
        err = "bad magic: not a .msdf file";
        return false;
    }
    uint32_t version = 0;
    uint32_t headerSize = 0;
    readLEU32(buf.data() + 4, version);
    readLEU32(buf.data() + 8, headerSize);
    if (version != kFormatVersion)
    {
        err = "unsupported format version " + std::to_string(version);
        return false;
    }
    if (headerSize != kHeaderSize)
    {
        err = "unexpected header size " + std::to_string(headerSize);
        return false;
    }

    SDFHeader h;
    uint32_t nx = 0, ny = 0, nz = 0;
    for (int i = 0; i < 3; ++i)
    {
        uint32_t v = 0;
        readLEU32(buf.data() + 12 + 4 * i, v);
        h.resolution[i] = v;
    }
    nx = h.resolution[0];
    ny = h.resolution[1];
    nz = h.resolution[2];
    for (int i = 0; i < 3; ++i)
        readLEF32(buf.data() + 24 + 4 * i, h.bboxMin[i]);
    for (int i = 0; i < 3; ++i)
        readLEF32(buf.data() + 36 + 4 * i, h.bboxMax[i]);
    readLEF32(buf.data() + 48, h.voxelSize);
    readLEF32(buf.data() + 52, h.normalizationScale);
    readLEF32(buf.data() + 56, h.paddingWorld);
    h.signConvention = SignConvention(buf[60]);
    h.signReliable = buf[61] != 0;
    uint16_t warningCount = 0;
    readLEU16(buf.data() + 62, warningCount);
    uint64_t dataOffset = 0;
    uint64_t dataCount = 0;
    uint64_t checksumOffset = 0;
    readLEU64(buf.data() + 64, dataOffset);
    readLEU64(buf.data() + 72, dataCount);
    readLEU64(buf.data() + 80, checksumOffset);
    h.dataCount = dataCount;

    if (dataCount != uint64_t(nx) * uint64_t(ny) * uint64_t(nz))
    {
        err = "data count does not match resolution";
        return false;
    }
    if (dataOffset < kHeaderSize || checksumOffset != dataOffset + dataCount * 4 || checksumOffset + 8 != uint64_t(fileSize))
    {
        err = "corrupt file layout (offsets do not line up)";
        return false;
    }
    if (checksumOffset > uint64_t(fileSize))
    {
        err = "checksum offset out of range";
        return false;
    }

    const uint64_t storedChecksum = [&]() {
        uint64_t c = 0;
        readLEU64(buf.data() + checksumOffset, c);
        return c;
    }();
    const uint64_t computedChecksum = fnv1a64(buf.data(), size_t(checksumOffset));
    if (storedChecksum != computedChecksum)
    {
        err = "checksum mismatch: file is corrupted";
        return false;
    }

    // Warnings section.
    warnings.clear();
    size_t pos = size_t(kHeaderSize);
    for (uint16_t i = 0; i < warningCount; ++i)
    {
        if (pos + 2 > size_t(dataOffset))
        {
            err = "corrupt warnings section";
            return false;
        }
        uint16_t len = 0;
        readLEU16(buf.data() + pos, len);
        pos += 2;
        if (pos + len > size_t(dataOffset))
        {
            err = "corrupt warnings section";
            return false;
        }
        warnings.push_back({WarningCode::Unknown, std::string(reinterpret_cast<const char*>(buf.data() + pos), len)});
        pos += len;
    }

    distances.resize(size_t(dataCount));
    std::memcpy(distances.data(), buf.data() + size_t(dataOffset), distances.size() * sizeof(float));

    header = h;
    return true;
}

// -------------------------------------------------------------------------------------
// JSON report (minimal hand-rolled writer, no external library)
// -------------------------------------------------------------------------------------

namespace
{

void jsonEscape(std::string& out, const std::string& s)
{
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<uint8_t>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", uint8_t(c));
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
}

std::string fmtFloat(float v)
{
    std::ostringstream os;
    os << std::setprecision(9) << v;
    return os.str();
}

std::string fmtVec3(const std::array<float, 3>& v)
{
    return "[" + fmtFloat(v[0]) + ", " + fmtFloat(v[1]) + ", " + fmtFloat(v[2]) + "]";
}

} // namespace

bool writeJSONReport(const std::filesystem::path& path, const SDFBuildResult& result, std::string& err)
{
    const SDFHeader& h = result.header;
    std::string j;
    j.reserve(4096);
    j += "{\n";
    j += "  \"tool\": \"MeshSDFBuilder\",\n";
    j += "  \"formatVersion\": 1,\n";
    j += "  \"signConvention\": \"positive-outside/negative-inside\",\n";
    j += "  \"signReliable\": ";
    j += h.signReliable ? "true" : "false";
    j += ",\n";
    j += "  \"mesh\": {\n";
    j += "    \"vertexCount\": " + std::to_string(result.analysis.vertexCount) + ",\n";
    j += "    \"triangleCount\": " + std::to_string(result.analysis.triangleCount) + ",\n";
    j += "    \"validTriangleCount\": " + std::to_string(result.analysis.validTriangleCount) + ",\n";
    j += "    \"degenerateTriangleCount\": " + std::to_string(result.analysis.degenerateTriangleCount) + ",\n";
    j += "    \"boundaryEdgeCount\": " + std::to_string(result.analysis.boundaryEdgeCount) + ",\n";
    j += "    \"nonManifoldEdgeCount\": " + std::to_string(result.analysis.nonManifoldEdgeCount) + ",\n";
    j += "    \"componentCount\": " + std::to_string(result.analysis.componentCount) + ",\n";
    j += "    \"watertight\": ";
    j += result.analysis.watertight ? "true" : "false";
    j += ",\n";
    j += "    \"contentHash\": \"" + result.contentHashHex + "\"\n";
    j += "  },\n";
    j += "  \"grid\": {\n";
    j += "    \"resolution\": " + fmtVec3({float(h.resolution[0]), float(h.resolution[1]), float(h.resolution[2])}) + ",\n";
    j += "    \"bboxMin\": " + fmtVec3(h.bboxMin) + ",\n";
    j += "    \"bboxMax\": " + fmtVec3(h.bboxMax) + ",\n";
    j += "    \"paddingWorld\": " + fmtFloat(h.paddingWorld) + ",\n";
    j += "    \"voxelSize\": " + fmtFloat(h.voxelSize) + ",\n";
    j += "    \"normalizationScale\": " + fmtFloat(h.normalizationScale) + ",\n";
    j += "    \"totalVoxels\": " + std::to_string(h.dataCount) + "\n";
    j += "  },\n";
    j += "  \"sign\": {\n";
    j += "    \"ambiguousVoxelCount\": " + std::to_string(result.ambiguousVoxelCount) + ",\n";
    j += "    \"ambiguousRatio\": " + fmtFloat(result.ambiguousRatio) + ",\n";
    j += "    \"voteDisagreementCount\": " + std::to_string(result.voteDisagreementCount) + ",\n";
    j += "    \"thinVoxelCount\": " + std::to_string(result.thinVoxelCount) + "\n";
    j += "  },\n";
    j += "  \"warnings\": [\n";
    for (size_t i = 0; i < result.warnings.size(); ++i)
    {
        const Warning& w = result.warnings[i];
        std::string escaped;
        jsonEscape(escaped, w.message);
        j += "    {\"code\": \"" + std::string(warningCodeName(w.code)) + "\", \"message\": \"" + escaped + "\"}";
        j += (i + 1 < result.warnings.size()) ? ",\n" : "\n";
    }
    j += "  ],\n";
    j += "  \"output\": {\n";
    j += "    \"dataCount\": " + std::to_string(h.dataCount) + "\n";
    j += "  },\n";
    j += "  \"timingMs\": {\n";
    j += "    \"analysis\": " + fmtFloat(float(result.analysisMs)) + ",\n";
    j += "    \"hash\": " + fmtFloat(float(result.hashMs)) + ",\n";
    j += "    \"distance\": " + fmtFloat(float(result.distanceMs)) + ",\n";
    j += "    \"total\": " + fmtFloat(float(result.totalMs)) + "\n";
    j += "  }\n";
    j += "}\n";

    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        err = "cannot open report file: " + path.string();
        return false;
    }
    out << j;
    if (!out)
    {
        err = "failed writing report file: " + path.string();
        return false;
    }
    return true;
}

} // namespace MeshSDF
} // namespace LumenGI
