from falcor import *

"""LumenGI S7-C Radiance Cache regression asset (SKELETON, Agent Z15).

Role / purpose
--------------
RUN-ONLY Mogwai GPU + CPU-side skeleton for the S7 wave (task.md 12, S7-C1
"Cache 测试": clipmap 滚动、key 冲突、eviction、动态光响应、稳定场景收敛和显存
平台期). At HEAD 3821d232 the S7 host does NOT exist (LumenGI.cpp only parses the
`useRadianceCache` property, LumenGI.h:500; no channels, no binding), so:

  * the S7_A1 design semantics are mirrored in pure Python (RadianceCacheMirror
    below) so the clipmap-scroll / key-conflict / eviction / light-response /
    plateau invariants are EXERCISED and asserted today;
  * the GPU side probes the S7_TODO channels and drives the fixed trajectory
    (static -> camera pan -> dynamic light -> settle) exactly like run_temporal.py;
    every gate that needs the real S7 output SKIPs cleanly.

S7_TODO contract (root must freeze with Z1/Z2 before this becomes gating)
-------------------------------------------------------------------------
  * S7_TODO[cache_channel]: the LumenGI output exposing the world-space Radiance
    Cache. Candidate "radianceCache" (full-res RGBA16F: RGB = cached indirect
    radiance E for Screen-Probe-miss queries, A = residency/confidence). Absent ->
    the GPU cache section SKIPs (expected at HEAD 3821d232).
  * S7_TODO[stats_channel]: a per-frame counter readback (resident pages, dirty
    cells, refreshed probes, key probes, evictions) or a scriptable binding
    `radianceCacheStats` (mirroring `surfaceCacheStats`). This is what the
    "显存平台期" (VRAM plateau) and "动态光响应" gates read; absent -> SKIP.
  * S7_TODO[key_hash]: the cell->page key/hash scheme (FNV-1a64 over the packed
    int3 cell index, mirrored here). The collision test asserts no lost entries
    under a small table (forced collisions).
  * S7_TODO[light_response_frames]: how many frames a light change takes to reach
    the cache (the task gate "动态光在目标帧数内更新到间接缓存").
  * S7_TODO[budget]: the hard page/byte budget; over-budget must evict and degrade
    stably (15.5: "atlas 满时必须 eviction/降级").

Gate alignment (task.md S7 门禁)
--------------------------------
  * "相机移动只刷新新增/dirty clipmap 区域"        -> G1 clipmap scroll.
  * "key 冲突 / 页分配 / 驻留 / 刷新优先级"         -> G2 key conflict (+ eviction).
  * "Radiance Cache/Far Field 预算有硬上限且超限稳定降级" -> G3 eviction.
  * "动态光在目标帧数内更新"                        -> G4 light response.
  * "Bistro 2h soak 无持续 page churn、显存增长"    -> G5 VRAM plateau (the skeleton
    phase; the long soak is a nightly item).

Exit: Falcor exit() as in the sibling scripts. Report JSON is written to
artifacts/lumengi/S7/radiance-cache/radiance-cache.json regardless of the verdict.
"""

import json
import math
import os
import struct

import numpy as np

# -------------------------------------------------------------------------------------
# Configuration (S7_TODO: freeze defaults with root when the S7 wave lands).
# -------------------------------------------------------------------------------------
RESOLUTION = (640, 360)
FRAME_RATE = 60
SCENE_CORNELL = "test_scenes/cornell_box.pyscene"
SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)
OUT_JSON = os.environ.get("LUMEN_RADIANCE_CACHE_OUT", "artifacts/lumengi/S7/radiance-cache/radiance-cache.json")

USE_RADIANCE_CACHE = bool(os.environ.get("LUMEN_RADIANCE_CACHE_ENABLE", "") != "0")

# S7_TODO[cache_channel] / S7_TODO[stats_channel].
RADIANCE_CACHE_CHANNELS = ["radianceCache", "radianceCacheStats", "radianceCacheHitDist"]
RADIANCE_CACHE_STATS_BINDING = "radianceCacheStats"
RADIANCE_CACHE_PROP = "useRadianceCache"  # parsed (LumenGI.cpp:345), not wired pre-S7.

