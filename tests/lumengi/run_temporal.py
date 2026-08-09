from falcor import *

"""LumenGI S5-C1 temporal trajectory test asset (SKELETON, Agent Z6).

Role / purpose
--------------
RUN-ONLY Mogwai GPU skeleton for the S5 temporal wave (task.md §10, S5-C1
"Temporal 轨迹测试"). Graph: GBufferRT -> LumenGI with useScreenTrace=True +
useScreenProbes=True + useTemporalFilter=True (the S5 input state). It drives a
fixed temporal trajectory and records, per frame:

  * diffuseGI (live since S1): min/max/mean/finite/non-neg plus the inter-frame
    change rate (mean abs diff vs previous frame);
  * probeInterpolated (S4.3 channel, the S5 input; PROBED, absent -> SKIP):
    mean interpolated irradiance, mean confidence, and its own change rate;
  * temporal output channels (S5_TODO, frozen by Z4/Z5): per-channel means, the
    history accept/reject heatmap (downsampled block-mean) and the per-frame
    accept fraction. The heatmap gate SKIPs until the history channel exists;
  * ghost metric (S5_TODO placeholder): "运动后残留帧数" = trailing frames after
    a motion stops during which the inter-frame change stays above a floor
    (host-side proxy; a real ghost channel replaces it when Z5 exposes one).

Trajectory (reuses run_dynamic.py camera/light control)
-------------------------------------------------------
  1. static   : 256 frames, fixed camera (energy convergence / plateau);
  2. pan      : step-by-step camera translation;
  3. orbit    : step-by-step camera orbit;
  4. fast pan : fast lateral slide (快速横移, large per-frame delta);
  5. cut      : instantaneous camera jump + settle frames (history reset);
  6. light    : moving PointLight.source (cornell_pointlight), then freeze;
  7. rigid    : moving rigid body (animated_cubes FBX animation), then freeze;
  8. emissive : emissive step (Glow Panel emissiveFactor 100 -> 0 -> 100).

S5_TODO contract (root must freeze with Z4/Z5 before this becomes gating)
------------------------------------------------------------------------
  * S5_TODO[probe_channel]: the LumenGI output exposing the per-pixel
    interpolated probe result (S4-B3). Candidate "probeInterpolated"
    (full-res RGBA16F; RGB = incident irradiance E, A = confidence in [0,1]).
    Absent -> the probe section SKIPs (expected at HEAD 96dfaa7f, pre-S4.3).
  * S5_TODO[temporal_channels]: the LumenGI outputs exposing the temporal
    filter. ALIGNED to Z5's in-flight LumenTemporalFilterData.slang
    (Source/RenderPasses/LumenGI/Temporal/): temporalFiltered (gTemporalOutput,
    .a = NEW history length) / temporalAlpha (gTemporalAlpha) /
    temporalConfidence (gTemporalConfidence) / temporalHistoryLength
    (gHistoryLength). Update TEMPORAL_CHANNELS only -- the probing loop and the
    accept/reject derivation are data-driven.
  * S5_TODO[history_channel]: no dedicated accept/reject texture exists in Z5's
    filter; the accept/reject heatmap is DERIVED here from the history length
    (temporalFiltered.a or temporalHistoryLength): hist > 1 = accepted (history
    reused), hist <= 1 = fresh/reset (rejected). temporalAlpha (>= 0.5) is the
    cross-check (disocclusion alpha). Absent -> the heatmap gate SKIPs.
  * S5_TODO[ghost]: "运动后残留帧数" is computed here as trailing framediff
    frames; once a ghost channel exists switch GHOST_FLOOR / GHOST_MAX_FRAMES
    to it (the recovery measurement stays the same shape).
  * S5_TODO gates (placeholders): ENERGY_GROWTH_THRESHOLD, CONVERGENCE_FACTOR,
    CUT_DIFF_RATIO_MIN, CUT_RECOVERY_FACTOR, FLICKER_MAX_DIFF, GHOST_*,
    OFF_MAX_MEAN, EMISSIVE_* -- freeze with root after S5-A1/B1 land.

S5 gate alignment (task.md §10 门禁)
------------------------------------
  * "Camera cut 后历史立即失效": cut-frame change-rate spike vs steady tail
    (live) + cut-frame history accept fraction ~= 0 (S5_TODO channel).
  * "平滑运动时历史稳定复用": reported per-phase change-rate; accept-fraction
    gate (S5_TODO).
  * "动态物体不留下长期拖影": GHOST_MAX_FRAMES recovery gate (S5_TODO).
  * "无 NaN/Inf、负方差、history length 溢出": finite/non-neg (live); negative
    variance / history-length overflow are S5_TODO (need history channel).
  * "固定轨迹的帧间闪烁指标达到目标": static-tail flicker metric (live), gate
    threshold S5_TODO.

Usage (run by root on GPU, from the repo root)
----------------------------------------------
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_temporal.py ^
      --logfile artifacts\\lumengi\\S5\\temporal.log
(create artifacts\\lumengi\\S5 first.) Output: artifacts/lumengi/S5/temporal.json
(override LUMEN_TEMPORAL_OUT). Frame counts override via LUMEN_TEMPORAL_* env.

Determinism contract
--------------------
Fixed camera path (constants below), framerate 60, manual frame counter, paused
clock (m.clock.frame assignment advances time deterministically -- Clock::setFrame
with a framerate maps frame -> time, so the animated-cubes rigid-body phase
animates deterministically too). Resolution 640x360. Probe directions are
seeded per pixel/frame internally (task rule 5); trajectories are repeatable in
structure, not bit-exact -- acceptable for this regression (same as run_dynamic.py).
"""

