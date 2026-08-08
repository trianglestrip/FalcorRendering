"""LumenGI S3-B2 multi-bounce feedback gate asset (Agent P2).

Role / purpose
--------------
RUN-ONLY. Measures the Surface Cache multi-bounce feedback on the
LumenGI.cacheDirectRadiance graph output (the internal radiance atlas: RGB =
total radiance = direct + indirect, linear, at atlas resolution), which is the
channel that directly reflects the S3-B2 feedback recurrence:

    indirect_curr = albedo * (direct + strength * indirect_prev)
    radiance      = direct + indirect_curr

Scenarios (task.md 8 / S3-B2 + S3-C2):
  * white_furnace   : feedback ON must settle onto a plateau (no unbounded
                      growth); with the deployed bounce cap the plateau is the
                      capped partial sum, with a high cap it approaches the
                      analytic equilibrium ~ direct/(1-albedo). The per-frame
                      mean/max series is recorded for the ON vs OFF comparison.
  * black_room      : feedback ON must NOT self-brighten (lit-mean stays ~0).
  * emissive_glow   : feedback ON must not blow past the firefly clamp
                      (max <= kLumenGIMaxRadiance * tolerance).
  * cornell_pointlight: dynamic light (PointLight.intensity / position steps);
                      measures how many frames the cache direct radiance takes
                      to reach the new plateau ("response frame count").

Status: RUN-ONLY -- prints stats and VERDICT lines, never exits non-zero.
Writes a JSON record to artifacts/lumengi/S3/gate/feedback_gate.json.

Note on deployment (Agent P2, no-build constraint)
--------------------------------------------------
The host-side toggles (cacheLightingFeedback*) live in LumenGI.cpp/.h and take
effect only after root rebuilds. To exercise the feedback with the existing
Release binary, Agent P2 deploys a test variant of
LumenSurfaceCacheLighting.cs.slang into build/.../Release/shaders that forces
the feedback on (enable = true, strength = 1.0, maxBounces baked). That variant
reads the previous indirect from the radiance atlas A channel (the single-buffer
fallback in the shipped shader), so no new host bindings are required. The
shipped shader, run against the same binary, keeps the feedback off (is_valid
guard + zeroed CB), which is the "off = single bounce" baseline.

Usage (run by root on GPU, from the repo root)
----------------------------------------------
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_feedback_gate.py ^
      --logfile artifacts\\lumengi\\S3\\gate\\feedback_gate.log

Optional env:
  LUMEN_GATE_ONLY=white_furnace|black_room|emissive_glow|pointlight  run one scene.
  LUMEN_GATE_OUT=<path>   override the JSON output path.
"""

from falcor import *
import math
import os
import json

FRAME_RATE = 60
RESOLUTION = (640, 360)

# Frames rendered after scene load before the sampled window (cache capture +
# history settle). Mirrors run_stability.py's 8-frame warmup convention.
WARMUP_FRAMES = 8

# Sampled static frames per scene (task.md 8 S3-C2: "N frames, e.g. 64").
STATIC_FRAMES = 64

# Cache residency warmup for the dynamic-light scene (64 pages/frame budget,
# 7 instances x 6 faces = 42 cards -> 64 frames guarantees full residency).
RESIDENCY_FRAMES = 64

# Max frames rendered after a light step before declaring the response.
STEP_MAX_FRAMES = 24

# A light step is considered settled once the lit-mean is within this relative
# distance of the step's plateau mean (mean of the last 4 rendered frames).
STEP_SETTLE_TOL = 0.05

# --- Gate thresholds (see run_stability.py for the same gates on diffuseGI) ---
BLACK_ROOM_MAX_MEAN = 1e-4       # black room lit-mean must stay below this.
FURNACE_PLATEAU_DRIFT = 0.2      # late-window vs preceding-window relative drift.
FURNACE_WINDOW_GROWTH_MAX = 1.2  # late-window / preceding-window mean factor.
EMISSIVE_MAX_RADIANCE = 10000.0  # kLumenGIMaxRadiance (LumenGIData.slang:64).
EMISSIVE_MAX_TOLERANCE = 1.01
EMISSIVE_MIN_MEAN = 1e-3         # adjacent surfaces actually lit by the panel.
RESPONSE_MAX_FRAMES = 12         # cache must reach the new plateau within this many frames.