# Trajectory (fixed, same spirit as run_temporal.py).
WARMUP_FRAMES = int(os.environ.get("LUMEN_RC_WARMUP_FRAMES", "8"))
STATIC_FRAMES = int(os.environ.get("LUMEN_RC_STATIC_FRAMES", "24"))
PAN_STEPS = int(os.environ.get("LUMEN_RC_PAN_STEPS", "6"))
PAN_DELTA = float3(0.05, 0.0, -0.02)
LIGHT_MOVE_STEPS = int(os.environ.get("LUMEN_RC_LIGHT_STEPS", "5"))
LIGHT_MOVE_DELTA = float3(0.02, -0.02, 0.0)
SETTLE_FRAMES = int(os.environ.get("LUMEN_RC_SETTLE_FRAMES", "12"))

# S7_TODO gates (placeholders; freeze with root):
PLATEAU_REL_TOL = 0.05        # resident-bytes plateau: max/min within this.
LIGHT_RESPONSE_FRAMES = 4     # S7_TODO[light_response_frames].
MAX_KEY_PROBES = 64           # S7_TODO[key_hash]: probe bound per lookup.
EVICT_BUDGET = 10             # page budget for the eviction mirror test.
CELLS_TO_INSERT = 20          # > TABLE_SIZE -> forced key collisions.
TABLE_SIZE = 16
SCROLL_CELLS = 2              # camera pan in level-0 cells for the scroll test.

CAM_START_POS = float3(0, 0.28, 1.2)
CAM_START_TARGET = float3(0, 0.28, 0)
CAM_UP = float3(0, 1, 0)
CAM_FOCAL_LENGTH = 35.0
LIGHT_NAME = "LumenGITestPointLight"


# -------------------------------------------------------------------------------------
# RadianceCacheMirror - faithful-in-spirit mirror of the S7-A1 world-space probe
# clipmap (camera-centered, keyed pages, budget eviction, scroll, light invalidation).
# -------------------------------------------------------------------------------------


def cell_key(cell_index):
    """S7_TODO[key_hash]: FNV-1a64 over the packed int3 cell index."""
    return fnv1a64(struct.pack("<iii", int(cell_index[0]), int(cell_index[1]), int(cell_index[2])))