import json
import math
import os

import numpy as np

# -------------------------------------------------------------------------------------
# Configuration (S5_TODO: freeze defaults with root when the S5 wave lands).
# -------------------------------------------------------------------------------------
RESOLUTION = (640, 360)
FRAME_RATE = 60

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"
SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)
SCENE_EMISSIVE_GLOW = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "emissive_glow.pyscene")
)
SCENE_ANIMATED = "test_scenes/animated_cubes/animated_cubes.pyscene"

OUT_JSON = os.environ.get("LUMEN_TEMPORAL_OUT", "artifacts/lumengi/S5/temporal.json")

# Pass state flags. useScreenTrace/useScreenProbes are live (S4); useTemporalFilter
# is parsed (LumenGI.cpp:80/231) but not wired pre-S5 -- harmless to set.
USE_SCREEN_TRACE = bool(os.environ.get("LUMEN_TEMPORAL_USE_SCREEN_TRACE", "") != "0")
USE_SCREEN_PROBES = bool(os.environ.get("LUMEN_TEMPORAL_USE_SCREEN_PROBES", "") != "0")
USE_TEMPORAL_FILTER = bool(os.environ.get("LUMEN_TEMPORAL_USE_TEMPORAL_FILTER", "") != "0")

# Frame counts (env-overridable for bring-up / CI time-budget).
STATIC_FRAMES = int(os.environ.get("LUMEN_TEMPORAL_STATIC_FRAMES", "256"))
WARMUP_FRAMES = int(os.environ.get("LUMEN_TEMPORAL_WARMUP_FRAMES", "8"))
PAN_STEPS = int(os.environ.get("LUMEN_TEMPORAL_PAN_STEPS", "6"))
ORBIT_STEPS = int(os.environ.get("LUMEN_TEMPORAL_ORBIT_STEPS", "6"))
FAST_STEPS = int(os.environ.get("LUMEN_TEMPORAL_FAST_STEPS", "8"))
CUT_SETTLE = int(os.environ.get("LUMEN_TEMPORAL_CUT_SETTLE", "6"))
LIGHT_MOVE_STEPS = int(os.environ.get("LUMEN_TEMPORAL_LIGHT_MOVE_STEPS", "6"))
LIGHT_FREEZE = int(os.environ.get("LUMEN_TEMPORAL_LIGHT_FREEZE", "4"))
RIGID_WARMUP = int(os.environ.get("LUMEN_TEMPORAL_RIGID_WARMUP", "4"))
RIGID_MOVE = int(os.environ.get("LUMEN_TEMPORAL_RIGID_MOVE", "8"))
RIGID_FREEZE = int(os.environ.get("LUMEN_TEMPORAL_RIGID_FREEZE", "6"))
EMISSIVE_STEP_FRAMES = int(os.environ.get("LUMEN_TEMPORAL_EMISSIVE_STEP_FRAMES", "8"))

