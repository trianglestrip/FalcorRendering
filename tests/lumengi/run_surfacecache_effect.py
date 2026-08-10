"""C6 runtime gate: Surface Cache lookup effect and invalidation/budget matrix.

This is a Mogwai-only runner.  It keeps one fixed scene, camera, resolution and
frame-index seed schedule while executing four isolated cases in series:

``lookup_on``
    ``useSurfaceCache=True`` and ``useCacheLighting=True``.  This is the
    cache-backed screen-probe lookup path (``LUMEN_GI_PROBE_CACHE_LOOKUP``).
``lookup_off``
    Both cache switches are disabled.  This is the HWRT/screen-probe control.
``invalidate``
    Cache lookup is enabled, then the same scene is loaded again halfway through
    the checkpoint schedule.  The report records the post-reload cache reset and
    compares the re-warmed output with ``lookup_on``.
``low_budget``
    Cache lookup is enabled with ``captureMaxPagesPerFrame=1``.  This exercises
    the bounded capture path and records whether coverage/energy changes while
    keeping all outputs finite and non-negative.

The LumenGI pass has no host seed property.  Reproducibility is therefore defined
by the same scene/camera and the same explicit frame-index sequence in every
case; the JSON records this as ``seed_schedule`` instead of pretending that an
unbound ``seed`` property was applied.

The script never edits host/shader code and does not run a build.  Run it after a
Release build through Mogwai, with a unique ``LUMEN_C6_OUT`` directory per run.

Environment (optional):

    LUMEN_C6_OUT=artifacts/lumengi/C6/surfacecache-effect.json
    LUMEN_C6_SCENE=test_scenes/cornell_box.pyscene
    LUMEN_C6_RESOLUTION=640x360
    LUMEN_C6_CHECKPOINTS=1,8,16
    LUMEN_C6_INVALIDATE_AT=8
    LUMEN_C6_LOW_BUDGET=1
    LUMEN_C6_CASES=lookup_on,lookup_off,invalidate,low_budget
    LUMEN_C6_SAVE_ARRAYS=0

The numeric gate is intentionally diagnostic rather than a fake pass threshold:
all channels must be finite/non-negative, cache stats must be available and
valid, and each enabled-vs-control comparison must report a finite difference.
The C6 plan owns the final image-quality threshold once the cache lookup contract
is frozen.
"""

from falcor import *

import json
import math
import os

import numpy as np


FRAME_RATE = 60
DEFAULT_RESOLUTION = (640, 360)
DEFAULT_CHECKPOINTS = (1, 8, 16)
DEFAULT_CASES = ("lookup_on", "lookup_off", "invalidate", "low_budget")

SCENE = os.environ.get("LUMEN_C6_SCENE", "test_scenes/cornell_box.pyscene")
OUT_JSON = os.environ.get(
    "LUMEN_C6_OUT", "artifacts/lumengi/C6/surfacecache-effect.json"
)
SAVE_ARRAYS = os.environ.get("LUMEN_C6_SAVE_ARRAYS", "0") == "1"
LOW_BUDGET = max(1, int(os.environ.get("LUMEN_C6_LOW_BUDGET", "1")))
INVALIDATE_AT = max(1, int(os.environ.get("LUMEN_C6_INVALIDATE_AT", "8")))


def _parse_resolution(value):
    if not value:
        return DEFAULT_RESOLUTION
    try:
        width, height = (int(part) for part in value.lower().replace(" ", "").split("x", 1))
        if width > 0 and height > 0:
            return width, height
    except Exception:
        pass
    print("C6 WARNING invalid resolution; using", DEFAULT_RESOLUTION)
    return DEFAULT_RESOLUTION


def _parse_checkpoints(value):
    if not value:
        return DEFAULT_CHECKPOINTS
    result = []
    for token in value.split(","):
        try:
            frame = int(token.strip())
            if frame > 0:
                result.append(frame)
        except Exception:
            print("C6 WARNING invalid checkpoint", repr(token))
    return tuple(sorted(set(result))) or DEFAULT_CHECKPOINTS


