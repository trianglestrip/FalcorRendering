"""LumenGI S3 Gate validation asset: stability / divergence tests (S3-C2, Agent R).

Role / purpose
--------------
RUN-ONLY. Loads four scenes -- black room, white furnace, strong emissive
glow and the stock Cornell Box -- renders WARMUP_FRAMES + STABILITY_FRAMES
frames each and samples LumenGI.diffuseGI per frame (RGB, mean/max/finite/
non-negative). Enforces the task.md 15.4 hard invariants:

  * black scene does not brighten by itself  (black_room: every-frame mean
    stays below an absolute floor),
  * static feedback reaches a plateau, not unbounded growth (white_furnace:
    late-window mean is flat and the last-frame max is bounded relative to
    the first sampled frame),
  * no NaN/Inf, radiance non-negative (all scenes),
  * strong emissive output is bounded by the firefly clamp (emissive_glow:
    every-frame max <= the clamp ceiling kLumenGIMaxRadiance * tolerance).

Status: RUN-ONLY -- prints stats and VERDICT lines, never exits non-zero; no
golden-image comparison. Root evaluates the verdicts at the S3 Gate
(task.md 8 / 18). Writes a per-frame JSON series to
artifacts/lumengi/S3/stability.json.

Usage (run by root on GPU, from the repo root)
----------------------------------------------
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_stability.py ^
      --logfile artifacts\\lumengi\\S3\\stability.log
(create artifacts\\lumengi\\S3 first.)

Scenes / loading
----------------
* tests/lumengi/scenes/black_room.pyscene      -- no lights, no env, no emissive
* tests/lumengi/scenes/white_furnace.pyscene   -- high-albedo closed room + weak emissive panel
* tests/lumengi/scenes/emissive_glow.pyscene   -- neutral room + strong emissive panel
* media/test_scenes/cornell_box.pyscene        -- stock Cornell (emissive-lit)
The three custom scenes live outside media/ and are loaded by ABSOLUTE PATH
(see run_analytic.py header for the AssetResolver caveat).

Threshold freeze (S3_TODO)
--------------------------
All gate thresholds below are PLACEHOLDERS. They must be re-frozen with root
after the S3-B1/B2 wiring is integrated, because the direct/feedback energy
scale of the surface-cache path is not known until then. The white-furnace
max-growth threshold especially must accommodate the legitimate ~1/(1-albedo)
equilibrium factor of the feedback loop while still catching exponential
divergence (gain >= 1).
"""

from falcor import *
import math
import os
import json

FRAME_RATE = 60
RESOLUTION = (640, 360)

# Frames rendered after scene load before the sampled window (cache capture
# + history settle). Mirrors run_dynamic.py's 8-frame warmup convention.
WARMUP_FRAMES = 8

# Sampled static frames per scene (task.md 8 S3-C2: "N frames, e.g. 64").
STABILITY_FRAMES = 64

# --- S3_TODO placeholder gates (freeze with root after S3-B1/B2 lands) ----

# Black room: every sampled frame's diffuseGI mean must stay below this
# absolute level ("does not brighten by itself"). 0 is the expected value;
# 1e-4 leaves room for fp16 readback rounding.
BLACK_ROOM_MAX_MEAN = 1e-4

# White furnace: last sampled frame's max must not exceed the first sampled
# frame's max by more than this factor (catches exponential feedback
# divergence; must be frozen against the real ~1/(1-albedo) equilibrium).
FURNACE_MAX_GROWTH = 20.0

# White furnace plateau gate: the mean of the LAST 4 sampled frames must not
# drift by more than this RELATIVE amount from the mean of the preceding 4
# frames (energy settled onto a flat plateau => no continued brightening).
FURNACE_PLATEAU_DRIFT = 0.2

# White furnace window-growth gate: late-window mean vs its predecessor
# window mean must not exceed this factor (no monotonic growth within the
# sampled window).
FURNACE_WINDOW_GROWTH_MAX = 1.2

# Strong emissive: per-frame max must stay <= the clamp ceiling
# kLumenGIMaxRadiance (default LUMEN_GI_MAX_RADIANCE = 10000,
# LumenGIData.slang:64) times a readback/rounding tolerance.
EMISSIVE_GLOW_MAX_RADIANCE = 10000.0
EMISSIVE_GLOW_MAX_TOLERANCE = 1.01

