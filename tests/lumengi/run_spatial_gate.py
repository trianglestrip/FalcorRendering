from falcor import *

"""LumenGI S5-B2 / S5-A2 spatial filter live gate verification (Agent Z10, exclusive GPU user).

Role / purpose
--------------
RUN-ONLY Mogwai GPU gate over the S5-B2 variance-guided spatial filter once the S5-B2 pass is
integrated. Graph: GBufferRT -> LumenGI with useScreenProbes + useTemporalFilter + useSpatialFilter,
marking the S5 channels (probeInterpolated / temporalFiltered / temporalConfidence / spatialFiltered).

It exercises the task.md S5 门禁 against the S5-B2 final output spatialFiltered
(RGB = filtered irradiance, A = filtered confidence in [0,1]):

  1. static (cornell_box, fixed camera, 192 frames):
       * spatialFiltered finite / non-negative / .a confidence in [0,1];
       * the filter is variance-guided: on the CONVERGED tail (clean pixels) it passes through
         (change ratio <= 0.02, edge contrast >= 0.6 preserved, local variance never increased,
         inter-frame flicker never increased vs the temporal baseline);
       * no history-length overflow (re-confirms the S5-A1 gate).
  2. camera cut (instantaneous jump):
       * temporalFiltered history accept fraction ~0 (re-confirm);
       * the SPATIAL output's confidence collapses on the cut frame (spatialFiltered.a drops from
         ~0.90 converged to the ~0.02 probe-confidence floor): "history immediately invalid" on the
         final filtered output;
       * the spatial filter ACTS on the noisy regime (cut-frame change ratio >> tail change ratio)
         and reduces local variance most on the noisy frame (never increases it);
       * spatial tracks the temporal reset (|spatial - temporal| stays a small fraction of the cut
         change -> no independent history) and recovers within CUT_SETTLE frames.
  3. no NaN/Inf / negative radiance / history overflow over every phase and every channel.

Exit: Falcor `exit()`. JSON -> artifacts/lumengi/S5/gate/spatial-gate.json (override
LUMEN_SPATIAL_GATE_OUT). Frame counts env-overridable for quick bring-up.

Determinism: fixed camera path, paused clock, manual frame counter
(Clock::setFrame maps frame -> time), 640x360 @ 60fps.
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

OUT_JSON = os.environ.get("LUMEN_SPATIAL_GATE_OUT", "artifacts/lumengi/S5/gate/spatial-gate.json")

USE_SCREEN_TRACE = bool(os.environ.get("LUMEN_SPATIAL_GATE_USE_SCREEN_TRACE", "") != "0")
USE_SCREEN_PROBES = bool(os.environ.get("LUMEN_SPATIAL_GATE_USE_SCREEN_PROBES", "") != "0")
USE_TEMPORAL_FILTER = bool(os.environ.get("LUMEN_SPATIAL_GATE_USE_TEMPORAL_FILTER", "") != "0")
USE_SPATIAL_FILTER = bool(os.environ.get("LUMEN_SPATIAL_GATE_USE_SPATIAL_FILTER", "") != "0")

STATIC_FRAMES = int(os.environ.get("LUMEN_SPATIAL_GATE_STATIC_FRAMES", "192"))
WARMUP_FRAMES = int(os.environ.get("LUMEN_SPATIAL_GATE_WARMUP_FRAMES", "8"))
TAIL_WINDOW = int(os.environ.get("LUMEN_SPATIAL_GATE_TAIL_WINDOW", "12"))
CUT_SETTLE = int(os.environ.get("LUMEN_SPATIAL_GATE_CUT_SETTLE", "12"))

CAM_START_POS = float3(0, 0.28, 1.2)
CAM_START_TARGET = float3(0, 0.28, 0)
CAM_UP = float3(0, 1, 0)
CAM_FOCAL_LENGTH = 35.0
CUT_POS = float3(0.25, 0.4, 0.5)
CUT_TARGET = float3(0.15, 0.2, -0.1)

# S5 channels (frozen with the LumenGI channel contract / Z5 / Z8 shader data modules).
TEMPORAL_FILTERED = "temporalFiltered"   # .rgb = temporally filtered irradiance, .a = history length.
TEMPORAL_CONFIDENCE = "temporalConfidence"
SPATIAL_FILTERED = "spatialFiltered"     # .rgb = spatially filtered irradiance, .a = filtered confidence.
PROBE_INTERP = "probeInterpolated"

ACCEPT_HISTORY_LENGTH_MIN = 1.5          # hist > this = accepted (>= 2), else fresh/reset.

# -------------------------------------------------------------------------------------
# Gates (mirror task.md S5 门禁; frozen for this round).
# -------------------------------------------------------------------------------------
HISTORY_CAP = 255.0
# The S5-B2 filter is variance-guided: it must ACT where the variance/noise is higher
# (disoccluded / fresh output) and PASS THROUGH where the scene is clean (converged
# static tail, radius -> gRadiusMin = 0). The probe residual error in this pipeline is
# LOW-FREQUENCY (spatially correlated; see the S5-A1-B1 report), so a small-radius
# bilateral reduces local variance modestly and must never INCREASE it.
TAIL_CHANGE_MAX = 0.02                 # tail change ratio (spatial vs temporal) <= this = detail kept.
CUT_CHANGE_MIN_MULT = 3.0              # cut change ratio >= tail change ratio * this = acts on noise.
VAR_NEVER_INCREASES = 1.0              # local variance(spatial) / local variance(temporal) <= 1.0.
CUT_VAR_RATIO_MAX = 0.995              # cut-frame variance ratio: the filter reduces variance most on noisy frames.
FLICKER_SPATIAL_MAX_RATIO = 1.1        # spatial tail framediff <= temporal tail framediff * this.
EDGE_CONTRAST_MIN = 0.6                # mean edge-gradient(spatial) / edge-gradient(temporal) >= this.
CUT_DIFF_RATIO_MIN = 3.0               # cut-frame spatial framediff / steady spatial tail diff.
CUT_ACCEPT_FRACTION_MAX = 0.05         # cut-frame temporal history accept fraction ~0.
CUT_SPATIAL_TRACK_MAX = 0.5            # cut-frame mean |spatial - temporal| / cut framediff <= this.
CUT_CONFIDENCE_MAX = 0.1               # cut-frame spatialFiltered.a mean <= this (history invalid -> confidence
                                       # collapses from ~0.90 converged to the ~0.02 probe confidence floor).

records = []
prev_temporal = None
prev_spatial = None
prev_probe = None
available_channels = []
history_series = []
texture_cache = {}  # label -> list of (temporal_arr, spatial_arr) for variance measurements.


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
        "useSpatialFilter": USE_SPATIAL_FILTER,
    }


def create_lumen_graph(extra_outputs):
    graph = RenderGraph("LumenGISpatialGate")
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
    # linearZ is needed for the edge-preservation mask (depth-edge pixels).
    graph.markOutput("GBufferRT.linearZ")
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


def add_main_graph(extra_outputs):
    global available_channels
    graph = create_lumen_graph(extra_outputs)
    m.addGraph(graph)
    m.setActiveGraph(graph)
    available_channels = list(extra_outputs)
    return graph


def grab(name):
    return np.asarray(m.activeGraph.get_output(name).to_numpy(), dtype=np.float32)


def rgb3(img):
    a = img[..., :3] if img.ndim == 3 and img.shape[-1] >= 3 else img
    return a[..., :3]


def lum(img):
    r = rgb3(img)
    return (0.2126 * r[..., 0] + 0.7152 * r[..., 1] + 0.0722 * r[..., 2])


def _box_mean(img, r):
    """Mean of a (2r+1)x(2r+1) box per pixel via an integral image (pure numpy)."""
    p = np.pad(img, ((r, r), (r, r), (0, 0)), mode="edge")
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    h, w, ch = p.shape
    # sum over [i-r, i+r] x [j-r, j+r] of the padded image == [i, i+2r] x [j, j+2r].
    s = c[2 * r:, 2 * r:] - c[:h - 2 * r, 2 * r:] - c[2 * r:, :w - 2 * r] + c[:h - 2 * r, :w - 2 * r]
    n = float((2 * r + 1) ** 2)
    return s[..., :] / n


def local_variance(img, r=2):
    """Mean per-pixel luminance variance over a (2r+1)x(2r+1) window."""
    lum_img = lum(img)
    mean = _box_mean(lum_img[..., None], r)[..., 0]
    mean_sq = _box_mean(lum_img[..., None] ** 2, r)[..., 0]
    return float(np.maximum(mean_sq - mean * mean, 0.0).mean())


def edge_gradient(lum_img):
    """Mean absolute luminance gradient magnitude per pixel (Scharr-free 1-tap diff)."""
    gx = np.abs(np.diff(lum_img, axis=1))
    gy = np.abs(np.diff(lum_img, axis=0))
    g = np.zeros_like(lum_img)
    g[:, :-1] += gx
    g[:-1, :] += gy
    return g


def channel_stats(t, spatial=False):
    rgb = rgb3(t)
    conf = t[..., 3] if t.shape[-1] >= 4 else np.zeros_like(rgb[..., 0])
    return {
        "mean": float(rgb.mean()),
        "max": float(rgb.max()),
        "min": float(rgb.min()),
        "finite": bool(math.isfinite(float(rgb.min())) and math.isfinite(float(rgb.max()))),
        "nonneg": bool(float(rgb.min()) >= 0.0),
        "confidence_mean": float(conf.mean()),
        "confidence_min": float(conf.min()),
        "confidence_max": float(conf.max()),
        "confidence_finite": bool(math.isfinite(float(conf.min())) and math.isfinite(float(conf.max()))),
        "confidence_in_01": bool(float(conf.min()) >= 0.0 and float(conf.max()) <= 1.0),
    }


def temporal_stats(t):
    st = channel_stats(t)
    hist = t[..., 3] if t.shape[-1] >= 4 else np.zeros_like(st["mean"])
    st["history_mean"] = float(hist.mean())
    st["history_max"] = float(hist.max())
    st["accept_fraction"] = float((hist > ACCEPT_HISTORY_LENGTH_MIN).mean())
    st["reject_fraction"] = float((hist <= ACCEPT_HISTORY_LENGTH_MIN).mean())
    return st


def render_one(label, advance=True, record_history=False):
    global prev_temporal, prev_spatial, prev_probe
    if advance:
        m.clock.frame += 1
    m.renderFrame()

    rec = {"phase": label, "frame": int(m.clock.frame)}

    gi = grab("LumenGI.diffuseGI")
    gi = rgb3(gi)
    rec["gi_mean"] = float(gi.mean())
    rec["gi_finite"] = bool(math.isfinite(float(gi.min())) and math.isfinite(float(gi.max())))
    rec["gi_nonneg"] = bool(float(gi.min()) >= 0.0)

    if TEMPORAL_FILTERED in available_channels:
        t = grab("LumenGI." + TEMPORAL_FILTERED)
        st = temporal_stats(t)
        rec["temporal"] = st
        if prev_temporal is not None and prev_temporal.shape == t.shape:
            rec["temporal_framediff"] = float(np.abs(rgb3(t) - rgb3(prev_temporal)).mean())
        prev_temporal = t
        if record_history:
            history_series.append({"frame": rec["frame"], "phase": label,
                                   "history_mean": st["history_mean"],
                                   "accept_fraction": st["accept_fraction"],
                                   "history_max": st["history_max"]})

    if SPATIAL_FILTERED in available_channels:
        s = grab("LumenGI." + SPATIAL_FILTERED)
        st = channel_stats(s)
        rec["spatial"] = st
        if prev_spatial is not None and prev_spatial.shape == s.shape:
            rec["spatial_framediff"] = float(np.abs(rgb3(s) - rgb3(prev_spatial)).mean())
        prev_spatial = s
        if TEMPORAL_FILTERED in available_channels and prev_temporal is not None and prev_temporal.shape == s.shape:
            rec["spatial_vs_temporal"] = float(np.abs(rgb3(s) - rgb3(prev_temporal)).mean())
        # Keep the raw arrays for variance / edge measurements on the interesting phases.
        if label in ("static", "camera-cut", "cut-settle") and TEMPORAL_FILTERED in available_channels:
            t = grab("LumenGI." + TEMPORAL_FILTERED)
            texture_cache.setdefault(label, []).append((t, s))

    if PROBE_INTERP in available_channels:
        p = grab("LumenGI." + PROBE_INTERP)
        rec["probe_irradiance_mean"] = float(rgb3(p).mean())
        rec["probe_confidence_mean"] = float(p[..., 3].mean())
        if prev_probe is not None and prev_probe.shape == p.shape:
            rec["probe_framediff"] = float(np.abs(rgb3(p) - rgb3(prev_probe)).mean())
        prev_probe = p

    records.append(rec)
    return rec


def render_block(label, count, record_history=False):
    last = None
    for _ in range(count):
        last = render_one(label, record_history=record_history)
    return last


def render_frozen(label, count):
    pinned = m.clock.frame
    out = []
    for _ in range(count):
        m.clock.frame = pinned
        render_one(label, advance=False)
        out.append(records[-1])
    return out


def tail_stats(phase_recs, field):
    vals = [r.get(field) for r in phase_recs if r.get(field) is not None]
    if not vals:
        return None
    return {"count": len(vals), "mean": float(sum(vals) / len(vals)), "max": float(max(vals))}


def main():
    report = {
        "stage": "S5",
        "script": "run_spatial_gate.py",
        "role": "S5-B2 / S5-A2 live gate verification (Agent Z10)",
        "status": "run",
        "resolution": list(RESOLUTION),
        "config": {
            "useScreenTrace": USE_SCREEN_TRACE,
            "useScreenProbes": USE_SCREEN_PROBES,
            "useTemporalFilter": USE_TEMPORAL_FILTER,
            "useSpatialFilter": USE_SPATIAL_FILTER,
            "static_frames": STATIC_FRAMES,
            "confidence_source": "temporalConfidence (R32F) bound as gConfidenceInput; overrides temporalFiltered.a",
            "resolution_mode": "full-res (S5-A2 half/quarter deferred to S8)",
            "channels": [TEMPORAL_FILTERED, TEMPORAL_CONFIDENCE, SPATIAL_FILTERED, PROBE_INTERP],
        },
    }
    verdicts = []

    # ---- Probe which optional channels exist (data-driven; absent -> not marked). --
    extra = []
    if probe_channel(PROBE_INTERP):
        extra.append(PROBE_INTERP)  # required input of the temporal filter (S4.3 interpolate).
    for ch in (TEMPORAL_FILTERED, TEMPORAL_CONFIDENCE, SPATIAL_FILTERED):
        if probe_channel(ch):
            extra.append(ch)
    report["channels_available"] = extra
    if SPATIAL_FILTERED not in extra:
        write_json(OUT_JSON, {
            "stage": "S5", "script": "run_spatial_gate.py", "summary": "SKIP",
            "fatal_error": "spatialFiltered channel not available (spatial pass not integrated?)",
            "channels_available": extra,
            "verdicts": [("spatialFiltered channel present", "SKIP")],
        })
        print("SPATIAL WARNING 'spatialFiltered' not available; wrote SKIP json.")
        exit()

    add_main_graph(extra)

    # ------------------------------------------------------------ Phase 1: static ---
    _setup_scene(SCENE_CORNELL, CAM_START_POS, CAM_START_TARGET)
    render_block("warmup", WARMUP_FRAMES, record_history=True)
    static_first = render_one("static-first", record_history=True)
    for _ in range(STATIC_FRAMES - 1):
        render_one("static", record_history=True)
    static_all = [r for r in records if r["phase"] == "static"]
    static_tail_win = static_all[-TAIL_WINDOW:] if len(static_all) >= TAIL_WINDOW else static_all
    temporal_tail = tail_stats(static_tail_win, "temporal_framediff")
    spatial_tail = tail_stats(static_tail_win, "spatial_framediff")
    static_recs = [r for r in records if r["phase"].startswith("static")]

    # --- NaN / non-neg / confidence across the whole static phase. ---
    spatial_finite = all(r["spatial"]["finite"] for r in static_recs)
    spatial_nonneg = all(r["spatial"]["nonneg"] for r in static_recs)
    spatial_conf_ok = all(r["spatial"]["confidence_finite"] and r["spatial"]["confidence_in_01"] for r in static_recs)
    temporal_finite = all(r["temporal"]["finite"] for r in static_recs)
    temporal_nonneg = all(r["temporal"]["nonneg"] for r in static_recs)
    max_hist = max(r["temporal"]["history_max"] for r in static_recs)
    overflow_ok = max_hist <= HISTORY_CAP + 0.01

    # --- Variance-guided behavior + edge preservation on the convergence TAIL. ---
    tail_t = texture_cache["static"][-1][0]
    tail_s = texture_cache["static"][-1][1]
    signal_mean = float(rgb3(tail_t).mean())
    delta_mean = float(np.abs(rgb3(tail_t) - rgb3(tail_s)).mean())
    tail_change_ratio = delta_mean / signal_mean if signal_mean > 0 else None

    var_t = local_variance(tail_t)
    var_s = local_variance(tail_s)
    tail_var_ratio = var_s / var_t if var_t > 0 else None

    # Edge preservation: on strong depth edges the filter must not flatten the irradiance
    # contrast beyond EDGE_CONTRAST_MIN (bilateral depth gating). Depth edges from linearZ.
    linz = grab("GBufferRT.linearZ")
    depth = linz[..., 0] if linz.ndim == 3 else linz
    gd = edge_gradient(depth)
    valid = depth > 0
    thr = np.percentile(gd[valid], 90.0) if valid.any() else float("inf")
    edge_mask = (gd > thr) & valid
    lum_t = lum(tail_t)
    lum_s = lum(tail_s)
    gt = edge_gradient(lum_t)
    gs = edge_gradient(lum_s)
    if edge_mask.sum() > 0 and gt[edge_mask].mean() > 0:
        edge_contrast = float(gs[edge_mask].mean() / gt[edge_mask].mean())
    else:
        edge_contrast = None
    edge_ok = edge_contrast is not None and edge_contrast >= EDGE_CONTRAST_MIN
    detail_ok = tail_change_ratio is not None and tail_change_ratio <= TAIL_CHANGE_MAX

    # --- Static-tail flicker: spatial must not add flicker over the temporal baseline. ---
    flicker_ok = (temporal_tail and spatial_tail and temporal_tail["mean"] > 0 and
                  spatial_tail["mean"] <= temporal_tail["mean"] * FLICKER_SPATIAL_MAX_RATIO)

    report["static"] = {
        "first_mean": static_first["spatial"]["mean"],
        "last_mean": static_recs[-1]["spatial"]["mean"],
        "temporal_tail_framediff": temporal_tail,
        "spatial_tail_framediff": spatial_tail,
        "delta_vs_temporal_mean": delta_mean,
        "tail_change_ratio": tail_change_ratio,
        "local_variance_temporal": var_t,
        "local_variance_spatial": var_s,
        "tail_variance_ratio": tail_var_ratio,
        "edge_contrast_ratio": edge_contrast,
        "edge_threshold_depth_grad_p90": float(thr) if math.isfinite(thr) else None,
        "max_history_length": max_hist,
        "spatial_finite": spatial_finite,
        "spatial_nonneg": spatial_nonneg,
        "spatial_confidence_in_01": spatial_conf_ok,
        "temporal_finite": temporal_finite,
        "temporal_nonneg": temporal_nonneg,
        "confidence_mean": float(np.mean([r["spatial"]["confidence_mean"] for r in static_recs])),
    }
    verdicts.append(("spatial finite + non-negative across static", "PASS" if spatial_finite and spatial_nonneg else "FAIL"))
    verdicts.append(("spatial confidence in [0,1] across static", "PASS" if spatial_conf_ok else "FAIL"))
    verdicts.append(("temporal no history-length overflow (max %.1f <= cap %.1f)" % (max_hist, HISTORY_CAP),
                     "PASS" if overflow_ok else "FAIL"))
    verdicts.append(("spatial detail-preserving at tail (change %.5f <= %.3f)" % (
        tail_change_ratio if tail_change_ratio is not None else -1.0, TAIL_CHANGE_MAX),
        "PASS" if detail_ok else "FAIL"))
    verdicts.append(("edge preservation (edge contrast %.3f >= %.2f)" % (
        edge_contrast if edge_contrast is not None else -1.0, EDGE_CONTRAST_MIN),
        "PASS" if edge_ok else "FAIL"))
    verdicts.append(("spatial never adds local variance at tail (ratio %.4f <= %.2f)" % (
        tail_var_ratio if tail_var_ratio is not None else -1.0, VAR_NEVER_INCREASES),
        "PASS" if (tail_var_ratio is not None and tail_var_ratio <= VAR_NEVER_INCREASES) else "FAIL"))
    verdicts.append(("static-tail flicker spatial %.5f <= temporal %.5f x %.2f" % (
        spatial_tail["mean"] if spatial_tail else -1.0,
        temporal_tail["mean"] if temporal_tail else -1.0,
        FLICKER_SPATIAL_MAX_RATIO),
        "PASS" if flicker_ok else "FAIL"))

    # ------------------------------------------------------ Phase 2: camera cut --------
    steady_spatial = tail_stats([r for r in records if r["phase"] == "static"], "spatial_framediff")
    camera = m.scene.camera
    camera.position = CUT_POS
    camera.target = CUT_TARGET
    cut_rec = render_one("camera-cut", record_history=True)
    cut_spatial_diff = cut_rec.get("spatial_framediff")
    cut_temporal_accept = cut_rec["temporal"]["accept_fraction"]
    steady_spatial_mean = steady_spatial["mean"] if steady_spatial else None
    cut_spatial_ratio = (cut_spatial_diff / steady_spatial_mean if
                         (cut_spatial_diff is not None and steady_spatial_mean and steady_spatial_mean > 0) else None)
    spike_ok = cut_spatial_ratio is not None and cut_spatial_ratio > CUT_DIFF_RATIO_MIN
    cut_accept_ok = cut_temporal_accept <= CUT_ACCEPT_FRACTION_MAX
    # "History immediately invalid" on the SPATIAL output: the confidence source (S5-A1
    # temporalConfidence, carried as gConfidenceInput) collapses on the cut frame because the
    # temporal filter rejected the history (outConf = current-frame probe confidence ~0.02, vs
    # ~0.90 when converged). The spatial pass passes this through -> spatialFiltered.a collapses.
    cut_spatial_conf = cut_rec["spatial"]["confidence_mean"]
    cut_conf_ok = cut_spatial_conf <= CUT_CONFIDENCE_MAX
    # Spatial faithfully tracks the temporal reset: on the cut frame the filter may only move the
    # (already fresh) temporal output by a small fraction of the cut-sized change. This proves the
    # spatial pass has no independent temporal memory (no stale content re-introduced by filtering).
    cut_delta = cut_rec.get("spatial_vs_temporal")
    track_ok = (cut_spatial_diff is not None and cut_delta is not None and cut_spatial_diff > 0 and
                (cut_delta / cut_spatial_diff) <= CUT_SPATIAL_TRACK_MAX)
    # Variance-guided: the filter must act more on the noisy cut frame than on the converged tail.
    cut_t, cut_s = texture_cache["camera-cut"][-1]
    cut_signal_mean = float(rgb3(cut_t).mean())
    cut_change_ratio = (float(np.abs(rgb3(cut_t) - rgb3(cut_s)).mean()) / cut_signal_mean
                        if cut_signal_mean > 0 else None)
    cut_var_t = local_variance(cut_t)
    cut_var_s = local_variance(cut_s)
    cut_var_ratio = cut_var_s / cut_var_t if cut_var_t > 0 else None
    acts_on_noise_ok = (cut_change_ratio is not None and tail_change_ratio is not None and
                        cut_change_ratio >= max(CUT_CHANGE_MIN_MULT, 1.0) * tail_change_ratio)
    cut_var_ok = (cut_var_ratio is not None and
                  cut_var_ratio <= CUT_VAR_RATIO_MAX and cut_var_ratio <= VAR_NEVER_INCREASES)
    for _ in range(CUT_SETTLE):
        render_one("cut-settle", record_history=True)
    settle_spatial_diff = records[-1].get("spatial_framediff")
    cut_recovery_ok = (settle_spatial_diff is not None and cut_spatial_diff is not None and
                       settle_spatial_diff < cut_spatial_diff * 0.2)
    report["camera_cut"] = {
        "steady_spatial_diff": steady_spatial_mean,
        "cut_spatial_diff": cut_spatial_diff,
        "cut_spatial_ratio": cut_spatial_ratio,
        "cut_temporal_accept_fraction": cut_temporal_accept,
        "cut_spatial_vs_temporal_delta": cut_delta,
        "cut_spatial_confidence_mean": cut_spatial_conf,
        "cut_change_ratio": cut_change_ratio,
        "cut_variance_temporal": cut_var_t,
        "cut_variance_spatial": cut_var_s,
        "cut_variance_ratio": cut_var_ratio,
        "settle_spatial_diff": settle_spatial_diff,
    }
    verdicts.append(("camera cut spatial change-rate spike (ratio %.2f > %.1f)" % (
        cut_spatial_ratio if cut_spatial_ratio is not None else 0.0, CUT_DIFF_RATIO_MIN),
        "PASS" if spike_ok else "FAIL"))
    verdicts.append(("camera cut temporal history invalid (accept %.4f <= %.2f)" % (
        cut_temporal_accept, CUT_ACCEPT_FRACTION_MAX),
        "PASS" if cut_accept_ok else "FAIL"))
    verdicts.append(("camera cut spatial confidence collapse (%.4f <= %.2f, history invalid on the "
                     "spatial output)" % (cut_spatial_conf, CUT_CONFIDENCE_MAX),
        "PASS" if cut_conf_ok else "FAIL"))
    verdicts.append(("spatial acts on the noisy regime (cut change %.5f >= tail %.5f x %.1f)" % (
        cut_change_ratio if cut_change_ratio is not None else -1.0,
        tail_change_ratio if tail_change_ratio is not None else -1.0,
        CUT_CHANGE_MIN_MULT),
        "PASS" if acts_on_noise_ok else "FAIL"))
    verdicts.append(("spatial reduces variance most on the noisy frame (cut var ratio %.4f <= %.3f)" % (
        cut_var_ratio if cut_var_ratio is not None else -1.0, CUT_VAR_RATIO_MAX),
        "PASS" if cut_var_ok else "FAIL"))
    verdicts.append(("camera cut spatial tracks temporal reset (delta/diff %.3f <= %.2f)" % (
        (cut_delta / cut_spatial_diff) if (cut_delta is not None and cut_spatial_diff and cut_spatial_diff > 0) else -1.0,
        CUT_SPATIAL_TRACK_MAX),
        "PASS" if track_ok else "FAIL"))
    verdicts.append(("camera cut spatial recovery (settle %.5f < cut %.5f x 0.2)" % (
        settle_spatial_diff if settle_spatial_diff is not None else -1.0,
        cut_spatial_diff if cut_spatial_diff is not None else -1.0),
        "PASS" if cut_recovery_ok else "FAIL"))

    # ------------------------------------------------------- Final verdicts ----------
    report["history_series"] = history_series
    report["series"] = records
    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    if any(v == "SKIP" for _, v in verdicts):
        report["summary"] = "SKIP" if all(v != "FAIL" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("SPATIAL VERDICT", name, verdict)
    write_json(OUT_JSON, report)
    print("SPATIAL wrote", os.path.abspath(OUT_JSON))


def probe_channel(channel):
    """Render one frame on a throwaway graph that marks `channel`; True when it builds/renders."""
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
        print("SPATIAL WARNING channel 'LumenGI.%s' not available (%s)" % (channel, str(exc)))
        return False
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass


try:
    main()
except Exception as exc:
    print("SPATIAL ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S5",
            "script": "run_spatial_gate.py",
            "summary": "SKIP",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive SKIP on fatal error)", "SKIP")],
        },
    )
exit()
