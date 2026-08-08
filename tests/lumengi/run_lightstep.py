"""LumenGI S3 Gate validation asset: light step response (S3-C2, Agent R).

Role / purpose
--------------
RUN-ONLY. Exercises the "dynamic light updates the indirect cache in the
target frame count" invariant (task.md 8 / S3-C2: light intensity step). Two
step sequences, each with >= STEP_FRAMES frames rendered per step and the
plateau measured on the LAST PLATEAU_WINDOW frames:

  Phase A (analytic): cornell_pointlight scene, PointLight
      "LumenGITestPointLight", intensity 40 -> 0 -> 40.
  Phase B (emissive): emissive_glow scene (only light is the emissive panel),
      renderSettings.useEmissiveLights True -> False -> True.

Each step must respond to the NEW plateau: the off state reaches ~zero, the
restored on state returns to the original on plateau within tolerance, and
within every plateau window the last-4-frame mean is stable (no oscillation)
and does not keep brightening (no sustained growth). Also checks the hard
invariants: no NaN/Inf, non-negative radiance.

Status: RUN-ONLY -- prints stats and VERDICT lines, never exits non-zero.
Writes a JSON record to artifacts/lumengi/S3/lightstep.json.

Usage (run by root on GPU, from the repo root)
----------------------------------------------
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_lightstep.py ^
      --logfile artifacts\\lumengi\\S3\\lightstep.log
(create artifacts\\lumengi\\S3 first.)

Verified APIs
-------------
* light.intensity = float3(...) -- Light::setIntensity, pybind def_property
  (Light.cpp:411); a change flags Changes::Intensity (Light.cpp:61) and is
  picked up by the scene update, so the cache must invalidate/refresh.
* m.scene.renderSettings.useEmissiveLights = bool -- as run_analytic.py.
* Scene active-light refresh lag: >= 8 frames after every change before the
  plateau window (TOGGLE_WARMUP_FRAMES in run_analytic.py) -- encoded by
  STEP_FRAMES = 8 below.

Threshold freeze (S3_TODO)
--------------------------
All gate thresholds are PLACEHOLDERS and must be re-frozen with root after
the S3-B1/B2 wiring is integrated (the cache-direct/feedback energy scale is
not known until then).
"""

from falcor import *
import math
import os
import json

FRAME_RATE = 60
RESOLUTION = (640, 360)

# Frames rendered per step (>= 8, covers the active-light refresh lag).
STEP_FRAMES = 8

# The plateau is the mean of the LAST 4 frames of each step.
PLATEAU_WINDOW = 4

# --- S3_TODO placeholder gates (freeze with root after S3-B1/B2 lands) ----

# Off state (analytic intensity 0 / emissive off): plateau mean must be below
# this absolute level. Mirrors run_analytic.py OFF_MAX_MEAN.
OFF_MAX_MEAN = 1e-3

# On->off response: on_mean / off_mean must exceed this factor to prove the
# output actually moved to the new (black) plateau.
RESPONSE_RATIO_MIN = 100.0

# Off->on restore: restored plateau mean must land within +/- this factor of
# the original on plateau (1/PLATEAU_TOLERANCE .. PLATEAU_TOLERANCE).
PLATEAU_TOLERANCE = 1.3

# Plateau-window stability: relative spread (max-min)/max of the last-4-frame
# means must be at most this (no oscillation within the plateau).
OSCILLATION_TOL = 0.2

# No continued brightening: within a plateau window the last-frame mean must
# not exceed the first-frame mean by more than this factor.
IN_STEP_GROWTH_MAX = 1.1

LIGHT_NAME = "LumenGITestPointLight"
LIGHT_ON = float3(40.0, 40.0, 40.0)
LIGHT_OFF = float3(0.0, 0.0, 0.0)

SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)
SCENE_EMISSIVE_GLOW = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "emissive_glow.pyscene")
)

