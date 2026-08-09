from falcor import *

"""LumenGI S5-C1 ghost / trailing-artifact quantitative skeleton (Agent Z6).

Role / purpose
--------------
RUN-ONLY Mogwai GPU skeleton quantifying ghost / 拖影 (task.md §10, S5-C1 "输出
history accept/reject 热图及 ghost 指标"). Same graph as run_temporal.py
(GBufferRT -> LumenGI, useScreenTrace + useScreenProbes + useTemporalFilter).

It measures, quantitatively:

  A. static noise floor (cornell_box, fixed camera, 16 frames) -- the reference
     "steady" framediff every ghost metric is compared against;

  B. moving light, then freeze (cornell_pointlight): 运动物体经过后背景恢复帧数
     = number of trailing frames after the light stops before the inter-frame
     change drops back to the noise floor, plus the decay series;

  C. moving rigid body, then freeze (animated_cubes): same recovery metric, plus
     a validity check that the animation actually produced motion (GBufferRT
     mvec moving-pixel fraction > floor -- otherwise the phase is vacuous);

  D. camera cut first-frame statistics (cornell_box): the instantaneous camera
     jump, reporting the first-frame-after-cut change rate vs the steady tail and
     (S5_TODO) the history accept fraction on that frame ("cut 后历史立即失效").

Ghost-metric note (S5_TODO[ghost])
----------------------------------
The "background recovery frames" metric is computed host-side from the diffuseGI
inter-frame change rate. Pre-S5 LumenGI resets history on every scene update, so
recovery is ~1 frame; post-S5 the temporal filter accumulates, and a long tail
means the filter is keeping stale history where the object passed -- which is
exactly the artifact the gate must catch. When Z5 exposes a real ghost channel
(add it to TEMPORAL_CHANNELS below) the recovery measurement keeps its shape and
the framediff proxy is cross-checked against it.

S5_TODO contract (root freezes with Z4/Z5; SKIP, never crash, when absent)
-------------------------------------------------------------------------
  * S5_TODO[probe_channel]: "probeInterpolated" (S4-B3). Absent -> probe stats
    are not recorded (diffuseGI metrics still run).
  * S5_TODO[temporal_channels]: temporalFiltered (gTemporalOutput, .a = history
    length) / temporalAlpha (gTemporalAlpha) / temporalConfidence /
    temporalHistoryLength (gHistoryLength) -- aligned to Z5's in-flight
    LumenTemporalFilterData.slang. Absent -> the accept-fraction gate SKIPs.
  * S5_TODO[history_channel]: accept/reject is derived from the history length
    (temporalFiltered.a or temporalHistoryLength): hist > 1 = accepted,
    hist <= 1 = fresh/reset; temporalAlpha (>= 0.5) is the cross-check.
  * S5_TODO[ghost]: recovery-frames floor / GHOST_MAX_FRAMES placeholders.
  * S5_TODO gates: MVEC thresholds, CUT_DIFF_RATIO_MIN, GHOST_* placeholders.

Exit: Falcor `exit()`. JSON -> artifacts/lumengi/S5/ghost.json (override
LUMEN_TEMPORAL_GHOST_OUT). Determinism: fixed camera path, paused clock, manual
frame counter (Clock::setFrame maps frame->time, so the baked FBX animation in
phase C is deterministic), 640x360, 60fps.
"""

import json
import math
import os

import numpy as np

RESOLUTION = (640, 360)
FRAME_RATE = 60

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"
SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)
SCENE_ANIMATED = "test_scenes/animated_cubes/animated_cubes.pyscene"

OUT_JSON = os.environ.get("LUMEN_TEMPORAL_GHOST_OUT", "artifacts/lumengi/S5/ghost.json")

USE_SCREEN_TRACE = bool(os.environ.get("LUMEN_TEMPORAL_GHOST_USE_SCREEN_TRACE", "") != "0")
USE_SCREEN_PROBES = bool(os.environ.get("LUMEN_TEMPORAL_GHOST_USE_SCREEN_PROBES", "") != "0")
USE_TEMPORAL_FILTER = bool(os.environ.get("LUMEN_TEMPORAL_GHOST_USE_TEMPORAL_FILTER", "") != "0")

