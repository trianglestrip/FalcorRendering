"""Runtime C5 full-scan/grid comparison in one Mogwai graph.

The ordinary C5 equivalence gate compares two independent Mogwai processes.
That is useful for a production smoke check, but Surface Cache feedback and
temporal history can advance differently between processes.  This runner
keeps one scene, one GBuffer and one frame schedule, then executes two
independent LumenGI pass instances side by side:

* ``LumenGIFull``: ``useCacheCardGrid=false``
* ``LumenGIGrid``: ``useCacheCardGrid=true``

It is deliberately a run-only asset.  It does not modify production files or
change the existing offline gate.  Runtime output is a compact JSON artifact
containing side-by-side output health/differences and explicit stats for both
passes.  The default gate is strict: the mean absolute difference of every
primary output must be <= 1e-4 on every captured frame.

Run inside Mogwai, for example::

    mogwai -headless -script tests/lumengi/run_c5_paired_equivalence.py

The standard Python ``--self-test`` path does not import Falcor, which keeps
the script suitable for py_compile and offline validation.
"""

from __future__ import annotations

import json
import math
import os
import sys
import time
from typing import Any, Dict, Iterable, Mapping, Sequence, Tuple

import numpy as np


OUT_DIR = os.path.abspath(
    os.environ.get(
        "LUMEN_C5_PAIRED_EQ_OUT",
        "artifacts/lumengi/C5/paired-equivalence",
    )
)
SCENE = os.environ.get(
    "LUMEN_C5_PAIRED_EQ_SCENE",
    "media/test_scenes/cornell_box.pyscene",
)
RESOLUTION = tuple(
    int(value)
    for value in os.environ.get("LUMEN_C5_PAIRED_EQ_RESOLUTION", "320x180")
    .lower()
    .replace("x", ",")
    .split(",")
)
WARMUP = max(1, int(os.environ.get("LUMEN_C5_PAIRED_EQ_WARMUP", "8")))
TRACE_MODE = os.environ.get("LUMEN_C5_PAIRED_EQ_TRACE_MODE", "MeshSDF")
RIGHT_USE_GRID = os.environ.get("LUMEN_C5_PAIRED_EQ_RIGHT_GRID", "1") not in ("0", "false", "off")
LEFT_USE_GRID = os.environ.get("LUMEN_C5_PAIRED_EQ_LEFT_GRID", "0") not in ("0", "false", "off")
USE_SURFACE_CACHE = os.environ.get("LUMEN_C5_PAIRED_EQ_SURFACE_CACHE", "1") not in ("0", "false", "off")
USE_CACHE_LIGHTING = os.environ.get("LUMEN_C5_PAIRED_EQ_CACHE_LIGHTING", "1") not in ("0", "false", "off")
USE_TEMPORAL = os.environ.get("LUMEN_C5_PAIRED_EQ_TEMPORAL", "1") not in ("0", "false", "off")
USE_SPATIAL = os.environ.get("LUMEN_C5_PAIRED_EQ_SPATIAL", "1") not in ("0", "false", "off")
SAVE_ARRAYS = os.environ.get("LUMEN_C5_PAIRED_EQ_SAVE_ARRAYS", "0") not in ("0", "false", "off")
SAVE_ARRAYS_MAX_FRAMES = max(1, int(os.environ.get("LUMEN_C5_PAIRED_EQ_SAVE_ARRAYS_MAX_FRAMES", "8")))


def _timeout_seconds() -> float:
    raw = os.environ.get("LUMEN_C5_PAIRED_EQ_TIMEOUT_SECONDS", "0").strip()
    try:
        value = float(raw)
    except (TypeError, ValueError):
        return 0.0
    return value if math.isfinite(value) and value > 0.0 else 0.0