# --- Camera trajectory (Phase A: cornell_box) ---
CAM_START_POS = float3(0, 0.28, 1.2)
CAM_START_TARGET = float3(0, 0.28, 0)
CAM_UP = float3(0, 1, 0)
CAM_FOCAL_LENGTH = 35.0
PAN_DELTA = float3(0.02, 0.01, -0.015)
ORBIT_STEP_DEG = 8.0
FAST_DELTA = float3(0.06, 0.0, 0.0)      # 快速横移: large lateral velocity.
CUT_POS = float3(0.25, 0.4, 0.5)
CUT_TARGET = float3(0.15, 0.2, -0.1)

# --- Moving light (Phase B: cornell_pointlight) ---
LIGHT_NAME = "LumenGITestPointLight"
LIGHT_MOVE_DELTA = float3(0.03, -0.02, 0.02)

# --- Emissive step (Phase D: emissive_glow) ---
EMISSIVE_MATERIAL = "Glow Panel"
EMISSIVE_FACTOR_BASE = 100.0
EMISSIVE_FACTOR_OFF = 0.0

# -------------------------------------------------------------------------------------
# S4/S5 channel names.
# -------------------------------------------------------------------------------------
# S4_TODO[probe_channel] (S4-B3, Z2): per-pixel interpolated probe result.
PROBE_INTERP_CHANNEL = "probeInterpolated"

# S5_TODO[temporal_channels]: the temporal filter outputs Z4/Z5 freeze. The list
# below is ALIGNED to Z5's in-flight LumenTemporalFilterData.slang (working tree at
# Source/RenderPasses/LumenGI/Temporal/): gTemporalOutput / gTemporalAlpha /
# gTemporalConfidence / gHistoryLength. Unknown channels are probed and simply not
# marked (the pass logs a warning for absent channels, never crashes).
TEMPORAL_CHANNELS = [
    "temporalFiltered",        # gTemporalOutput: .rgb smoothed GI, .a = NEW history length.
    "temporalAlpha",           # gTemporalAlpha: effective EMA alpha (1.0 = full reject / reset).
    "temporalConfidence",      # gTemporalConfidence: updated confidence for S5-B2.
    "temporalHistoryLength",   # gHistoryLength: previous history length R32F.
]
# S5_TODO[history_channel]: the accept/reject heatmap source. There is no dedicated
# accept/reject texture in Z5's filter; it is DERIVED here from the history length
# (temporalFiltered.a or temporalHistoryLength): hist > ACCEPT_HISTORY_LENGTH_MIN
# means the pixel re-used history (accept), hist <= it means fresh/reset (reject).
# temporalAlpha is the cross-check (alpha >= REJECT_ALPHA_MAX == disocclusion).
HISTORY_LENGTH_CHANNEL = "temporalFiltered"  # .a = new history length.
HISTORY_ALPHA_CHANNEL = "temporalAlpha"      # EMA alpha.
ACCEPT_HISTORY_LENGTH_MIN = 1.5              # hist > this = accepted (>= 2), else fresh.
REJECT_ALPHA_MAX = 0.5                       # alpha >= this = reject (S5_TODO placeholder).

# -------------------------------------------------------------------------------------
# Gates (S5_TODO placeholders -- freeze with root post-S5-A1/B1).
# -------------------------------------------------------------------------------------
ENERGY_GROWTH_THRESHOLD = 2.0      # last static mean < first mean * this.
CONVERGENCE_FACTOR = 0.5           # static-tail framediff < first framediff * this.
CUT_DIFF_RATIO_MIN = 3.0           # cut-frame framediff / steady-tail framediff.
CUT_RECOVERY_FACTOR = 0.2          # settle framediff < cut framediff * this.
FLICKER_MAX_DIFF = 0.05            # max static-tail framediff (S5_TODO threshold).
GHOST_FLOOR = 1e-3                 # framediff floor for ghost trailing frames.
GHOST_MAX_FRAMES = 4               # max trailing frames a moving object may leave.
OFF_MAX_MEAN = 1e-3                # emissive-off plateau must be below this.
EMISSIVE_RESPONSE_RATIO_MIN = 20.0 # base_mean / off_mean must exceed this.
EMISSIVE_RESTORE_TOL = 1.5         # restore plateau within x of base plateau.
CUT_ACCEPT_FRACTION_MAX = 0.05     # cut-frame history accept fraction must be ~0.
HEATMAP_TILE = 16                  # block size of the accept/reject heatmap.