# Frame counts (env-overridable for bring-up).
STATIC_CAL_FRAMES = int(os.environ.get("LUMEN_TEMPORAL_GHOST_STATIC_CAL", "16"))
STEADY_WINDOW = int(os.environ.get("LUMEN_TEMPORAL_GHOST_STEADY_WINDOW", "8"))
LIGHT_MOVE_FRAMES = int(os.environ.get("LUMEN_TEMPORAL_GHOST_LIGHT_MOVE", "6"))
LIGHT_FREEZE_FRAMES = int(os.environ.get("LUMEN_TEMPORAL_GHOST_LIGHT_FREEZE", "8"))
RIGID_WARMUP = int(os.environ.get("LUMEN_TEMPORAL_GHOST_RIGID_WARMUP", "4"))
RIGID_MOVE_FRAMES = int(os.environ.get("LUMEN_TEMPORAL_GHOST_RIGID_MOVE", "8"))
RIGID_FREEZE_FRAMES = int(os.environ.get("LUMEN_TEMPORAL_GHOST_RIGID_FREEZE", "8"))
CUT_STEADY = int(os.environ.get("LUMEN_TEMPORAL_GHOST_CUT_STEADY", "4"))
CUT_SETTLE = int(os.environ.get("LUMEN_TEMPORAL_GHOST_CUT_SETTLE", "6"))

CAM_START_POS = float3(0, 0.28, 1.2)
CAM_START_TARGET = float3(0, 0.28, 0)
CAM_UP = float3(0, 1, 0)
CAM_FOCAL_LENGTH = 35.0
CUT_POS = float3(0.25, 0.4, 0.5)
CUT_TARGET = float3(0.15, 0.2, -0.1)

LIGHT_NAME = "LumenGITestPointLight"
LIGHT_MOVE_DELTA = float3(0.03, -0.02, 0.02)

# Channels (aligned to Z5's LumenTemporalFilterData.slang, working tree).
PROBE_INTERP_CHANNEL = "probeInterpolated"
TEMPORAL_CHANNELS = [
    "temporalFiltered",        # gTemporalOutput: .rgb smoothed GI, .a = NEW history length.
    "temporalAlpha",           # gTemporalAlpha: effective EMA alpha (1.0 = full reject / reset).
    "temporalConfidence",      # gTemporalConfidence: updated confidence.
    "temporalHistoryLength",   # gHistoryLength: previous history length R32F.
]
HISTORY_LENGTH_CHANNEL = "temporalFiltered"  # .a = new history length (accept/reject source).
HISTORY_ALPHA_CHANNEL = "temporalAlpha"      # EMA alpha cross-check.
ACCEPT_HISTORY_LENGTH_MIN = 1.5              # hist > this = accepted (>= 2), else fresh.
REJECT_ALPHA_MAX = 0.5                       # alpha >= this = reject (S5_TODO placeholder).

# -------------------------------------------------------------------------------------
# Gates (S5_TODO placeholders; freeze with root).
# -------------------------------------------------------------------------------------
GHOST_FLOOR_MULT = 2.0        # recovery floor = max(1e-4, noise_floor * this).
GHOST_MAX_FRAMES = 4          # max trailing frames a moving object may leave.
MVEC_MOVING_PX = 1.0          # |mvec| above this counts as a moving pixel.
MVEC_MOVING_FRACTION_MIN = 0.001  # rigid phase must actually move more than this.
CUT_DIFF_RATIO_MIN = 3.0      # first-frame-after-cut diff / steady tail diff.
CUT_ACCEPT_FRACTION_MAX = 0.05  # cut-frame history accept fraction must be ~0.
MVEC_STATIC_EPS = 0.25        # |mvec| below this = static-background pixel.

records = []
prev_gi = None
prev_probe = None
available_channels = []


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


def lumen_props():
    return {
        "enabled": True,
        "traceMode": "HardwareRT",
        "qualityPreset": "High",
        "useScreenTrace": USE_SCREEN_TRACE,
        "useScreenProbes": USE_SCREEN_PROBES,
        "useTemporalFilter": USE_TEMPORAL_FILTER,
    }


def create_lumen_graph(extra_outputs):
    graph = RenderGraph("LumenGIGhost")
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
    graph.markOutput("LumenGI.confidence")
    graph.markOutput("GBufferRT.mvec")
    for ch in extra_outputs:
        graph.markOutput("LumenGI." + ch)
    return graph


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


