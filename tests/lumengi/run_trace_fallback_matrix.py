"""C3 trace-mode/fallback matrix for LumenGI.

This GPU/Mogwai asset exercises all three ``TraceMode`` values against the
``useGDF``, ``useSurfaceCache`` and ``useScreenProbes`` feature switches.  It
is intentionally diagnostic rather than a quality gate: a failed graph or
render is recorded and the next matrix case is attempted.  The report keeps
black-frame, NaN/Inf and missing-output evidence separate from Python/GPU
exceptions so a fallback can be diagnosed without guessing from a screenshot.

The script does not build, launch a subprocess, or mutate source files.  Root
must run it on the single GPU with a unique output directory.

Environment overrides:

``LUMEN_TRACE_MATRIX_OUT`` (or ``LUMEN_C3_OUT``)
    Output directory for the JSON report and optional captures.
``LUMEN_TRACE_MATRIX_RESOLUTION`` (or ``LUMEN_C3_RESOLUTION``)
    ``WIDTHxHEIGHT`` or ``WIDTH,HEIGHT``; default ``640x360``.
``LUMEN_TRACE_MATRIX_MODES`` (or ``LUMEN_C3_MODES``)
    Comma-separated ``HardwareRT,MeshSDF,Hybrid`` subset; declaration order
    is preserved and the default is all three.
``LUMEN_TRACE_MATRIX_FEATURES`` (or ``LUMEN_C3_FEATURES``)
    Comma-separated feature-case names; default is all eight combinations.
``LUMEN_TRACE_MATRIX_WARMUP``
    Number of frames rendered per case before sampling; default ``4``.
``LUMEN_TRACE_MATRIX_CAPTURE``
    ``0``/``false`` disables one-frame captures; enabled by default.
"""

from falcor import *

import json
import math
import os
import re
import traceback

import numpy as np


FRAME_RATE = 60
DEFAULT_RESOLUTION = (640, 360)
SCENE = "test_scenes/cornell_box.pyscene"
TRACE_MODES = ("HardwareRT", "MeshSDF", "Hybrid")

# Declared order is the execution contract.  Keep the feature dictionary
# explicit so each C3 manifest is reproducible and easy to compare in logs.
FEATURE_CASES = (
    ("none", {"useGDF": False, "useSurfaceCache": False, "useScreenProbes": False}),
    ("gdf", {"useGDF": True, "useSurfaceCache": False, "useScreenProbes": False}),
    ("surface_cache", {"useGDF": False, "useSurfaceCache": True, "useScreenProbes": False}),
    ("screen_probes", {"useGDF": False, "useSurfaceCache": False, "useScreenProbes": True}),
    ("gdf_surface_cache", {"useGDF": True, "useSurfaceCache": True, "useScreenProbes": False}),
    ("gdf_screen_probes", {"useGDF": True, "useSurfaceCache": False, "useScreenProbes": True}),
    ("surface_cache_screen_probes", {"useGDF": False, "useSurfaceCache": True, "useScreenProbes": True}),
    ("all", {"useGDF": True, "useSurfaceCache": True, "useScreenProbes": True}),
)


def _parse_resolution(value):
    if not value:
        return DEFAULT_RESOLUTION
    try:
        tokens = value.lower().replace("x", ",").split(",")
        if len(tokens) != 2:
            raise ValueError("expected WIDTHxHEIGHT or WIDTH,HEIGHT")
        width, height = (int(token.strip()) for token in tokens)
        if width <= 0 or height <= 0:
            raise ValueError("dimensions must be positive")
        return (width, height)
    except Exception as exc:
        print("C3_CONFIG invalid resolution", repr(value), "using", DEFAULT_RESOLUTION, repr(exc))
        return DEFAULT_RESOLUTION


def _parse_positive_int(value, fallback):
    try:
        parsed = int(value)
        return parsed if parsed > 0 else fallback
    except Exception:
        return fallback


def _select_declared(value, declared, kind):
    """Select a declared-order subset, warning on unknown names."""
    if not value or not value.strip():
        return tuple(declared)
    requested = {token.strip() for token in value.split(",") if token.strip()}
    known = {name for name in declared}
    unknown = sorted(requested - known)
    if unknown:
        print("C3_CONFIG unknown", kind, "ignored", json.dumps(unknown, sort_keys=True))
    selected = tuple(name for name in declared if name in requested)
    if not selected:
        print("C3_CONFIG no known", kind, "selected from", repr(value))
    return selected