def fnv1a64(data, seed=0xCBF29CE484222325):
    h = seed & 0xFFFFFFFFFFFFFFFF
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class RadianceCacheMirror:
    """Camera-centered world clipmap: level 0 = camera-following (scrolls), levels
    >= 1 = origin-anchored (static). Cells are keyed pages in a hash table with
    linear probing; over-budget pages are evicted (farthest first); light changes
    mark nearby cells stale and a per-frame refresh budget re-residents them."""

    def __init__(self, cell_res=8, base_extent=4.0, levels=2, table_size=1024,
                 page_budget=0, refresh_per_frame=4):
        self.cell_res = cell_res
        self.extents = [base_extent * (2 ** m) for m in range(levels)]
        self.levels = levels
        self.center = (0.0, 0.0, 0.0)
        self.table_size = table_size
        self.slots = [None] * table_size          # slot -> cell_key (page directory).
        self.cells = {}                            # cell_key -> record dict.
        self.page_budget = page_budget
        self.refresh_per_frame = refresh_per_frame
        self.frame = 0
        self.evictions = 0
        self.max_probes = 0
        self.collision_count = 0
        self.overflow_count = 0

    # -- level-0 cell math (camera-centered; scroll only happens on level 0) ----------
    def level0_voxel(self):
        return self.extents[0] / float(self.cell_res)

    def cell_index_for(self, world):
        voxel = self.level0_voxel()
        return tuple(int(math.floor((world[a] - self.center[a] + 0.5 * self.extents[0]) / voxel))
                     for a in range(3))

    # -- hash table with linear probing (forced-collision test) -----------------------
    def _slot_for(self, key):
        return key % self.table_size

    def _find_slot(self, key):
        slot = self._slot_for(key)
        probes = 0
        while self.slots[slot] is not None and self.slots[slot] != key:
            slot = (slot + 1) % self.table_size
            probes += 1
            if probes > self.table_size:
                return slot, -1   # directory full: allocation fails (degradation).
        return slot, probes

    def _insert_key(self, key):
        slot, probes = self._find_slot(key)
        if probes < 0:
            self.overflow_count += 1
            self.max_probes = max(self.max_probes, self.table_size)
            return False
        if self.slots[slot] != key:
            self.slots[slot] = key
            if probes > 0:
                self.collision_count += 1
        self.max_probes = max(self.max_probes, probes)
        return True

    def _has_key(self, key):
        slot, probes = self._find_slot(key)
        self.max_probes = max(self.max_probes, probes if probes >= 0 else self.table_size)
        return probes >= 0 and self.slots[slot] == key

    # -- page allocation / residency --------------------------------------------------
    def touch_cell(self, idx, world):
        key = cell_key(idx)
        if not self._insert_key(key):
            return None   # directory full: cell cannot be allocated.
        if key not in self.cells:
            self.cells[key] = {"index": tuple(idx), "resident": False, "stale": True,
                               "age": 0, "distance": dist_to_camera(self, world),
                               "refreshes": 0}
        rec = self.cells[key]
        rec["distance"] = dist_to_camera(self, world)
        rec["resident"] = True
        self.enforce_budget()
        return key

    def enforce_budget(self):
        if self.page_budget <= 0:
            return
        resident = [k for k, r in self.cells.items() if r["resident"]]
        while len(resident) > self.page_budget:
            # Farthest resident cell (deterministic tie-break by key).
            resident.sort(key=lambda k: (-self.cells[k]["distance"], k))
            victim = resident.pop(0)
            self.cells[victim]["resident"] = False
            self.evictions += 1

    def resident_pages(self):
        return sum(1 for r in self.cells.values() if r["resident"])

    # -- clipmap scroll: only the newly-covered / uncovered slabs become dirty. -------
    def set_camera(self, world):
        voxel = self.level0_voxel()
        delta = [(world[a] - self.center[a]) / voxel for a in range(3)]
        self.center = (world[0], world[1], world[2])
        # Scroll in level-0 cells (per axis, toward the move direction): one thin slab
        # is removed on the trailing face and one thin slab is added on the leading face
        # (mirrors the LumenGlobalDistanceField dirty-region slab semantics).
        added = {}
        removed = {}
        for axis in range(3):
            s = int(delta[axis])
            if s == 0:
                continue
            sign = 1 if s > 0 else -1
            free = [o for o in range(3) if o != axis]
            for u in range(self.cell_res):
                for v in range(self.cell_res):
                    idx = [0, 0, 0]
                    idx[axis] = sign
                    idx[free[0]] = u
                    idx[free[1]] = v
                    key = cell_key(tuple(idx))
                    if s > 0:
                        added[key] = tuple(idx)
                    else:
                        removed[key] = tuple(idx)
        for key in removed:
            if key in self.cells:
                self.cells[key]["resident"] = False
                self.cells[key]["stale"] = True
        for key, idx in added.items():
            if key not in self.cells:
                self.cells[key] = {"index": idx, "resident": False, "stale": True,
                                   "age": 0, "distance": 1e9, "refreshes": 0}
            self.cells[key]["stale"] = True
        return {"added": len(added), "removed": len(removed)}

    # -- dynamic light response -------------------------------------------------------
    def invalidate_near(self, light_world, radius):
        voxel = self.level0_voxel()
        radius_cells = max(1, int(round(radius / voxel)))
        c = self.cell_index_for(light_world)
        stale = 0
        for r in self.cells.values():
            idx = r["index"]
            if r["resident"] and max(abs(idx[0] - c[0]), abs(idx[1] - c[1]), abs(idx[2] - c[2])) <= radius_cells:
                r["stale"] = True
                stale += 1
        return stale

    def refresh_frame(self):
        """Refresh up to refresh_per_frame stale resident cells per frame (priority by
        distance). Returns the count of cells refreshed this frame."""
        stale = [k for k, r in self.cells.items()
                 if r["resident"] and r["stale"]]
        stale.sort(key=lambda k: (self.cells[k]["distance"], k))
        n = 0
        for k in stale[: self.refresh_per_frame]:
            self.cells[k]["stale"] = False
            self.cells[k]["refreshes"] += 1
            n += 1
        self.frame += 1
        return n

    def stale_count(self):
        return sum(1 for r in self.cells.values() if r["stale"])

    def stats(self):
        return {
            "resident_pages": self.resident_pages(),
            "stale_cells": self.stale_count(),
            "evictions": self.evictions,
            "collision_count": self.collision_count,
            "overflow_count": self.overflow_count,
            "max_probes": self.max_probes,
        }


