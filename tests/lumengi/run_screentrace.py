from falcor import *

"""S4-A1 screen-trace verification skeleton (Agent R2).

Run by root AFTER S4-A1 (HZB host) + S4-B1 (screen trace shader) are integrated
into LumenGI.cpp. Graph: GBufferRT -> LumenGI (useSurfaceCache defaults OFF so
the check isolates the S4 screen-trace path from the S2/S3 cache path).

CONTRACT READ FROM LumenScreenTraceData.slang (frozen for the S4 wave):
  * gScreenTraceResult is RGBA16F, one pixel per screen texel (1 ray / pixel).
  * Hit:  RGB = float3(hitUV, hitLinearDistance), A = confidence in (0, 1].
  * Miss: RGB = 0, A = -((float)missReason + 1.0) (always < 0); decode
          reason = uint(-A) - 1. Reasons are the kLumenScreenTraceMissReason*
          constants: 0 = None(Hit), 1 = OutOfScreen, 2 = BehindSurface,
          3 = MaxSteps, 4 = ThinGeometry (reserved). Count = 5.
  * hitLinearDistance is the distance accumulated along the view-space march;
    it is NOT automatically the world-space ray distance HWRT reports.

HWRT REFERENCE (existing, already shipped):
  * LumenGI.diffuseRadianceHitDist (RGBA16F): RGB = unmodulated diffuse
    radiance, A = hit distance (RayTCurrent in world units) or
    kLumenGIMissHitDistance = 65504.f on miss.

S4_TODO CONTRACT (root must confirm before this becomes a gating script):
  * S4_TODO[channel]: the LumenGI output channel that exposes the screen trace
    result. Candidate name "screenTrace"; root freezes it in reflect() next to
    diffuseRadianceHitDist. This script PROBES the channel and SKIPs (never
    crashes) when it is absent or when graph compile fails on the marked output
    -- both are the expected pre-S4 behavior right now.
  * S4_TODO[tolerance]: S4-C1 gate #1 requires the screen-trace hit distance to
    match HWRT within tolerance. The two distances live in different
    parametrizations (view-space march distance vs world ray distance); root
    must freeze how they are made comparable (exact conversion for the pinhole
    case, or a bounded relative tolerance) before HIT_DIST_REL_TOL is trusted.
  * S4_TODO[rays]: the "rays launched" denominator. This skeleton assumes
    1 ray / pixel (W*H); if S4 integrates a per-pixel ray budget or a
    ray-count counter (like LumenGIFrameCounters.tracedSamples), switch the
    denominator to that readback.
  * S4_TODO[stats]: the per-reason histogram is recomputed here from the output
    texture. If S4-C1 later adds a host stats buffer (miss-reason totals), this
    script should also cross-check the histogram against that buffer.

GATES (task.md S4 gate list):
  G1. Visible-hit pixels: screen-trace hitLinearDistance vs HWRT
      diffuseRadianceHitDist.a within HIT_DIST_REL_TOL (placeholder).
  G2. Channel completeness: hits + misses == W*H == rays launched, and every
      decoded miss reason lies in [1, kLumenScreenTraceMissReasonCount-1]
      (invalid encodings are flagged, not silently counted).

Exit: Falcor `exit()` as in the sibling scripts. Report JSON is written to
artifacts/lumengi/S4/screentrace.json regardless of the verdict so the runner
can consume a machine-readable SKIP/PASS/FAIL.
"""

import json
import math
import os

import numpy as np

# -------------------------------------------------------------------------------------
# Frozen S4 contract values (LumenScreenTraceData.slang).
# -------------------------------------------------------------------------------------
MISS_REASON_NONE = 0          # Hit.
MISS_REASON_OUT_OF_SCREEN = 1
MISS_REASON_BEHIND_SURFACE = 2
MISS_REASON_MAX_STEPS = 3
MISS_REASON_THIN_GEOMETRY = 4  # Reserved.
MISS_REASON_COUNT = 5
MISS_REASON_NAMES = {
    0: "Hit",
    1: "OutOfScreen",
    2: "BehindSurface",
    3: "MaxSteps",
    4: "ThinGeometry",
}