# Opt-in watchdog: zero keeps the historical unlimited run. The watchdog is
# checked between frames so a slow shader compile/render remains observable in
# the report instead of being mistaken for a Python-side hang.
PAIR_TIMEOUT_SECONDS = _timeout_seconds()
# Frozen by the paired-equivalence contract.  There is intentionally no
# environment override: a runtime invocation must not be made to pass by
# widening the threshold.
OUTPUT_TOLERANCE = 1e-4
COMPARE_CHANNELS = tuple(
    item.strip()
    for item in os.environ.get(
        "LUMEN_C5_PAIRED_EQ_CHANNELS",
        "probeInterpolated,resolvedDiffuseGI,diffuseGI",
    ).split(",")
    if item.strip()
)
ALL_CHANNELS = (
    "diffuseGI",
    "resolvedDiffuseGI",
    "probeInterpolated",
    "diffuseRadianceHitDist",
    "confidence",
    "gdfTrace",
)
# The cache-direct atlas is optional and much larger than the screen outputs.
# Keep only a compact health fingerprint instead of retaining a full atlas copy
# for every paired frame. This remains diagnostic and never relaxes the pixel gate.
CACHE_DIRECT_CHANNEL = "cacheDirectRadiance"
CACHE_CAPTURE_CHANNEL = "cacheCaptureRadiance"
STATS_FIELDS = (
    # Producer provenance must match between paired passes before an output delta can be
    # attributed to the cache-lighting implementation. Missing fields remain BLOCKED.
    "cacheLightingSeed",
    "cacheLightingFrameIndex",
    "cacheLightingSurfaceCacheFrameIndex",
    "cacheLightingRayTypeCount",
    "cacheLightingTlasPresent",
    "cacheLightingUseEnvLight",
    "cacheLightingUseAnalyticLights",
    "cacheLightingUseEmissiveLights",
    "cacheLightingEnvSampler",
    "cacheLightingEmissiveSampler",
    "cacheLightingShadowsEnabled",
    "cacheLightingFeedbackEnabled",
    "cacheLightingVariantFingerprint",
    "cardGridCandidateCount",
    "cardGridOverflowCells",
    "cardGridCardCount",
    "cardGridCardsIndexed",
    "cardGridIndexedCards",
    "cardGridMissingCards",
)

# A paired comparison is only meaningful when the screen-probe/card-grid path
# actually executed.  Without this guard an empty graph can report identical
# zero counters and accidentally turn an unexercised candidate into PASS.
ACTIVITY_FIELDS = (
    "probeCount",
    "directionsPerProbe",
    "cacheLookupAttempts",
)
STAT_ALIASES = {
    "cardGridCandidateCount": ("cardGridCandidateCount", "candidateCount"),
    "cardGridOverflowCells": ("cardGridOverflowCells", "overflowCells"),
    "cardGridCardCount": ("cardGridCardCount", "cardCount", "cards"),
    "cardGridCardsIndexed": ("cardGridCardsIndexed", "indexedCards"),
    "cardGridIndexedCards": ("cardGridIndexedCards", "cardGridCardsIndexed", "indexedCards"),
    "cardGridMissingCards": ("cardGridMissingCards", "missingCards"),
}


def _safe(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {str(key): _safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_safe(item) for item in value]
    if isinstance(value, (float, np.floating)):
        number = float(value)
        return number if math.isfinite(number) else None
    if isinstance(value, (int, bool, str)) or value is None:
        return value
    return str(value)


def _write_json(path: str, payload: Mapping[str, Any]) -> None:
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temporary, path)


def _health(array: Any) -> Dict[str, Any]:
    data = np.asarray(array, dtype=np.float64)
    if data.size == 0:
        return {
            "finite": False,
            "nonnegative": False,
            "mean": None,
            "max": None,
            "shape": list(data.shape),
        }
    finite = bool(np.isfinite(data).all())
    return {
        "finite": finite,
        "nonnegative": bool(finite and np.min(data) >= 0.0),
        "mean": float(np.mean(data)) if finite else None,
        "max": float(np.max(data)) if finite else None,
        "shape": list(data.shape),
    }


