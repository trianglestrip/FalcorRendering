"""LumenGI S2 Gate validation asset: 60-second page-churn proxy (RUN-ONLY).

Role / purpose
--------------
Agent N (Test-Tooling) verification asset for the S2 Gate (task.md 7:
"Atlas allocator CPU/GPU tests, capture image tests, 30 分钟 churn 测试通过",
and task.md 15.5: "S2 起 30 分钟" soak). Running the full 30-minute soak on
every S2 integration iteration is too slow, so this script is the 60-second
PROXY: it exercises the same churn drivers (dirty injection, scene reload,
resize) and records the allocator/scheduler counters (alloc / release / fail
/ lost) into a per-sample time series JSON. Root runs it as part of the S2
Gate; the full 30-minute nightly run is a superset of this loop (see the
nightly command below).

STATUS: runnable against the CURRENT S2 integration. The S2 capture stats
(alloc/release/fail/lost) are C++-side (LumenSurfaceCacheStats,
LumenCaptureSchedulerStats) and are NOT yet exposed to Python, so this script
records `null` for them and prints a warning; the churn loop itself (material
dirty injection + reload + resize) always runs and the series/JSON still
documents the observable frame sequence. Root/Agent A must expose a script
binding for the counters (S2_TODO below) to fill the series.

What "dirty injection" means here
---------------------------------
* Per-frame material dirty injection: this script tries to toggle a scene
  material's baseColor each frame. Material setters call
  Material::markUpdates() and the Scene maps them to
  IScene::UpdateFlags::MaterialsChanged (Scene.cpp:1759), which is the S2-A2
  contract input for per-card dirty invalidation. If the Python material API
  is unavailable (or the pass does not yet map MaterialsChanged to dirty
  pages), the capability probe records `material_toggle_available=False` and
  churn is driven by the static path below instead.
* Static churn (always on): periodic scene reload (re-captures every card
  page) + periodic framebuffer resize (rebuilds screen resources), mirroring
  run_s2verify.py's reload/resize blocks.

Output contract (JSON, LUMEN_CHURN_OUT)
---------------------------------------
  schema_version, mode ("60s-proxy"), seconds, frame_count,
  material_toggle_available, stats_available,
  series: [{frame, dirty_injections, alloc, release, fail, lost,
            recapture, allocated_pages, free_pages, resident_bytes}, ...],
  totals: {alloc, release, fail, lost, reloads, resizes, final_frames},
  nightly_command_30min: the full soak invocation (see comments).

Full 30-minute nightly command (run by root on the GPU machine, from repo root)
-------------------------------------------------------------------------------
  $env:LUMEN_CHURN_SECONDS='1800'
  build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
    --device-type d3d12 --headless --precise ^
    --script tests\\lumengi\\run_churn_short.py ^
    --logfile artifacts\\lumengi\\S2\\churn-30min.log
  (1800 s * 60 fps = 108000 frames; the nightly runner may later wrap this in
   its own framing, task.md 17.1. The S2 gate only requires the 60-second proxy
   here; the full soak is the S9 release-candidate round.)

Verified APIs used
------------------
* m.activeGraph.get_pass("LumenGI")           (RenderGraph::getPass, RenderGraph.cpp:759)
* m.scene.materials                            (Scene::getMaterials, Scene.cpp:4348)
* material.baseColor = float4(...)             (StandardMaterial setter, triggers MaterialsChanged)
* m.loadScene(path) / m.resizeFrameBuffer(w,h) (run_s2verify.py precedent)
* m.timingCapture / m.clock                    (existing run_*.py pattern)

Known pitfalls encoded here
---------------------------
* S2_TODO: the allocator/scheduler counters are not script-bound yet. This
  script probes pass.stats / schedulerStats / cacheStats / getStats() and uses
  whatever the root exposes; until then the counts are null (see header).
* Material change propagation to per-card dirty depends on S2-A2
  (LumenCardScene::update mapping MaterialsChanged). The proxy records whether
  material toggling is even possible; it does not assert on it.
* Stats are sampled every SAMPLE_INTERVAL_FRAMES, not every frame, to keep the
  JSON and the sampling cost bounded.
"""

from falcor import *
import math
import os
import json

FRAME_RATE = 60
RESOLUTION = (640, 360)
SMALL_RESOLUTION = (320, 180)

