"""LumenGI S3 Gate validation asset (SKELETON): Surface Cache lighting components.

Role / purpose
--------------
Agent N (Test-Tooling) verification asset for S3 (Surface Cache Lighting,
task.md 8, S3-C1). RUN-ONLY: prints stats and VERDICT lines, never exits
non-zero; root evaluates the verdicts at the S3 Gate.

STATUS: SKELETON, created for the S3 integration round. It is deliberately
runnable TODAY against the S2 integration state (capture only): when the S3
wiring is absent it degrades gracefully (prints hints + SKIP verdicts) instead
of crashing. The moment root integrates S3-B1 this file becomes a real Gate
asset with only the placeholder thresholds below left to freeze.

What this script verifies (S3-C1 light-component tests)
-------------------------------------------------------
For each scene the Surface Cache is warmed to residency (>= 64 frames), then
the cache-direct radiance channel is sampled and each light component is
toggled on/off (8 frames settle per toggle, matching the scene active-light
refresh lag documented in run_analytic.py) and the cache-direct channel must
move in the corresponding direction:
  * cornell_box      : emissive on/off  (the scene's only light is the
                       emissive "Light" quad) and env on/off (negative control:
                       cornell_box has NO env map, so the env toggle must be a
                       no-op -> ratio ~1; S3_TODO: re-run env toggle on an
                       env-lit scene such as Arcade for a real delta).
  * cornell_pointlight: analytic on/off (the scene's only light is the analytic
                       PointLight "LumenGITestPointLight").
Component means are compared with RATIO gates (component-on mean must exceed
component-off mean by RATIO_MIN). Absolute gates are NOT used here because the
S3_B1 direct channel includes the per-texel `emitted` term (material emission,
LumenSurfaceCacheLighting.cs.slang:518), so the emissive-off state does not
reach ~zero even when the emissive NEE term is gone. All thresholds below are
PLACEHOLDERS (S3_TODO: freeze with root after S3-B1 is integrated and the
channel name is confirmed).

S3_TODO list (integration interface alignment; resolved by root + Agent M)
--------------------------------------------------------------------------
* S3_TODO: CONFIRM the LumenGI property name for the cache lighting toggle.
  This file uses "useCacheLighting" (mirrors the existing "useSurfaceCache"
  convention, LumenGI.cpp:46). Unknown properties only warn (LumenGI.cpp:160),
  so pre-S3 this is a no-op; post-S3 root must wire it and confirm the name.
* S3_TODO: CONFIRM the graph output channel exposing the cache direct radiance.
  The S3_B1 pass writes gRadianceAtlas (RGB = direct, A = indirect,
  LumenSurfaceCacheLightingData.slang:299) which is an internal UAV, not a
  graph output. Root/Agent M must re-export it (candidate names:
  "cacheDirectRadiance" / "radianceAtlas"). This script samples
  CACHE_DIRECT_CHANNEL and degrades to SKIP when the channel is absent.
* S3_TODO: FREEZE the direct-channel sampling metric. This skeleton samples
  mean over non-black (lit) atlas texels; the frozen metric (e.g.
  coverage-weighted mean over valid pages, masked by the visibility atlas
  confidence) must be agreed with root at freeze time.
* S3_TODO: FREEZE the component RATIO thresholds (RATIO_MIN) and the analytic
  baseline energy levels after S3-B1 lands. Values below are placeholders.

Usage (run by root on GPU, from the repo root, after S3 integration)
--------------------------------------------------------------------
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_cachelighting.py ^
      --logfile artifacts\\lumengi\\S3\\cachelighting.log
(create artifacts\\lumengi\\S3 first. Pre-S3 the same command is expected to
produce SKIP verdicts and a warning, which is the intended fast-fail.)

Scenes / loading
----------------
* cornell_box      : "test_scenes/cornell_box.pyscene" (media, emissive "Light" quad).
* cornell_pointlight: tests/lumengi/scenes/cornell_pointlight.pyscene, loaded by
  ABSOLUTE PATH (see run_analytic.py header for the resolver caveat).
"""