def _difference(full: Any, grid: Any) -> Dict[str, Any]:
    left = np.asarray(full, dtype=np.float64)
    right = np.asarray(grid, dtype=np.float64)
    result: Dict[str, Any] = {
        "sameShape": list(left.shape) == list(right.shape),
        "meanAbs": None,
        "p95Abs": None,
        "maxAbs": None,
        "meanDelta": None,
        "changedNonzeroFraction": None,
        "finite": False,
        "withinTolerance": False,
    }
    if not result["sameShape"] or left.size == 0:
        return result
    if not np.isfinite(left).all() or not np.isfinite(right).all():
        return result
    delta = np.abs(left - right)
    mean_abs = float(np.mean(delta))
    p95_abs = float(np.percentile(delta, 95.0))
    max_abs = float(np.max(delta))
    mean_delta = abs(float(np.mean(left)) - float(np.mean(right)))
    nonzero = (np.abs(left) > 0.0) | (np.abs(right) > 0.0)
    changed_nonzero = (delta > 0.0) & nonzero
    nonzero_count = int(np.count_nonzero(nonzero))
    result.update(
        {
            "meanAbs": mean_abs,
            "p95Abs": p95_abs,
            "maxAbs": max_abs,
            "meanDelta": mean_delta,
            "changedNonzeroFraction": (
                float(np.count_nonzero(changed_nonzero)) / float(nonzero_count)
                if nonzero_count
                else 0.0
            ),
            "finite": True,
            # Both the pixel-wise mean and the scalar output mean are kept
            # under the same strict gate.  The max is diagnostic only: a
            # single noisy pixel must not hide the aggregate result.
            "withinTolerance": bool(
                mean_abs <= OUTPUT_TOLERANCE and mean_delta <= OUTPUT_TOLERANCE
            ),
        }
    )
    return result


def _stat_value(stats: Mapping[str, Any], field: str) -> Any:
    for alias in STAT_ALIASES.get(field, (field,)):
        if alias in stats:
            return stats[alias]
    return None


def _pass_properties(use_grid: bool) -> Dict[str, Any]:
    return {
        "enabled": True,
        "traceMode": TRACE_MODE,
        "useGDF": True,
        "useScreenProbes": True,
        "useScreenTrace": True,
        "useSurfaceCache": USE_SURFACE_CACHE,
        "useCacheLighting": USE_CACHE_LIGHTING,
        "useCacheCardGrid": bool(use_grid),
        "markGDFTrace": True,
        "markRawGI": True,
        "markProbeInterpolated": True,
        "surfaceCacheAtlasSize": 4096,
        "captureMaxPagesPerFrame": 64,
        "probeDirectionsPerProbe": 16,
        "useTemporalFilter": USE_TEMPORAL,
        "useSpatialFilter": USE_SPATIAL,
        "gdfLevelCount": 2,
        "gdfResolution": 64,
        "gdfBaseExtent": 4.0,
        "gdfTraceMaxSteps": 32,
        "gdfTraceMaxDistance": 20.0,
    }


def _create_graph():
    """Create one GBuffer plus the two fixed-property LumenGI passes."""
    graph = RenderGraph("C5PairedEquivalence")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", _pass_properties(LEFT_USE_GRID)), "LumenGIFull")
    graph.addPass(createPass("LumenGI", _pass_properties(RIGHT_USE_GRID)), "LumenGIGrid")
    channels = (
        "vbuffer",
        "linearZ",
        "mvec",
        "mvecW",
        "normWRoughnessMaterialID",
        "viewW",
        "diffuseOpacity",
        "emissive",
    )
    for pass_name in ("LumenGIFull", "LumenGIGrid"):
        for channel in channels:
            graph.addEdge("GBufferRT." + channel, pass_name + "." + channel)
        for channel in ALL_CHANNELS:
            graph.markOutput(pass_name + "." + channel)
        graph.markOutput(pass_name + "." + CACHE_DIRECT_CHANNEL)
        graph.markOutput(pass_name + "." + CACHE_CAPTURE_CHANNEL)
    return graph