def probe_channel(channel):
    graph = None
    try:
        graph = create_lumen_graph([channel])
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(SCENE_CORNELL, CAM_START_POS, CAM_START_TARGET)
        m.clock.frame += 1
        m.renderFrame()
        return True
    except Exception as exc:
        print("GHOST WARNING channel 'LumenGI.%s' not available (%s)" % (channel, str(exc)))
        return False
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass


def add_main_graph(extra_outputs):
    global available_channels
    graph = create_lumen_graph(extra_outputs)
    m.addGraph(graph)
    m.setActiveGraph(graph)
    available_channels = list(extra_outputs)
    return graph


def grab(name):
    return np.asarray(m.activeGraph.get_output(name).to_numpy(), dtype=np.float32)


def mvec_stats():
    mv = grab("GBufferRT.mvec")
    if mv.ndim == 3 and mv.shape[-1] >= 2:
        mag = np.sqrt(mv[..., 0] ** 2 + mv[..., 1] ** 2)
    else:
        mag = np.abs(mv)
    return {
        "mvec_mean": float(mag.mean()),
        "mvec_moving_fraction": float((mag > MVEC_MOVING_PX).mean()),
        "mvec_static_fraction": float((mag <= MVEC_STATIC_EPS).mean()),
    }


def render_one(label, with_mvec=False, advance=True):
    global prev_gi, prev_probe
    if advance:
        m.clock.frame += 1
    m.renderFrame()

    rec = {"phase": label, "frame": int(m.clock.frame)}
    gi = grab("LumenGI.diffuseGI")
    gi = gi[..., :3] if gi.ndim == 3 and gi.shape[-1] >= 3 else gi
    gi = gi[..., :3]
    rec["gi_mean"] = float(gi.mean())
    rec["gi_max"] = float(gi.max())
    rec["gi_finite"] = bool(math.isfinite(float(gi.min())) and math.isfinite(float(gi.max())))
    rec["gi_nonneg"] = bool(float(gi.min()) >= 0.0)
    if prev_gi is not None and prev_gi.shape == gi.shape:
        rec["gi_framediff"] = float(np.abs(gi - prev_gi).mean())
    prev_gi = gi

    if PROBE_INTERP_CHANNEL in available_channels:
        p = grab("LumenGI." + PROBE_INTERP_CHANNEL)
        rec["probe_irradiance_mean"] = float(p[..., :3].mean())
        rec["probe_confidence_mean"] = float(p[..., 3].mean())
        if prev_probe is not None and prev_probe.shape == p.shape:
            rec["probe_framediff"] = float(np.abs(p - prev_probe).mean())
        prev_probe = p

    for ch in available_channels:
        if ch == "temporalFiltered":
            t = grab("LumenGI." + ch)
            if t.ndim == 3 and t.shape[-1] >= 4:
                hist = t[..., 3]
                rec["history_length_mean"] = float(hist.mean())
                rec["history_accept_fraction"] = float((hist > ACCEPT_HISTORY_LENGTH_MIN).mean())
                rec["history_reject_fraction"] = float((hist <= ACCEPT_HISTORY_LENGTH_MIN).mean())
        elif ch == "temporalAlpha":
            t = grab("LumenGI." + ch)
            s = t[..., 0] if t.ndim == 3 else t
            rec["temporal_alpha_mean"] = float(s.mean())
            rec["temporal_alpha_reject_fraction"] = float((s >= REJECT_ALPHA_MAX).mean())
        elif ch == "temporalHistoryLength":
            t = grab("LumenGI." + ch)
            s = t[..., 0] if t.ndim == 3 else t
            rec["temporal_history_length_mean"] = float(s.mean())
            rec["history_accept_fraction"] = float((s > ACCEPT_HISTORY_LENGTH_MIN).mean())
            rec["history_reject_fraction"] = float((s <= ACCEPT_HISTORY_LENGTH_MIN).mean())

    if with_mvec:
        rec["mvec"] = mvec_stats()

    records.append(rec)
    return rec


def render_frozen(label, count):
    pinned = m.clock.frame
    out = []
    for _ in range(count):
        m.clock.frame = pinned
        render_one(label, advance=False)
        out.append(records[-1])
    return out