# kLumenGIMissHitDistance (LumenGIData.slang): HWRT rays that missed write this
# sentinel to diffuseRadianceHitDist.a. A real (finite) hit is strictly below it.
HWRT_MISS_HIT_DISTANCE = 65504.0

# -------------------------------------------------------------------------------------
# Configuration (S4_TODO: freeze defaults with root when the gate lands).
# -------------------------------------------------------------------------------------
RESOLUTION = (640, 360)
FRAME_RATE = 60
SCENE = "test_scenes/cornell_box.pyscene"
OUT_JSON = os.environ.get("LUMEN_SCREENTRACE_OUT", "artifacts/lumengi/S4/screentrace.json")

# S4_TODO[channel]: candidate LumenGI output channel exposing the screen trace
# result texture (RGBA16F, LumenScreenTraceData encoding).
SCREEN_TRACE_CHANNEL = "screenTrace"

# useSurfaceCache is optional for this check: the screen trace path is
# independent of the S2/S3 cache path. Root may force it on later to test the
# hybrid mode; keep it off here by default.
USE_SURFACE_CACHE = bool(os.environ.get("LUMEN_SCREENTRACE_USE_SURFACE_CACHE", "") == "1")

# The S4-A1 screen-trace pass only runs when the LumenGI "useScreenTrace" property
# is set (default false). This gate script turns it on by default so the output
# channel is actually produced after the S4-A1 integration.
USE_SCREEN_TRACE = bool(os.environ.get("LUMEN_SCREENTRACE_USE_SCREEN_TRACE", "") != "0")

# S4_TODO[tolerance]: placeholder relative tolerance for the screen-trace vs
# HWRT hit-distance comparison. See header note; freeze with root before gating.
HIT_DIST_REL_TOL = 0.25

# Valid pixels for G1 must be HWRT hits (finite, real distance) AND screen-trace
# hits (A > 0). Pixels where either side reports a miss are excluded from the
# distance gate and only counted in the completeness gate.
MIN_VALID_HIT_PIXELS = 100  # Placeholder floor; freeze with root. 0.0 always passes.
MIN_VALID_HIT_FRACTION = 0.05  # Of the rendered pixels; placeholder.

# -------------------------------------------------------------------------------------
# Small helpers mirroring the sibling scripts.
# -------------------------------------------------------------------------------------


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
# Graph / scene / probe.
# -------------------------------------------------------------------------------------


def create_lumen_graph(mark_screentrace):
    """GBufferRT -> LumenGI. Optionally marks the S4 screen-trace channel as a
    graph output (the probe uses this to test channel existence)."""
    graph = RenderGraph("LumenGIScreenTrace")
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
                "enabled": True,
                "traceMode": "HardwareRT",
                "qualityPreset": "High",
                "useSurfaceCache": USE_SURFACE_CACHE,
                "useScreenTrace": USE_SCREEN_TRACE,
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
    # HWRT reference for the hit-distance gate (always available post-S1).
    graph.markOutput("LumenGI.diffuseRadianceHitDist")
    if mark_screentrace:
        # S4_TODO[channel]: if the final channel name differs, update
        # SCREEN_TRACE_CHANNEL (this line is the only wiring change needed).
        graph.markOutput("LumenGI." + SCREEN_TRACE_CHANNEL)
    return graph


def _setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 1


def probe_screentrace(scene_path):
    """Try to render with the S4 channel marked. Returns (available, graph).

    Pre-S4 the channel does not exist: graph.markOutput('LumenGI.screenTrace')
    throws immediately (field validation), so the whole probe body -- including
    create_lumen_graph -- must be inside the try. On failure we rebuild the graph
    without the channel, render once, and report available=False so the gates
    become SKIP (never a crash).
    """
    graph = None
    try:
        graph = create_lumen_graph(mark_screentrace=True)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(scene_path)
        m.clock.frame = 1
        m.renderFrame()
        return True, graph
    except Exception as exc:  # pragma: no cover - pre-S4 path
        print(
            "SCREENTRACE WARNING screen-trace channel 'LumenGI.%s' not available "
            "(pre-S4 integration expected); channel absent -> %s"
            % (SCREEN_TRACE_CHANNEL, str(exc))
        )
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass
        graph = create_lumen_graph(mark_screentrace=False)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(scene_path)
        m.clock.frame = 1
        m.renderFrame()
        return False, graph


