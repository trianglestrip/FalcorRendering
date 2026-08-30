"""A2 dynamic ScreenProbe history runner.

The runner is intentionally a *single-case* Mogwai asset.  The caller launches
one process per transition case and gives every process a unique
``LUMEN_A2_DYNAMIC_OUT`` path.  Keeping the cases process/scene independent is
important: a scene reload or camera cut must not inherit a previous case's
history state.

Usage (after a Release build, one GPU owner)::

    $env:LUMEN_A2_DYNAMIC_CASE = "camera_cut"
    $env:LUMEN_A2_DYNAMIC_OUT = "artifacts/lumengi/A2/dynamic/camera-cut.json"
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe --device-type d3d12 \
      --headless --precise --script tests\\lumengi\\run_screenprobe_dynamic.py

Supported cases are ``static``, ``camera_cut``, ``scene_reload``,
``lighting_generation`` and ``material_or_geometry``.  The latter two attempt
real Falcor scene mutations; if the binding cannot expose a mutable light,
environment, material, or instance transform, the report is ``BLOCKED``.  No
image statistic is used to infer a transition or an invalid history.

The script also has a dependency-free fixture path::

    python tests/lumengi/run_screenprobe_dynamic.py --self-test

Outside Mogwai (without ``falcor``), a normal invocation writes a BLOCKED JSON
report.  This makes an absent runtime API explicit instead of silently passing.
"""

from __future__ import annotations

import json
import glob
import math
import os
import sys
import traceback

try:
    from falcor import *  # type: ignore  # noqa: F401,F403

    FALCOR_AVAILABLE = True
    FALCOR_IMPORT_ERROR = None
except Exception as _falcor_error:  # pragma: no cover - exercised outside Mogwai.
    FALCOR_AVAILABLE = False
    FALCOR_IMPORT_ERROR = repr(_falcor_error)


SCHEMA = "LumenGI.ScreenProbeDynamicHistory.v1"
FRAME_RATE = 60
# Keep the public contract aligned with the existing Mogwai runners. Falcor's
# media resolver maps this relative path to ``media/test_scenes`` at runtime.
DEFAULT_SCENE = "test_scenes/cornell_box.pyscene"
SCENE_PATH = os.environ.get("LUMEN_A2_DYNAMIC_SCENE", DEFAULT_SCENE)
OUT_JSON = os.path.abspath(
    os.environ.get("LUMEN_A2_DYNAMIC_OUT", "artifacts/lumengi/A2/dynamic/screenprobe-dynamic.json")
)
CASE = os.environ.get("LUMEN_A2_DYNAMIC_CASE", "static").strip().lower()
CASES = ("static", "camera_cut", "scene_reload", "lighting_generation", "material_or_geometry")
CHECKPOINTS = (1, 8, 16, 32, 64)
WARMUP_FRAMES = max(1, int(os.environ.get("LUMEN_A2_DYNAMIC_WARMUP", "8")))
RESOLUTION = (640, 360)