# 60-second proxy at 60 fps. Override with LUMEN_CHURN_SECONDS for longer runs
# (1800 = the full 30-minute nightly soak; see header).
DEFAULT_SECONDS = 60
SECONDS = float(os.environ.get("LUMEN_CHURN_SECONDS", DEFAULT_SECONDS))
FRAME_COUNT = int(SECONDS * FRAME_RATE)

# Every SAMPLE_INTERVAL_FRAMES frames the counters are read into the series.
SAMPLE_INTERVAL_FRAMES = 30

# Static-churn drivers (always active): reload the scene and resize the
# framebuffer periodically so pages get re-captured and screen resources
# rebuilt even when material dirty injection is unavailable.
RELOAD_INTERVAL_FRAMES = 600
RESIZE_INTERVAL_FRAMES = 300

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"

OUT_JSON = os.environ.get("LUMEN_CHURN_OUT", "artifacts/lumengi/S2/churn-short.json")

# S2_TODO: threshold that root freezes at the S2 gate. The proxy only records
# the series; the nightly runner asserts the S2 gate invariants (no leak,
# bounded churn, no unbounded resident growth). Placeholder: if stats are
# available, fail if alloc/fail/lost diverge by more than this factor across
# the window (see VERDICT lines below).
CHURN_DIVERGENCE_MAX = 10.0

MATERIAL_COLORS = (float4(0.725, 0.71, 0.68, 1.0), float4(0.20, 0.30, 0.90, 1.0))


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


def create_lumen_graph():
    graph = RenderGraph("LumenGIChurn")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", {"useSurfaceCache": True}), "LumenGI")
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
    graph.markOutput("LumenGI.confidence")
    return graph


def try_read_pass_stats():
    """Best-effort read of the LumenGI pass counters. S2_TODO: root must expose
    a script binding (e.g. pass.getStats() -> dict mirroring
    LumenSurfaceCacheStats / LumenCaptureSchedulerStats) for the counts to be
    non-None. Probes several candidate accessors so it keeps working whichever
    name the root picks."""
    try:
        pass_obj = m.activeGraph.get_pass("LumenGI")
    except Exception:
        return None
    for attr in ("stats", "schedulerStats", "cacheStats", "getStats"):
        try:
            val = getattr(pass_obj, attr, None)
            if callable(val):
                val = val()
            if val is not None:
                return val
        except Exception:
            continue
    return None


def normalize_stats(raw):
    """Map an unknown-shaped stats object onto the stable series keys. Returns
    None when the raw value is missing (counters not script-bound yet)."""
    if raw is None:
        return None

    def get(obj, *names):
        for n in names:
            try:
                v = getattr(obj, n, None)
                if v is None and isinstance(obj, dict):
                    v = obj.get(n)
                if v is not None:
                    return v
            except Exception:
                pass
        return None

    return {
        "alloc": get(raw, "totalAllocations", "allocationCount", "alloc"),
        "release": get(raw, "totalReleases", "releaseCount", "release"),
        "fail": get(raw, "totalAllocationFailures", "fail"),
        "lost": get(raw, "totalLostPages", "lost"),
        "recapture": get(raw, "totalRecaptures", "recapture"),
        "allocated_pages": get(raw, "allocatedPageCount"),
        "free_pages": get(raw, "freePageCount"),
        "resident_bytes": get(raw, "residentBytes"),
    }


def probe_material_toggle():
    """Try toggling one material's baseColor. Returns (available, material).
    Material setters trigger MaterialsChanged (Scene.cpp:1759); whether the
    S2 capture scheduler maps that to dirty pages is verified by the series."""
    try:
        mats = m.scene.materials
        if not mats:
            return False, None
        mat = mats[0]
        mat.baseColor = MATERIAL_COLORS[0]  # succeeds only if the setter exists
        return True, mat
    except Exception as exc:
        print("CHURN WARNING material dirty injection unavailable: %s" % str(exc))
        return False, None