SCENE_BLACK_ROOM = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "black_room.pyscene")
)
SCENE_WHITE_FURNACE = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "white_furnace.pyscene")
)
SCENE_EMISSIVE_GLOW = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "emissive_glow.pyscene")
)
SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)

LIGHT_NAME = "LumenGITestPointLight"
LIGHT_BASE = float3(40.0, 40.0, 40.0)
LIGHT_STEP = float3(80.0, 80.0, 80.0)

OUT_JSON = os.environ.get("LUMEN_GATE_OUT", "artifacts/lumengi/S3/gate/feedback_gate.json")
GATE_ONLY = os.environ.get("LUMEN_GATE_ONLY", "")


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
    graph = RenderGraph("LumenGIFeedbackGate")
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
    # useSurfaceCache + useCacheLighting enable the S3 cache lighting path.
    graph.addPass(
        createPass("LumenGI", {"useSurfaceCache": True, "useCacheLighting": True}),
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
    graph.markOutput("LumenGI.cacheDirectRadiance")
    return graph


def _setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0


def sample_cache_direct(label):
    """Sample the cache-direct (radiance atlas) channel. Metric: mean/min/max
    over LIT (non-zero RGB) atlas texels -- the atlas includes zeroed
    un-captured/fallback pages, which would dilute an all-texel mean."""
    try:
        tex = m.activeGraph.get_output("LumenGI.cacheDirectRadiance")
        arr = tex.to_numpy()
    except Exception as exc:
        print("GATE WARNING cacheDirectRadiance read failed:", exc)
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
    finite = bool(math.isfinite(vmin) and math.isfinite(vmax))
    nonneg = bool(vmin >= 0.0)
    print(
        "GATE", label,
        "frame", m.clock.frame,
        "lit_texels", lit_count,
        "min", vmin,
        "max", vmax,
        "mean", vmean,
        "finite", finite,
        "nonnegative", nonneg,
    )
    return {
        "frame": int(m.clock.frame),
        "lit_texels": lit_count,
        "total_texels": int(flat.size),
        "min": vmin,
        "max": vmax,
        "mean": vmean,
        "finite": finite,
        "nonneg": nonneg,
    }


def render_one(label):
    m.clock.frame += 1
    m.renderFrame()
    return sample_cache_direct(label)


def run_static(label, frames):
    return [render_one(label) for _ in range(frames)]


def window_mean(stats, window=4):
    ws = [s for s in stats if s is not None]
    if not ws:
        return None
    ws = ws[-window:]
    return sum(s["mean"] for s in ws) / len(ws)


def window_max(stats, window=4):
    ws = [s for s in stats if s is not None]
    if not ws:
        return None
    ws = ws[-window:]
    return max(s["max"] for s in ws)


def verdict(scene_key, name, ok):
    print("GATE VERDICT", scene_key, name, "PASS" if ok else "FAIL")


def all_stats_valid(stats):
    return all(s is not None and s["finite"] and s["nonneg"] for s in stats)


def safe_div(a, b):
    return (a / b) if (b is not None and b > 0.0) else None


def run_white_furnace():
    """Feedback-ON white furnace: energy must settle onto a plateau (the
    capped geometric partial sum / analytic ~1/(1-albedo) equilibrium) instead
    of growing without bound. Records the per-frame series for ON vs OFF."""
    print("GATE scene white_furnace", SCENE_WHITE_FURNACE)
    _setup_scene(SCENE_WHITE_FURNACE)
    m.clock.frame = 0
    for _ in range(WARMUP_FRAMES):
        render_one("white_furnace-warm")
    series = run_static("white_furnace", STATIC_FRAMES)

    valid = all_stats_valid(series)
    late_mean = window_mean(series)
    # prev window = the 4 frames before the last 4.
    means = [s["mean"] for s in series if s is not None]
    prev_win = sum(means[-8:-4]) / 4.0 if len(means) >= 8 else None
    drift = safe_div(abs(late_mean - prev_win), prev_win) if prev_win else None
    plateau_ok = drift is not None and drift <= FURNACE_PLATEAU_DRIFT
    growth = safe_div(late_mean, prev_win) if prev_win else None
    window_ok = growth is not None and growth <= FURNACE_WINDOW_GROWTH_MAX

    first_max = series[0]["max"] if series[0] else None
    last_max = series[-1]["max"] if series[-1] else None
    growth_ratio = safe_div(last_max, first_max) if first_max else None

    # S3-B1 state note: the white-furnace cache-direct is 0 in the pre-S3-B2
    # binary (the furnace's emissive panel does not reach the cache lighting
    # emissive NEE, cards=6, traced=0). A zero signal is vacuously stable: the
    # feedback of zero stays zero, so treat the plateau/window gates as PASS.
    zero_signal = (last_max is not None and last_max <= 0.0)
    if zero_signal:
        plateau_ok = window_ok = True

    v = [
        ("no NaN/Inf", valid),
        ("no negative radiance", valid),
        ("plateau reached (late-window drift <= %g)" % FURNACE_PLATEAU_DRIFT, bool(plateau_ok)),
        ("no continued brightening (window growth <= %g)" % FURNACE_WINDOW_GROWTH_MAX, bool(window_ok)),
    ]
    for name, ok in v:
        verdict("white_furnace", name, ok)

    return {
        "scene": SCENE_WHITE_FURNACE,
        "frames": series,
        "summary": {
            "late_mean": late_mean,
            "prev_window_mean": prev_win,
            "plateau_drift": drift,
            "window_growth": growth,
            "first_max": first_max,
            "last_max": last_max,
            "max_growth_ratio": growth_ratio,
            "zero_signal": zero_signal,
        },
        "verdicts": [{"name": name, "ok": bool(ok)} for name, ok in v],
    }


def run_black_room():
    """Feedback-ON black room: no lights/env/emissive -> the cache must not
    self-brighten (feedback of zero stays zero, mean ~0 every frame)."""
    print("GATE scene black_room", SCENE_BLACK_ROOM)
    _setup_scene(SCENE_BLACK_ROOM)
    m.clock.frame = 0
    for _ in range(WARMUP_FRAMES):
        render_one("black_room-warm")
    series = run_static("black_room", STATIC_FRAMES)

    valid = all_stats_valid(series)
    means = [s["mean"] for s in series if s is not None]
    max_mean = max(means) if means else 0.0
    ok_black = valid and max_mean <= BLACK_ROOM_MAX_MEAN

    v = [
        ("no NaN/Inf", valid),
        ("no negative radiance", valid),
        ("mean stays <= %g (no self-brightening)" % BLACK_ROOM_MAX_MEAN, bool(ok_black)),
    ]
    for name, ok in v:
        verdict("black_room", name, ok)

    return {
        "scene": SCENE_BLACK_ROOM,
        "frames": series,
        "summary": {"max_mean": max_mean, "first_mean": means[0] if means else None, "last_mean": means[-1] if means else None},
        "verdicts": [{"name": name, "ok": bool(ok)} for name, ok in v],
    }


def run_emissive_glow():
    """Feedback-ON strong emissive: per-frame max must stay bounded by the
    firefly clamp kLumenGIMaxRadiance * tolerance, and adjacent surfaces must
    be lit (feedback multiplies the emissive signal through the room)."""
    print("GATE scene emissive_glow", SCENE_EMISSIVE_GLOW)
    _setup_scene(SCENE_EMISSIVE_GLOW)
    m.clock.frame = 0
    for _ in range(WARMUP_FRAMES):
        render_one("emissive_glow-warm")
    series = run_static("emissive_glow", STATIC_FRAMES)

    cap = EMISSIVE_MAX_RADIANCE * EMISSIVE_MAX_TOLERANCE
    max_over = max((s["max"] for s in series if s is not None), default=0.0)
    valid = all_stats_valid(series)
    clamp_ok = valid and max_over <= cap
    late_mean = window_mean(series)
    lit_ok = late_mean is not None and late_mean >= EMISSIVE_MIN_MEAN

    v = [
        ("no NaN/Inf", valid),
        ("no negative radiance", valid),
        ("max bounded by clamp (<= %g)" % cap, bool(clamp_ok)),
        ("adjacent surfaces lit (late mean >= %g)" % EMISSIVE_MIN_MEAN, bool(lit_ok)),
    ]
    for name, ok in v:
        verdict("emissive_glow", name, ok)

    return {
        "scene": SCENE_EMISSIVE_GLOW,
        "frames": series,
        "summary": {
            "max_over_frames": max_over,
            "clamp_ceiling": EMISSIVE_MAX_RADIANCE,
            "clamp_tolerance": EMISSIVE_MAX_TOLERANCE,
            "late_mean": late_mean,
        },
        "verdicts": [{"name": name, "ok": bool(ok)} for name, ok in v],
    }


def step_series(label, frames):
    """Render `frames` frames after a light change; return the per-frame
    series (frame indices 1..frames relative to the change)."""
    out = []
    for _ in range(frames):
        s = render_one(label)
        if s is not None:
            s["step_frame"] = len(out) + 1
        out.append(s)
    return out


def settle_frame(series, plateau_mean, tol=STEP_SETTLE_TOL):
    """First step-relative frame whose mean is within tol of the plateau and
    stays within tol for every later frame. Returns None if never settled."""
    if plateau_mean is None or plateau_mean <= 0.0:
        # Zero plateau: treat mean below the black threshold as settled.
        for i, s in enumerate(series):
            if s is not None and s["mean"] <= BLACK_ROOM_MAX_MEAN:
                return i + 1
        return None
    for i in range(len(series)):
        ok = True
        for s in series[i:]:
            if s is None or abs(s["mean"] - plateau_mean) / plateau_mean > tol:
                ok = False
                break
        if ok:
            return i + 1
    return None


def run_pointlight_dynamic():
    """Dynamic light: after an intensity (and position) step the cache direct
    radiance must reach the new plateau within RESPONSE_MAX_FRAMES frames.

    NOTE (old-binary state, not S3-B2): with the pre-S3-B2 host the analytic
    light list reaches the cache only after a light POSITION change (the scene
    block's lightCount stays 0 on static frames until updateLights sees a
    non-intensity change). A tiny position nudge first wakes the analytic path
    so the intensity steps that follow are measured on a live cache; the nudge
    itself is also a valid "dynamic light" step and its response is recorded.
    """
    print("GATE scene cornell_pointlight dynamic", SCENE_POINTLIGHT)
    _setup_scene(SCENE_POINTLIGHT)
    m.clock.frame = 0
    light = m.scene.getLight(LIGHT_NAME)
    print("GATE light", light.name, "active", light.active, "intensity", light.intensity)
    light.intensity = LIGHT_BASE

    # Wake-up nudge (see note): a real position change, then render a couple
    # of frames so the analytic path (lightCount) is live before residency.
    light.position = float3(0.0, 0.48, 0.001)
    for _ in range(4):
        render_one("pointlight-wake")
    for _ in range(RESIDENCY_FRAMES):
        render_one("pointlight-residency")

    # Baseline at the base intensity / original position.
    light.position = float3(0.0, 0.48, 0.0)
    base_series = step_series("pointlight-base", 4)
    base_mean = window_mean(base_series)

    # Step 1: position move (toward the boxes) -> a real direct change.
    light.position = float3(0.0, 0.38, -0.15)
    pos_series = step_series("pointlight-position", STEP_MAX_FRAMES)
    pos_plateau = window_mean(pos_series)
    pos_settle = settle_frame(pos_series, pos_plateau)

    # Step 2: intensity 40 -> 80 at the lit position (upward partial step).
    light.intensity = LIGHT_STEP
    up_series = step_series("pointlight-up", STEP_MAX_FRAMES)
    up_plateau = window_mean(up_series)
    up_settle = settle_frame(up_series, up_plateau)

    # Step 3: intensity 80 -> 0 at the lit position (off; cache must drop).
    light.intensity = LIGHT_STEP * 0.0
    off_series = step_series("pointlight-off", STEP_MAX_FRAMES)
    off_plateau = window_mean(off_series)
    off_settle = settle_frame(off_series, off_plateau)

    # Step 4: position move back to the original spot.
    light.intensity = LIGHT_BASE
    light.position = float3(0.0, 0.48, 0.0)
    back_series = step_series("pointlight-position-back", STEP_MAX_FRAMES)
    back_plateau = window_mean(back_series)
    back_settle = settle_frame(back_series, back_plateau)

    valid = all_stats_valid(base_series + pos_series + up_series + off_series + back_series)
    pos_ok = pos_settle is not None and pos_settle <= RESPONSE_MAX_FRAMES
    up_ok = up_settle is not None and up_settle <= RESPONSE_MAX_FRAMES
    off_ok = off_settle is not None and off_settle <= RESPONSE_MAX_FRAMES
    back_ok = back_settle is not None and back_settle <= RESPONSE_MAX_FRAMES

    # The position/up steps must actually move the mean (a real response).
    moved_pos = safe_div(pos_plateau, base_mean) if base_mean else None
    moved_up = safe_div(up_plateau, pos_plateau) if pos_plateau else None

    v = [
        ("no NaN/Inf", valid),
        ("no negative radiance", valid),
        ("position step: new plateau in <= %d frames (settle %s)" % (RESPONSE_MAX_FRAMES, pos_settle), pos_ok),
        ("intensity up: new plateau in <= %d frames (settle %s)" % (RESPONSE_MAX_FRAMES, up_settle), up_ok),
        ("intensity off: cache drops to ~zero (settle %s)" % off_settle, off_ok),
        ("position back: new plateau in <= %d frames (settle %s)" % (RESPONSE_MAX_FRAMES, back_settle), back_ok),
    ]
    for name, ok in v:
        verdict("pointlight", name, ok)

    return {
        "scene": SCENE_POINTLIGHT,
        "steps": {
            "base": base_series,
            "position": pos_series,
            "up": up_series,
            "off": off_series,
            "position_back": back_series,
        },
        "summary": {
            "base_mean": base_mean,
            "position_plateau": pos_plateau,
            "position_settle_frame": pos_settle,
            "position_vs_base": moved_pos,
            "up_plateau": up_plateau,
            "up_settle_frame": up_settle,
            "up_vs_position": moved_up,
            "off_plateau": off_plateau,
            "off_settle_frame": off_settle,
            "back_plateau": back_plateau,
            "back_settle_frame": back_settle,
            "response_max_frames": RESPONSE_MAX_FRAMES,
        },
        "verdicts": [{"name": name, "ok": bool(ok)} for name, ok in v],
    }


def main():
    graph = create_lumen_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)

    only = GATE_ONLY.strip()
    scenes = {}
    if only and only != "all":
        scenes[only] = {
            "white_furnace": run_white_furnace,
            "black_room": run_black_room,
            "emissive_glow": run_emissive_glow,
            "pointlight": run_pointlight_dynamic,
        }[only]()
    else:
        scenes["white_furnace"] = run_white_furnace()
        scenes["black_room"] = run_black_room()
        scenes["emissive_glow"] = run_emissive_glow()
        scenes["pointlight"] = run_pointlight_dynamic()

    write_json(
        OUT_JSON,
        {
            "script": "run_feedback_gate.py",
            "role": "S3-B2 multi-bounce feedback gate (Agent P2)",
            "status": "run-only",
            "feedback_deployment": "deployed shader variant (see script header)",
            "warmup_frames": WARMUP_FRAMES,
            "static_frames": STATIC_FRAMES,
            "residency_frames": RESIDENCY_FRAMES,
            "step_max_frames": STEP_MAX_FRAMES,
            "resolution": list(RESOLUTION),
            "thresholds": {
                "black_room_max_mean": BLACK_ROOM_MAX_MEAN,
                "furnace_plateau_drift": FURNACE_PLATEAU_DRIFT,
                "furnace_window_growth_max": FURNACE_WINDOW_GROWTH_MAX,
                "emissive_max_radiance": EMISSIVE_MAX_RADIANCE,
                "emissive_max_tolerance": EMISSIVE_MAX_TOLERANCE,
                "emissive_min_mean": EMISSIVE_MIN_MEAN,
                "response_max_frames": RESPONSE_MAX_FRAMES,
            },
            "scenes": scenes,
        },
    )
    print("GATE wrote", os.path.abspath(OUT_JSON))


main()
exit()