OUT_JSON = os.environ.get("LUMEN_LIGHTSTEP_OUT", "artifacts/lumengi/S3/lightstep.json")


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
    graph = RenderGraph("LumenGILightStep")
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
    graph.addPass(createPass("LumenGI"), "LumenGI")

    graph.addEdge("GBufferRT.vbuffer", "LumenGI.vbuffer")
    graph.addEdge("GBufferRT.linearZ", "LumenGI.linearZ")
    graph.addEdge("GBufferRT.mvec", "LumenGI.mvec")
    graph.addEdge("GBufferRT.mvecW", "LumenGI.mvecW")
    graph.addEdge("GBufferRT.normWRoughnessMaterialID", "LumenGI.normWRoughnessMaterialID")
    graph.addEdge("GBufferRT.viewW", "LumenGI.viewW")
    graph.addEdge("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity")
    graph.addEdge("GBufferRT.emissive", "LumenGI.emissive")

    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.confidence")
    return graph


def render_one(label):
    """Render one frame, return a per-frame stats dict."""
    m.clock.frame += 1
    m.renderFrame()
    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
    # RGB only: alpha channel is hardcoded to 1.0 in the trace shader.
    gi = gi[..., :3]
    vmin = float(gi.min())
    vmax = float(gi.max())
    vmean = float(gi.mean())
    finite = math.isfinite(vmin) and math.isfinite(vmax)
    nonneg = vmin >= 0.0
    print(
        "LIGHTSTEP", label,
        "frame", m.clock.frame,
        "min", vmin,
        "max", vmax,
        "mean", vmean,
        "finite", finite,
        "nonnegative", nonneg,
    )
    return {"frame": int(m.clock.frame), "min": vmin, "max": vmax, "mean": vmean, "finite": finite, "nonneg": nonneg}


def run_step(label, frames):
    return [render_one(label) for _ in range(frames)]


def window_mean(stats, window):
    ws = stats[-window:]
    return sum(s["mean"] for s in ws) / len(ws)


def rel_spread(stats, window):
    ws = stats[-window:]
    means = [s["mean"] for s in ws]
    top = max(means)
    if top <= 0.0:
        return 0.0
    return (top - min(means)) / top


def window_growth(stats, window):
    ws = stats[-window:]
    first, last = ws[0]["mean"], ws[-1]["mean"]
    if first <= 0.0:
        return None
    return last / first


def safe_div(a, b):
    return (a / b) if (b is not None and b > 0.0) else None


def verdict(phase, name, ok):
    print("LIGHTSTEP VERDICT", phase, name, "PASS" if ok else "FAIL")


def evaluate_step(phase, on_stats, off_stats, onb_stats, off_expected_zero):
    """Shared gate evaluation for one phase. Returns a record dict with
    plateaus, ratios, per-step window metrics and verdicts."""
    on_mean = window_mean(on_stats, PLATEAU_WINDOW)
    off_mean = window_mean(off_stats, PLATEAU_WINDOW)
    onb_mean = window_mean(onb_stats, PLATEAU_WINDOW)
    drop_ratio = safe_div(on_mean, off_mean)
    ret_ratio = safe_div(onb_mean, on_mean)

    all_finite = all(s["finite"] for s in on_stats + off_stats + onb_stats)
    all_nonneg = all(s["nonneg"] for s in on_stats + off_stats + onb_stats)

    spread_on = rel_spread(on_stats, PLATEAU_WINDOW)
    spread_off = rel_spread(off_stats, PLATEAU_WINDOW)
    spread_onb = rel_spread(onb_stats, PLATEAU_WINDOW)
    growth_on = window_growth(on_stats, PLATEAU_WINDOW)
    growth_onb = window_growth(onb_stats, PLATEAU_WINDOW)

    v = [
        ("off state reaches ~zero (mean <= %g)" % OFF_MAX_MEAN, off_mean <= OFF_MAX_MEAN),
        ("on->off responds to new plateau (drop ratio >= %g)" % RESPONSE_RATIO_MIN,
         drop_ratio is not None and drop_ratio >= RESPONSE_RATIO_MIN),
        ("off->on returns to original plateau (within %g)" % PLATEAU_TOLERANCE,
         ret_ratio is not None and (1.0 / PLATEAU_TOLERANCE) <= ret_ratio <= PLATEAU_TOLERANCE),
        ("no oscillation in on window (spread <= %g)" % OSCILLATION_TOL, spread_on <= OSCILLATION_TOL),
        ("no oscillation in off window (spread <= %g)" % OSCILLATION_TOL, spread_off <= OSCILLATION_TOL),
        ("no oscillation in restore window (spread <= %g)" % OSCILLATION_TOL, spread_onb <= OSCILLATION_TOL),
        ("no continued brightening in on window (growth <= %g)" % IN_STEP_GROWTH_MAX,
         growth_on is None or growth_on <= IN_STEP_GROWTH_MAX),
        ("no continued brightening in restore window (growth <= %g)" % IN_STEP_GROWTH_MAX,
         growth_onb is None or growth_onb <= IN_STEP_GROWTH_MAX),
        ("no NaN/Inf", all_finite),
        ("no negative radiance", all_nonneg),
    ]
    for name, ok in v:
        verdict(phase, name, ok)

    return {
        "steps": {"on": on_stats, "off": off_stats, "on_restore": onb_stats},
        "plateaus": {"on_mean": on_mean, "off_mean": off_mean, "on_restore_mean": onb_mean},
        "ratios": {"on_off_drop": drop_ratio, "restore_on_ratio": ret_ratio},
        "window_metrics": {
            "on_spread": spread_on,
            "off_spread": spread_off,
            "restore_spread": spread_onb,
            "on_growth": growth_on,
            "restore_growth": growth_onb,
        },
        "verdicts": [{"name": name, "ok": bool(ok)} for name, ok in v],
        "off_expected_zero": off_expected_zero,
    }