# -------------------------------------------------------------------------------------
# Decode + metrics.
# -------------------------------------------------------------------------------------


def decode_screentrace(tex):
    """Decode gScreenTraceResult (RGBA16F) into:
      * hit        : bool (W, H)  - result.a > 0
      * miss_reason: uint8 (W, H) - decoded reason for misses (0 for hits)
      * hit_uv     : float (W, H, 2)
      * hit_distance: float (W, H) - hitLinearDistance (0 on miss)
    Raises ValueError on an out-of-range miss encoding (A outside (-COUNT, 0])."""
    arr = np.asarray(tex, dtype=np.float32)
    if arr.ndim == 2:
        arr = np.stack([arr, np.zeros_like(arr), np.zeros_like(arr), np.ones_like(arr)], axis=-1)
    if arr.shape[-1] < 4:
        raise ValueError("screen trace output has <4 channels: %s" % (arr.shape,))

    a = arr[..., 3]
    hit = a > 0.0
    # Miss A is always negative; reason = uint(-A) - 1.
    miss_a = a[~hit]
    reason = np.zeros(a.shape, dtype=np.int32)
    reason[hit] = MISS_REASON_NONE
    if miss_a.size:
        dec = (-miss_a).astype(np.float32) - 1.0
        r = np.rint(dec).astype(np.int32)
        if np.any(r < 1) or np.any(r >= MISS_REASON_COUNT):
            bad = int(np.count_nonzero((r < 1) | (r >= MISS_REASON_COUNT)))
            raise ValueError(
                "screen trace miss encoding out of range: %d texels decode outside "
                "reason [1, %d] (corrupt gScreenTraceResult?)" % (bad, MISS_REASON_COUNT - 1)
            )
        reason[~hit] = r
    return {
        "hit": hit,
        "miss_reason": reason,
        "hit_uv": arr[..., :2],
        "hit_distance": np.where(hit, arr[..., 2], 0.0),
        "raw": arr,
    }


def gather_metrics(st_result, hwrt_hit_dist):
    """Compute the per-gate metrics from the decoded screen trace and the HWRT
    reference. Returns a plain dict (JSON-safe)."""
    hit = st_result["hit"]
    reason = st_result["miss_reason"]
    st_dist = st_result["hit_distance"]

    w, h = hit.shape
    total = w * h

    # HWRT: A == hit distance; miss sentinel is kLumenGIMissHitDistance.
    # diffuseRadianceHitDist is RGBA16F (RGB radiance, A hit distance), so the
    # readback is (W, H, 4); keep a 2-D fallback for robustness.
    hwrt_dist = np.asarray(hwrt_hit_dist, dtype=np.float32)
    hwrt_ok = hwrt_dist[..., 3] if hwrt_dist.ndim == 3 else hwrt_dist
    hwrt_hit = hwrt_ok < HWRT_MISS_HIT_DISTANCE

    # G1: pixels where BOTH sides hit (a real, finite distance on both sides).
    both_hit = hit & hwrt_hit
    rel_err = np.abs(st_dist - hwrt_ok) / np.maximum(np.abs(hwrt_ok), 1e-6)
    g1_ok = rel_err[both_hit] <= HIT_DIST_REL_TOL if np.any(both_hit) else np.array([], dtype=bool)

    # G2: channel completeness. hits + misses must equal the ray count (W*H for
    # a 1-ray-per-pixel full-res dispatch; see S4_TODO[rays]).
    miss_count = int((~hit).sum())
    hist = {MISS_REASON_NAMES.get(int(r), str(int(r))): int((reason == r).sum()) for r in range(1, MISS_REASON_COUNT)}

    return {
        "width": w,
        "height": h,
        "rays_launched": total,
        "hits": int(hit.sum()),
        "misses": miss_count,
        "hit_pixels": int(hit.sum()),
        "miss_reason_histogram": hist,
        "hwrt_hit_pixels": int(hwrt_hit.sum()),
        "both_hit_pixels": int(both_hit.sum()),
        "hit_dist_rel_tol": HIT_DIST_REL_TOL,
        "hit_dist_g1_valid": int(np.count_nonzero(both_hit)),
        "hit_dist_g1_ok": int(np.count_nonzero(g1_ok)),
        "hit_dist_max_rel_err": float(np.max(rel_err[both_hit])) if np.any(both_hit) else None,
        "hit_dist_mean_rel_err": float(np.mean(rel_err[both_hit])) if np.any(both_hit) else None,
    }


