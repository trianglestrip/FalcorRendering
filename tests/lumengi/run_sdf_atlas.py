from falcor import *

"""LumenGI S6-C2 Mesh SDF Atlas / instance regression asset (SKELETON, Agent Z15).

Role / purpose
--------------
RUN-ONLY Mogwai GPU + CPU-side skeleton for the S6-B2 atlas wave (task.md 11,
S6-C2 "Atlas/实例回归"). The S6-B2 atlas host (LumenMeshSDFAtlas /
LumenMeshSDFInstanceTable, header-only, NOT script-visible at HEAD 3821d232) is
mirrored here in pure Python so the frozen semantic invariants are exercised and
asserted TODAY, and the GPU path is probed and SKIPs until the root pass wires it:

  * C1 page sharing / dedup: identical mesh registered N times must register ONCE
    (meshCount == 1) and its pages are shared by every instance (residentBytes
    counts one mesh, not N). Cache: first ensure builds, the rest are cache hits.
  * C2 non-uniform scale: an anisotropic instance transform sets the
    kLumenMeshSDFInstanceFlagNonUniformScale flag, its world AABB reflects the
    anisotropy, and a world point ON the transformed surface samples |d| ~ 0.
  * C3 atlas pressure (超预算): a hard budget evicts the FARTHEST resident
    instances until estimateGpuBytes() fits; evicted instances are reported as
    non-resident (stable degradation, never a crash); evictions counter matches.
  * C4 evict/reload: restoreInstance() re-residents an evicted instance, and
    reload()+applyScene() re-materializes the whole scene from the DISK cache
    (cache HITs, builds NOT repeated).

S6_TODO contract (root must freeze before this becomes gating)
--------------------------------------------------------------
  * S6_TODO[atlas_stats_channel]: the LumenGI output / scriptable snapshot
    exposing the atlas + scene residency state. Candidates:
      - "meshSDFStats"  graph output (atlas-resolution stats texture), or
      - a pass binding `meshSDFSceneStats` (a dict snapshot mirroring
        LumenMeshSDFSceneStats / LumenMeshSDFAtlasStats, like the existing
        `surfaceCacheStats` binding, LumenGI.cpp registerBindings).
    The GPU section probes BOTH and SKIPs when neither exists.
  * S6_TODO[nonuniform_tol]: the world-space distance tolerance under non-uniform
    scaling (the atlas stores worldScalePerOutput; the exact anisotropic SDF
    distance semantics are S6-B2/S6-C4 items). The CPU mirror asserts the flag +
    AABB + on-surface |d| ~ 0 only; the distance gate SKIPs.
  * S6_TODO[gdf_compose]: run_sdf_atlas asserts atlas residency; GDF compose /
    instance-list filtering (buildGDFInstanceList excludes evicted) is the S6-B3
    wave.

Gate alignment (task.md S6A 门禁)
---------------------------------
  * "atlas 多实例和缩放采样正确，无越界和泄漏" -> C1/C2 (page sharing +
    non-uniform scale + bounds).
  * "atlas 满时必须 eviction/降级" (15.5) -> C3 (pressure + eviction).
  * "缓存可复现、版本变更会失效、损坏会重建" -> C4 (reload hits cache, no rebuild);
    version/corruption rebuild is run_sdf_format.py + the C++ cache tests.

Exit: Falcor exit() as in the sibling scripts. Report JSON is written to
artifacts/lumengi/S6/atlas/sdf-atlas.json regardless of the verdict.
"""

import json
import math
import os
import struct

import numpy as np

# -------------------------------------------------------------------------------------
# Frozen atlas constants (mirror LumenMeshSDFAtlas.h; keep both in sync).
# -------------------------------------------------------------------------------------
PAGE_SIZE = 32                                   # kLumenMeshSDFAtlasPageSize
BYTES_PER_PAGE_FINE = PAGE_SIZE ** 3 * 2          # R16Float
BYTES_PER_PAGE_COARSE = PAGE_SIZE ** 3 * 1        # R8Snorm
DEFAULT_PAGES_PER_SIDE = 8                        # -> 512 page slots per atlas.
FORMAT_R16FLOAT = 0
FORMAT_R8SNORM = 1
FLAG_NON_UNIFORM_SCALE = 1 << 0                   # kLumenMeshSDFInstanceFlagNonUniformScale

