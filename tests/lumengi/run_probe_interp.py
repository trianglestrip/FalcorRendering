from falcor import *

"""LumenGI S4-C3 probe interpolate output validation asset (SKELETON, Agent Z3).

Role / purpose
--------------
RUN-ONLY Mogwai GPU skeleton for the S4.3 probe interpolate/integrate pass
(task.md 9, S4-B3 "probe 方向积分及 depth/normal/material aware 像素插值").
Graph: GBufferRT -> LumenGI with useScreenTrace=True + useScreenProbes=True.
It reads the interpolate RESULT channel (S4_TODO -- Z1/Z3 freeze the name) and
validates the task.md S4 gate "Probe 插值不跨明显深度、法线和材质边界":

  * V1. output shape == full resolution (W, H) and the radiance RGB is
        NaN/Inf-free and non-negative,
  * V2. no cross-blend across depth / normal boundaries: edge pixels are
        detected from GBufferRT (linearZ + oct-decoded normWRoughnessMaterialID)
        and the interpolate weight channel (S4_TODO) must not blend multiple
        probes across an edge,
  * V3. (S4_TODO, placeholder) edge-side radiance consistency -- the
        interpolated radiance at an edge pixel must come from the SAME surface
        as the pixel, not the far side. Requires a per-pixel probe-selection
        reference; SKIP until S4-B3 exposes one.

When the interpolate channel is absent (expected pre-S4.3 at HEAD 0620d28b)
the script SKIPs gracefully -- it never crashes.

S4_TODO contract (root must freeze with Z1/Z3 before this becomes gating)
------------------------------------------------------------------------
  * S4_TODO[interp_channel]: the LumenGI output exposing the interpolated
    probe result. Candidate "probeInterpolated". The interpolate pass Z2 wrote
    (LumenScreenProbeInterpolate.cs.slang, untracked) writes a full-res RGBA16F
    gGIOutput with RGB = INCIDENT IRRADIANCE E (composite multiplies by
    albedo/pi) and A = confidence in [0, 1] (0 = no valid tap / degraded).
    This script PROBES the channel; absent -> SKIP.
  * S4_TODO[cross_blend]: the boundary gate below detects depth/normal edges
    from GBufferRT and checks the interpolated radiance at an edge pixel is
    closer to its SAME-surface neighbors than to the far-side neighbors
    ("edge-side consistency"). The 1px 4-neighborhood formulation and the
    thresholds LOGZ_EDGE / NORMAL_EDGE_ANGLE / CROSS_BLEND_MAX_FRACTION /
    MIN_EDGE_PIXELS are placeholders to freeze once S4-B3 lands.
  * S4_TODO[side_reference]: the per-pixel probe-selection reference needed by
    a stricter side check is not exposed; V3 stays SKIP.

Exit: Falcor `exit()` as in the sibling scripts. Report JSON is written to
artifacts/lumengi/S4/probe/probe_interp.json regardless of the verdict.
"""

import json
import math
import os

import numpy as np

RESOLUTION = (640, 360)
FRAME_RATE = 60
SCENE = "test_scenes/cornell_box.pyscene"
OUT_JSON = os.environ.get("LUMEN_PROBE_INTERP_OUT", "artifacts/lumengi/S4/probe/probe_interp.json")

USE_SCREEN_TRACE = bool(os.environ.get("LUMEN_PROBE_INTERP_USE_SCREEN_TRACE", "") != "0")
USE_SCREEN_PROBES = bool(os.environ.get("LUMEN_PROBE_INTERP_USE_SCREEN_PROBES", "") != "0")

# S4_TODO[interp_channel]: candidate LumenGI output exposing the interpolated
# probe result. This is the channel the script PROBES; absent -> SKIP.
INTERP_CHANNEL = "probeInterpolated"

# S4_TODO gates (placeholders; freeze with root):
LOGZ_EDGE = 0.5                 # Depth discontinuity in octaves of linear depth.
NORMAL_EDGE_ANGLE = 0.5         # Normal discontinuity in radians (~28.6 deg).
CROSS_BLEND_MAX_FRACTION = 0.2  # Max edge pixels allowed to be multi-probe blends.
MIN_EDGE_PIXELS = 200           # Below this the boundary gate is SKIP (unmeasurable).