def evaluate(metrics):
    """Turn metrics into verdicts (PASS/FAIL/SKIP). Placeholder gates, freeze
    with root (see S4_TODO markers)."""
    verdicts = []
    total = metrics["rays_launched"]
    n_valid = metrics["hit_dist_g1_valid"]
    n_ok = metrics["hit_dist_g1_ok"]

    # G2 first: completeness (hard invariant of the encoding).
    if metrics["hits"] + metrics["misses"] == total:
        verdicts.append(("screentrace completeness (hits+misses == rays)", "PASS"))
    else:
        verdicts.append(
            ("screentrace completeness (hits+misses == rays)", "FAIL")
        )

    # G1: distance tolerance on pixels where both sides hit.
    if n_valid == 0:
        # No comparable pixels (e.g. a fully sky scene). Not a failure by
        # itself, but not a pass either - flag for root review.
        verdicts.append(("hit distance vs HWRT (no comparable pixels)", "SKIP"))
    elif n_valid < MIN_VALID_HIT_PIXELS or n_valid < MIN_VALID_HIT_FRACTION * total:
        verdicts.append(("hit distance vs HWRT (insufficient samples)", "SKIP"))
    elif n_ok == n_valid:
        verdicts.append(("hit distance vs HWRT (%.0f px, tol %g)" % (n_valid, HIT_DIST_REL_TOL), "PASS"))
    else:
        verdicts.append(("hit distance vs HWRT (%.0f px, tol %g)" % (n_valid, HIT_DIST_REL_TOL), "FAIL"))
    return verdicts


def main(scene_path, out_json):
    available, graph = probe_screentrace(scene_path)
    report = {
        "stage": "S4",
        "script": "run_screentrace.py",
        "scene": scene_path,
        "resolution": list(RESOLUTION),
        "useSurfaceCache": USE_SURFACE_CACHE,
        "useScreenTrace": USE_SCREEN_TRACE,
        "screentrace_channel": SCREEN_TRACE_CHANNEL if available else None,
        "screentrace_available": available,
    }

    if not available:
        report["verdicts"] = [("screen trace channel unavailable", "SKIP")]
        report["summary"] = "SKIP"
        for name, verdict in report["verdicts"]:
            print("SCREENTRACE VERDICT", name, verdict)
        print("SCREENTRACE SKIP pre-S4 integration (S4_TODO channel '%s' absent)" % SCREEN_TRACE_CHANNEL)
        write_json(out_json, report)
        return

    # S4_TODO[channel]: when available, read it exactly here.
    st_tex = m.activeGraph.get_output("LumenGI.%s" % SCREEN_TRACE_CHANNEL).to_numpy()
    hwrt_tex = m.activeGraph.get_output("LumenGI.diffuseRadianceHitDist").to_numpy()

    st = decode_screentrace(st_tex)
    metrics = gather_metrics(st, hwrt_tex)
    verdicts = evaluate(metrics)
    report["metrics"] = metrics
    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("SCREENTRACE VERDICT", name, verdict)
    write_json(out_json, report)


# --- entry point (mirrors the sibling scripts: read config, run, exit) --------
# NOTE (Agent W, S4-A1): Falcor's embedded Python executes the script with
# __name__ == 'builtins', so an `if __name__ == "__main__":` guard never runs
# (the pass would silently do nothing and Mogwai would stay open). Call main()
# unconditionally like the other working run_*.py scripts.
main(SCENE, OUT_JSON)
exit()