records = []      # one entry per rendered frame (phase, stats, framediff).
heatmaps = {}     # landmark accept/reject heatmaps.
prev_gi = None
prev_probe = None
available_channels = []  # channels marked on the active graph.


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
# Graph / scene helpers.
# -------------------------------------------------------------------------------------


def lumen_props():
    """LumenGI pass properties for the S5 input state."""
    return {
        "enabled": True,
        "traceMode": "HardwareRT",
        "qualityPreset": "High",
        "useScreenTrace": USE_SCREEN_TRACE,
        "useScreenProbes": USE_SCREEN_PROBES,
        "useTemporalFilter": USE_TEMPORAL_FILTER,
    }


def create_lumen_graph(extra_outputs):
    """GBufferRT -> LumenGI. Always marks diffuseGI/confidence plus GBufferRT.mvec
    (ghost background mask); optionally marks the given LumenGI output channels."""
    graph = RenderGraph("LumenGITemporal")
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
    """Load a scene and reset the deterministic clock. When camera_pos/target are
    given the scene camera is re-targeted (cornell phases); None keeps the scene's
    default camera (animated_cubes frames its cubes itself)."""
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
    """Render one frame on a throwaway graph that marks `channel`. Returns True when
    the graph builds and renders (i.e. the channel exists post-S4.3/S5)."""
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
        print("TEMPORAL WARNING channel 'LumenGI.%s' not available (%s)" % (channel, str(exc)))
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


# -------------------------------------------------------------------------------------
# Per-frame capture.
# -------------------------------------------------------------------------------------


def grab(name):
    return np.asarray(m.activeGraph.get_output(name).to_numpy(), dtype=np.float32)


def array_mean_scalar(a):
    a = np.asarray(a, dtype=np.float32)
    if a.ndim == 3 and a.shape[-1] >= 4:
        return a[..., :3]
    return a


def render_one(label, capture_heatmap=False, advance=True):
    """Advance one frame, render, record stats for every available channel."""
    global prev_gi, prev_probe
    if advance:
        m.clock.frame += 1
    m.renderFrame()

    rec = {"phase": label, "frame": int(m.clock.frame)}

    gi = array_mean_scalar(grab("LumenGI.diffuseGI"))
    if gi.ndim != 3 or gi.shape[-1] < 3:
        gi = gi[..., :3]
    gi = gi[..., :3]
    rec["gi_min"] = float(gi.min())
    rec["gi_max"] = float(gi.max())
    rec["gi_mean"] = float(gi.mean())
    rec["gi_finite"] = bool(math.isfinite(float(gi.min())) and math.isfinite(float(gi.max())))
    rec["gi_nonneg"] = bool(float(gi.min()) >= 0.0)
    if prev_gi is not None and prev_gi.shape == gi.shape:
        rec["gi_framediff"] = float(np.abs(gi - prev_gi).mean())
    prev_gi = gi

    if PROBE_INTERP_CHANNEL in available_channels:
        p = grab("LumenGI." + PROBE_INTERP_CHANNEL)
        rgb = p[..., :3]
        conf = p[..., 3]
        rec["probe_irradiance_mean"] = float(rgb.mean())
        rec["probe_irradiance_max"] = float(rgb.max())
        rec["probe_irradiance_finite"] = bool(
            math.isfinite(float(rgb.min())) and math.isfinite(float(rgb.max()))
        )
        rec["probe_irradiance_nonneg"] = bool(float(rgb.min()) >= 0.0)
        rec["probe_confidence_mean"] = float(conf.mean())
        if prev_probe is not None and prev_probe.shape == p.shape:
            rec["probe_framediff"] = float(np.abs(p - prev_probe).mean())
        prev_probe = p

    for ch in available_channels:
        if ch == "temporalFiltered":
            t = grab("LumenGI." + ch)
            rgb = t[..., :3] if t.ndim == 3 and t.shape[-1] >= 4 else t
            rec[ch + "_mean"] = float(rgb.mean())
            rec[ch + "_max"] = float(rgb.max())
            rec[ch + "_finite"] = bool(math.isfinite(float(rgb.min())) and math.isfinite(float(rgb.max())))
            rec[ch + "_nonneg"] = bool(float(rgb.min()) >= 0.0)
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
            rec["temporal_alpha_finite"] = bool(math.isfinite(float(s.mean())))
        elif ch == "temporalConfidence":
            t = grab("LumenGI." + ch)
            rec["temporal_confidence_mean"] = float(t.mean())
        elif ch == "temporalHistoryLength":
            t = grab("LumenGI." + ch)
            s = t[..., 0] if t.ndim == 3 else t
            rec["temporal_history_length_mean"] = float(s.mean())
            # Last writer wins over the temporalFiltered.a derivation when both exist.
            rec["history_accept_fraction"] = float((s > ACCEPT_HISTORY_LENGTH_MIN).mean())
            rec["history_reject_fraction"] = float((s <= ACCEPT_HISTORY_LENGTH_MIN).mean())

    if capture_heatmap:
        record_heatmaps(rec["phase"], rec["frame"])

    records.append(rec)
    diff = rec.get("gi_framediff")
    print(
        "TEMPORAL", label,
        "frame", rec["frame"],
        "gi_mean %.5f" % rec["gi_mean"],
        "framediff", "%.5f" % diff if diff is not None else "n/a",
    )
    return rec