RESOLUTION = _parse_resolution(
    os.environ.get("LUMEN_TRACE_MATRIX_RESOLUTION")
    or os.environ.get("LUMEN_C3_RESOLUTION")
)
OUT_DIR = os.path.abspath(
    os.environ.get("LUMEN_TRACE_MATRIX_OUT")
    or os.environ.get("LUMEN_C3_OUT")
    or "artifacts/lumengi/C3/trace-fallback-matrix"
)
WARMUP_FRAMES = _parse_positive_int(os.environ.get("LUMEN_TRACE_MATRIX_WARMUP"), 4)
CAPTURE_ENABLED = os.environ.get("LUMEN_TRACE_MATRIX_CAPTURE", "1").strip().lower() not in (
    "0",
    "false",
    "off",
    "no",
)
OUTPUT_LABEL = os.environ.get("LUMEN_TRACE_MATRIX_LABEL", "c3-trace-fallback")

_mode_filter = os.environ.get("LUMEN_TRACE_MATRIX_MODES") or os.environ.get("LUMEN_C3_MODES") or ""
_feature_filter = (
    os.environ.get("LUMEN_TRACE_MATRIX_FEATURES")
    or os.environ.get("LUMEN_C3_FEATURES")
    or ""
)
SELECTED_MODES = _select_declared(_mode_filter, TRACE_MODES, "modes")
SELECTED_FEATURES = _select_declared(_feature_filter, tuple(name for name, _ in FEATURE_CASES), "features")
FEATURES_BY_NAME = dict(FEATURE_CASES)


def _safe_label(value):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", str(value)).strip("_") or "case"


def _build_graph(trace_mode, features):
    options = {
        "enabled": True,
        "traceMode": trace_mode,
        "qualityPreset": "High",
    }
    options.update(features)
    # These options are consumed only when useGDF is true, but keeping them in
    # the manifest/graph makes MeshSDF and Hybrid cases comparable.
    options.update(
        {
            "meshSDFResolution": 32,
            "gdfLevelCount": 2,
            "gdfResolution": 64,
            "gdfBaseExtent": 4.0,
            "gdfTraceMaxSteps": 32,
            "gdfTraceMaxDistance": 20.0,
            "gdfEmptyDistanceScale": 8.0,
        }
    )

    graph = RenderGraph(OUTPUT_LABEL + "Graph")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", options), "LumenGI")
    for channel in (
        "vbuffer",
        "linearZ",
        "mvec",
        "mvecW",
        "normWRoughnessMaterialID",
        "viewW",
        "diffuseOpacity",
        "emissive",
    ):
        graph.addEdge("GBufferRT." + channel, "LumenGI." + channel)

    # Mark the primary/fallback channels and optional diagnostics in every
    # case.  A disabled stage then produces an explicit cleared/black channel
    # rather than an absent output, which is important for fallback diagnosis.
    for channel in (
        "diffuseGI",
        "diffuseRadianceHitDist",
        "confidence",
        "gdfTrace",
        "screenTrace",
        "probeRadiance",
        "probeInterpolated",
        "probeHistory",
        "resolvedDiffuseGI",
        "temporalMoments",
        "filteredVariance",
    ):
        graph.markOutput("LumenGI." + channel)
    return graph


def _setup_scene():
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.resizeFrameBuffer(*RESOLUTION)


def _json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    return str(value)


def _write_report(records):
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, "trace-fallback-matrix.json")
    temp = path + ".tmp"
    payload = {
        "script": "run_trace_fallback_matrix.py",
        "output_label": OUTPUT_LABEL,
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "warmup_frames": WARMUP_FRAMES,
        "selected_modes": list(SELECTED_MODES),
        "selected_features": list(SELECTED_FEATURES),
        "capture_enabled": CAPTURE_ENABLED,
        "records": records,
    }
    with open(temp, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temp, path)
    print("C3_SUMMARY wrote", path)


def _texture_stats(channel):
    """Return finite/black/NaN evidence for one marked graph output."""
    try:
        array = np.asarray(m.activeGraph.get_output("LumenGI." + channel).to_numpy(), dtype=np.float32)
    except Exception as exc:
        return {"available": False, "error": repr(exc)}
    if array.size == 0:
        return {
            "available": True,
            "shape": list(array.shape),
            "finite": True,
            "nan_count": 0,
            "inf_count": 0,
            "black": True,
            "nonzero_fraction": 0.0,
            "min": 0.0,
            "max": 0.0,
            "mean": 0.0,
        }
    finite_mask = np.isfinite(array)
    rgb = array[..., :3] if array.ndim >= 3 and array.shape[-1] >= 3 else array
    rgb_abs = np.abs(rgb)
    finite_values = array[finite_mask]
    minimum = float(finite_values.min()) if finite_values.size else None
    maximum = float(finite_values.max()) if finite_values.size else None
    mean = float(finite_values.mean()) if finite_values.size else None
    black = bool(np.all(rgb_abs <= 1.0e-6)) if rgb_abs.size else True
    nonzero_fraction = float(np.count_nonzero(rgb_abs > 1.0e-6)) / float(rgb_abs.size) if rgb_abs.size else 0.0
    return {
        "available": True,
        "shape": list(array.shape),
        "finite": bool(finite_mask.all()),
        "nan_count": int(np.isnan(array).sum()),
        "inf_count": int(np.isinf(array).sum()),
        "black": black,
        "nonzero_fraction": nonzero_fraction,
        "min": minimum,
        "max": maximum,
        "mean": mean,
    }