from falcor import *
import math
import os
import json

FRAME_RATE = 60
RESOLUTION = (640, 360)

# Frames rendered after the graph/scene setup before any sampling: the S2
# capture scheduler is budget-limited (default 64 pages/frame,
# LumenCaptureScheduler.h:106) and Cornell has 7 instances x 6 faces = 42
# cards, so 64 frames guarantees every card page is captured AND lit.
CACHE_WARMUP_FRAMES = 64

# Frames rendered after every renderSettings change before sampling (scene
# active-light refresh lag; see run_analytic.py TOGGLE_WARMUP_FRAMES).
TOGGLE_WARMUP_FRAMES = 8

# S3_TODO: confirm the LumenGI property name for the cache lighting toggle
# with root. Unknown props only warn (LumenGI.cpp:160) so this is safe today.
CACHE_LIGHTING_TOGGLE = "useCacheLighting"
USE_CACHE_LIGHTING = True

# S3_TODO: confirm the graph output channel name with root/Agent M (candidate
# names: cacheDirectRadiance / radianceAtlas). Absent channel -> SKIP.
CACHE_DIRECT_CHANNEL = "cacheDirectRadiance"

# S3_TODO placeholder component-ratio gates. Component-on mean must exceed
# component-off mean by RATIO_MIN. Freeze with root after S3-B1 integrates.
COMPONENT_RATIO_MIN = 2.0
# Negative-control gate for a scene WITHOUT an env map: env on vs off must be
# ~identical (ratio below NO_OP_RATIO_MAX). S3_TODO: once an env-lit scene is
# added, replace this negative control with a real env delta assertion.
NO_OP_RATIO_MAX = 1.5

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"
SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)

OUT_JSON = os.environ.get("LUMEN_CACHELIGHTING_OUT", "artifacts/lumengi/S3/cachelighting-summary.json")


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


def create_lumen_graph(mark_cache_direct):
    """Build GBufferRT -> LumenGI (useSurfaceCache + reserved cache-lighting
    toggle). Optionally marks the cache-direct channel as a graph output."""
    graph = RenderGraph("LumenGICacheLighting")
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
    # useCacheLighting is S3_TODO (see header); unknown props only warn.
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "useSurfaceCache": True,
                CACHE_LIGHTING_TOGGLE: USE_CACHE_LIGHTING,
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
    if mark_cache_direct:
        # S3_TODO: the channel may not exist pre-S3; graph compile fails on the
        # first render and we fall back (see probe_available below).
        graph.markOutput("LumenGI." + CACHE_DIRECT_CHANNEL)
    return graph


def _setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0


def probe_cache_direct(scene_path):
    """Try to run with the cache-direct channel marked. Returns (available,
    graph). If the channel (or graph compile) is missing, rebuild without it."""
    graph = create_lumen_graph(mark_cache_direct=True)
    m.addGraph(graph)
    m.setActiveGraph(graph)
    _setup_scene(scene_path)
    try:
        m.clock.frame = 1
        m.renderFrame()
        return True, graph
    except Exception as exc:  # pragma: no cover - pre-S3 path
        print(
            "CACHELIGHTING WARNING cache-direct channel 'LumenGI.%s' not available "
            "(pre-S3 integration expected); channel absent -> %s"
            % (CACHE_DIRECT_CHANNEL, str(exc))
        )
        m.removeGraph(graph)
        graph = create_lumen_graph(mark_cache_direct=False)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(scene_path)
        m.clock.frame = 1
        m.renderFrame()
        return False, graph