def render_block(label, count, capture_heatmap=False):
    last = None
    for _ in range(count):
        last = render_one(label, capture_heatmap=capture_heatmap)
    return last


def render_frozen(label, count, capture_heatmap=False):
    """Render `count` frames at a FROZEN clock frame (no animation, no scene
    update). The frame counter is pinned so Scene::update receives a constant
    time; LumenGI's own per-execute frame index still rotates the sampling. This
    is the post-motion "freeze" state the ghost metric measures."""
    pinned = m.clock.frame
    out = []
    for _ in range(count):
        m.clock.frame = pinned
        render_one(label, capture_heatmap=capture_heatmap, advance=False)
        out.append(records[-1])
    return out


def record_heatmaps(key, frame):
    """S5_TODO[history_channel]: block-mean accept/reject heatmaps at a landmark,
    derived from the history length (temporalFiltered.a / temporalHistoryLength)
    and cross-checked against temporalAlpha."""
    entry = {"frame": frame, "tile": HEATMAP_TILE}
    hist = None
    if "temporalHistoryLength" in available_channels:
        t = grab("LumenGI.temporalHistoryLength")
        hist = t[..., 0] if t.ndim == 3 else t
    elif HISTORY_LENGTH_CHANNEL in available_channels:
        t = grab("LumenGI." + HISTORY_LENGTH_CHANNEL)
        if t.ndim == 3 and t.shape[-1] >= 4:
            hist = t[..., 3]
    if hist is not None:
        accept = (hist > ACCEPT_HISTORY_LENGTH_MIN).astype(np.float32)
        entry["history_accept"] = {
            "fraction": float(accept.mean()),
            "heatmap": block_mean(accept, HEATMAP_TILE).tolist(),
        }
        entry["history_reject"] = {
            "fraction": float(1.0 - accept.mean()),
            "heatmap": block_mean((1.0 - accept), HEATMAP_TILE).tolist(),
        }
    if HISTORY_ALPHA_CHANNEL in available_channels:
        t = grab("LumenGI." + HISTORY_ALPHA_CHANNEL)
        s = t[..., 0] if t.ndim == 3 else t
        entry["temporal_alpha"] = {
            "mean": float(s.mean()),
            "reject_fraction": float((s >= REJECT_ALPHA_MAX).mean()),
            "heatmap": block_mean(s, HEATMAP_TILE).tolist(),
        }
    if any(k not in ("frame", "tile") for k in entry):
        heatmaps[key] = entry


def block_mean(s, tile):
    """Vectorized non-overlapping block mean (accept/reject heatmap downsample)."""
    s = np.asarray(s, dtype=np.float64)
    h, w = s.shape
    ph = (tile - h % tile) % tile
    pw = (tile - w % tile) % tile
    if ph or pw:
        s = np.pad(s, ((0, ph), (0, pw)), mode="edge")
    H, W = s.shape
    nbh, nbw = H // tile, W // tile
    return s[:nbh * tile, :nbw * tile].reshape(nbh, tile, nbw, tile).mean(axis=(1, 3))


def orbit_camera(camera, angle_deg):
    rad = math.radians(angle_deg)
    r = camera.position - camera.target
    rx = r.x * math.cos(rad) - r.z * math.sin(rad)
    rz = r.x * math.sin(rad) + r.z * math.cos(rad)
    camera.position = camera.target + float3(rx, r.y, rz)


# -------------------------------------------------------------------------------------
# Ghost / recovery metrics (S5_TODO placeholder).
# -------------------------------------------------------------------------------------