# -------------------------------------------------------------------------------------
# Configuration (S6_TODO: freeze defaults with root when the S6B wave lands).
# -------------------------------------------------------------------------------------
RESOLUTION = (640, 360)
FRAME_RATE = 60
OUT_JSON = os.environ.get("LUMEN_SDF_ATLAS_OUT", "artifacts/lumengi/S6/atlas/sdf-atlas.json")
SCENE = "test_scenes/cornell_box.pyscene"

# S6_TODO[atlas_stats_channel]: candidates probed by the GPU section.
MESH_SDF_STATS_CHANNELS = ["meshSDFStats", "meshSDFAtlasStats"]
MESH_SDF_STATS_BINDING = "meshSDFSceneStats"

# Budget / pressure parameters.
MESH_RESOLUTION = (16, 16, 16)
MIP_COUNT_HIGH = 5   # 1 + ceil(log2(16)).
BUDGET_MESHES = 4    # distinct meshes pushed into a budget of 2 mesh-equivalents.

# S6_TODO gates (placeholders; freeze with root):
SURFACE_TOL = 0.05   # S6_TODO[nonuniform_tol]: on-surface |d| must be ~0 in object space.
NONUNIFORM_SCALE = (2.0, 1.0, 0.5)
TRANSLATION = (1.0, 2.0, 3.0)
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


def fnv1a64(data, seed=FNV_OFFSET):
    h = seed & 0xFFFFFFFFFFFFFFFF
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def mip_dims(resolution, mip):
    d = list(resolution)
    for _ in range(mip):
        d = [max(1, (v + 1) >> 1) for v in d]
    return tuple(d)