def sample_cache_direct(available):
    """Sample the cache-direct radiance channel. Returns a stats dict, or None
    when the channel is unavailable. Metric: mean/min/max over NON-BLACK
    (lit) atlas texels, RGB channels only. S3_TODO: freeze the metric with
    root (see header)."""
    if not available:
        return None
    try:
        tex = m.activeGraph.get_output("LumenGI." + CACHE_DIRECT_CHANNEL)
        arr = tex.to_numpy()
    except Exception as exc:
        print("CACHELIGHTING WARNING cache-direct read failed: %s" % str(exc))
        return None
    if arr.ndim == 3:
        rgb = arr[..., :3]
    else:  # single-channel (R16F) fallback
        rgb = arr
    flat = rgb.reshape(-1)
    lit = flat > 0.0
    lit_count = int(lit.sum())
    lit_vals = flat[lit]
    vmin = float(lit_vals.min()) if lit_count else 0.0
    vmax = float(lit_vals.max()) if lit_count else 0.0
    vmean = float(lit_vals.mean()) if lit_count else 0.0
    finite = bool(math.isfinite(vmin) and math.isfinite(vmax))
    nonneg = bool(vmin >= 0.0)
    return {
        "lit_texels": lit_count,
        "total_texels": int(flat.size),
        "lit_fraction": float(lit_count) / float(flat.size) if flat.size else 0.0,
        "min": vmin,
        "max": vmax,
        "mean": vmean,
        "finite": finite,
        "nonnegative": nonneg,
    }


def render_and_sample(label, frames, available):
    """Render `frames` frames, then sample the cache-direct channel."""
    stats = None
    for _ in range(frames):
        m.clock.frame += 1
        m.renderFrame()
        stats = sample_cache_direct(available)
    if stats is None:
        print("CACHELIGHTING", label, "unavailable")
    else:
        print(
            "CACHELIGHTING", label,
            "mean", stats["mean"],
            "min", stats["min"],
            "max", stats["max"],
            "lit_fraction", stats["lit_fraction"],
            "finite", stats["finite"],
            "nonnegative", stats["nonnegative"],
        )
    return stats


def component_ratio_on_off(on_stats, off_stats):
    """Return on_mean / off_mean (guards divide-by-zero)."""
    if on_stats is None or off_stats is None:
        return None
    if off_stats["mean"] > 0.0:
        return on_stats["mean"] / off_stats["mean"]
    return None


def no_op_ratio(a_stats, b_stats):
    """Ratio of two sampling means used for the env negative control."""
    if a_stats is None or b_stats is None:
        return None
    a, b = a_stats["mean"], b_stats["mean"]
    if a <= 0.0 and b <= 0.0:
        return 1.0
    if a <= 0.0 or b <= 0.0:
        return None
    return max(a, b) / min(a, b)


def run_component_toggle(scene_path, component_name, settings_attr, available):
    """Generic component on/off/restore toggle sequence. Returns a dict of
    sampled stats and verdicts."""
    print("CACHELIGHTING component", component_name, "on scene", scene_path)
    # Baseline: cache residency already established by the caller.
    setattr(m.scene.renderSettings, settings_attr, True)
    render_and_sample("component-%s-warm" % component_name, TOGGLE_WARMUP_FRAMES, available)
    on_stats = render_and_sample("component-%s-on" % component_name, TOGGLE_WARMUP_FRAMES, available)
    setattr(m.scene.renderSettings, settings_attr, False)
    off_stats = render_and_sample("component-%s-off" % component_name, TOGGLE_WARMUP_FRAMES, available)
    setattr(m.scene.renderSettings, settings_attr, True)
    on2_stats = render_and_sample("component-%s-on-again" % component_name, TOGGLE_WARMUP_FRAMES, available)
    return {"on": on_stats, "off": off_stats, "on_again": on2_stats}


def verdict(name, ok):
    print("CACHELIGHTING VERDICT", name, "PASS" if ok else "FAIL")