def _parse_cases(value):
    if not value:
        return DEFAULT_CASES
    allowed = set(DEFAULT_CASES)
    result = tuple(token.strip() for token in value.split(",") if token.strip() in allowed)
    return result or DEFAULT_CASES


RESOLUTION = _parse_resolution(os.environ.get("LUMEN_C6_RESOLUTION"))
CHECKPOINTS = _parse_checkpoints(os.environ.get("LUMEN_C6_CHECKPOINTS"))
CASES = _parse_cases(os.environ.get("LUMEN_C6_CASES"))

FIXED_CAMERA_POSITION = float3(0, 0.28, 1.2)
FIXED_CAMERA_TARGET = float3(0, 0.28, 0)
FIXED_CAMERA_UP = float3(0, 1, 0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0

OUTPUTS = ("diffuseGI", "resolvedDiffuseGI")
STAT_KEYS = (
    "frameIndex",
    "useSurfaceCache",
    "useCacheLighting",
    "maxPagesPerFrame",
    "cards",
    "dirtyCards",
    "totalPages",
    "allocatedPages",
    "freePages",
    "coverage",
    "residentBytesMB",
    "allocations",
    "releases",
    "evictions",
    "invalidations",
    "schedCaptureCommands",
    "schedAllocations",
    "schedRecaptures",
    "schedAllocFailures",
    "schedStarvationFrames",
    "schedReleases",
    "schedLostPages",
    "schedCompletedCaptures",
    "pendingQueueDepth",
    "lastRequestedCards",
    "lastCaptureCommands",
    "lastNewPageAllocations",
    "lastRecaptureWithPage",
    "lastAllocFailures",
    "lastBudgetCappedCards",
    "lastInFlightCards",
    "lastPendingCards",
    "lastStarvationFrames",
    "lastReleasedPages",
    "lastLostPages",
    "lastTouchCalls",
    "cacheLightingActive",
    "cacheLightingPagesLit",
)


def _json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, np.generic):
        return _json_safe(value.item())
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    return str(value)