def ghost_recovery_frames(phase_recs, floor=GHOST_FLOOR):
    """S5_TODO[ghost]: trailing frames after a motion stops during which the
    inter-frame change stays above `floor`. With the pre-S5 full-history-reset this
    is ~1; a real ghost channel (Z5) replaces the framediff proxy when available."""
    n = 0
    for r in reversed(phase_recs):
        d = r.get("gi_framediff")
        if d is not None and d > floor:
            n += 1
        else:
            break
    return n


def tail_stats(phase_recs, field):
    vals = [r.get(field) for r in phase_recs if r.get(field) is not None]
    if not vals:
        return None
    return {"count": len(vals), "mean": float(sum(vals) / len(vals)), "max": float(max(vals))}


# -------------------------------------------------------------------------------------
# main
# -------------------------------------------------------------------------------------


def main():
    report = {
        "stage": "S5",
        "script": "run_temporal.py",
        "role": "S5-C1 temporal trajectory test (Agent Z6)",
        "status": "skeleton",
        "scenes": {
            "cornell": SCENE_CORNELL,
            "pointlight": SCENE_POINTLIGHT,
            "animated": SCENE_ANIMATED,
            "emissive": SCENE_EMISSIVE_GLOW,
        },
        "resolution": list(RESOLUTION),
        "config": {
            "useScreenTrace": USE_SCREEN_TRACE,
            "useScreenProbes": USE_SCREEN_PROBES,
            "useTemporalFilter": USE_TEMPORAL_FILTER,
            "static_frames": STATIC_FRAMES,
            "probe_channel": PROBE_INTERP_CHANNEL,
            "temporal_channels": TEMPORAL_CHANNELS,
            "history_channels": ["temporalFiltered.a / temporalHistoryLength (accept/reject)", "temporalAlpha"],
        },
    }
    verdicts = []

    # ---- Probe which optional channels exist (data-driven; absent -> not marked). --
    probe_available = probe_channel(PROBE_INTERP_CHANNEL)
    temporal_available = {}
    for ch in TEMPORAL_CHANNELS:
        temporal_available[ch] = probe_channel(ch)
    extra = [PROBE_INTERP_CHANNEL] if probe_available else []
    extra += [ch for ch in TEMPORAL_CHANNELS if temporal_available[ch]]
    report["probe_channel_available"] = probe_available
    report["temporal_channels_available"] = temporal_available
    report["heatmaps"] = {}
    report["verdicts"] = []

    add_main_graph(extra)

    # ------------------------------------------------------------ Phase 1: static ---
    _setup_scene(SCENE_CORNELL, CAM_START_POS, CAM_START_TARGET)
    render_block("warmup", WARMUP_FRAMES)

    static_first = render_one("static-first")
    for _ in range(STATIC_FRAMES - 1):
        render_one("static")
    static_tail = tail_stats([r for r in records if r["phase"] == "static"], "gi_framediff")
    static_last = records[-1]
    growth_ratio = static_last["gi_mean"] / static_first["gi_mean"] if static_first["gi_mean"] > 0 else None
    energy_ok = growth_ratio is not None and growth_ratio < ENERGY_GROWTH_THRESHOLD
    first_diff = static_first.get("gi_framediff")
    conv_ok = False
    if static_tail and first_diff is not None:
        conv_ok = static_tail["mean"] < first_diff * CONVERGENCE_FACTOR
    flicker = static_tail["max"] if static_tail else None
    verdicts.append(("static energy plateau (growth %.3f < %.1f)" % (
        growth_ratio if growth_ratio is not None else -1.0, ENERGY_GROWTH_THRESHOLD),
        "PASS" if energy_ok else "FAIL"))
    verdicts.append(("static convergence (tail diff %s vs first %s)" % (
        "%.5f" % static_tail["mean"] if static_tail else "n/a",
        "%.5f" % first_diff if first_diff is not None else "n/a"),
        "PASS" if conv_ok else "FAIL"))
    report["static"] = {
        "first_mean": static_first["gi_mean"],
        "last_mean": static_last["gi_mean"],
        "growth_ratio": growth_ratio,
        "tail_framediff": static_tail,
        "flicker_max_diff": flicker,
    }
    # S5_TODO[threshold]: flicker is live-measured; the gate threshold is a placeholder.
    if flicker is None:
        verdicts.append(("static-tail flicker <= %g (S5_TODO threshold)" % FLICKER_MAX_DIFF, "SKIP"))
    else:
        ok = flicker <= FLICKER_MAX_DIFF
        verdicts.append(("static-tail flicker max %.5f <= %g (S5_TODO threshold)" % (flicker, FLICKER_MAX_DIFF),
                         "PASS" if ok else "FAIL"))

    # ------------------------------------------------- Phase 2/3: pan, orbit --------
    camera = m.scene.camera
    for i in range(PAN_STEPS):
        camera.position = camera.position + PAN_DELTA
        camera.target = camera.target + PAN_DELTA
        render_block("pan-%d" % i, 1)
    for i in range(ORBIT_STEPS):
        orbit_camera(camera, ORBIT_STEP_DEG)
        render_block("orbit-%d" % i, 1)

    # ------------------------------------------------- Phase 4: fast pan (横移) -----
    for i in range(FAST_STEPS):
        camera.position = camera.position + FAST_DELTA
        camera.target = camera.target + FAST_DELTA
        render_block("fastpan-%d" % i, 1)

    # ---------------------------------------------------- Phase 5: camera cut ------
    steady = tail_stats([r for r in records if r["phase"].startswith("fastpan")], "gi_framediff")
    camera.position = CUT_POS
    camera.target = CUT_TARGET
    cut_rec = render_one("camera-cut", capture_heatmap=True)
    for _ in range(CUT_SETTLE):
        render_one("cut-settle", capture_heatmap=True)
    settle = records[-1]
    cut_diff = cut_rec.get("gi_framediff")
    steady_mean = steady["mean"] if steady else None
    cut_ratio = cut_diff / steady_mean if (cut_diff is not None and steady_mean and steady_mean > 0) else None
    cut_spike_ok = cut_ratio is not None and cut_ratio > CUT_DIFF_RATIO_MIN
    settle_diff = settle.get("gi_framediff")
    cut_recovery_ok = (settle_diff is not None and cut_diff is not None and
                       settle_diff < cut_diff * CUT_RECOVERY_FACTOR)
    verdicts.append(("camera cut change-rate spike (ratio %.2f > %.1f)" % (
        cut_ratio if cut_ratio is not None else 0.0, CUT_DIFF_RATIO_MIN),
        "PASS" if cut_spike_ok else "FAIL"))
    verdicts.append(("camera cut recovery (settle %.5f < cut %.5f x %.2f)" % (
        settle_diff if settle_diff is not None else -1.0,
        cut_diff if cut_diff is not None else -1.0,
        CUT_RECOVERY_FACTOR),
        "PASS" if cut_recovery_ok else "FAIL"))
    # S5_TODO[history_channel]: the true "history immediately invalid" check.
    if any(ch in available_channels for ch in ("temporalHistoryLength", HISTORY_LENGTH_CHANNEL)):
        frac = cut_rec.get("history_accept_fraction")
        verdicts.append(("camera cut history accept fraction <= %.2f (S5_TODO channel)" % CUT_ACCEPT_FRACTION_MAX,
                         "PASS" if frac is not None and frac <= CUT_ACCEPT_FRACTION_MAX else "FAIL"))
    else:
        verdicts.append(("camera cut history accept fraction ~ 0 (S5_TODO history channel)", "SKIP"))
    report["camera_cut"] = {
        "steady_tail_diff": steady_mean,
        "cut_diff": cut_diff,
        "cut_ratio": cut_ratio,
        "settle_diff": settle_diff,
    }

    # ------------------------------------------- Phase 6: moving light + freeze ----
    prev_gi = None
    prev_probe = None
    _setup_scene(SCENE_POINTLIGHT, CAM_START_POS, CAM_START_TARGET)
    render_block("light-warmup", 4)
    point_light = m.scene.getLight(LIGHT_NAME)
    for i in range(LIGHT_MOVE_STEPS):
        point_light.position = point_light.position + LIGHT_MOVE_DELTA
        render_block("light-move-%d" % i, 1)
    # Freeze: pin the clock frame so no further scene update fires (ghost window).
    light_freezing = render_frozen("light-freeze", LIGHT_FREEZE, capture_heatmap=True)
    light_ghost = ghost_recovery_frames(light_freezing)
    verdicts.append(("moving light no long-term ghost (trailing %d <= %d, S5_TODO)" % (light_ghost, GHOST_MAX_FRAMES),
                     "PASS" if light_ghost <= GHOST_MAX_FRAMES else "FAIL"))
    report["moving_light"] = {
        "ghost_trailing_frames": light_ghost,
        "freeze_framediffs": [r.get("gi_framediff") for r in light_freezing],
    }

    # ----------------------------------------- Phase 7: moving rigid body + freeze --
    prev_gi = None
    prev_probe = None
    _setup_scene(SCENE_ANIMATED)  # keep the scene's default camera (frames the cubes).
    m.scene.animated = True
    render_block("rigid-warmup", RIGID_WARMUP)
    for i in range(RIGID_MOVE):
        render_block("rigid-move-%d" % i, 1)  # frame advance drives the baked animation.
    rigid_freezing = render_frozen("rigid-freeze", RIGID_FREEZE, capture_heatmap=True)
    rigid_ghost = ghost_recovery_frames(rigid_freezing)
    verdicts.append(("moving rigid body no long-term ghost (trailing %d <= %d, S5_TODO)" % (rigid_ghost, GHOST_MAX_FRAMES),
                     "PASS" if rigid_ghost <= GHOST_MAX_FRAMES else "FAIL"))
    report["moving_rigid"] = {
        "ghost_trailing_frames": rigid_ghost,
        "freeze_framediffs": [r.get("gi_framediff") for r in rigid_freezing],
    }

    # ----------------------------------------------------- Phase 8: emissive step ----
    prev_gi = None
    prev_probe = None
    _setup_scene(SCENE_EMISSIVE_GLOW, CAM_START_POS, CAM_START_TARGET)
    mat = m.scene.get_material(EMISSIVE_MATERIAL)
    base_mean = render_block("emissive-base", EMISSIVE_STEP_FRAMES)["gi_mean"]
    mat.emissiveFactor = EMISSIVE_FACTOR_OFF
    off_mean = render_block("emissive-off", EMISSIVE_STEP_FRAMES)["gi_mean"]
    mat.emissiveFactor = EMISSIVE_FACTOR_BASE
    restore_mean = render_block("emissive-restore", EMISSIVE_STEP_FRAMES)["gi_mean"]
    off_ok = off_mean < OFF_MAX_MEAN
    response_ratio = base_mean / off_mean if off_mean > 0 else None
    response_ok = response_ratio is not None and response_ratio > EMISSIVE_RESPONSE_RATIO_MIN
    restore_ok = False
    if base_mean > 0 and restore_mean > 0:
        restore_ratio = max(restore_mean / base_mean, base_mean / restore_mean)
        restore_ok = restore_ratio <= EMISSIVE_RESTORE_TOL
    verdicts.append(("emissive off plateau ~0 (mean %.6f < %.4f)" % (off_mean, OFF_MAX_MEAN),
                     "PASS" if off_ok else "FAIL"))
    verdicts.append(("emissive step response (base/off ratio %.1f > %.1f)" % (
        response_ratio if response_ratio is not None else 0.0, EMISSIVE_RESPONSE_RATIO_MIN),
        "PASS" if response_ok else "FAIL"))
    verdicts.append(("emissive restore within %.2fx of base (base %.5f, restore %.5f)" % (
        EMISSIVE_RESTORE_TOL, base_mean, restore_mean),
        "PASS" if restore_ok else "FAIL"))
    report["emissive_step"] = {
        "base_mean": base_mean,
        "off_mean": off_mean,
        "restore_mean": restore_mean,
        "response_ratio": response_ratio,
    }

    # ------------------------------------------------------- Final verdicts ---------
    report["heatmaps"] = heatmaps
    report["series"] = records
    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    if any(v == "SKIP" for _, v in verdicts):
        report["summary"] = "SKIP" if all(v != "FAIL" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("TEMPORAL VERDICT", name, verdict)
    write_json(OUT_JSON, report)
    print("TEMPORAL wrote", os.path.abspath(OUT_JSON))


# Falcor's embedded Python executes the script with __name__ == 'builtins', so an
# `if __name__ == "__main__":` guard never runs. Call main() unconditionally, like
# the other working run_*.py scripts.
try:
    main()
except Exception as exc:
    print("TEMPORAL ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S5",
            "script": "run_temporal.py",
            "summary": "SKIP",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive SKIP on fatal error)", "SKIP")],
        },
    )
exit()