def main():
    # ------------------------------------------------------------ Scene A: Cornell ---
    # Emissive on/off + env on/off (negative control, no env map in this scene).
    available, graph = probe_cache_direct(SCENE_CORNELL)
    render_and_sample("cornell-cache-warmup", CACHE_WARMUP_FRAMES, available)

    emissive = run_component_toggle(SCENE_CORNELL, "emissive", "useEmissiveLights", available)

    # Env negative control: cornell_box has no env map, so on vs off must be ~identical.
    print("CACHELIGHTING NOTE cornell_box has no env map; env toggle is a negative control")
    print("CACHELIGHTING NOTE S3_TODO: run the env delta on an env-lit scene (e.g. Arcade) once S3 integrates")
    env_on = render_and_sample("env-on", TOGGLE_WARMUP_FRAMES, available)
    setattr(m.scene.renderSettings, "useEnvLight", False)
    env_off = render_and_sample("env-off", TOGGLE_WARMUP_FRAMES, available)
    setattr(m.scene.renderSettings, "useEnvLight", True)

    # ------------------------------------------------------ Scene B: cornell_pointlight ---
    # Analytic on/off (the scene's only light is the analytic point light).
    m.removeGraph(graph)
    available2, graph2 = probe_cache_direct(SCENE_POINTLIGHT)
    render_and_sample("pointlight-cache-warmup", CACHE_WARMUP_FRAMES, available2)
    analytic = run_component_toggle(SCENE_POINTLIGHT, "analytic", "useAnalyticLights", available2)

    # ------------------------------------------------------------- Verdicts (S3_TODO gates) ---
    records = {}
    verdicts = []
    for name, toggles, avail in (
        ("emissive", emissive, available),
        ("analytic", analytic, available2),
    ):
        rec = {"scene": SCENE_CORNELL if name == "emissive" else SCENE_POINTLIGHT}
        on_mean = (toggles["on"] or {}).get("mean")
        off_mean = (toggles["off"] or {}).get("mean")
        ratio = component_ratio_on_off(toggles["on"], toggles["off"])
        finite_ok = all(
            (s or {}).get("finite", True) and (s or {}).get("nonnegative", True)
            for s in toggles.values()
        )
        if avail and on_mean is not None and off_mean is not None and ratio is not None:
            comp_ok = ratio > COMPONENT_RATIO_MIN
        else:
            comp_ok = None  # channel unavailable -> SKIP
        rec.update(
            {
                "on_mean": on_mean,
                "off_mean": off_mean,
                "on_again_mean": (toggles["on_again"] or {}).get("mean"),
                "on_off_ratio": ratio,
                "finite_nonnegative": bool(finite_ok),
                "component_delta_ok": comp_ok,
            }
        )
        records[name] = rec
        verdicts.append((name + " component delta (on > off * ratio)", comp_ok))
        verdicts.append((name + " no NaN/Inf, non-negative", finite_ok))

    env_ratio = no_op_ratio(env_on, env_off)
    env_noop_ok = env_ratio is not None and env_ratio < NO_OP_RATIO_MAX
    records["env"] = {
        "on_mean": (env_on or {}).get("mean"),
        "off_mean": (env_off or {}).get("mean"),
        "on_off_ratio": env_ratio,
        "negative_control_noop_ok": env_noop_ok,
    }
    verdicts.append(("env negative control (no env scene -> no-op)", env_noop_ok))

    for name, ok in verdicts:
        if ok is None:
            print("CACHELIGHTING VERDICT", name, "SKIP (channel unavailable, pre-S3)")
        else:
            verdict(name, ok)

    write_json(
        OUT_JSON,
        {
            "script": "run_cachelighting.py",
            "status": "skeleton",
            "cache_direct_channel": CACHE_DIRECT_CHANNEL,
            "cache_lighting_toggle": CACHE_LIGHTING_TOGGLE,
            "cache_direct_available": available and available2,
            "component_ratio_min_placeholder": COMPONENT_RATIO_MIN,
            "warmup_frames": CACHE_WARMUP_FRAMES,
            "toggle_warmup_frames": TOGGLE_WARMUP_FRAMES,
            "scenes": {"cornell": SCENE_CORNELL, "pointlight": SCENE_POINTLIGHT},
            "components": records,
        },
    )
    print("CACHELIGHTING wrote", os.path.abspath(OUT_JSON))


main()
exit()