# Strong emissive: the late-window mean must be above this absolute level to
# prove the adjacent surfaces are actually lit by the panel.
EMISSIVE_GLOW_MIN_MEAN = 1e-3

# Cornell (emissive-lit baseline): loose static-energy growth gate, mirroring
# run_dynamic.py ENERGY_GROWTH_THRESHOLD = 5.0.
CORNELL_MAX_GROWTH = 5.0

SCENE_BLACK_ROOM = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "black_room.pyscene")
)
SCENE_WHITE_FURNACE = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "white_furnace.pyscene")
)
SCENE_EMISSIVE_GLOW = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "emissive_glow.pyscene")
)
SCENE_CORNELL = "test_scenes/cornell_box.pyscene"

OUT_JSON = os.environ.get("LUMEN_STABILITY_OUT", "artifacts/lumengi/S3/stability.json")


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
    graph = RenderGraph("LumenGIStability")
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


def render_one(scene_key):
    """Render one frame, return a per-frame stats dict."""
    m.clock.frame += 1
    m.renderFrame()
    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
    # RGB only: alpha channel is hardcoded to 1.0 in the trace shader
    # (LumenHardwareTrace.rt.slang:318 float4(diffuseGI, 1.f)).
    gi = gi[..., :3]
    vmin = float(gi.min())
    vmax = float(gi.max())
    vmean = float(gi.mean())
    finite = math.isfinite(vmin) and math.isfinite(vmax)
    nonneg = vmin >= 0.0
    print(
        "STABILITY", scene_key,
        "frame", m.clock.frame,
        "min", vmin,
        "max", vmax,
        "mean", vmean,
        "finite", finite,
        "nonnegative", nonneg,
    )
    return {"frame": int(m.clock.frame), "min": vmin, "max": vmax, "mean": vmean, "finite": finite, "nonneg": nonneg}


def run_scene(scene_path, scene_key):
    """Load a scene, warm up, sample STABILITY_FRAMES frames. Returns the
    per-frame series."""
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    for _ in range(WARMUP_FRAMES):
        render_one(scene_key)
    return [render_one(scene_key) for _ in range(STABILITY_FRAMES)]


def window_means(series, tail=4):
    """Mean of the LAST `tail` frames and of the `tail` frames before them."""
    late = [f["mean"] for f in series[-tail:]]
    prev = [f["mean"] for f in series[-2 * tail:-tail]]
    return sum(prev) / len(prev), sum(late) / len(late)


def safe_div(a, b):
    return (a / b) if (b is not None and b > 0.0) else None


def verdict(scene_key, name, ok):
    print("STABILITY VERDICT", scene_key, name, "PASS" if ok else "FAIL")