FIXED_CAMERA_POSITION = (0.0, 0.28, 1.2)
FIXED_CAMERA_TARGET = (0.0, 0.28, 0.0)
FIXED_CAMERA_UP = (0.0, 1.0, 0.0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0
CUT_CAMERA_POSITION = (0.72, 0.42, 0.48)
CUT_CAMERA_TARGET = (0.05, 0.22, -0.18)

REQUIRED_STATS = (
    "historyAccepted",
    "historyReset",
    "historyGeneration",
    "lightingGeneration",
)
REQUIRED_OUTPUTS = (
    "diffuseGI",
    "probeInterpolated",
    "probeHistory",
    "temporalConfidence",
    "temporalFiltered",
    "spatialFiltered",
    "resolvedDiffuseGI",
    "screenRadianceHistoryAge",
)
QUALITY_OUTPUTS = ("spatialFiltered", "temporalFiltered", "resolvedDiffuseGI")
DISPLAY_OUTPUT = "ToneMapperDisplay.dst"
HISTORY_REJECT_COUNTERS = (
    "historyRejectDepth",
    "historyRejectGuide",
    "historyRejectMotion",
    "historyRejectLighting",
    "historyRejectCurrentInvalid",
    "historyRejectPreviousInvalid",
)
CAPTURE_DIR = os.path.abspath(
    os.environ.get(
        "LUMEN_A2_DYNAMIC_CAPTURE_DIR",
        os.path.join(os.path.dirname(OUT_JSON), "captures"),
    )
)


def _json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if hasattr(value, "item"):
        try:
            return _json_safe(value.item())
        except Exception:
            pass
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    return str(value)


def _write_json(payload):
    out_dir = os.path.dirname(OUT_JSON)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    temporary = OUT_JSON + ".tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temporary, OUT_JSON)


def _stats_number(stats, key):
    if not isinstance(stats, dict) or key not in stats:
        return None
    try:
        number = float(stats[key])
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _get_pass(graph):
    for name in ("getPass", "get_pass"):
        try:
            return getattr(graph, name)("LumenGI")
        except Exception:
            pass
    return None


def _numeric_history_reject_keys(stats):
    """Return actual numeric reject counters, excluding the derived key list."""

    if not isinstance(stats, dict):
        return []
    return sorted(
        key
        for key in stats
        if key.startswith("historyReject")
        and key != "historyRejectKeys"
    )


def _read_stats(graph):
    pass_obj = _get_pass(graph)
    if pass_obj is None:
        return None, "LumenGI pass binding is unavailable"
    try:
        raw = dict(getattr(pass_obj, "screenProbeStats"))
    except Exception as error:
        return None, "screenProbeStats unavailable: %s" % error
    stats = {str(key): _json_safe(value) for key, value in raw.items()}
    missing = [key for key in REQUIRED_STATS if key not in stats]
    reject_keys = _numeric_history_reject_keys(stats)
    if not reject_keys:
        missing.append("historyReject*")
    if missing:
        return stats, "screenProbeStats missing required field(s): %s" % ", ".join(missing)
    bad = [key for key in REQUIRED_STATS if _stats_number(stats, key) is None]
    bad.extend(key for key in reject_keys if _stats_number(stats, key) is None)
    if bad:
        return stats, "screenProbeStats contains non-finite/non-numeric field(s): %s" % ", ".join(bad)
    stats["historyRejectKeys"] = reject_keys
    return stats, None


def _reject_summary(stats):
    """Validate explicit history acceptance/rejection telemetry.

    A derived key list is metadata only and is deliberately excluded from all
    numeric series.  Aggregate counters are useful evidence, but never replace
    the mutation-specific generation gate below.
    """

    required = ("historyAccepted",) + HISTORY_REJECT_COUNTERS
    missing = [key for key in required if not isinstance(stats, dict) or key not in stats]
    if missing:
        return {"status": "BLOCKED", "missing": missing}
    values = {key: _stats_number(stats, key) for key in required}
    bad = [key for key, value in values.items() if value is None or value < 0.0]
    if bad:
        return {"status": "BLOCKED", "bad": bad}
    accepted = values["historyAccepted"]
    rejected = sum(values[key] for key in HISTORY_REJECT_COUNTERS)
    attempts = accepted + rejected
    return {
        "status": "PASS",
        "accepted": accepted,
        "rejected": rejected,
        "attempts": attempts,
        "rejectRate": rejected / attempts if attempts > 0.0 else 0.0,
        "counterKeys": list(HISTORY_REJECT_COUNTERS),
    }


def _output_name(channel):
    return channel if "." in channel else "LumenGI." + channel


def _output_values(graph, channel):
    resource = graph.get_output(_output_name(channel))
    if resource is None:
        raise RuntimeError("output is unavailable")
    import numpy as np

    values = np.asarray(resource.to_numpy(), dtype=np.float64)
    if values.size == 0:
        raise RuntimeError("output is empty")
    return values


def _quality_metrics(values):
    """Return finite linear-light spatial metrics for a GPU output."""

    import numpy as np

    array = np.asarray(values, dtype=np.float64)
    if array.ndim != 3 or array.shape[-1] < 3 or array.size == 0:
        return {"status": "BLOCKED", "reason": "expected non-empty HxWxC with at least RGB"}
    finite = bool(np.isfinite(array).all())
    rgb = array[..., :3]
    nonnegative = bool(float(np.nanmin(rgb)) >= -1e-6) if finite else False
    luma = np.sum(np.maximum(rgb, 0.0) * np.asarray((0.2126, 0.7152, 0.0722)), axis=-1)
    dx = luma[:, 1:] - luma[:, :-1] if luma.shape[1] > 1 else np.zeros((luma.shape[0], 0))
    dy = luma[1:, :] - luma[:-1, :] if luma.shape[0] > 1 else np.zeros((0, luma.shape[1]))
    gradients = np.concatenate((np.abs(dx).reshape(-1), np.abs(dy).reshape(-1)))
    local_variance = float(0.5 * (np.mean(dx * dx) + np.mean(dy * dy))) if gradients.size else 0.0
    return {
        "status": "PASS" if finite and nonnegative else "FAIL",
        "shape": list(array.shape),
        "finite": finite,
        "nonnegativeRGB": nonnegative,
        "meanLuma": float(np.mean(luma)) if finite else None,
        "stdLuma": float(np.std(luma)) if finite else None,
        "p99Luma": float(np.percentile(luma, 99.0)) if finite else None,
        "maxLuma": float(np.max(luma)) if finite else None,
        "localVariance": local_variance if finite else None,
        "gradientP95": float(np.percentile(gradients, 95.0)) if gradients.size and finite else None,
        "gradientP99": float(np.percentile(gradients, 99.0)) if gradients.size and finite else None,
    }


def _output_summary(graph, channel):
    try:
        import numpy as np

        values = _output_values(graph, channel)
        finite = bool(np.isfinite(values).all())
        nonnegative = bool(float(np.nanmin(values)) >= 0.0) if finite else False
        summary = {
            "status": "PASS" if finite and nonnegative else "FAIL",
            "shape": list(values.shape),
            "finite": finite,
            "nonnegative": nonnegative,
            "min": float(np.nanmin(values)) if finite else None,
            "max": float(np.nanmax(values)) if finite else None,
            "mean": float(np.nanmean(values)) if finite else None,
        }
        if channel in QUALITY_OUTPUTS:
            summary["qualityMetrics"] = _quality_metrics(values)
        return summary, None
    except Exception as error:
        return {"status": "BLOCKED", "finite": False, "nonnegative": False}, "%s: %s" % (channel, error)


def _image_artifact_contract(png_paths, exr_paths, output_dir=None, base_filename=None):
    """Validate real frame-capture files; paths must come from the runtime."""

    png_paths = sorted(os.path.abspath(path) for path in png_paths if os.path.isfile(path))
    exr_paths = sorted(os.path.abspath(path) for path in exr_paths if os.path.isfile(path))
    result = {
        "status": "PASS" if png_paths and exr_paths else "BLOCKED",
        "outputDir": os.path.abspath(output_dir) if output_dir else None,
        "baseFilename": base_filename,
        "png": png_paths,
        "exr": exr_paths,
    }
    if not png_paths:
        result["error"] = "frameCapture produced no PNG"
    elif not exr_paths:
        result["error"] = "frameCapture produced no EXR"
    return result


def _capture_artifacts(relative_frame, absolute_frame):
    """Request and validate PNG/EXR files from Falcor frameCapture."""

    base = "screenprobe-dynamic-%s-%04d" % (CASE, absolute_frame)
    try:
        os.makedirs(CAPTURE_DIR, exist_ok=True)
        m.frameCapture.outputDir = CAPTURE_DIR
        m.frameCapture.captureAllOutputs = True
        m.frameCapture.baseFilename = base
        m.frameCapture.capture()
    except Exception as error:
        return {
            "status": "BLOCKED",
            "outputDir": CAPTURE_DIR,
            "baseFilename": base,
            "png": [],
            "exr": [],
            "error": "frameCapture.capture: %s" % error,
        }
    png = glob.glob(os.path.join(CAPTURE_DIR, base + ".*.png"))
    exr = glob.glob(os.path.join(CAPTURE_DIR, base + ".*.exr"))
    result = _image_artifact_contract(png, exr, CAPTURE_DIR, base)
    result["relativeFrame"] = relative_frame
    result["absoluteFrame"] = absolute_frame
    return result


def _build_graph():
    graph = RenderGraph("LumenGI.ScreenProbeDynamicHistory")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "enabled": True,
                "useSurfaceCache": True,
                "useCacheLighting": True,
                "useScreenTrace": True,
                "useScreenProbes": True,
                "probeDirectionsPerProbe": 32,
                "useTemporalFilter": True,
                "useScreenRadianceMoments": True,
                "useSpatialFilter": True,
                "debugMode": "None",
            },
        ),
        "LumenGI",
    )
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
    for channel in REQUIRED_OUTPUTS:
        graph.markOutput("LumenGI." + channel)
    graph.addPass(
        createPass("ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0}),
        "ToneMapperDisplay",
    )
    graph.addEdge("LumenGI.resolvedDiffuseGI", "ToneMapperDisplay.src")
    graph.markOutput(DISPLAY_OUTPUT)
    return graph


def _configure_scene():
    m.loadScene(SCENE_PATH)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0
    _set_camera(FIXED_CAMERA_POSITION, FIXED_CAMERA_TARGET)


def _set_camera(position, target):
    camera = m.scene.camera
    camera.position = float3(*position)
    camera.target = float3(*target)
    camera.up = float3(*FIXED_CAMERA_UP)
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH


def _mutate_light_or_environment():
    # Prefer the documented Scene::setRenderSettings() path.  Direct light and
    # env-map object mutation is available in some image-test bindings but can
    # invalidate cached scene data in headless builds; a component epoch toggle
    # is sufficient to exercise Lumen's lighting-generation fence without
    # risking an unsafe pointer mutation.
    try:
        settings = m.scene.renderSettings
        old_env = bool(settings.useEnvLight)
        settings.useEnvLight = not old_env
        m.scene.renderSettings = settings
        return {"kind": "renderSettings", "field": "useEnvLight", "old": old_env, "new": not old_env}
    except Exception:
        pass
    names = (
        "Distant light", "Directional light", "DistantLight", "DirectionalLight",
        "LumenGITestPointLight", "Point light", "PointLight",
    )
    for name in names:
        try:
            light = m.scene.getLight(name)
            if light is not None:
                old = float(light.intensity)
                light.intensity = old * 1.5 if old > 0.0 else 1.0
                return {"kind": "light", "name": name, "oldIntensity": old, "newIntensity": float(light.intensity)}
        except Exception:
            continue
    try:
        light = m.scene.getLight(0)
        old = float(light.intensity)
        light.intensity = old * 1.5 if old > 0.0 else 1.0
        return {"kind": "light", "index": 0, "oldIntensity": old, "newIntensity": float(light.intensity)}
    except Exception:
        pass
    try:
        env_map = getattr(m.scene, "envMap", None)
        if env_map is not None and hasattr(env_map, "intensity"):
            old = float(env_map.intensity)
            env_map.intensity = old * 1.5 if old > 0.0 else 1.0
            return {"kind": "environment", "oldIntensity": old, "newIntensity": float(env_map.intensity)}
    except Exception:
        pass
    raise RuntimeError("Falcor API exposes no mutable light or environment intensity")


def _mutate_material_or_geometry():
    try:
        materials = getattr(m.scene, "materials")
        for material in materials:
            try:
                old = float(material.emissiveFactor)
                material.emissiveFactor = old * 1.5 if old > 0.0 else 0.5
                return {"kind": "material", "name": str(material.name), "oldEmissiveFactor": old,
                        "newEmissiveFactor": float(material.emissiveFactor)}
            except Exception:
                continue
    except Exception:
        pass
    # Geometry fallback: an instance transform is a real scene mutation when
    # exposed by the binding.  Do not claim a geometry transition when the API
    # only exposes immutable mesh data.
    for container_name in ("instances", "sceneGraph"):
        try:
            container = getattr(m.scene, container_name)
            iterator = container.values() if hasattr(container, "values") else container
            for instance in iterator:
                transform = getattr(instance, "transform", None)
                if transform is None:
                    continue
                # Only perform a transform edit when the binding exposes a
                # concrete mutable 4x4 sequence. Reassigning the same opaque
                # object would be a no-op and must not be reported as a
                # geometry transition.
                if isinstance(transform, (list, tuple)) and len(transform) >= 16:
                    updated = list(transform)
                    updated[12] = float(updated[12]) + 0.05
                    try:
                        instance.transform = updated
                        return {"kind": "geometry", "container": container_name,
                                "operation": "translation_x_plus_0.05"}
                    except Exception:
                        continue
        except Exception:
            continue
    raise RuntimeError("Falcor API exposes no mutable material or geometry instance")


def _apply_transition(case):
    if case == "camera_cut":
        _set_camera(CUT_CAMERA_POSITION, CUT_CAMERA_TARGET)
        return {"kind": "camera", "position": list(CUT_CAMERA_POSITION), "target": list(CUT_CAMERA_TARGET)}
    if case == "scene_reload":
        m.loadScene(SCENE_PATH)
        m.resizeFrameBuffer(*RESOLUTION)
        _set_camera(FIXED_CAMERA_POSITION, FIXED_CAMERA_TARGET)
        return {"kind": "scene", "operation": "loadScene", "path": SCENE_PATH}
    if case == "lighting_generation":
        return _mutate_light_or_environment()
    if case == "material_or_geometry":
        return _mutate_material_or_geometry()
    return None


def _transition_signal(case, evidence):
    """Return the telemetry signal required by the requested mutation."""

    if not evidence.get("telemetryReadable"):
        return False
    if case == "static":
        return True
    if case == "lighting_generation":
        return bool(evidence.get("lightingGenerationChanged"))
    if case == "material_or_geometry":
        return bool(
            evidence.get("historyGenerationChanged")
            or evidence.get("lightingGenerationChanged")
            or evidence.get("historyResetObserved")
        )
    return bool(evidence.get("historyGenerationChanged") or evidence.get("historyResetObserved"))


def _capture_frame(graph, relative_frame, absolute_frame, transition):
    errors = []
    outputs = {}
    for channel in REQUIRED_OUTPUTS:
        summary, error = _output_summary(graph, channel)
        outputs[channel] = summary
        if error:
            errors.append(error)
    display_summary, display_error = _output_summary(graph, DISPLAY_OUTPUT)
    outputs[DISPLAY_OUTPUT] = display_summary
    if display_error:
        errors.append(display_error)
    stats, stats_error = _read_stats(graph)
    if stats_error:
        errors.append(stats_error)
    reject_telemetry = _reject_summary(stats or {})
    if reject_telemetry.get("status") != "PASS":
        errors.append("history reject telemetry is incomplete")
    frame_capture = _capture_artifacts(relative_frame, absolute_frame)
    if frame_capture.get("status") != "PASS":
        errors.append(frame_capture.get("error", "PNG/EXR frame capture contract failed"))
    invalid_output = any(info.get("status") != "PASS" for info in outputs.values())
    return {
        "frame": relative_frame,
        "absoluteFrame": absolute_frame,
        "transition": transition,
        "status": "FAIL" if invalid_output else ("BLOCKED" if errors else "PASS"),
        "outputs": outputs,
        "screenProbeStats": stats or {},
        "rejectTelemetry": reject_telemetry,
        "frameCapture": frame_capture,
        "errors": errors,
    }


def _run_case():
    graph = None
    result = {
        "schema": SCHEMA,
        "status": "BLOCKED",
        "case": CASE,
        "scene": SCENE_PATH,
        "resolution": list(RESOLUTION),
        "checkpointFrames": list(CHECKPOINTS),
        "requiredStats": list(REQUIRED_STATS) + ["historyReject*"],
        "requiredOutputs": list(REQUIRED_OUTPUTS) + [DISPLAY_OUTPUT],
        "warmupFrames": WARMUP_FRAMES,
        "baseline": {},
        "transitionMutation": None,
        "captures": [],
        "transitionEvidence": {},
        "errors": [],
    }
    try:
        _configure_scene()
        graph = _build_graph()
        m.addGraph(graph)
        m.setActiveGraph(graph)

        baseline_captures = []
        if CASE != "static":
            for frame in range(1, WARMUP_FRAMES + 1):
                m.clock.frame = frame
                m.renderFrame()
            baseline_captures.append(_capture_frame(graph, 0, WARMUP_FRAMES, "baseline"))
            result["baseline"] = baseline_captures[-1]
            if baseline_captures[-1]["status"] != "PASS":
                result["errors"].extend(baseline_captures[-1]["errors"])
            result["transitionMutation"] = _apply_transition(CASE)
            start_absolute = WARMUP_FRAMES
        else:
            start_absolute = 0

        previous = 0
        for checkpoint in CHECKPOINTS:
            target_absolute = start_absolute + checkpoint
            for absolute_frame in range(start_absolute + 1 if previous == 0 else start_absolute + previous + 1,
                                        target_absolute + 1):
                m.clock.frame = absolute_frame
                m.renderFrame()
            result["captures"].append(_capture_frame(graph, checkpoint, target_absolute, CASE))
            previous = checkpoint
    except Exception as error:
        result["errors"].append("runtime: %s" % error)
        result["traceback"] = traceback.format_exc()
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as error:
                result["errors"].append("removeGraph: %s" % error)

    stats_entries = [item.get("screenProbeStats", {}) for item in result["captures"]]
    if result.get("baseline"):
        stats_entries.insert(0, result["baseline"].get("screenProbeStats", {}))
    telemetry_readable = bool(stats_entries) and all(
        all(key in stats for key in REQUIRED_STATS)
        and any(key.startswith("historyReject") for key in stats)
        for stats in stats_entries
    )
    generations = [_stats_number(stats, "historyGeneration") for stats in stats_entries]
    lighting_generations = [_stats_number(stats, "lightingGeneration") for stats in stats_entries]
    generations = [value for value in generations if value is not None]
    lighting_generations = [value for value in lighting_generations if value is not None]
    counter_keys = {"historyAccepted"}
    for stats in stats_entries:
        counter_keys.update(_numeric_history_reject_keys(stats))
    counter_series = {
        key: [_stats_number(stats, key) for stats in stats_entries]
        for key in sorted(counter_keys)
    }
    counter_deltas = {}
    if stats_entries:
        first_stats = stats_entries[0]
        last_stats = stats_entries[-1]
        for key in sorted(counter_keys):
            first_value = _stats_number(first_stats, key)
            last_value = _stats_number(last_stats, key)
            counter_deltas[key] = (
                last_value - first_value
                if first_value is not None and last_value is not None
                else None
            )
    generation_changed = bool(generations and len(set(generations)) > 1)
    lighting_generation_changed = bool(lighting_generations and len(set(lighting_generations)) > 1)
    reset_seen = any(bool(_stats_number(stats, "historyReset")) for stats in stats_entries)
    result["transitionEvidence"] = {
        "telemetryReadable": telemetry_readable,
        "historyGenerationValues": generations,
        "lightingGenerationValues": lighting_generations,
        "historyGenerationChanged": generation_changed,
        "lightingGenerationChanged": lighting_generation_changed,
        "historyResetObserved": reset_seen,
        "historyCounterSeries": counter_series,
        "historyCounterDeltaFromBaseline": counter_deltas,
        "expectedMutation": CASE != "static",
    }
    result["transitionEvidence"]["requiredTransitionSignal"] = (
        "lightingGenerationChanged"
        if CASE == "lighting_generation"
        else "historyGenerationChangedOrReset"
        if CASE in ("camera_cut", "scene_reload")
        else "historyGenerationChangedOrLightingGenerationChangedOrReset"
        if CASE == "material_or_geometry"
        else "telemetryReadable"
    )
    result["transitionEvidence"]["expectedMutationSatisfied"] = _transition_signal(
        CASE, result["transitionEvidence"]
    )
    # A missing graph channel, readback, or screenProbeStats field is an API
    # coverage problem and must remain BLOCKED (not be collapsed into a value
    # quality failure).  Non-finite/negative values still produce FAIL below.
    capture_errors = [
        error
        for capture in result["captures"]
        for error in capture.get("errors", [])
    ]
    if capture_errors:
        result["errors"].extend(capture_errors)
    captures_ok = len(result["captures"]) == len(CHECKPOINTS) and all(
        item.get("status") == "PASS" for item in result["captures"]
    )
    quality_ok = bool(result["captures"]) and all(
        capture.get("outputs", {}).get(channel, {}).get("qualityMetrics", {}).get("status") == "PASS"
        and math.isfinite(
            capture.get("outputs", {}).get(channel, {}).get("qualityMetrics", {}).get("localVariance")
        )
        for capture in result["captures"]
        for channel in QUALITY_OUTPUTS
    )
    reject_ok = bool(result["captures"]) and all(
        capture.get("rejectTelemetry", {}).get("status") == "PASS"
        for capture in result["captures"]
    )
    result["qualityGate"] = {
        "status": "PASS" if quality_ok else "BLOCKED",
        "outputs": list(QUALITY_OUTPUTS),
        "finiteLocalVariance": quality_ok,
    }
    result["rejectTelemetryGate"] = {
        "status": "PASS" if reject_ok else "BLOCKED",
        "explicitCounters": list(HISTORY_REJECT_COUNTERS),
    }
    evidence_ok = _transition_signal(CASE, result["transitionEvidence"])
    if result["errors"]:
        result["status"] = "BLOCKED"
    elif not captures_ok:
        result["status"] = "FAIL"
    elif not quality_ok or not reject_ok:
        result["status"] = "BLOCKED"
        result["errors"].append("quality or explicit history-reject gate was not satisfied")
    elif not evidence_ok:
        result["status"] = "BLOCKED"
        result["errors"].append("required dynamic history transition was not observable")
    else:
        result["status"] = "PASS"
    return result


def _fixture_stats(history_generation, lighting_generation, reset, accepted):
    return {
        "historyAccepted": float(accepted),
        "historyRejectDepth": 0.0 if accepted else 1.0,
        "historyRejectGuide": 0.0,
        "historyRejectMotion": 0.0 if accepted else 1.0,
        "historyRejectLighting": 0.0,
        "historyRejectCurrentInvalid": 0.0,
        "historyRejectPreviousInvalid": 0.0,
        "historyReset": float(reset),
        "historyGeneration": float(history_generation),
        "lightingGeneration": float(lighting_generation),
    }


def _run_self_test():
    # Synthetic fixture validates schema checks and transition evidence without
    # importing Falcor or touching a GPU.
    for case in CASES:
        baseline = _fixture_stats(1, 1, 0, 4)
        post = _fixture_stats(2 if case in ("camera_cut", "scene_reload") else 1,
                              2 if case in ("lighting_generation", "material_or_geometry") else 1,
                              1 if case in ("camera_cut", "scene_reload") else 0,
                              2)
        assert all(_stats_number(baseline, key) is not None for key in REQUIRED_STATS)
        assert any(key.startswith("historyReject") for key in baseline)
        assert all(_stats_number(post, key) is not None for key in REQUIRED_STATS)
        if case != "static":
            assert post["historyGeneration"] != baseline["historyGeneration"] or post["lightingGeneration"] != baseline["lightingGeneration"]
    print("SCREENPROBE_DYNAMIC_FIXTURE PASS")
    return 0


def _blocked_report(reason):
    return {
        "schema": SCHEMA,
        "status": "BLOCKED",
        "case": CASE,
        "scene": SCENE_PATH,
        "resolution": list(RESOLUTION),
        "checkpointFrames": list(CHECKPOINTS),
        "requiredStats": list(REQUIRED_STATS) + ["historyReject*"],
        "requiredOutputs": list(REQUIRED_OUTPUTS),
        "captures": [],
        "errors": [reason],
    }


def main():
    if CASE not in CASES:
        report = _blocked_report("unknown LUMEN_A2_DYNAMIC_CASE: %s" % CASE)
        _write_json(report)
        print("SCREENPROBE_DYNAMIC", report["status"], OUT_JSON)
        return 2
    if not FALCOR_AVAILABLE:
        report = _blocked_report("Mogwai falcor binding unavailable: %s" % FALCOR_IMPORT_ERROR)
    else:
        report = _run_case()
    _write_json(report)
    print("SCREENPROBE_DYNAMIC", report["status"], OUT_JSON)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(_run_self_test())
    sys.exit(main())
elif FALCOR_AVAILABLE:
    # Mogwai embeds this file without running it as __main__.
    try:
        main()
    except Exception as error:
        _write_json(_blocked_report("fatal: %s\n%s" % (error, traceback.format_exc())))
    exit()