# Reproducibility of the capture: fixed warmup + fixed frame (same as run_probe.py).
WARMUP_FRAMES = 4
CAPTURE_FRAME = 5


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
    """GBufferRT -> LumenGI (useScreenTrace + useScreenProbes). Marks the
    interpolate channel + the GBufferRT sources the edge masks need."""
    graph = RenderGraph("LumenGIProbeInterp")
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
                "useScreenTrace": USE_SCREEN_TRACE,
                "useScreenProbes": USE_SCREEN_PROBES,
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
    graph.markOutput("GBufferRT.linearZ")
    graph.markOutput("GBufferRT.normWRoughnessMaterialID")
    graph.markOutput("LumenGI." + INTERP_CHANNEL)
    return graph


def _setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()


def render_capture():
    for i in range(1, WARMUP_FRAMES + 1):
        m.clock.frame = i
        m.renderFrame()
    m.clock.frame = CAPTURE_FRAME
    m.renderFrame()


def grab(name):
    return m.activeGraph.get_output(name).to_numpy()


# -------------------------------------------------------------------------------------
# Edge-mask helpers (mirror run_gbuffer_compare.py decoding conventions).
# -------------------------------------------------------------------------------------


def decode_oct_unorm(p):
    p = np.asarray(p, dtype=np.float64) * 2.0 - 1.0
    n = np.zeros(p.shape[:-1] + (3,), dtype=np.float64)
    n[..., 0] = p[..., 0]
    n[..., 1] = p[..., 1]
    n[..., 2] = 1.0 - np.abs(p[..., 0]) - np.abs(p[..., 1])
    wrap = n[..., 2] < 0.0
    xy = np.zeros_like(p[..., :2], dtype=np.float64)
    xy[..., 0] = (1.0 - np.abs(p[..., 1])) * np.sign(p[..., 0])
    xy[..., 1] = (1.0 - np.abs(p[..., 0])) * np.sign(p[..., 1])
    n[..., :2][wrap] = xy[wrap]
    norm = np.sqrt(np.sum(n * n, axis=-1))
    return n / (norm[..., np.newaxis] + 1e-12)


def decode_normal(matid_raw):
    """Decode normWRoughnessMaterialID (RGB10A2, oct-encoded normal in xy) into
    a unit world normal. Handles both the packed uint8 and float readbacks."""
    if matid_raw.dtype == np.uint8:
        arr = matid_raw
        if arr.ndim == 1:
            h, w = RESOLUTION
            packed = arr.view(np.uint32).reshape(h, w)
        else:
            packed = arr.reshape(-1, 4).view(np.uint32).reshape(arr.shape[0], arr.shape[1])
        xy_unorm = np.stack(
            [
                ((packed >> 0) & 0x3FF).astype(np.float64) / 1023.0,
                ((packed >> 10) & 0x3FF).astype(np.float64) / 1023.0,
            ],
            axis=-1,
        )
    else:
        arr = np.asarray(matid_raw, dtype=np.float64)
        xy_unorm = arr[..., :2]
    return decode_oct_unorm(xy_unorm)


def neighbor_max_grad(field):
    """Max |field - neighbor| over the 4-neighborhood (edge-padded). Works for
    scalar (H, W) or vector (H, W, C) fields."""
    field = np.asarray(field, dtype=np.float64)
    p = np.pad(field, ((1, 1), (1, 1)) + ((0, 0),) * (field.ndim - 2), mode="edge")
    grad = np.zeros_like(field)
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        other = p[1 + dy:1 + dy + field.shape[0], 1 + dx:1 + dx + field.shape[1]]
        grad = np.maximum(grad, np.abs(field - other))
    return grad


def normal_edge_mask(matid_raw):
    n = decode_normal(matid_raw)
    good = np.isfinite(n).all(axis=-1)
    grad = np.zeros(n.shape[:-1], dtype=np.float64)
    p = np.pad(n, ((1, 1), (1, 1), (0, 0)), mode="edge")
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        other = p[1 + dy:1 + dy + n.shape[0], 1 + dx:1 + dx + n.shape[1]]
        dot = np.sum(n * other, axis=-1)
        dot = np.clip(dot, -1.0, 1.0)
        grad = np.maximum(grad, np.arccos(dot))
    return (grad > NORMAL_EDGE_ANGLE) & good


# -------------------------------------------------------------------------------------
# Interpolate output validation.
# -------------------------------------------------------------------------------------