def main():
    m.addGraph(create_lumen_graph())
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0

    scenes = [
        ("black_room", SCENE_BLACK_ROOM),
        ("white_furnace", SCENE_WHITE_FURNACE),
        ("emissive_glow", SCENE_EMISSIVE_GLOW),
        ("cornell", SCENE_CORNELL),
    ]

    results = {}
    for key, path in scenes:
        print("STABILITY scene", key, path)
        m.clock.frame = 0
        series = run_scene(path, key)

        frames_finite = all(f["finite"] for f in series)
        frames_nonneg = all(f["nonneg"] for f in series)

        rec = {"path": path, "frames": series}

        if key == "black_room":
            means = [f["mean"] for f in series]
            max_mean = max(means)
            ok_black = frames_finite and frames_nonneg and max_mean <= BLACK_ROOM_MAX_MEAN
            rec["summary"] = {"max_mean": max_mean, "first_mean": means[0], "last_mean": means[-1]}
            rec["verdicts"] = [
                ("no NaN/Inf", frames_finite),
                ("no negative radiance", frames_nonneg),
                ("mean stays <= %g (no self-brightening)" % BLACK_ROOM_MAX_MEAN, ok_black),
            ]
            for n, o in rec["verdicts"]:
                verdict(key, n, o)

        elif key == "white_furnace":
            first_max = series[0]["max"]
            last_max = series[-1]["max"]
            growth_ratio = safe_div(last_max, first_max)
            growth_ok = frames_finite and frames_nonneg and growth_ratio is not None and growth_ratio <= FURNACE_MAX_GROWTH

            prev_mean, late_mean = window_means(series)
            drift = safe_div(abs(late_mean - prev_mean), prev_mean)
            plateau_ok = drift is not None and drift <= FURNACE_PLATEAU_DRIFT
            window_growth = safe_div(late_mean, prev_mean)
            window_ok = window_growth is not None and window_growth <= FURNACE_WINDOW_GROWTH_MAX

            rec["summary"] = {
                "first_max": first_max,
                "last_max": last_max,
                "max_growth_ratio": growth_ratio,
                "prev_window_mean": prev_mean,
                "late_window_mean": late_mean,
                "plateau_drift": drift,
                "window_growth_ratio": window_growth,
            }
            rec["verdicts"] = [
                ("no NaN/Inf", frames_finite),
                ("no negative radiance", frames_nonneg),
                ("max does not diverge (last <= first * %g)" % FURNACE_MAX_GROWTH, growth_ok),
                ("plateau reached (late-window drift <= %g)" % FURNACE_PLATEAU_DRIFT, plateau_ok),
                ("no continued brightening (window growth <= %g)" % FURNACE_WINDOW_GROWTH_MAX, window_ok),
            ]
            for n, o in rec["verdicts"]:
                verdict(key, n, o)

        elif key == "emissive_glow":
            cap = EMISSIVE_GLOW_MAX_RADIANCE * EMISSIVE_GLOW_MAX_TOLERANCE
            max_over_cap = max(f["max"] for f in series)
            clamp_ok = frames_finite and frames_nonneg and max_over_cap <= cap
            prev_mean, late_mean = window_means(series)
            lit_ok = late_mean >= EMISSIVE_GLOW_MIN_MEAN
            rec["summary"] = {
                "max_over_frames": max_over_cap,
                "clamp_ceiling": EMISSIVE_GLOW_MAX_RADIANCE,
                "clamp_tolerance": EMISSIVE_GLOW_MAX_TOLERANCE,
                "late_window_mean": late_mean,
                "min_mean": EMISSIVE_GLOW_MIN_MEAN,
            }
            rec["verdicts"] = [
                ("no NaN/Inf", frames_finite),
                ("no negative radiance", frames_nonneg),
                ("max bounded by clamp (<= %g)" % cap, clamp_ok),
                ("adjacent surfaces lit (late mean >= %g)" % EMISSIVE_GLOW_MIN_MEAN, lit_ok),
            ]
            for n, o in rec["verdicts"]:
                verdict(key, n, o)

        else:  # cornell baseline
            first_max = series[0]["max"]
            last_max = series[-1]["max"]
            growth_ratio = safe_div(last_max, first_max)
            growth_ok = frames_finite and frames_nonneg and growth_ratio is not None and growth_ratio <= CORNELL_MAX_GROWTH
            prev_mean, late_mean = window_means(series)
            rec["summary"] = {
                "first_max": first_max,
                "last_max": last_max,
                "max_growth_ratio": growth_ratio,
                "prev_window_mean": prev_mean,
                "late_window_mean": late_mean,
            }
            rec["verdicts"] = [
                ("no NaN/Inf", frames_finite),
                ("no negative radiance", frames_nonneg),
                ("static energy bounded (max growth <= %g)" % CORNELL_MAX_GROWTH, growth_ok),
            ]
            for n, o in rec["verdicts"]:
                verdict(key, n, o)

        results[key] = rec

    write_json(
        OUT_JSON,
        {
            "script": "run_stability.py",
            "role": "S3-C2 stability/divergence asset (Agent R)",
            "status": "run-only",
            "warmup_frames": WARMUP_FRAMES,
            "stability_frames": STABILITY_FRAMES,
            "resolution": list(RESOLUTION),
            "thresholds_placeholder": {
                "black_room_max_mean": BLACK_ROOM_MAX_MEAN,
                "furnace_max_growth": FURNACE_MAX_GROWTH,
                "furnace_plateau_drift": FURNACE_PLATEAU_DRIFT,
                "furnace_window_growth_max": FURNACE_WINDOW_GROWTH_MAX,
                "emissive_glow_max_radiance": EMISSIVE_GLOW_MAX_RADIANCE,
                "emissive_glow_max_tolerance": EMISSIVE_GLOW_MAX_TOLERANCE,
                "emissive_glow_min_mean": EMISSIVE_GLOW_MIN_MEAN,
                "cornell_max_growth": CORNELL_MAX_GROWTH,
            },
            "scenes": results,
        },
    )
    print("STABILITY wrote", os.path.abspath(OUT_JSON))


main()
exit()