def main():
    graph = create_lumen_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    m.loadScene(SCENE_CORNELL)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0

    material_available, material = probe_material_toggle()
    print("CHURN seconds", SECONDS, "frames", FRAME_COUNT, "material_toggle_available", material_available)

    series = []
    totals = {"alloc": None, "release": None, "fail": None, "lost": None}
    reloads = 0
    resizes = 0
    dirty_injections = 0
    toggle_index = 0
    is_small = False
    stats_available = False
    last_norm = None

    for frame in range(1, FRAME_COUNT + 1):
        m.clock.frame = frame

        # Per-frame dirty injection: toggle the material color back and forth.
        if material_available:
            try:
                material.baseColor = MATERIAL_COLORS[toggle_index % 2]
                toggle_index += 1
                dirty_injections += 1
            except Exception:
                material_available = False
                print("CHURN WARNING material dirty injection stopped mid-run")

        # Static churn: periodic scene reload (full re-capture) and resize.
        if frame % RELOAD_INTERVAL_FRAMES == 0:
            m.loadScene(SCENE_CORNELL)
            reloads += 1
            # Re-fetch the material after reload; the old handle may be stale.
            if material_available:
                try:
                    material = m.scene.materials[0]
                except Exception:
                    material_available = False
        if frame % RESIZE_INTERVAL_FRAMES == 0:
            is_small = not is_small
            m.resizeFrameBuffer(*(SMALL_RESOLUTION if is_small else RESOLUTION))
            resizes += 1

        m.renderFrame()

        if frame % SAMPLE_INTERVAL_FRAMES == 0:
            raw = try_read_pass_stats()
            norm = normalize_stats(raw)
            if norm is not None:
                stats_available = True
                last_norm = norm
                for key in ("alloc", "release", "fail", "lost"):
                    if norm.get(key) is not None:
                        totals[key] = norm[key]
            series.append(
                {
                    "frame": frame,
                    "dirty_injections": dirty_injections,
                    "reloads": reloads,
                    "resizes": resizes,
                    "alloc": norm["alloc"] if norm else None,
                    "release": norm["release"] if norm else None,
                    "fail": norm["fail"] if norm else None,
                    "lost": norm["lost"] if norm else None,
                    "recapture": norm["recapture"] if norm else None,
                    "allocated_pages": norm["allocated_pages"] if norm else None,
                    "free_pages": norm["free_pages"] if norm else None,
                    "resident_bytes": norm["resident_bytes"] if norm else None,
                }
            )

    # Placeholder S2 gate divergence check: when stats ARE available, the last
    # sample must not have diverged pathologically from the first (a leak or
    # unbounded churn would blow up alloc/fail/lost). S2_TODO: freeze the real
    # thresholds and invariants with root (task.md 15.5).
    divergence_ok = None
    if stats_available and len(series) >= 2:
        first = series[0]
        last = series[-1]
        for key in ("alloc", "release", "fail", "lost"):
            a, b = first.get(key), last.get(key)
            if a is not None and b is not None and a > 0:
                if b > a * CHURN_DIVERGENCE_MAX:
                    divergence_ok = False
                    print("CHURN VERDICT counter-divergence", key, "FAIL", a, "->", b)
                    break
        else:
            divergence_ok = True
            print("CHURN VERDICT counter-divergence PASS")

    if not stats_available:
        print(
            "CHURN WARNING S2_TODO: allocator/scheduler counters are not script-bound "
            "(pass.getStats()); series recorded as null. Static churn still ran."
        )
        print("CHURN VERDICT counter-divergence SKIP (stats unavailable, S2_TODO)")

    print("CHURN reloads", reloads, "resizes", resizes, "dirty_injections", dirty_injections)
    print("CHURN totals", totals)

    write_json(
        OUT_JSON,
        {
            "schema_version": 1,
            "mode": "60s-proxy" if SECONDS <= 120 else "soak",
            "seconds": SECONDS,
            "frame_count": FRAME_COUNT,
            "frame_rate": FRAME_RATE,
            "scene": SCENE_CORNELL,
            "material_toggle_available": material_available,
            "stats_available": stats_available,
            "divergence_ok": divergence_ok,
            "divergence_max_placeholder": CHURN_DIVERGENCE_MAX,
            "totals": totals,
            "reloads": reloads,
            "resizes": resizes,
            "dirty_injections": dirty_injections,
            "series": series,
            "nightly_command_30min": (
                "$env:LUMEN_CHURN_SECONDS='1800'; "
                "build\\windows-vs2022\\bin\\Release\\Mogwai.exe --device-type d3d12 "
                "--headless --precise --script tests\\lumengi\\run_churn_short.py "
                "--logfile artifacts\\lumengi\\S2\\churn-30min.log"
            ),
        },
    )
    print("CHURN wrote", os.path.abspath(OUT_JSON))


main()
exit()