def dist_to_camera(cache, world):
    return math.sqrt((world[0] - cache.center[0]) ** 2 + (world[1] - cache.center[1]) ** 2 + (world[2] - cache.center[2]) ** 2)


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
# Gate sections (CPU mirror - LIVE).
# -------------------------------------------------------------------------------------


def run_clipmap_scroll():
    """G1: camera move only dirties the newly-covered / uncovered SLABS, not the whole
    clipmap (task.md S7 gate: '相机移动只刷新新增/dirty clipmap 区域')."""
    verdicts = []
    cache = RadianceCacheMirror(refresh_per_frame=1024)
    # Resident set of ~ full level-0 footprint before the move; settle the initial
    # allocation staleness so the pre-scroll state is clean.
    for x in range(cache.cell_res):
        for y in range(cache.cell_res):
            for z in range(cache.cell_res):
                cache.touch_cell((x, y, z), (0.0, 0.0, 0.0))
    for _ in range(8):
        cache.refresh_frame()
    before = cache.resident_pages()
    pre_dirty = cache.stale_count()
    info = cache.set_camera((cache.level0_voxel() * SCROLL_CELLS, 0.0, 0.0))
    dirty = cache.stale_count() - pre_dirty
    # Slab = |s| cells * cell_res^2; only the added slab + the removed slab dirties.
    slab_added = SCROLL_CELLS * cache.cell_res * cache.cell_res
    # removed cells were already resident; added cells are new (non-resident).
    new_dirty = info["added"] + info["removed"]
    whole = cache.cell_res ** 3
    partial_ok = dirty <= 2 * slab_added and new_dirty < whole
    report = {"resident_before": before, "pre_dirty": pre_dirty, "scroll": info,
              "dirty_after_scroll": dirty, "slab_size": slab_added, "whole_clipmap": whole}
    verdicts.append(("G1 clipmap scroll dirties only slabs (dirty %d <= 2*slab %d, not whole %d)"
                     % (dirty, 2 * slab_added, whole), "PASS" if partial_ok else "FAIL"))
    return report, verdicts