def _capture_pass(graph: Any, pass_name: str) -> Tuple[Dict[str, Any], Dict[str, np.ndarray]]:
    lumen_pass = graph.getPass(pass_name)
    try:
        screen_stats = dict(lumen_pass.screenProbeStats)
    except Exception:
        screen_stats = {}
    try:
        cache_stats = dict(lumen_pass.surfaceCacheStats)
    except Exception:
        cache_stats = {}
    arrays: Dict[str, np.ndarray] = {}
    outputs: Dict[str, Any] = {}
    for channel in ALL_CHANNELS:
        try:
            arrays[channel] = np.asarray(
                graph.get_output(pass_name + "." + channel).to_numpy(),
                dtype=np.float64,
            ).copy()
            outputs[channel] = _health(arrays[channel])
        except Exception as error:
            outputs[channel] = {"status": "BLOCKED", "error": str(error)}
    try:
        cache_direct = np.asarray(
            graph.get_output(pass_name + "." + CACHE_DIRECT_CHANNEL).to_numpy(),
            dtype=np.float64,
        )
        rgb = cache_direct[..., :3] if cache_direct.ndim == 3 else cache_direct
        finite = bool(cache_direct.size and np.isfinite(cache_direct).all())
        outputs[CACHE_DIRECT_CHANNEL] = {
            "finite": finite,
            "mean": float(np.mean(rgb)) if finite else None,
            "max": float(np.max(rgb)) if finite else None,
            "nonzeroFraction": float(np.count_nonzero(rgb > 0.0) / rgb.size) if finite else None,
            "shape": list(cache_direct.shape),
        }
        arrays[CACHE_DIRECT_CHANNEL] = cache_direct.copy()
    except Exception as error:
        outputs[CACHE_DIRECT_CHANNEL] = {"status": "BLOCKED", "error": str(error)}
    try:
        cache_capture = np.asarray(
            graph.get_output(pass_name + "." + CACHE_CAPTURE_CHANNEL).to_numpy(),
            dtype=np.float64,
        )
        rgb = cache_capture[..., :3] if cache_capture.ndim == 3 else cache_capture
        finite = bool(cache_capture.size and np.isfinite(cache_capture).all())
        outputs[CACHE_CAPTURE_CHANNEL] = {
            "finite": finite,
            "mean": float(np.mean(rgb)) if finite else None,
            "max": float(np.max(rgb)) if finite else None,
            "nonzeroFraction": float(np.count_nonzero(rgb > 0.0) / rgb.size) if finite else None,
            "shape": list(cache_capture.shape),
        }
        arrays[CACHE_CAPTURE_CHANNEL] = cache_capture.copy()
    except Exception as error:
        outputs[CACHE_CAPTURE_CHANNEL] = {"status": "BLOCKED", "error": str(error)}
    return (
        {
            "screenProbeStats": _safe(screen_stats),
            "surfaceCacheStats": _safe(cache_stats),
            "outputs": outputs,
        },
        arrays,
    )


def _frame_pair(graph: Any) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    full, full_arrays = _capture_pass(graph, "LumenGIFull")
    grid, grid_arrays = _capture_pass(graph, "LumenGIGrid")
    differences: Dict[str, Any] = {}
    # Include the pre-lighting capture snapshot as a diagnostic input check. It
    # is intentionally not part of the production output contract, but a
    # mismatch here identifies capture publication before investigating TLAS or
    # cache-lighting sampling.
    for channel in tuple(COMPARE_CHANNELS) + (CACHE_DIRECT_CHANNEL, CACHE_CAPTURE_CHANNEL):
        if channel in full_arrays and channel in grid_arrays:
            differences[channel] = _difference(full_arrays[channel], grid_arrays[channel])
        else:
            differences[channel] = {
                "sameShape": False,
                "finite": False,
                "withinTolerance": False,
                "meanAbs": None,
                "p95Abs": None,
                "maxAbs": None,
                "meanDelta": None,
                "changedNonzeroFraction": None,
            }
    return (
        {"fullscan": full, "grid": grid, "differences": differences},
        {"fullscan": full_arrays, "grid": grid_arrays},
    )


def _save_frame_arrays(frame_id: int, arrays: Mapping[str, Mapping[str, np.ndarray]]) -> None:
    """Persist optional paired arrays for producer diagnostics, never for the normal gate."""
    if not SAVE_ARRAYS or frame_id > SAVE_ARRAYS_MAX_FRAMES:
        return
    array_dir = os.path.join(OUT_DIR, "arrays")
    os.makedirs(array_dir, exist_ok=True)
    channels = ("cacheCaptureRadiance", "cacheDirectRadiance", "probeInterpolated", "resolvedDiffuseGI")
    for side, side_arrays in arrays.items():
        for channel in channels:
            array = side_arrays.get(channel)
            if array is None:
                continue
            np.save(os.path.join(array_dir, "%s_f%03d_%s.npy" % (side, frame_id, channel)), array)