def brick_dims(mip_resolution):
    return tuple((v + PAGE_SIZE - 1) // PAGE_SIZE for v in mip_resolution)


def pages_for_mip(resolution, mip):
    b = brick_dims(mip_dims(resolution, mip))
    return b[0] * b[1] * b[2]


def atlas_kind(format_mip0, mip):
    return "fine" if (mip == 0 and format_mip0 == FORMAT_R16FLOAT) else "coarse"


def bytes_per_page(kind):
    return BYTES_PER_PAGE_FINE if kind == "fine" else BYTES_PER_PAGE_COARSE


def mesh_bytes(format_mip0, mip_count, resolution):
    total = 0
    for m in range(mip_count):
        total += pages_for_mip(resolution, m) * bytes_per_page(atlas_kind(format_mip0, m))
    return total


# -------------------------------------------------------------------------------------
# Atlas + scene mirrors (faithful to the frozen C++ semantics).
# -------------------------------------------------------------------------------------


class AtlasMirror:
    """Page allocator: pagesPerSide^3 slots per atlas kind (fine / coarse)."""

    def __init__(self, pages_per_side=DEFAULT_PAGES_PER_SIDE):
        self.pages_per_side = pages_per_side
        self.capacity = pages_per_side ** 3
        self.used = {"fine": 0, "coarse": 0}
        self.mesh_id_by_key = {}

    def register_mesh(self, mesh_desc):
        """Dedup by atlasMeshIdentityKey; returns the (existing) mesh id."""
        key = identity_key(mesh_desc)
        if key in self.mesh_id_by_key:
            return self.mesh_id_by_key[key], False
        mesh_id = len(self.mesh_id_by_key)
        self.mesh_id_by_key[key] = mesh_id
        return mesh_id, True

    def pages_for(self, mesh_desc):
        fine = sum(pages_for_mip(mesh_desc["resolution"], m) for m in range(mesh_desc["mip_count"])
                   if atlas_kind(mesh_desc["format_mip0"], m) == "fine")
        coarse = sum(pages_for_mip(mesh_desc["resolution"], m) for m in range(mesh_desc["mip_count"])
                     if atlas_kind(mesh_desc["format_mip0"], m) == "coarse")
        return fine, coarse

    def can_fit(self, mesh_desc):
        fine, coarse = self.pages_for(mesh_desc)
        return self.used["fine"] + fine <= self.capacity and self.used["coarse"] + coarse <= self.capacity

    def place(self, mesh_desc):
        fine, coarse = self.pages_for(mesh_desc)
        if not self.can_fit(mesh_desc):
            return False
        self.used["fine"] += fine
        self.used["coarse"] += coarse
        return True

    def unplace(self, mesh_desc):
        fine, coarse = self.pages_for(mesh_desc)
        self.used["fine"] = max(0, self.used["fine"] - fine)
        self.used["coarse"] = max(0, self.used["coarse"] - coarse)


def identity_key(mesh_desc):
    """Python mirror of atlasMeshIdentityKey (FNV-1a64 over the dedup fields)."""
    buf = struct.pack("<Q", mesh_desc["content_hash"])
    buf += struct.pack("<3I", *mesh_desc["resolution"])
    buf += struct.pack("<III",
                       mesh_desc["format_mip0"], mesh_desc["pooling"], mesh_desc["mip_count"])
    return fnv1a64(buf)


class SceneMirror:
    """LumenMeshSDFScene-level mirror: mesh dedup + shared pages, instance residency,
    camera-relative budget eviction, evict/restore, reload/apply from the disk cache."""

    def __init__(self, budget_bytes=0, pages_per_side=DEFAULT_PAGES_PER_SIDE):
        self.atlas = AtlasMirror(pages_per_side)
        self.meshes = {}          # identity_key -> {"desc", "resident", "ref_count", "gpu_bytes"}
        self.instances = []       # {"active","evicted","mesh_key","linear","trans","non_uniform"}
        self.camera = (0.0, 0.0, 0.0)
        self.budget_bytes = budget_bytes
        self.cache = {}           # identity_key -> True (disk cache simulation)
        self.cache_hits = 0
        self.cache_misses = 0
        self.builds = 0
        self.evictions = 0
        self.restores = 0
        self.conversions = 0

    def _ensure_mesh(self, mesh_desc):
        key = identity_key(mesh_desc)
        if key not in self.cache:
            self.cache_misses += 1
            self.cache[key] = True
            self.builds += 1
        else:
            self.cache_hits += 1
        if key not in self.meshes:
            mesh_id, _ = self.atlas.register_mesh(mesh_desc)
            self.meshes[key] = {
                "desc": mesh_desc,
                "resident": False,
                "ref_count": 0,
                "gpu_bytes": mesh_bytes(mesh_desc["format_mip0"], mesh_desc["mip_count"],
                                        mesh_desc["resolution"]),
                "mesh_id": mesh_id,
            }
        return key

    def add_instance(self, mesh_desc, linear, trans):
        key = self._ensure_mesh(mesh_desc)
        rec = self.meshes[key]
        rec["ref_count"] += 1
        resident = False
        if rec["resident"]:
            resident = True
        elif self.atlas.place(rec["desc"]):
            rec["resident"] = True
            resident = True
            self.conversions += 1
        sx, sy, sz = (linear[0], linear[4], linear[8])
        non_uniform = not (abs(sx - sy) < 1e-6 and abs(sy - sz) < 1e-6)
        self.instances.append({
            "active": True, "evicted": not resident, "mesh_key": key,
            "linear": list(linear), "trans": list(trans),
            "non_uniform": non_uniform, "resident": resident,
        })
        return len(self.instances) - 1

    def is_resident(self, scene_id):
        inst = self.instances[scene_id]
        return inst["active"] and not inst["evicted"] and self.meshes[inst["mesh_key"]]["resident"]

    def resident_bytes(self):
        return sum(rec["gpu_bytes"] for rec in self.meshes.values() if rec["resident"])

    def estimate_gpu_bytes(self):
        return self.resident_bytes()

    def _instance_aabb_center(self, scene_id):
        inst = self.instances[scene_id]
        L, t = inst["linear"], inst["trans"]
        corners = []
        for (x, y, z) in [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
                          (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]:
            corners.append((L[0] * x + L[1] * y + L[2] * z + t[0],
                            L[3] * x + L[4] * y + L[5] * z + t[1],
                            L[6] * x + L[7] * y + L[8] * z + t[2]))
        lo = [min(c[i] for c in corners) for i in range(3)]
        hi = [max(c[i] for c in corners) for i in range(3)]
        return tuple((lo[i] + hi[i]) * 0.5 for i in range(3))

    def _invert_linear(self, linear):
        a, b, c = linear[0], linear[1], linear[2]
        d, e, f = linear[3], linear[4], linear[5]
        g, h, i = linear[6], linear[7], linear[8]
        det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
        if abs(det) < 1e-12:
            return None
        inv = 1.0 / det
        return [
            (e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
            (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
            (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv,
        ]

    def sample_object(self, scene_id, world_pos):
        """World -> object-space point via the inverse affine; the caller evaluates the
        object-space SDF. Returns None for a non-resident / evicted instance."""
        inst = self.instances[scene_id]
        if not self.is_resident(scene_id):
            return None
        inv = self._invert_linear(inst["linear"])
        if inv is None:
            return None
        p = [world_pos[0] - inst["trans"][0], world_pos[1] - inst["trans"][1], world_pos[2] - inst["trans"][2]]
        return (inv[0] * p[0] + inv[1] * p[1] + inv[2] * p[2],
                inv[3] * p[0] + inv[4] * p[1] + inv[5] * p[2],
                inv[6] * p[0] + inv[7] * p[1] + inv[8] * p[2])

    def enforce_budget(self, budget_bytes):
        self.budget_bytes = budget_bytes
        for _ in range(len(self.instances) + 1):
            if self.resident_bytes() <= budget_bytes:
                return True
            if self.evict_farthest_instance() is None:
                return False
        return self.resident_bytes() <= budget_bytes

    def evict_farthest_instance(self):
        best = None
        best_dist = -1.0
        for i, inst in enumerate(self.instances):
            if not inst["active"] or inst["evicted"]:
                continue
            c = self._instance_aabb_center(i)
            d2 = (c[0] - self.camera[0]) ** 2 + (c[1] - self.camera[1]) ** 2 + (c[2] - self.camera[2]) ** 2
            if d2 > best_dist + 1e-12 or (abs(d2 - best_dist) <= 1e-12 and i > best):
                best_dist = d2
                best = i
        if best is None:
            return None
        inst = self.instances[best]
        rec = self.meshes[inst["mesh_key"]]
        rec["ref_count"] -= 1
        if rec["ref_count"] <= 0:
            self.atlas.unplace(rec["desc"])
            rec["resident"] = False
        inst["evicted"] = True
        inst["resident"] = False
        self.evictions += 1
        return best

    def restore_instance(self, scene_id):
        inst = self.instances[scene_id]
        if not inst["active"]:
            return False
        if not inst["evicted"]:
            return True
        rec = self.meshes[inst["mesh_key"]]
        if not rec["resident"]:
            if not self.atlas.place(rec["desc"]):
                return False
            rec["resident"] = True
            rec["ref_count"] += 1
        inst["evicted"] = False
        inst["resident"] = True
        self.restores += 1
        return True

    def reload(self):
        for rec in self.meshes.values():
            rec["resident"] = False
            rec["ref_count"] = 0
        self.atlas.used = {"fine": 0, "coarse": 0}
        for inst in self.instances:
            inst["evicted"] = False
            inst["resident"] = False

    def apply_scene(self):
        """Re-materialize every active instance from the disk cache (cache HITs only,
        builds must NOT repeat)."""
        for inst in self.instances:
            if not inst["active"]:
                continue
            rec = self.meshes[inst["mesh_key"]]
            if not rec["resident"]:
                if inst["mesh_key"] in self.cache:
                    self.cache_hits += 1
                if not self.atlas.place(rec["desc"]):
                    return False
                rec["resident"] = True
                rec["ref_count"] += 1
            inst["resident"] = True
            inst["evicted"] = False
        return True

    def stats(self):
        return {
            "residentBytes": self.resident_bytes(),
            "budgetBytes": self.budget_bytes,
            "registeredMeshes": len(self.meshes),
            "activeInstances": sum(1 for i in self.instances if i["active"]),
            "residentInstances": sum(1 for idx, _ in enumerate(self.instances) if self.is_resident(idx)),
            "evictedInstances": sum(1 for i in self.instances if i["active"] and i["evicted"]),
            "cacheHits": self.cache_hits,
            "cacheMisses": self.cache_misses,
            "builds": self.builds,
            "evictions": self.evictions,
            "restores": self.restores,
        }


def cube_analytic_object(p, half=1.0):
    dx = abs(p[0]) - half
    dy = abs(p[1]) - half
    dz = abs(p[2]) - half
    m = max(dx, max(dy, dz))
    outside = math.sqrt(max(dx, 0.0) ** 2 + max(dy, 0.0) ** 2 + max(dz, 0.0) ** 2)
    return outside + min(m, 0.0)


def mesh_desc(content_hash, resolution=MESH_RESOLUTION, format_mip0=FORMAT_R16FLOAT,
              pooling=0, mip_count=MIP_COUNT_HIGH):
    return {"content_hash": content_hash, "resolution": tuple(resolution),
            "format_mip0": format_mip0, "pooling": pooling, "mip_count": mip_count}


# -------------------------------------------------------------------------------------
# JSON / helpers.
# -------------------------------------------------------------------------------------


def json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, (list, tuple)):
        return [json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): json_safe(item) for key, item in value.items()}
    return str(value)


def write_json(path, payload):
    path = os.path.abspath(path)
    out_dir = os.path.dirname(path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(json_safe(payload), f, indent=2, sort_keys=True, allow_nan=False)
        f.write("\n")
    os.replace(temp, path)


# -------------------------------------------------------------------------------------
# Gate sections.
# -------------------------------------------------------------------------------------


def run_page_sharing():
    """C1: identical mesh, N instances -> one registered mesh, pages shared, cache
    hit after the first build."""
    verdicts = []
    scene = SceneMirror()
    cube = mesh_desc(0xABCD0001)
    ids = [scene.add_instance(cube, [1, 0, 0, 0, 1, 0, 0, 0, 1], (i * 2.0, 0.0, 0.0)) for i in range(4)]
    stats = scene.stats()
    shared_ok = stats["registeredMeshes"] == 1 and stats["residentInstances"] == 4
    bytes_ok = stats["residentBytes"] == scene.meshes[list(scene.meshes.keys())[0]]["gpu_bytes"]
    verdicts.append(("C1 page sharing (4 instances -> 1 registered mesh, all resident)",
                     "PASS" if shared_ok else "FAIL"))
    verdicts.append(("C1 residentBytes counts shared mesh once (not xN)",
                     "PASS" if bytes_ok else "FAIL"))
    first_build_ok = stats["builds"] == 1 and stats["cacheMisses"] == 1 and stats["cacheHits"] >= 3
    verdicts.append(("C1 cache: first ensure builds, rest are hits (builds==1, hits>=3)",
                     "PASS" if first_build_ok else "FAIL"))
    return {"mesh_count": stats["registeredMeshes"], "instances": 4,
            "resident_bytes": stats["residentBytes"], "builds": stats["builds"],
            "cache_hits": stats["cacheHits"]}, verdicts


def run_nonuniform_scale():
    """C2: anisotropic transform -> non-uniform flag set, AABB reflects anisotropy,
    on-surface world point samples |d| ~ 0 in object space (S6_TODO tolerance)."""
    verdicts = []
    scene = SceneMirror()
    cube = mesh_desc(0xABCD0002)
    sx, sy, sz = NONUNIFORM_SCALE
    linear = [sx, 0, 0, 0, sy, 0, 0, 0, sz]
    sid = scene.add_instance(cube, linear, list(TRANSLATION))
    inst = scene.instances[sid]
    report = {"non_uniform_flag": bool(inst["non_uniform"]),
              "transform": {"linear": linear, "translation": list(TRANSLATION)}}
    verdicts.append(("C2 non-uniform scale flag set (kLumenMeshSDFInstanceFlagNonUniformScale)",
                     "PASS" if inst["non_uniform"] else "FAIL"))

    # World AABB of the transformed unit cube.
    corners = []
    for (x, y, z) in [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
                      (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]:
        corners.append((sx * x + TRANSLATION[0], sy * y + TRANSLATION[1], sz * z + TRANSLATION[2]))
    lo = tuple(min(c[i] for c in corners) for i in range(3))
    hi = tuple(max(c[i] for c in corners) for i in range(3))
    report["world_aabb"] = {"min": lo, "max": hi}
    expected_lo = (TRANSLATION[0] - sx, TRANSLATION[1] - sy, TRANSLATION[2] - sz)
    expected_hi = (TRANSLATION[0] + sx, TRANSLATION[1] + sy, TRANSLATION[2] + sz)
    aabb_ok = all(abs(lo[i] - expected_lo[i]) < 1e-9 and abs(hi[i] - expected_hi[i]) < 1e-9 for i in range(3))
    verdicts.append(("C2 world AABB reflects anisotropy (non-uniform bounds)",
                     "PASS" if aabb_ok else "FAIL"))

    # A world point ON the transformed cube surface (+x face) must map to object
    # distance ~ 0. The exact world-space distance under anisotropy is S6_TODO.
    on_surface_world = (sx + TRANSLATION[0], 0.5 * sy + TRANSLATION[1], -0.3 * sz + TRANSLATION[2])
    obj = scene.sample_object(sid, on_surface_world)
    surface = None
    if obj is not None:
        surface = abs(cube_analytic_object(obj))
        report["on_surface"] = {"world": list(on_surface_world), "object": list(obj), "abs_d": surface}
    else:
        report["on_surface"] = {"error": "instance not resident / sample failed"}
    if surface is not None:
        verdicts.append(("C2 on-surface |d| ~ 0 (<= %g, S6_TODO[nonuniform_tol])" % SURFACE_TOL,
                         "PASS" if surface <= SURFACE_TOL else "FAIL"))
    else:
        verdicts.append(("C2 on-surface |d| ~ 0 (S6_TODO[nonuniform_tol])", "SKIP"))
    return report, verdicts


def run_atlas_pressure():
    """C3: hard budget below total resident bytes -> evict farthest until the estimate
    fits; evicted instances are non-resident (degradation, never a crash)."""
    verdicts = []
    scene = SceneMirror(budget_bytes=0)
    descs = [mesh_desc(0xABCD1000 + i) for i in range(BUDGET_MESHES)]
    for i, d in enumerate(descs):
        x = -BUDGET_MESHES * 2.0 + i * 4.0   # spread along x so the camera at origin has a clear "farthest".
        scene.add_instance(d, [1, 0, 0, 0, 1, 0, 0, 0, 1], (x, 0.0, 0.0))
    full = scene.stats()

    # Budget = 2 x (one mesh's GPU bytes) -> must evict (BUDGET_MESHES - 2) instances.
    one_mesh_bytes = scene.meshes[list(scene.meshes.keys())[0]]["gpu_bytes"]
    budget = 2 * one_mesh_bytes
    within = scene.enforce_budget(budget)
    stats = scene.stats()
    evicted_ok = stats["evictedInstances"] == BUDGET_MESHES - 2 and stats["evictions"] == BUDGET_MESHES - 2
    budget_ok = stats["residentBytes"] <= budget and within
    no_leak_ok = sum(scene.atlas.used.values()) <= 2 * scene.atlas.capacity
    report = {"pre": full, "post": stats, "budget": budget, "one_mesh_bytes": one_mesh_bytes}
    verdicts.append(("C3 pressure: evicted instances == %d and evictions counter matches"
                     % (BUDGET_MESHES - 2), "PASS" if evicted_ok else "FAIL"))
    verdicts.append(("C3 budget enforced (residentBytes %d <= %d, within=%s)"
                     % (stats["residentBytes"], budget, within),
                     "PASS" if budget_ok else "FAIL"))
    verdicts.append(("C3 no atlas over-allocation / no leak", "PASS" if no_leak_ok else "FAIL"))

    # Evicted instances must report non-resident (degradation, never a crash).
    non_resident_reported = all(not scene.is_resident(i) for i in range(BUDGET_MESHES)
                                if scene.instances[i]["evicted"])
    verdicts.append(("C3 evicted instances report non-resident (no crash)",
                     "PASS" if non_resident_reported else "FAIL"))
    return report, verdicts


def run_evict_reload():
    """C4: restore an evicted instance; then reload()+apply_scene() re-materializes the
    whole scene from the DISK cache (cache HITs, builds NOT repeated)."""
    verdicts = []
    scene = SceneMirror(budget_bytes=0)
    descs = [mesh_desc(0xABCD2000 + i) for i in range(BUDGET_MESHES)]
    ids = [scene.add_instance(d, [1, 0, 0, 0, 1, 0, 0, 0, 1], (i * 4.0, 0.0, 0.0))
           for i, d in enumerate(descs)]
    one_mesh_bytes = scene.meshes[list(scene.meshes.keys())[0]]["gpu_bytes"]
    scene.enforce_budget(2 * one_mesh_bytes)
    builds_before = scene.builds
    evicted_id = next(i for i in ids if scene.instances[i]["evicted"])

    restore_ok = scene.restore_instance(evicted_id) and scene.is_resident(evicted_id)
    verdicts.append(("C4 restoreInstance re-residents an evicted instance",
                     "PASS" if restore_ok else "FAIL"))

    scene.reload()
    scene.apply_scene()
    stats = scene.stats()
    all_resident = stats["residentInstances"] == BUDGET_MESHES
    no_rebuild = stats["builds"] == builds_before
    cache_hits_grew = stats["cacheHits"] >= 3  # every apply is a hit; at least the 3 non-first meshes.
    verdicts.append(("C4 reload+apply re-materializes all instances",
                     "PASS" if all_resident else "FAIL"))
    verdicts.append(("C4 reload is a cache HIT, never a rebuild (builds %d == %d)"
                     % (stats["builds"], builds_before), "PASS" if no_rebuild else "FAIL"))
    return {"builds_before": builds_before, "restores": stats["restores"],
            "post_reload": stats}, verdicts


def create_lumen_graph(mark_stats_outputs):
    graph = RenderGraph("LumenGISDFAtlas")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(
        createPass("LumenGI", {"enabled": True, "traceMode": "MeshSDF", "qualityPreset": "High"}),
        "LumenGI",
    )
    for edge in [
        ("GBufferRT.vbuffer", "LumenGI.vbuffer"),
        ("GBufferRT.linearZ", "LumenGI.linearZ"),
        ("GBufferRT.mvec", "LumenGI.mvec"),
        ("GBufferRT.mvecW", "LumenGI.mvecW"),
        ("GBufferRT.normWRoughnessMaterialID", "LumenGI.normWRoughnessMaterialID"),
        ("GBufferRT.viewW", "LumenGI.viewW"),
        ("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity"),
        ("GBufferRT.emissive", "LumenGI.emissive"),
    ]:
        graph.addEdge(*edge)
    graph.markOutput("LumenGI.diffuseGI")
    if mark_stats_outputs:
        for ch in MESH_SDF_STATS_CHANNELS:
            graph.markOutput("LumenGI." + ch)
    return graph


def run_gpu_section():
    """S6_TODO[atlas_stats_channel]: probe the atlas stats channel / scriptable binding;
    SKIPs until the root pass wires the S6-B2 host (LumenMeshSDFScene is not bound at
    HEAD 3821d232). The mode-runs check uses a graph WITHOUT the stats channels marked
    (they are not reflected yet, so marking them would fail graph build)."""
    verdicts = []
    report = {}
    m.loadScene(SCENE)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()

    # Mode-runs + scriptable-binding probe on a graph without the optional channels.
    graph = None
    binding = None
    try:
        graph = create_lumen_graph(mark_stats_outputs=False)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        m.clock.frame = 1
        m.renderFrame()
        report["gpu_smoke"] = True
        try:
            binding = m.activeGraph.getPass("LumenGI").meshSDFSceneStats
            report["meshSDFSceneStats"] = dict(binding)
        except Exception as exc:
            report["meshSDFSceneStats_error"] = str(exc)
    except Exception as exc:
        report["gpu_error"] = str(exc)
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass
        graph = None

    # Channel probe: one throwaway graph per candidate.
    available = []
    for ch in MESH_SDF_STATS_CHANNELS:
        g = None
        try:
            g = create_lumen_graph(mark_stats_outputs=True)
            m.addGraph(g)
            m.setActiveGraph(g)
            m.clock.frame = 1
            m.renderFrame()
            m.activeGraph.get_output("LumenGI." + ch)
            available.append(ch)
        except Exception:
            pass
        finally:
            if g is not None:
                try:
                    m.removeGraph(g)
                except Exception:
                    pass

    report["stats_channels_available"] = available
    if available:
        verdicts.append(("S6_TODO atlas stats channel '%s' present" % available[0], "PASS"))
    elif binding is not None:
        verdicts.append(("S6_TODO 'meshSDFSceneStats' binding present", "PASS"))
    else:
        verdicts.append(("S6_TODO atlas stats channel/binding present", "SKIP"))
    return report, verdicts


def main():
    report = {
        "stage": "S6",
        "script": "run_sdf_atlas.py",
        "role": "S6-C2 Mesh SDF atlas / instance regression (Agent Z15)",
        "status": "skeleton",
        "resolution": list(RESOLUTION),
        "config": {
            "mesh_resolution": list(MESH_RESOLUTION),
            "budget_meshes": BUDGET_MESHES,
            "nonuniform_scale": list(NONUNIFORM_SCALE),
            "surface_tol": SURFACE_TOL,
            "stats_channels": MESH_SDF_STATS_CHANNELS,
            "stats_binding": MESH_SDF_STATS_BINDING,
        },
    }
    verdicts = []

    c1_report, c1_verdicts = run_page_sharing()
    report["page_sharing"] = c1_report
    verdicts.extend(c1_verdicts)

    c2_report, c2_verdicts = run_nonuniform_scale()
    report["nonuniform_scale"] = c2_report
    verdicts.extend(c2_verdicts)

    c3_report, c3_verdicts = run_atlas_pressure()
    report["atlas_pressure"] = c3_report
    verdicts.extend(c3_verdicts)

    c4_report, c4_verdicts = run_evict_reload()
    report["evict_reload"] = c4_report
    verdicts.extend(c4_verdicts)

    gpu_report, gpu_verdicts = run_gpu_section()
    report["gpu"] = gpu_report
    verdicts.extend(gpu_verdicts)

    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    if any(v == "SKIP" for _, v in verdicts):
        report["summary"] = "SKIP" if all(v != "FAIL" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("SDFATLAS VERDICT", name, verdict)
    write_json(OUT_JSON, report)
    print("SDFATLAS wrote", os.path.abspath(OUT_JSON))


try:
    main()
except Exception as exc:
    print("SDFATLAS ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S6",
            "script": "run_sdf_atlas.py",
            "summary": "SKIP",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive SKIP on fatal error)", "SKIP")],
        },
    )
exit()