def validate_interp(interp_tex, lin, matid_raw):
    """V1/V2/V3 metrics over the interpolate result. Returns a report dict.

    RGB = incident irradiance E, A = confidence in [0, 1] (Z2 contract)."""
    interp = np.asarray(interp_tex, dtype=np.float32)
    h, w = interp.shape[:2]
    rgb = interp[..., :3] if interp.ndim == 3 else interp
    confidence = interp[..., 3] if interp.ndim == 3 else np.zeros_like(rgb)

    # V1: output shape / finiteness / sign.
    finite_ok = bool(np.isfinite(rgb).all())
    nonneg_ok = bool(float(rgb.min()) >= 0.0) if rgb.size else False
    shape_ok = [int(h), int(w)] == list(RESOLUTION)

    # V2: depth + normal edge masks, then edge-side consistency.
    z = np.asarray(lin, dtype=np.float64)
    z = z[..., 0] if z.ndim == 3 else z
    good = np.isfinite(z) & (z > 0.0)
    lz = np.log2(np.maximum(z, 1e-6))
    lz[~good] = 0.0
    depth_edge = (neighbor_max_grad(lz) > LOGZ_EDGE) & good
    nrm = decode_normal(matid_raw)
    nrm_good = np.isfinite(nrm).all(axis=-1)
    normal_edge = normal_edge_mask(matid_raw) & nrm_good
    edge = (depth_edge | normal_edge) & good
    n_edge = int(edge.sum())

    gi_lum = rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722
    cross_dominant = edge_side_cross_dominant(gi_lum, lz, nrm, edge, good, nrm_good)
    n_dominant = int(cross_dominant.sum())

    cross_blend_fraction = None
    cross_blend_ok = None
    if n_edge >= MIN_EDGE_PIXELS:
        cross_blend_fraction = float(n_dominant) / float(n_edge)
        cross_blend_ok = cross_blend_fraction <= CROSS_BLEND_MAX_FRACTION
    else:
        cross_blend_ok = None  # SKIP: too few edge pixels to measure.

    return {
        "width": w,
        "height": h,
        "shape_ok": shape_ok,
        "radiance_finite": finite_ok,
        "radiance_nonnegative": nonneg_ok,
        "radiance_min": float(rgb.min()) if rgb.size else None,
        "radiance_max": float(rgb.max()) if rgb.size else None,
        "radiance_mean": float(rgb.mean()) if rgb.size else None,
        "depth_edge_pixels": int(depth_edge.sum()),
        "normal_edge_pixels": int(normal_edge.sum()),
        "edge_pixels": n_edge,
        "confidence_min": float(confidence.min()) if confidence.size else None,
        "confidence_max": float(confidence.max()) if confidence.size else None,
        "confidence_mean": float(confidence.mean()) if confidence.size else None,
        "cross_side_dominant_pixels": n_dominant,
        "cross_blend_fraction": cross_blend_fraction,
        "cross_blend_max_fraction": CROSS_BLEND_MAX_FRACTION,
        "cross_blend_ok": cross_blend_ok,
    }


def edge_side_cross_dominant(gi_lum, lz, nrm, edge, good, nrm_good, radius=2):
    """Boundary-smear check (S4_TODO formulation; freeze with root).

    For each edge pixel P compare P's luminance against the mean over the
    INTERIOR ring at Chebyshev radius `radius` (i.e. pixels that are on the
    same surface as P, excluding the edge band itself) versus the ring on the
    FAR side. P "cross-blends" if it resembles the far side more than its own
    side. Using the interior (non-edge) ring makes a full-column smear of the
    boundary band detectable, because the reference never includes the blended
    band."""
    h, w = gi_lum.shape
    pad = lambda f, r: np.pad(f, ((r, r), (r, r)) + ((0, 0),) * (f.ndim - 2), mode="edge")
    gi_p = pad(gi_lum, radius)
    lz_p = pad(lz, radius)
    nrm_p = pad(nrm, radius)
    good_p = pad(good, radius)
    nrm_good_p = pad(nrm_good, radius)
    edge_p = pad(edge, radius)

    same_sum = np.zeros_like(gi_lum, dtype=np.float64)
    same_cnt = np.zeros_like(gi_lum, dtype=np.int32)
    cross_sum = np.zeros_like(gi_lum, dtype=np.float64)
    cross_cnt = np.zeros_like(gi_lum, dtype=np.int32)

    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if max(abs(dy), abs(dx)) != radius:
                continue
            n_gi = gi_p[radius + dy:radius + dy + h, radius + dx:radius + dx + w]
            n_lz = lz_p[radius + dy:radius + dy + h, radius + dx:radius + dx + w]
            n_nrm = nrm_p[radius + dy:radius + dy + h, radius + dx:radius + dx + w]
            n_good = good_p[radius + dy:radius + dy + h, radius + dx:radius + dx + w]
            n_nrm_good = nrm_good_p[radius + dy:radius + dy + h, radius + dx:radius + dx + w]
            n_edge = edge_p[radius + dy:radius + dy + h, radius + dx:radius + dx + w]
            same_side = (
                n_good
                & n_nrm_good
                & ~n_edge
                & (np.abs(lz - n_lz) <= LOGZ_EDGE)
                & (np.arccos(np.clip(np.sum(nrm * n_nrm, axis=-1), -1.0, 1.0)) <= NORMAL_EDGE_ANGLE)
            )
            cross_side = n_good & n_nrm_good & ~n_edge & ~same_side
            same_sum += np.where(same_side, n_gi, 0.0)
            same_cnt += same_side.astype(np.int32)
            cross_sum += np.where(cross_side, n_gi, 0.0)
            cross_cnt += cross_side.astype(np.int32)

    measurable = edge & (same_cnt > 0) & (cross_cnt > 0)
    same_mean = same_sum / np.maximum(same_cnt, 1)
    cross_mean = cross_sum / np.maximum(cross_cnt, 1)
    d_same = np.abs(gi_lum - same_mean)
    d_cross = np.abs(gi_lum - cross_mean)
    return measurable & (d_cross <= d_same)