def _evaluate(frames: Sequence[Mapping[str, Any]], render_error: str | None) -> Dict[str, Any]:
    checks = []
    activity_values: Dict[str, List[float]] = {field: [] for field in ACTIVITY_FIELDS}
    activity_missing: Dict[str, bool] = {field: False for field in ACTIVITY_FIELDS}
    if render_error:
        checks.append({"name": "render", "status": "BLOCKED", "reason": render_error})
    if not frames:
        checks.append({"name": "frames", "status": "BLOCKED", "reason": "no paired frames"})
    for frame in frames:
        frame_id = frame.get("frame")
        differences = frame.get("pair", {}).get("differences", {})
        for channel in tuple(COMPARE_CHANNELS) + (CACHE_DIRECT_CHANNEL, CACHE_CAPTURE_CHANNEL):
            difference = differences.get(channel)
            ok = isinstance(difference, Mapping) and bool(
                difference.get("finite") and difference.get("withinTolerance")
            )
            checks.append(
                {
                    "name": "frame %s output %s" % (frame_id, channel),
                    "status": "PASS" if ok else "FAIL",
                    "tolerance": OUTPUT_TOLERANCE,
                    "difference": difference,
                }
            )
        pair = frame.get("pair", {})
        # Aggregate activity telemetry across the capture.  Warm-up frames may
        # legitimately report zero attempts, so the gate requires at least one
        # positive sample for every field rather than rejecting frame 1.
        for side in ("fullscan", "grid"):
            side_probe_stats = pair.get(side, {}).get("screenProbeStats", {})
            for field in ACTIVITY_FIELDS:
                value = side_probe_stats.get(field) if isinstance(side_probe_stats, Mapping) else None
                if isinstance(value, bool) or not isinstance(value, (int, float)):
                    activity_missing[field] = True
                elif not math.isfinite(float(value)) or float(value) < 0.0:
                    activity_missing[field] = True
                else:
                    activity_values[field].append(float(value))
        full_stats = pair.get("fullscan", {}).get("surfaceCacheStats", {})
        grid_stats = pair.get("grid", {}).get("surfaceCacheStats", {})
        for field in STATS_FIELDS:
            left = _stat_value(full_stats, field)
            right = _stat_value(grid_stats, field)
            present = isinstance(left, (int, float)) and isinstance(right, (int, float))
            equal = present and float(left) == float(right)
            checks.append(
                {
                    "name": "frame %s candidate stat %s" % (frame_id, field),
                    "status": "PASS" if equal else "BLOCKED",
                    "fullscan": left,
                    "grid": right,
                }
            )
    for field in ACTIVITY_FIELDS:
        values = activity_values[field]
        if activity_missing[field] or not values:
            checks.append(
                {
                    "name": "runtime activity %s" % field,
                    "status": "BLOCKED",
                    "reason": "missing or invalid screen-probe telemetry",
                    "observed": values,
                }
            )
        elif max(values) <= 0.0:
            checks.append(
                {
                    "name": "runtime activity %s" % field,
                    "status": "BLOCKED",
                    "reason": "screen-probe/card-grid path was not exercised",
                    "observed": values,
                }
            )
        else:
            checks.append(
                {
                    "name": "runtime activity %s" % field,
                    "status": "PASS",
                    "observedMax": max(values),
                }
            )
    statuses = [check["status"] for check in checks]
    status = "FAIL" if "FAIL" in statuses else "BLOCKED" if "BLOCKED" in statuses else "PASS"
    return {
        "status": status,
        "checks": checks,
        "summary": {
            "pass": statuses.count("PASS"),
            "fail": statuses.count("FAIL"),
            "blocked": statuses.count("BLOCKED"),
        },
    }


def _self_test() -> int:
    equal = np.zeros((2, 2, 4), dtype=np.float64)
    close = equal.copy()
    close[0, 0, 0] = OUTPUT_TOLERANCE
    over = equal.copy()
    over.fill(OUTPUT_TOLERANCE * 2.0)
    equal_diff = _difference(equal, equal)
    close_diff = _difference(equal, close)
    over_diff = _difference(equal, over)
    assert equal_diff["withinTolerance"]
    assert close_diff["withinTolerance"]
    assert not over_diff["withinTolerance"]
    assert close_diff["p95Abs"] <= OUTPUT_TOLERANCE
    assert over_diff["changedNonzeroFraction"] == 1.0
    assert _health(equal)["finite"]
    empty_gate = _evaluate([], None)
    assert empty_gate["status"] == "BLOCKED"
    assert any(check["name"].startswith("runtime activity ") for check in empty_gate["checks"])
    print("C5_PAIRED_EQUIVALENCE_SELF_TEST_PASS")
    return 0


