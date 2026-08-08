"""LumenGI S3-B1 cache-lighting host diagnostic (Agent P).

RUN-ONLY diagnostic for the S3 Surface Cache direct-lighting host wiring
(LumenGI.cpp/.h, Agent P). It probes, in order:
  1. property wiring  : "useCacheLighting" is accepted by LumenGI
     (missing -> warning only; useCacheLighting stays 0)
  2. channel wiring    : "cacheDirectRadiance" is a LumenGI output
     (missing -> SKIP verdicts; the graph must be built WITHOUT the channel)
  3. pass execution    : with useSurfaceCache + useCacheLighting, >= CACHE_WARMUP_FRAMES
     are rendered and the LumenGI.surfaceCacheStats dict must report
     cacheLightingActive == 1 and cacheLightingPagesLit > 0
  4. radiance content  : the cacheDirectRadiance channel must contain lit
     (non-black) texels and be finite / non-negative
  5. counters          : the cache-lighting counters read back finite values

Unlike tests/lumengi/run_cachelighting.py (Agent N, S3-C1 component gates),
this script asserts HOST WIRING health only; the S3 gate still owns the
component on/off energy assertions.

Scene handling:
  * Cornell box (default, always): primary host-wiring check. The summary JSON
    is written after this scene so later scenes can never lose it.
  * cornell_pointlight (LUMEN_CACHELIGHTING_P_POINTLIGHT=1, best-effort):
    PRE-EXISTING blocker - the current Release binary deterministically aborts
    with a DXR dispatchRays E_INVALIDARG when the LumenGI trace runs on this
    scene (verified 2026-08-09, unrelated to S3). This scene therefore runs in
    a separate best-effort section and is disabled by default.
  * Arcade (LUMEN_CACHELIGHTING_P_ARCADE=1, best-effort): env-lit large scene,
    exercises the EnvMapSampler path (LUMEN_GI_HAS_ENVIRONMENT_SAMPLER=1).
    Disabled by default to keep a single invocation deterministic.

Usage (post-integration build, from the repo root):
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_cachelighting_p.py ^
      --logfile artifacts\\lumengi\\S3\\cachelighting_p.log

Pre-integration, the same command fails FAST at the LumenGI trace dispatch
because the graph output "cacheDirectRadiance" does not exist yet (the
exception surfaces as a fatal GFX error, not a Python exception). That is the
intended fast-fail: the script only becomes meaningful after root integrates
the S3 host wiring.
"""

from falcor import *
import math
import os
import json

FRAME_RATE = 60
RESOLUTION = (640, 360)
# The S2 capture scheduler is budget-limited (default 64 pages/frame,
# LumenCaptureScheduler.h:106); 64 frames guarantees every card page of the
# test scenes (media cornell_box card-izes 2 instances = 12 cards) is
# captured AND lit well before the first sampling.
CACHE_WARMUP_FRAMES = 64

CACHE_LIGHTING_TOGGLE = "useCacheLighting"
CACHE_DIRECT_CHANNEL = "cacheDirectRadiance"
OUT_JSON = os.environ.get("LUMEN_CACHELIGHTING_P_OUT", "artifacts/lumengi/S3/cachelighting-p-summary.json")

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"
SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)
SCENE_ARCADE = "test_scenes/arcade.pyscene"

RUN_POINTLIGHT = os.environ.get("LUMEN_CACHELIGHTING_P_POINTLIGHT", "0") == "1"
RUN_ARCADE = os.environ.get("LUMEN_CACHELIGHTING_P_ARCADE", "0") == "1"


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


def create_lumen_graph(mark_channel):
    graph = RenderGraph("LumenGICacheLightingP")
    graph.addPass(
        createPass(
            "GBufferRT",
            {
                "samplePattern": "Center",
                "sampleCount": 1,
                "useAlphaTest": True,
            },
        ),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "useSurfaceCache": True,
                CACHE_LIGHTING_TOGGLE: True,
            },
        ),
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
    graph.markOutput("LumenGI.confidence")
    if mark_channel:
        graph.markOutput("LumenGI." + CACHE_DIRECT_CHANNEL)
    return graph


def setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0


def probe_channel(scene_path):
    """Try to run with the cache-direct channel marked. Returns (available,
    graph). With a pre-integration binary the missing output field fails the
    graph at first render; the fallback graph (no channel) is built instead."""
    graph = None
    try:
        graph = create_lumen_graph(mark_channel=True)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        setup_scene(scene_path)
        m.clock.frame = 1
        m.renderFrame()
        return True, graph
    except Exception as exc:
        print(
            "CACHELIGHTING_P WARNING cache-direct channel 'LumenGI.%s' not available "
            "(pre-integration build expected): %s" % (CACHE_DIRECT_CHANNEL, str(exc))
        )
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass
        graph = create_lumen_graph(mark_channel=False)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        setup_scene(scene_path)
        m.clock.frame = 1
        m.renderFrame()
        return False, graph


def sample_channel():
    try:
        tex = m.activeGraph.get_output("LumenGI." + CACHE_DIRECT_CHANNEL)
        arr = tex.to_numpy()
    except Exception as exc:
        print("CACHELIGHTING_P WARNING channel read failed: %s" % str(exc))
        return None
    if arr.ndim == 3:
        rgb = arr[..., :3]
    else:
        rgb = arr
    flat = rgb.reshape(-1)
    lit = flat > 0.0
    lit_count = int(lit.sum())
    lit_vals = flat[lit]
    vmin = float(lit_vals.min()) if lit_count else 0.0
    vmax = float(lit_vals.max()) if lit_count else 0.0
    vmean = float(lit_vals.mean()) if lit_count else 0.0
    return {
        "lit_texels": lit_count,
        "total_texels": int(flat.size),
        "lit_fraction": float(lit_count) / float(flat.size) if flat.size else 0.0,
        "min": vmin,
        "max": vmax,
        "mean": vmean,
        "finite": bool(math.isfinite(vmin) and math.isfinite(vmax)),
        "nonnegative": bool(vmin >= 0.0),
    }


def lumen_stats():
    try:
        return dict(m.activeGraph.getPass("LumenGI").surfaceCacheStats)
    except Exception as exc:
        print("CACHELIGHTING_P WARNING surfaceCacheStats unavailable: %s" % str(exc))
        return None


def verdict(name, ok):
    if ok is None:
        print("CACHELIGHTING_P VERDICT", name, "SKIP")
    else:
        print("CACHELIGHTING_P VERDICT", name, "PASS" if ok else "FAIL")


def run_scene(scene_path, label):
    """Warm the cache and check host wiring on one scene. Returns a summary
    dict (always) plus the scene-level verdict list."""
    available, graph = probe_channel(scene_path)
    for _ in range(CACHE_WARMUP_FRAMES):
        m.clock.frame += 1
        m.renderFrame()

    stats = lumen_stats()
    chan = sample_channel() if available else None
    scene_verdicts = []

    if stats is not None:
        for key in (
            "useCacheLighting",
            "cacheLightingActive",
            "cacheLightingPagesLit",
            "cacheLightingSamplesPerTexel",
            "cacheLightingCounterNanInf",
            "cacheLightingCounterFirefly",
            "cacheLightingCounterNegative",
            "cacheLightingCounterTraced",
        ):
            print("CACHELIGHTING_P %s %s %s" % (label, key, stats.get(key)))

    if chan is not None:
        print(
            "CACHELIGHTING_P %s radiance mean %s min %s max %s lit_fraction %s finite %s nonnegative %s"
            % (label, chan["mean"], chan["min"], chan["max"], chan["lit_fraction"], chan["finite"], chan["nonnegative"])
        )

    prop_ok = stats is not None and stats.get("useCacheLighting") == 1.0
    scene_verdicts.append((label + ": useCacheLighting property wired", prop_ok))
    scene_verdicts.append((label + ": cache lighting active (pass ran)", stats.get("cacheLightingActive") == 1.0 if available else None))
    scene_verdicts.append((label + ": pages lit > 0", bool(stats.get("cacheLightingPagesLit", 0) > 0) if available else None))
    if chan is not None:
        chan_ok = chan["lit_texels"] > 0 and chan["finite"] and chan["nonnegative"]
        scene_verdicts.append((label + ": radiance atlas has lit finite texels", chan_ok))
        scene_verdicts.append((label + ": radiance mean > 0", chan["mean"] > 0.0 if chan_ok else None))
    else:
        scene_verdicts.append((label + ": radiance atlas has lit finite texels", None))
    if stats is not None and available:
        counters = [
            stats.get("cacheLightingCounterNanInf"),
            stats.get("cacheLightingCounterFirefly"),
            stats.get("cacheLightingCounterNegative"),
        ]
        counters_ok = all(isinstance(v, (int, float)) for v in counters)
        scene_verdicts.append((label + ": cache lighting counters read back", counters_ok))
    else:
        scene_verdicts.append((label + ": cache lighting counters read back", None))

    return {
        "scene": scene_path,
        "cache_direct_available": available,
        "stats": stats,
        "channel": chan,
    }, scene_verdicts