def _capture(case_label, record):
    if not CAPTURE_ENABLED:
        record["capture"] = "disabled"
        return
    try:
        os.makedirs(OUT_DIR, exist_ok=True)
        m.frameCapture.outputDir = OUT_DIR
        m.frameCapture.baseFilename = _safe_label(case_label)
        m.frameCapture.capture()
        record["capture"] = "requested"
    except Exception as exc:
        record["capture"] = "EXC"
        record["capture_error"] = repr(exc)
        print("C3_CAPTURE EXC", case_label, repr(exc))


def _run_case(trace_mode, feature_name):
    features = dict(FEATURES_BY_NAME[feature_name])
    case_label = "%s-%s-%s" % (OUTPUT_LABEL, trace_mode, feature_name)
    manifest = {
        "script": "run_trace_fallback_matrix.py",
        "case": case_label,
        "trace_mode": trace_mode,
        "feature_name": feature_name,
        "features": features,
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "warmup_frames": WARMUP_FRAMES,
        "fallback_contract": (
            "MeshSDF/Hybrid with useGDF=false exercise the pass HWRT fallback; "
            "HardwareRT/Hybrid with useGDF=true retain HWRT primary and expose gdfTrace"
        ),
    }
    print("C3_MANIFEST", json.dumps(manifest, sort_keys=True))
    record = dict(manifest)
    graph = None
    try:
        graph = _build_graph(trace_mode, features)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        m.loadScene(SCENE)
        _setup_scene()
        for frame in range(1, WARMUP_FRAMES + 1):
            m.clock.frame = frame
            m.renderFrame()

        outputs = {
            channel: _texture_stats(channel)
            for channel in (
                "diffuseGI",
                "diffuseRadianceHitDist",
                "confidence",
                "gdfTrace",
                "screenTrace",
                "probeRadiance",
                "probeInterpolated",
                "resolvedDiffuseGI",
            )
        }
        record["outputs"] = outputs
        try:
            record["gdf_stats"] = _json_safe(m.activeGraph.getPass("LumenGI").gdfStats)
        except Exception as exc:
            record["gdf_stats"] = {"available": False, "error": repr(exc)}
        record["status"] = "OK"
        diffuse = outputs["diffuseGI"]
        record["black_frame"] = bool(diffuse.get("black", False))
        record["missing_outputs"] = [
            channel for channel, stats in outputs.items() if not bool(stats.get("available", False))
        ]
        record["nan_or_inf"] = any(
            bool(stats.get("available")) and not bool(stats.get("finite"))
            for stats in outputs.values()
        )
        if record["nan_or_inf"]:
            record["status"] = "NAN_OR_INF"
        elif record["black_frame"]:
            record["status"] = "BLACK_FRAME"
        elif record["missing_outputs"]:
            record["status"] = "MISSING_OUTPUT"
        _capture(case_label, record)
        print(
            "C3_CASE",
            case_label,
            record["status"],
            "black_frame",
            record["black_frame"],
            "nan_or_inf",
            record["nan_or_inf"],
            "missing_outputs",
            len(record["missing_outputs"]),
        )
    except Exception as exc:
        record["status"] = "EXC"
        record["exception"] = repr(exc)
        record["traceback"] = traceback.format_exc()
        print("C3_CASE", case_label, "EXC", repr(exc))
        traceback.print_exc()
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as exc:
                print("C3_CLEANUP removeGraph EXC", case_label, repr(exc))
    return record


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print(
        "C3_CONFIG",
        json.dumps(
            {
                "output_dir": OUT_DIR,
                "output_label": OUTPUT_LABEL,
                "scene": SCENE,
                "resolution": list(RESOLUTION),
                "warmup_frames": WARMUP_FRAMES,
                "modes": list(SELECTED_MODES),
                "features": list(SELECTED_FEATURES),
                "capture_enabled": CAPTURE_ENABLED,
            },
            sort_keys=True,
        ),
    )
    records = []
    for trace_mode in SELECTED_MODES:
        for feature_name in SELECTED_FEATURES:
            records.append(_run_case(trace_mode, feature_name))
    _write_report(records)
    print("C3_DONE", "cases", len(records), "output_dir", OUT_DIR)


main()
exit()