def recovery_frames(phase_recs, floor):
    """Trailing frames whose inter-frame change stays above `floor` (ghost tail)."""
    n = 0
    for r in reversed(phase_recs):
        d = r.get("gi_framediff")
        if d is not None and d > floor:
            n += 1
        else:
            break
    return n


def mean_of(phase_recs, field):
    vals = [r.get(field) for r in phase_recs if r.get(field) is not None]
    return float(sum(vals) / len(vals)) if vals else None


def main():
    report = {
        "stage": "S5",
        "script": "run_temporal_ghost.py",
        "role": "S5-C1 ghost / trailing-artifact quantification (Agent Z6)",
        "status": "skeleton",
        "resolution": list(RESOLUTION),
        "config": {
            "useScreenTrace": USE_SCREEN_TRACE,
            "useScreenProbes": USE_SCREEN_PROBES,
            "useTemporalFilter": USE_TEMPORAL_FILTER,
            "probe_channel": PROBE_INTERP_CHANNEL,
            "temporal_channels": TEMPORAL_CHANNELS,
        },
    }
    verdicts = []

    probe_available = probe_channel(PROBE_INTERP_CHANNEL)
    temporal_available = {ch: probe_channel(ch) for ch in TEMPORAL_CHANNELS}
    extra = [PROBE_INTERP_CHANNEL] if probe_available else []
    extra += [ch for ch in TEMPORAL_CHANNELS if temporal_available[ch]]
    report["probe_channel_available"] = probe_available
    report["temporal_channels_available"] = temporal_available

    add_main_graph(extra)

    # ----------------------------------------------------- A. static noise floor ----
    _setup_scene(SCENE_CORNELL, CAM_START_POS, CAM_START_TARGET)
    for _ in range(2):
        render_one("warmup")
    for _ in range(STATIC_CAL_FRAMES):
        render_one("static-cal")
    steady = [r for r in records if r["phase"] == "static-cal"]
    steady_diffs = [r.get("gi_framediff") for r in steady if r.get("gi_framediff") is not None]
    if steady_diffs:
        noise_floor = float(sum(steady_diffs[-STEADY_WINDOW:]) / len(steady_diffs[-STEADY_WINDOW:]))
    else:
        noise_floor = 1e-4
    ghost_floor = max(1e-4, noise_floor * GHOST_FLOOR_MULT)
    report["static_calibration"] = {
        "noise_floor": noise_floor,
        "steady_window": STEADY_WINDOW,
        "ghost_floor": ghost_floor,
    }
    finite_ok = all(r["gi_finite"] for r in records)
    verdicts.append(("no NaN/Inf across static calibration", "PASS" if finite_ok else "FAIL"))

    # ------------------------------------------ B. moving light, then freeze ---------
    prev_gi = None
    prev_probe = None
    _setup_scene(SCENE_POINTLIGHT, CAM_START_POS, CAM_START_TARGET)
    for _ in range(3):
        render_one("light-warmup")
    light = m.scene.getLight(LIGHT_NAME)
    for i in range(LIGHT_MOVE_FRAMES):
        light.position = light.position + LIGHT_MOVE_DELTA
        render_one("light-move-%d" % i, with_mvec=True)
    light_freezing = render_frozen("light-freeze", LIGHT_FREEZE_FRAMES)
    light_ghost = recovery_frames(light_freezing, ghost_floor)
    report["moving_light"] = {
        "move_mean_diff": mean_of([r for r in records if r["phase"].startswith("light-move")], "gi_framediff"),
        "freeze_diff_series": [r.get("gi_framediff") for r in light_freezing],
        "recovery_frames": light_ghost,
        "ghost_max_frames": GHOST_MAX_FRAMES,
    }
    verdicts.append(("moving light background recovery within %d frames (got %d, S5_TODO)" % (
        GHOST_MAX_FRAMES, light_ghost),
        "PASS" if light_ghost <= GHOST_MAX_FRAMES else "FAIL"))

    # ------------------------------------- C. moving rigid body, then freeze ---------
    prev_gi = None
    prev_probe = None
    _setup_scene(SCENE_ANIMATED)  # scene default camera frames the cubes.
    m.scene.animated = True
    for _ in range(RIGID_WARMUP):
        render_one("rigid-warmup")
    move_recs = []
    for i in range(RIGID_MOVE_FRAMES):
        move_recs.append(render_one("rigid-move-%d" % i, with_mvec=True))
    rigid_freezing = render_frozen("rigid-freeze", RIGID_FREEZE_FRAMES)
    rigid_ghost = recovery_frames(rigid_freezing, ghost_floor)
    moved_fracs = [r.get("mvec", {}).get("mvec_moving_fraction") for r in move_recs]
    moved_frac = float(sum(moved_fracs) / len(moved_fracs)) if moved_fracs else None
    moved_ok = moved_frac is not None and moved_frac >= MVEC_MOVING_FRACTION_MIN
    report["moving_rigid"] = {
        "move_mean_mvec_fraction": moved_frac,
        "freeze_diff_series": [r.get("gi_framediff") for r in rigid_freezing],
        "recovery_frames": rigid_ghost,
        "ghost_max_frames": GHOST_MAX_FRAMES,
    }
    verdicts.append(("rigid motion actually moved (mvec frac %.4f >= %.4f)" % (
        moved_frac if moved_frac is not None else 0.0, MVEC_MOVING_FRACTION_MIN),
        "PASS" if moved_ok else "FAIL"))
    verdicts.append(("moving rigid body background recovery within %d frames (got %d, S5_TODO)" % (
        GHOST_MAX_FRAMES, rigid_ghost),
        "PASS" if rigid_ghost <= GHOST_MAX_FRAMES else "FAIL"))

    # ----------------------------------- D. camera cut first-frame statistics --------
    prev_gi = None
    prev_probe = None
    _setup_scene(SCENE_CORNELL, CAM_START_POS, CAM_START_TARGET)
    for _ in range(CUT_STEADY):
        render_one("cut-steady")
    steady_tail = [r for r in records if r["phase"] == "cut-steady"]
    steady_diff = mean_of(steady_tail, "gi_framediff")
    camera = m.scene.camera
    camera.position = CUT_POS
    camera.target = CUT_TARGET
    cut_rec = render_one("camera-cut")
    for _ in range(CUT_SETTLE):
        render_one("cut-settle")
    cut_diff = cut_rec.get("gi_framediff")
    ratio = cut_diff / steady_diff if (cut_diff is not None and steady_diff and steady_diff > 0) else None
    spike_ok = ratio is not None and ratio > CUT_DIFF_RATIO_MIN
    report["camera_cut"] = {
        "steady_diff": steady_diff,
        "first_frame_after_cut_diff": cut_diff,
        "first_frame_after_cut_mean": cut_rec["gi_mean"],
        "ratio_vs_steady": ratio,
        "first_frame_accept_fraction": cut_rec.get("history_accept_fraction"),
    }
    verdicts.append(("camera cut first-frame change-rate spike (ratio %.2f > %.1f)" % (
        ratio if ratio is not None else 0.0, CUT_DIFF_RATIO_MIN),
        "PASS" if spike_ok else "FAIL"))
    # S5_TODO[history_channel]: the "历史立即失效" check.
    if any(ch in available_channels for ch in ("temporalHistoryLength", HISTORY_LENGTH_CHANNEL)):
        frac = cut_rec.get("history_accept_fraction")
        verdicts.append(("camera cut history accept fraction <= %.2f (S5_TODO channel)" % CUT_ACCEPT_FRACTION_MAX,
                         "PASS" if frac is not None and frac <= CUT_ACCEPT_FRACTION_MAX else "FAIL"))
    else:
        verdicts.append(("camera cut history accept fraction ~ 0 (S5_TODO history channel)", "SKIP"))

    # --------------------------------------------------------------- final ----------
    report["series"] = records
    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    if any(v == "SKIP" for _, v in verdicts):
        report["summary"] = "SKIP" if all(v != "FAIL" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("GHOST VERDICT", name, verdict)
    write_json(OUT_JSON, report)
    print("GHOST wrote", os.path.abspath(OUT_JSON))


# Falcor's embedded Python executes the script with __name__ == 'builtins', so an
# `if __name__ == "__main__":` guard never runs. Call main() unconditionally.
try:
    main()
except Exception as exc:
    print("GHOST ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S5",
            "script": "run_temporal_ghost.py",
            "summary": "SKIP",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive SKIP on fatal error)", "SKIP")],
        },
    )
exit()