def main():
    m.addGraph(create_lumen_graph())
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()

    results = {}

    # ------------------------------------------------------- Phase A: analytic ---
    print("LIGHTSTEP phase analytic scene", SCENE_POINTLIGHT)
    m.loadScene(SCENE_POINTLIGHT)
    m.clock.frame = 0
    light = m.scene.getLight(LIGHT_NAME)
    print("LIGHTSTEP light", light.name, "active", light.active, "intensity", light.intensity)
    light.intensity = LIGHT_ON
    on_stats = run_step("analytic-on", STEP_FRAMES)
    light.intensity = LIGHT_OFF
    off_stats = run_step("analytic-off", STEP_FRAMES)
    light.intensity = LIGHT_ON
    onb_stats = run_step("analytic-on-restore", STEP_FRAMES)
    results["analytic"] = evaluate_step(
        "analytic", on_stats, off_stats, onb_stats, off_expected_zero=True
    )
    results["analytic"]["scene"] = SCENE_POINTLIGHT

    # ------------------------------------------------------- Phase B: emissive ---
    print("LIGHTSTEP phase emissive scene", SCENE_EMISSIVE_GLOW)
    m.loadScene(SCENE_EMISSIVE_GLOW)
    m.clock.frame = 0
    m.scene.renderSettings.useEmissiveLights = True
    on_stats = run_step("emissive-on", STEP_FRAMES)
    m.scene.renderSettings.useEmissiveLights = False
    off_stats = run_step("emissive-off", STEP_FRAMES)
    m.scene.renderSettings.useEmissiveLights = True
    onb_stats = run_step("emissive-on-restore", STEP_FRAMES)
    results["emissive"] = evaluate_step(
        "emissive", on_stats, off_stats, onb_stats, off_expected_zero=True
    )
    results["emissive"]["scene"] = SCENE_EMISSIVE_GLOW

    write_json(
        OUT_JSON,
        {
            "script": "run_lightstep.py",
            "role": "S3-C2 light step response asset (Agent R)",
            "status": "run-only",
            "step_frames": STEP_FRAMES,
            "plateau_window": PLATEAU_WINDOW,
            "resolution": list(RESOLUTION),
            "thresholds_placeholder": {
                "off_max_mean": OFF_MAX_MEAN,
                "response_ratio_min": RESPONSE_RATIO_MIN,
                "plateau_tolerance": PLATEAU_TOLERANCE,
                "oscillation_tol": OSCILLATION_TOL,
                "in_step_growth_max": IN_STEP_GROWTH_MAX,
            },
            "phases": results,
        },
    )
    print("LIGHTSTEP wrote", os.path.abspath(OUT_JSON))


main()
exit()