def _run() -> int:
    # Falcor is intentionally imported only on the runtime path so standard
    # Python self-test/py_compile remain available outside Mogwai.
    import falcor

    # Mogwai injects the script helper ``m`` into the script globals.  It is
    # not exposed as ``falcor.m`` in current Mogwai builds; use the injected
    # binding first and keep the module fallback for older launchers.  Keeping
    # this import lazy is what makes ``python ... --self-test`` offline-safe.
    global RenderGraph, createPass, m
    RenderGraph = falcor.RenderGraph
    createPass = falcor.createPass
    m = globals().get("m", getattr(falcor, "m", None))
    if m is None:
        raise RuntimeError("Mogwai script helper 'm' was not injected")

    if len(RESOLUTION) != 2 or min(RESOLUTION) <= 0:
        raise ValueError("resolution must be WIDTHxHEIGHT")
    os.makedirs(OUT_DIR, exist_ok=True)
    m.loadScene(SCENE)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.pause()
    m.clock.frame = 0
    graph = _create_graph()
    m.addGraph(graph)
    frames = []
    render_error = None
    capture_start = time.monotonic()
    try:
        for frame_id in range(1, WARMUP + 1):
            if PAIR_TIMEOUT_SECONDS and time.monotonic() - capture_start >= PAIR_TIMEOUT_SECONDS:
                render_error = (
                    "paired capture wall-clock timeout after %.3fs (completed %d/%d frames)"
                    % (PAIR_TIMEOUT_SECONDS, len(frames), WARMUP)
                )
                break
            m.clock.frame = frame_id
            frame_start = time.monotonic()
            m.renderFrame()
            render_seconds = time.monotonic() - frame_start
            pair, arrays = _frame_pair(graph)
            _save_frame_arrays(frame_id, arrays)
            frames.append({"frame": frame_id, "renderSeconds": render_seconds, "pair": pair})
    except Exception as error:
        render_error = str(error)
    capture_elapsed = time.monotonic() - capture_start
    report = {
        "schema": "LumenGI.C5PairedEquivalence.v1",
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "warmup": WARMUP,
        "traceMode": TRACE_MODE,
        "useCacheCardGrid": {"fullscan": LEFT_USE_GRID, "grid": RIGHT_USE_GRID},
        "comparisonMode": "%s-%s" % ("grid" if LEFT_USE_GRID else "full", "grid" if RIGHT_USE_GRID else "full"),
        "controls": {
            "useSurfaceCache": USE_SURFACE_CACHE,
            "useCacheLighting": USE_CACHE_LIGHTING,
            "useTemporalFilter": USE_TEMPORAL,
            "useSpatialFilter": USE_SPATIAL,
        },
        "protocol": {
            "oneMogwaiProcess": True,
            "oneGBuffer": True,
            "twoLumenGIPasses": ["LumenGIFull", "LumenGIGrid"],
            "sameFrameSchedule": True,
            "strictOutputTolerance": OUTPUT_TOLERANCE,
            "comparedChannels": list(COMPARE_CHANNELS),
            "candidateStatFields": list(STATS_FIELDS),
            "wallClockTimeoutSeconds": PAIR_TIMEOUT_SECONDS,
        },
        "status": "BLOCKED",
        "renderError": render_error,
        "processId": os.getpid(),
        "captureElapsedSeconds": capture_elapsed,
        "frames": frames,
    }
    report["gate"] = _evaluate(frames, render_error)
    report["status"] = report["gate"]["status"]
    path = os.path.join(OUT_DIR, "c5-paired-equivalence.json")
    _write_json(path, report)
    try:
        m.removeGraph(graph)
        m.unloadScene()
    except Exception:
        pass
    print("C5_PAIRED_EQUIVALENCE", report["status"], path)
    if render_error:
        print("C5_PAIRED_EQUIVALENCE_ERROR", render_error)
    return 0 if report["status"] == "PASS" else 1 if report["status"] == "FAIL" else 2


if "--self-test" in sys.argv:
    sys.exit(_self_test())

sys.exit(_run())