def _write_json(path, payload):
    path = os.path.abspath(path)
    out_dir = os.path.dirname(path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    temp_path = path + ".tmp"
    with open(temp_path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temp_path, path)


def _configure_for_case(label):
    """Return immutable LumenGI properties for one isolated case."""
    if label == "lookup_off":
        return {
            "useSurfaceCache": False,
            "useCacheLighting": False,
            "captureMaxPagesPerFrame": 64,
        }
    if label == "low_budget":
        return {
            "useSurfaceCache": True,
            "useCacheLighting": True,
            "captureMaxPagesPerFrame": LOW_BUDGET,
        }
    # lookup_on and invalidate share the production cache configuration.
    return {
        "useSurfaceCache": True,
        "useCacheLighting": True,
        "captureMaxPagesPerFrame": 64,
    }


def _gbuffer_edges(graph):
    for source, destination in (
        ("vbuffer", "vbuffer"),
        ("linearZ", "linearZ"),
        ("mvec", "mvec"),
        ("mvecW", "mvecW"),
        ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"),
        ("diffuseOpacity", "diffuseOpacity"),
        ("emissive", "emissive"),
    ):
        graph.addEdge("GBufferRT." + source, "LumenGI." + destination)


def _make_graph(label, properties):
    graph = RenderGraph("LumenGIC6_%s" % label)
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    # Probes are enabled because this is the consumer of the Surface Cache
    # lookup define.  Filters stay off so this gate isolates cache lookup from
    # temporal/spatial convergence.
    lumen_properties = dict(properties)
    lumen_properties.update(
        {
            "enabled": True,
            "useScreenTrace": True,
            "useScreenProbes": True,
            "useTemporalFilter": False,
            "useSpatialFilter": False,
        }
    )
    graph.addPass(createPass("LumenGI", lumen_properties), "LumenGI")
    _gbuffer_edges(graph)
    for output_name in OUTPUTS + ("confidence",):
        graph.markOutput("LumenGI." + output_name)
    return graph


def _setup_scene():
    m.loadScene(SCENE)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0
    camera = m.scene.camera
    camera.position = FIXED_CAMERA_POSITION
    camera.target = FIXED_CAMERA_TARGET
    camera.up = FIXED_CAMERA_UP
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH


def _read_stats():
    try:
        raw = dict(m.activeGraph.getPass("LumenGI").surfaceCacheStats)
    except Exception as exc:
        return None, "surfaceCacheStats unavailable: %s" % str(exc)
    stats = {}
    for key in STAT_KEYS:
        if key in raw:
            try:
                stats[key] = float(raw[key])
            except Exception:
                stats[key] = None
    # Keep any newly exposed telemetry in the JSON without making the gate
    # depend on a future key list update.
    for key, value in raw.items():
        if key not in stats:
            try:
                stats[str(key)] = float(value)
            except Exception:
                stats[str(key)] = None
    return stats, None


def _summarize_array(array):
    arr = np.asarray(array, dtype=np.float32)
    rgb = arr[..., :3] if arr.ndim >= 3 and arr.shape[-1] >= 3 else arr
    finite = bool(np.isfinite(rgb).all())
    if rgb.size:
        min_value = float(np.min(rgb))
        max_value = float(np.max(rgb))
        mean_value = float(np.mean(rgb))
        nonnegative = bool(min_value >= 0.0)
        nonzero_fraction = float(np.count_nonzero(rgb > 0.0)) / float(rgb.size)
    else:
        min_value = max_value = mean_value = 0.0
        nonnegative = True
        nonzero_fraction = 0.0
    return {
        "shape": list(arr.shape),
        "min": min_value,
        "max": max_value,
        "mean": mean_value,
        "nonzeroFraction": nonzero_fraction,
        "finite": finite,
        "nonnegative": nonnegative,
    }


def _read_outputs():
    arrays = {}
    summaries = {}
    errors = []
    for output_name in OUTPUTS:
        try:
            array = np.asarray(m.activeGraph.get_output("LumenGI." + output_name).to_numpy(), dtype=np.float32)
            arrays[output_name] = array
            summaries[output_name] = _summarize_array(array)
        except Exception as exc:
            errors.append("%s: %s" % (output_name, str(exc)))
    return arrays, summaries, errors


def _sample(label, frame, phase):
    arrays, outputs, output_errors = _read_outputs()
    stats, stats_error = _read_stats()
    sample = {
        "label": label,
        "phase": phase,
        "frame": int(frame),
        "outputs": outputs,
        "stats": stats,
        "outputErrors": output_errors,
        "statsError": stats_error,
        "_arrays": arrays,
    }
    print(
        "C6_SAMPLE",
        label,
        phase,
        "frame",
        frame,
        "diffuseMean",
        outputs.get("diffuseGI", {}).get("mean"),
        "resolvedMean",
        outputs.get("resolvedDiffuseGI", {}).get("mean"),
        "allocatedPages",
        stats.get("allocatedPages") if stats else None,
    )
    return sample


def _sample_schedule(label, phase="warmup", checkpoints=None):
    checkpoints = CHECKPOINTS if checkpoints is None else tuple(checkpoints)
    samples = []
    max_frame = max(checkpoints)
    checkpoint_set = set(checkpoints)
    for frame in range(1, max_frame + 1):
        m.clock.frame = frame
        m.renderFrame()
        if frame in checkpoint_set:
            samples.append(_sample(label, frame, phase))
    return samples


def _save_arrays(case_label, sample):
    if not SAVE_ARRAYS:
        return {}
    case_dir = os.path.splitext(os.path.abspath(OUT_JSON))[0] + "_arrays"
    os.makedirs(case_dir, exist_ok=True)
    paths = {}
    for output_name, array in sample.get("_arrays", {}).items():
        path = os.path.join(case_dir, "%s_%s_f%03d.npy" % (case_label, output_name, sample["frame"]))
        np.save(path, array)
        paths[output_name] = path
    return paths


def _strip_internal(sample):
    result = dict(sample)
    result.pop("_arrays", None)
    return result


def _run_case(label):
    properties = _configure_for_case(label)
    graph = None
    result = {
        "case": label,
        "properties": properties,
        "status": "in-progress",
        "samples": [],
        "reload": None,
        "error": None,
    }
    try:
        graph = _make_graph(label, properties)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene()

        if label == "invalidate":
            # Render the same frame-index schedule before the reload, then
            # reload and replay it from frame 1.  This makes the reset and
            # re-warm behavior directly comparable to lookup_on.
            pre_checkpoints = tuple(frame for frame in CHECKPOINTS if frame <= INVALIDATE_AT)
            if not pre_checkpoints:
                pre_checkpoints = (INVALIDATE_AT,)
            pre_samples = _sample_schedule(label, phase="before_reload", checkpoints=pre_checkpoints)
            before = pre_samples[-1] if pre_samples else None
            m.loadScene(SCENE)
            m.resizeFrameBuffer(*RESOLUTION)
            m.clock.frame = 0
            camera = m.scene.camera
            camera.position = FIXED_CAMERA_POSITION
            camera.target = FIXED_CAMERA_TARGET
            camera.up = FIXED_CAMERA_UP
            camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH
            post_samples = _sample_schedule(label, phase="after_reload")
            after = post_samples[-1] if post_samples else None
            after_first = post_samples[0] if post_samples else None
            result["samples"] = [_strip_internal(sample) for sample in pre_samples + post_samples]
            if before and after:
                result["reload"] = {
                    "invalidateAt": INVALIDATE_AT,
                    "beforeFrameIndex": before.get("stats", {}).get("frameIndex"),
                    "afterFrameIndex": after_first.get("stats", {}).get("frameIndex") if after_first else None,
                    "resetObserved": (
                        before.get("stats", {}).get("frameIndex") is not None
                        and after_first is not None
                        and after_first.get("stats", {}).get("frameIndex") is not None
                        and after_first["stats"]["frameIndex"] < before["stats"]["frameIndex"]
                    ),
                    "beforeAllocatedPages": before.get("stats", {}).get("allocatedPages"),
                    "afterAllocatedPages": after.get("stats", {}).get("allocatedPages"),
                    "afterStats": after.get("stats"),
                }
            final_sample = after
        else:
            samples = _sample_schedule(label)
            result["samples"] = [_strip_internal(sample) for sample in samples]
            final_sample = samples[-1] if samples else None

        if final_sample is not None:
            result["finalArrays"] = _save_arrays(label, final_sample)
            final_outputs = final_sample.get("outputs", {})
            final_stats = final_sample.get("stats")
            required_stats = ("frameIndex", "maxPagesPerFrame", "allocatedPages", "coverage")
            stats_core_ok = bool(
                final_stats is not None
                and all(
                    key in final_stats
                    and final_stats[key] is not None
                    and math.isfinite(float(final_stats[key]))
                    for key in required_stats
                )
            )
            result["finalGate"] = {
                "outputsFinite": all(item.get("finite", False) for item in final_outputs.values())
                and set(final_outputs) == set(OUTPUTS),
                "outputsNonnegative": all(item.get("nonnegative", False) for item in final_outputs.values())
                and set(final_outputs) == set(OUTPUTS),
                "statsAvailable": stats_core_ok,
                "statsFinite": bool(
                    final_stats is not None
                    and all(value is None or math.isfinite(float(value)) for value in final_stats.values())
                ),
            }
            # Keep final arrays transiently for cross-case comparisons.
            result["_finalArrays"] = final_sample.get("_arrays", {})
        result["status"] = "complete"
    except Exception as exc:
        result["status"] = "error"
        result["error"] = str(exc)
        print("C6 WARNING case", label, "aborted:", str(exc))
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as exc:
                print("C6 WARNING removeGraph", label, str(exc))
    return result


def _compare_arrays(reference, current):
    comparison = {}
    for output_name in OUTPUTS:
        left = reference.get(output_name)
        right = current.get(output_name)
        if left is None or right is None or left.shape != right.shape:
            comparison[output_name] = {"status": "SKIP", "reason": "array unavailable or shape mismatch"}
            continue
        delta = np.abs(right.astype(np.float32) - left.astype(np.float32))
        scale = np.maximum(np.abs(left.astype(np.float32)), 1e-6)
        comparison[output_name] = {
            "status": "PASS" if np.isfinite(delta).all() else "FAIL",
            "meanAbs": float(np.mean(delta)),
            "maxAbs": float(np.max(delta)),
            "meanRelative": float(np.mean(delta / scale)),
            "differentPixels": int(np.count_nonzero(delta > 1e-6)),
            "totalValues": int(delta.size),
        }
    return comparison


def main():
    report = {
        "script": "run_surfacecache_effect.py",
        "stage": "C6",
        "status": "in-progress",
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "checkpoints": list(CHECKPOINTS),
        "seed_schedule": {
            "mode": "fixed frame-index sequence",
            "frames": list(CHECKPOINTS),
            "note": "LumenGI exposes no host seed property; every case replays the same frame indices.",
        },
        "camera": {
            "position": [0.0, 0.28, 1.2],
            "target": [0.0, 0.28, 0.0],
            "focalLength": FIXED_CAMERA_FOCAL_LENGTH,
        },
        "cases": {},
        "comparisons": {},
        "verdicts": [],
    }

    case_results = {}
    for label in CASES:
        result = _run_case(label)
        case_results[label] = result
        report["cases"][label] = dict(result)
        report["cases"][label].pop("_finalArrays", None)
        _write_json(OUT_JSON, report)

    baseline = case_results.get("lookup_on")
    if baseline and baseline.get("_finalArrays"):
        for label, result in case_results.items():
            if label == "lookup_on" or not result.get("_finalArrays"):
                continue
            report["comparisons"][label] = _compare_arrays(baseline["_finalArrays"], result["_finalArrays"])

    for label, result in case_results.items():
        gate = result.get("finalGate", {})
        outputs_ok = gate.get("outputsFinite") is True and gate.get("outputsNonnegative") is True
        stats_ok = gate.get("statsAvailable") is True and gate.get("statsFinite") is True
        comparison = report["comparisons"].get(label)
        comparison_ok = True
        if label != "lookup_on":
            comparison_ok = bool(
                comparison
                and all(
                    item.get("status") == "PASS"
                    and math.isfinite(float(item.get("meanAbs", 0.0)))
                    and math.isfinite(float(item.get("maxAbs", 0.0)))
                    and math.isfinite(float(item.get("meanRelative", 0.0)))
                    for item in comparison.values()
                )
            )
        if result.get("status") != "complete":
            verdict = "FAIL"
        elif outputs_ok and stats_ok and comparison_ok:
            verdict = "PASS"
        else:
            verdict = "FAIL"
        report["verdicts"].append(
            {"case": label, "status": verdict, "gate": gate, "comparisonFinite": comparison_ok}
        )
        print("C6 VERDICT", label, verdict)

    report["status"] = "complete"
    report["low_budget"] = LOW_BUDGET
    report["invalidate_at"] = INVALIDATE_AT
    report["array_capture"] = SAVE_ARRAYS
    _write_json(OUT_JSON, report)
    print("C6 wrote", os.path.abspath(OUT_JSON))


main()
exit()