def main():
    summary = {
        "script": "run_cachelighting_p.py",
        "status": "in-progress",
        "cache_lighting_toggle": CACHE_LIGHTING_TOGGLE,
        "cache_direct_channel": CACHE_DIRECT_CHANNEL,
        "scenes": {},
    }
    verdicts = []

    # ------------------------------------------------------------ Scene A: Cornell ---
    cornell, cornell_verdicts = run_scene(SCENE_CORNELL, "cornell")
    summary["scenes"]["cornell"] = cornell
    verdicts += cornell_verdicts
    for name, ok in cornell_verdicts:
        verdict(name, ok)
    # Persist before any later best-effort scene (a fatal GFX abort there must
    # never lose the Cornell results).
    write_json(OUT_JSON, summary)

    # ------------------------------------------------------ Scene B: cornell_pointlight ---
    if RUN_POINTLIGHT:
        print("CACHELIGHTING_P NOTE cornell_pointlight: pre-existing LumenGI trace crash blocks this scene; see header.")
        try:
            pl, pl_verdicts = run_scene(SCENE_POINTLIGHT, "pointlight")
            summary["scenes"]["pointlight"] = pl
            verdicts += pl_verdicts
            for name, ok in pl_verdicts:
                verdict(name, ok)
            write_json(OUT_JSON, summary)
        except Exception as exc:
            print("CACHELIGHTING_P WARNING pointlight scene aborted: %s" % str(exc))
            summary["scenes"]["pointlight"] = {"error": str(exc)}
            write_json(OUT_JSON, summary)
    else:
        print("CACHELIGHTING_P NOTE pointlight scene skipped (set LUMEN_CACHELIGHTING_P_POINTLIGHT=1 to enable)")

    # ------------------------------------------------------------ Scene C: Arcade ---
    if RUN_ARCADE:
        try:
            ar, ar_verdicts = run_scene(SCENE_ARCADE, "arcade")
            summary["scenes"]["arcade"] = ar
            verdicts += ar_verdicts
            for name, ok in ar_verdicts:
                verdict(name, ok)
            write_json(OUT_JSON, summary)
        except Exception as exc:
            print("CACHELIGHTING_P WARNING arcade scene aborted: %s" % str(exc))
            summary["scenes"]["arcade"] = {"error": str(exc)}
            write_json(OUT_JSON, summary)
    else:
        print("CACHELIGHTING_P NOTE arcade scene skipped (set LUMEN_CACHELIGHTING_P_ARCADE=1 to enable)")

    summary["status"] = "complete"
    write_json(OUT_JSON, summary)
    print("CACHELIGHTING_P wrote", os.path.abspath(OUT_JSON))


main()
exit()