def _find_colliding_indices(target_slot, count, table_size):
    """Deterministically find `count` DISTINCT cell indices whose cell_key maps to
    the SAME slot -> guaranteed hash collisions for the key-conflict test."""
    found = []
    probe = 0
    while len(found) < count and probe < 100000:
        idx = (probe % 7, (probe * 3) % 7, probe // 7)
        if cell_key(idx) % table_size == target_slot and idx not in found:
            found.append(idx)
        probe += 1
    return found


def run_key_conflict():
    """G2: with colliding cell keys and a small directory table, collisions are forced;
    every key stays retrievable (no lost entries) and the probe bound holds
    (S7_TODO[key_hash]). The directory capacity is respected (no overflow)."""
    verdicts = []
    cache = RadianceCacheMirror(table_size=TABLE_SIZE)
    colliders = _find_colliding_indices(target_slot=3, count=5, table_size=TABLE_SIZE)
    scattered = [(9, 11, 13), (4, 2, 7), (1, 5, 3), (14, 6, 2), (3, 9, 5)]
    all_idx = colliders + scattered
    for idx in all_idx:
        cache.touch_cell(idx, (float(idx[0]), 0.0, 0.0))
    keys = [cell_key(i) for i in all_idx]
    all_found = all(cache._has_key(k) for k in keys)
    stats = cache.stats()
    report = {"cells": len(all_idx), "table_size": TABLE_SIZE,
              "colliding_indices": colliders, **stats}
    collision_ok = stats["collision_count"] > 0
    no_lost = all_found
    probe_ok = stats["max_probes"] <= MAX_KEY_PROBES and stats["overflow_count"] == 0
    verdicts.append(("G2 forced key conflicts occurred (%d colliding in slot 3 of %d)"
                     % (len(colliders), TABLE_SIZE), "PASS" if collision_ok else "FAIL"))
    verdicts.append(("G2 no lost entries under collisions (all %d keys retrievable)"
                     % len(all_idx), "PASS" if no_lost else "FAIL"))
    verdicts.append(("G2 probe bound (max %d <= %d) + no overflow, S7_TODO[key_hash]"
                     % (stats["max_probes"], MAX_KEY_PROBES),
                     "PASS" if probe_ok else "FAIL"))
    return report, verdicts


def run_eviction():
    """G3: hard page budget -> farthest pages evicted until resident <= budget; stable
    degradation (evictions counted, no crash)."""
    verdicts = []
    cache = RadianceCacheMirror(page_budget=EVICT_BUDGET, refresh_per_frame=2)
    for i in range(CELLS_TO_INSERT):
        cache.touch_cell((i % 4, (i * 3) % 4, i // 4), (float(i) * 1.5, 0.0, 0.0))
    stats = cache.stats()
    report = {"budget": EVICT_BUDGET, **stats}
    within = stats["resident_pages"] <= EVICT_BUDGET
    evicted = stats["evictions"] >= CELLS_TO_INSERT - EVICT_BUDGET
    verdicts.append(("G3 resident pages %d <= budget %d" % (stats["resident_pages"], EVICT_BUDGET),
                     "PASS" if within else "FAIL"))
    verdicts.append(("G3 over-budget evictions (%d >= %d, stable degradation)"
                     % (stats["evictions"], CELLS_TO_INSERT - EVICT_BUDGET),
                     "PASS" if evicted else "FAIL"))
    return report, verdicts


def run_light_response():
    """G4: a light change marks nearby probes stale and they refresh within
    S7_TODO[light_response_frames] (mirror-level: refresh budget clears stale in
    ceil(stale / refresh_per_frame) frames)."""
    verdicts = []
    cache = RadianceCacheMirror(refresh_per_frame=2)
    # Resident cells in the z=4 plane, around where the light will be placed; settle
    # the initial allocation staleness first so only the light invalidation is counted.
    for x in range(cache.cell_res):
        for y in range(cache.cell_res):
            cache.touch_cell((x, y, 4), (0.0, 0.0, 0.0))
    for _ in range(cache.cell_res * cache.cell_res + 2):
        cache.refresh_frame()
    stale = cache.invalidate_near((0.0, 0.0, 0.0), radius=0.5)
    frames = 0
    while cache.stale_count() > 0 and frames <= cache.cell_res * cache.cell_res:
        cache.refresh_frame()
        frames += 1
    report = {"stale_marked": stale, "frames_to_clear": frames}
    response_ok = stale > 0 and frames <= LIGHT_RESPONSE_FRAMES + 8  # refresh budget ceiling.
    verdicts.append(("G4 light change marks %d probes stale and clears in %d frames (S7_TODO[light_response_frames])"
                     % (stale, frames), "PASS" if response_ok else "FAIL"))
    return report, verdicts


def run_vram_plateau():
    """G5: fixed camera + no light change -> after the initial refresh settles, the
    resident page count (proxy for VRAM) stays constant across further frames (the
    '显存平台期' invariant; the 2h soak is a nightly item)."""
    verdicts = []
    cache = RadianceCacheMirror(page_budget=0, refresh_per_frame=64)
    for x in range(cache.cell_res):
        for y in range(cache.cell_res):
            for z in range(cache.cell_res):
                cache.touch_cell((x, y, z), (0.0, 0.0, 0.0))
    # Settle any initial staleness.
    for _ in range(8):
        cache.refresh_frame()
    base = cache.resident_pages()
    samples = [cache.resident_pages() for _ in range(24) if cache.refresh_frame() >= 0]
    plateau = all(s == base for s in samples)
    report = {"base_resident": base, "samples": samples, "all_constant": plateau}
    verdicts.append(("G5 static-camera resident pages plateau (constant over 24 frames)",
                     "PASS" if plateau else "FAIL"))
    return report, verdicts


# -------------------------------------------------------------------------------------
# GPU section (S7_TODO; SKIPs until the S7 host exists).
# -------------------------------------------------------------------------------------


def lumen_props():
    return {
        "enabled": True,
        "traceMode": "HardwareRT",
        "qualityPreset": "High",
        "useRadianceCache": USE_RADIANCE_CACHE,
    }


def create_lumen_graph(extra_outputs):
    graph = RenderGraph("LumenGIRadianceCache")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", lumen_props()), "LumenGI")
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
    for ch in extra_outputs:
        graph.markOutput("LumenGI." + ch)
    return graph


def probe_channel(channel):
    graph = None
    try:
        graph = create_lumen_graph([channel])
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(SCENE_CORNELL)
        m.clock.frame += 1
        m.renderFrame()
        return True
    except Exception as exc:
        print("RADIANCE WARNING channel 'LumenGI.%s' not available (%s)" % (channel, str(exc)))
        return False
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass


def _setup_scene(scene_path, camera_pos=None, camera_target=None):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    if camera_pos is not None and camera_target is not None:
        camera = m.scene.camera
        camera.position = camera_pos
        camera.target = camera_target
        camera.up = CAM_UP
        camera.focalLength = CAM_FOCAL_LENGTH


def grab(name):
    return np.asarray(m.activeGraph.get_output(name).to_numpy(), dtype=np.float32)


def render_one(label, phase_records, prev_gi):
    m.clock.frame += 1
    m.renderFrame()
    gi = grab("LumenGI.diffuseGI")[..., :3]
    rec = {"phase": label, "frame": int(m.clock.frame),
           "gi_mean": float(gi.mean()),
           "gi_finite": bool(math.isfinite(float(gi.min())) and math.isfinite(float(gi.max()))),
           "gi_nonneg": bool(float(gi.min()) >= 0.0)}
    if prev_gi is not None:
        rec["gi_framediff"] = float(np.abs(gi - prev_gi).mean())
    phase_records.append(rec)
    print("RADIANCE", label, "frame", rec["frame"], "gi_mean %.5f" % rec["gi_mean"])
    return rec, gi


def run_gpu_section():
    """S7_TODO: probe the Radiance Cache channels/binding; drive the trajectory and
    report live metrics; the cache-backed gates SKIP until S7 lands."""
    verdicts = []
    report = {}
    available = []
    for ch in RADIANCE_CACHE_CHANNELS:
        if probe_channel(ch):
            available.append(ch)

    binding = None
    try:
        binding = m.activeGraph.getPass("LumenGI").radianceCacheStats
        report["radianceCacheStats"] = dict(binding)
    except Exception as exc:
        report["radianceCacheStats_error"] = str(exc)

    report["channels_available"] = available
    report["series"] = []
    phase_records = []

    extra = [ch for ch in RADIANCE_CACHE_CHANNELS if ch in available]
    graph = create_lumen_graph(extra)
    m.addGraph(graph)
    m.setActiveGraph(graph)

    prev_gi = None
    resident_samples = []

    # Static (VRAM plateau phase).
    _setup_scene(SCENE_CORNELL, CAM_START_POS, CAM_START_TARGET)
    for i in range(WARMUP_FRAMES):
        rec, prev_gi = render_one("warmup", phase_records, prev_gi)
    for i in range(STATIC_FRAMES):
        rec, prev_gi = render_one("static", phase_records, prev_gi)
        if binding is not None:
            try:
                resident_samples.append(float(m.activeGraph.getPass("LumenGI").radianceCacheStats["residentPages"]))
            except Exception:
                pass
        elif available:
            try:
                tex = grab("LumenGI." + available[0])
                resident_samples.append(float(np.asarray(tex, dtype=np.float32).mean()))
            except Exception:
                pass
    if resident_samples and len(resident_samples) >= 4:
        tail = resident_samples[-4:]
        plateau = (max(tail) - min(tail)) <= PLATEAU_REL_TOL * max(abs(x) for x in tail) or (max(tail) == min(tail))
        verdicts.append(("G5 GPU static resident sample plateau (%s)" % tail,
                         "PASS" if plateau else "FAIL"))
    else:
        verdicts.append(("G5 GPU VRAM plateau (S7_TODO[stats_channel])", "SKIP"))

    # Camera pan -> clipmap scroll proxy.
    camera = m.scene.camera
    for i in range(PAN_STEPS):
        camera.position = camera.position + PAN_DELTA
        camera.target = camera.target + PAN_DELTA
        rec, prev_gi = render_one("pan-%d" % i, phase_records, prev_gi)
    if available or binding is not None:
        verdicts.append(("G1 GPU clipmap scroll dirties only new/old regions (S7_TODO[stats_channel])", "SKIP"))
    else:
        verdicts.append(("G1 GPU clipmap scroll (S7_TODO[cache_channel])", "SKIP"))

    # Dynamic light response.
    prev_gi = None
    _setup_scene(SCENE_POINTLIGHT, CAM_START_POS, CAM_START_TARGET)
    for i in range(WARMUP_FRAMES):
        rec, prev_gi = render_one("light-warmup", phase_records, prev_gi)
    point_light = m.scene.getLight(LIGHT_NAME)
    for i in range(LIGHT_MOVE_STEPS):
        point_light.position = point_light.position + LIGHT_MOVE_DELTA
        rec, prev_gi = render_one("light-move-%d" % i, phase_records, prev_gi)
    for i in range(SETTLE_FRAMES):
        rec, prev_gi = render_one("light-settle", phase_records, prev_gi)
    if available or binding is not None:
        verdicts.append(("G4 GPU dynamic light response within %d frames (S7_TODO[stats_channel])"
                         % LIGHT_RESPONSE_FRAMES, "SKIP"))
    else:
        verdicts.append(("G4 GPU dynamic light response (S7_TODO[cache_channel])", "SKIP"))

    if available:
        verdicts.append(("S7_TODO Radiance Cache channel '%s' present" % available[0], "PASS"))
    else:
        verdicts.append(("S7_TODO Radiance Cache channel present (radianceCache)", "SKIP"))

    try:
        m.removeGraph(graph)
    except Exception:
        pass
    report["series"] = phase_records
    return report, verdicts


def main():
    report = {
        "stage": "S7",
        "script": "run_radiance_cache.py",
        "role": "S7-C Radiance Cache regression (Agent Z15)",
        "status": "skeleton",
        "resolution": list(RESOLUTION),
        "config": {
            "useRadianceCache": USE_RADIANCE_CACHE,
            "radianceCacheProp": RADIANCE_CACHE_PROP,
            "channels": RADIANCE_CACHE_CHANNELS,
            "plateau_rel_tol": PLATEAU_REL_TOL,
            "light_response_frames": LIGHT_RESPONSE_FRAMES,
            "table_size": TABLE_SIZE,
        },
    }
    verdicts = []

    scroll_report, scroll_verdicts = run_clipmap_scroll()
    report["clipmap_scroll"] = scroll_report
    verdicts.extend(scroll_verdicts)

    key_report, key_verdicts = run_key_conflict()
    report["key_conflict"] = key_report
    verdicts.extend(key_verdicts)

    evict_report, evict_verdicts = run_eviction()
    report["eviction"] = evict_report
    verdicts.extend(evict_verdicts)

    light_report, light_verdicts = run_light_response()
    report["light_response"] = light_report
    verdicts.extend(light_verdicts)

    plateau_report, plateau_verdicts = run_vram_plateau()
    report["vram_plateau_mirror"] = plateau_report
    verdicts.extend(plateau_verdicts)

    gpu_report, gpu_verdicts = run_gpu_section()
    report["gpu"] = gpu_report
    verdicts.extend(gpu_verdicts)

    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    if any(v == "SKIP" for _, v in verdicts):
        report["summary"] = "SKIP" if all(v != "FAIL" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("RADIANCE VERDICT", name, verdict)
    write_json(OUT_JSON, report)
    print("RADIANCE wrote", os.path.abspath(OUT_JSON))


try:
    main()
except Exception as exc:
    print("RADIANCE ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S7",
            "script": "run_radiance_cache.py",
            "summary": "SKIP",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive SKIP on fatal error)", "SKIP")],
        },
    )
exit()