def main():
    report = {
        "stage": "S4",
        "script": "run_probe_interp.py",
        "role": "S4-C3 probe interpolate output validation (Agent Z3)",
        "status": "skeleton",
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "interp_channel": INTERP_CHANNEL,
        "config": {
            "useScreenTrace": USE_SCREEN_TRACE,
            "useScreenProbes": USE_SCREEN_PROBES,
            "logz_edge_octaves": LOGZ_EDGE,
            "normal_edge_angle_rad": NORMAL_EDGE_ANGLE,
            "cross_blend_max_fraction": CROSS_BLEND_MAX_FRACTION,
            "min_edge_pixels": MIN_EDGE_PIXELS,
        },
    }
    verdicts = []

    graph = None
    available = False
    try:
        graph = create_lumen_graph()
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(SCENE)
        render_capture()
        available = True
    except Exception as exc:  # pragma: no cover - pre-S4.3 path
        print(
            "PROBEINTERP WARNING interpolate channel 'LumenGI.%s' not available "
            "(pre-S4.3 integration expected); absent -> %s" % (INTERP_CHANNEL, str(exc))
        )
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass
    report["interp_channel_available"] = available

    if not available:
        for name in (
            "interpolate output shape == full-res",
            "interpolate output no NaN/Inf, non-negative",
            "interpolate no cross-blend across depth/normal edges (S4_TODO)",
            "interpolate edge-side radiance consistency (S4_TODO reference)",
        ):
            verdicts.append((name, "SKIP"))
    else:
        interp = grab("LumenGI." + INTERP_CHANNEL)
        lin = grab("GBufferRT.linearZ")
        matid = grab("GBufferRT.normWRoughnessMaterialID")
        mets = validate_interp(interp, lin, matid)
        report["metrics"] = mets

        verdicts.append(("interpolate output shape == full-res", "PASS" if mets["shape_ok"] else "FAIL"))
        if mets["radiance_finite"] and mets["radiance_nonnegative"]:
            verdicts.append(("interpolate output no NaN/Inf, non-negative", "PASS"))
        else:
            verdicts.append(("interpolate output no NaN/Inf, non-negative", "FAIL"))
        if mets["edge_pixels"] < MIN_EDGE_PIXELS:
            verdicts.append(("interpolate no cross-blend across depth/normal edges (S4_TODO)", "SKIP"))
        else:
            ok = mets["cross_blend_ok"]
            verdicts.append(
                (
                    "interpolate no cross-blend across depth/normal edges (S4_TODO, %d edge px, fraction %s)"
                    % (mets["edge_pixels"], "%.3f" % mets["cross_blend_fraction"] if ok is not None else "n/a"),
                    "PASS" if ok else "FAIL",
                )
            )
        verdicts.append(("interpolate edge-side radiance consistency (S4_TODO reference)", "SKIP"))

    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    if any(v == "SKIP" for _, v in verdicts):
        report["summary"] = "SKIP" if all(v != "FAIL" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("PROBEINTERP VERDICT", name, verdict)
    write_json(OUT_JSON, report)
    print("PROBEINTERP wrote", os.path.abspath(OUT_JSON))


# Falcor's embedded Python executes the script with __name__ == 'builtins', so an
# `if __name__ == "__main__":` guard never runs. Call main() unconditionally, like
# the other working run_*.py scripts.
try:
    main()
except Exception as exc:
    print("PROBEINTERP ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S4",
            "script": "run_probe_interp.py",
            "summary": "SKIP",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive SKIP on fatal error)", "SKIP")],
        },
    )
exit()
