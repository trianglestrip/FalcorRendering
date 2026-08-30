"""I0 host-validity regression asset for LumenGI.

This is a run-only Mogwai asset.  It deliberately tests the observability
boundary before declaring a pass:

* stale temporal input when ``useScreenProbes`` is disabled;
* the ``filteredVariance`` graph output being a live/cleared mirror;
* Mesh/GDF scene invalidation and route-stat reset after a scene/geometry
  change.

The current LumenGIPass exposes read-only telemetry and does not currently
override RenderPass::setProperties().  The script therefore reports
``BLOCKED`` when a transition or generation cannot be observed; it never
infers a pass from a black/finite image alone.

Run-only invocation (single GPU):

    set LUMEN_I0_OUT=artifacts\\lumengi\\I0\\run
    mogwai -p tests\\lumengi\\run_i0_validity_regression.py

The only output root is ``LUMEN_I0_OUT``.  The JSON report is written as
``i0-validity-regression.json`` beneath that directory.
"""

from falcor import *

import json
import math
import os
import traceback

import numpy as np


SCENE = "test_scenes/cornell_box.pyscene"
RESOLUTION = (320, 180)
FRAME_RATE = 60
OUT_DIR = os.path.abspath(
    os.environ.get("LUMEN_I0_OUT", "artifacts/lumengi/I0/validity-regression")
)
OUT_JSON = os.path.join(OUT_DIR, "i0-validity-regression.json")
WARMUP_FRAMES = max(1, int(os.environ.get("LUMEN_I0_WARMUP", "3")))
POST_CHANGE_FRAMES = max(1, int(os.environ.get("LUMEN_I0_POST_CHANGE", "3")))
EPSILON = 1.0e-6


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


def _write_report(payload):
    os.makedirs(OUT_DIR, exist_ok=True)
    tmp = OUT_JSON + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(tmp, OUT_JSON)
    print("I0_SUMMARY wrote", OUT_JSON)


def _get_pass(graph):
    """Support both binding spellings used by existing Mogwai assets."""
    for name in ("getPass", "get_pass"):
        try:
            return getattr(graph, name)("LumenGI")
        except Exception:
            pass
    return None


def _get_stats(pass_obj, name):
    if pass_obj is None:
        return None
    try:
        value = getattr(pass_obj, name)
        return dict(value) if isinstance(value, dict) else _json_safe(value)
    except Exception as exc:
        return {"available": False, "error": repr(exc)}


def _texture(graph, channel):
    try:
        return np.asarray(graph.get_output("LumenGI." + channel).to_numpy(), dtype=np.float32)
    except Exception as exc:
        return None, repr(exc)


def _texture_stats(graph, channel):
    result = _texture(graph, channel)
    if isinstance(result, tuple):
        return {"available": False, "error": result[1]}
    array = result
    if array.size == 0:
        return {
            "available": True,
            "shape": list(array.shape),
            "finite": True,
            "nonnegative": True,
            "mean": 0.0,
            "max": 0.0,
            "nonzero_fraction": 0.0,
        }
    finite = np.isfinite(array)
    finite_values = array[finite]
    rgb = array[..., :3] if array.ndim >= 3 and array.shape[-1] >= 3 else array
    return {
        "available": True,
        "shape": list(array.shape),
        "finite": bool(finite.all()),
        "nonnegative": bool(float(array.min()) >= 0.0) if finite_values.size else False,
        "mean": float(finite_values.mean()) if finite_values.size else None,
        "max": float(finite_values.max()) if finite_values.size else None,
        "nonzero_fraction": float(np.count_nonzero(np.abs(rgb) > EPSILON)) / float(rgb.size)
        if rgb.size
        else 0.0,
    }


def _build_graph(label, **props):
    options = {
        "enabled": True,
        "traceMode": "HardwareRT",
        "qualityPreset": "High",
        "useScreenTrace": True,
        "useScreenProbes": True,
        "useTemporalFilter": True,
        "useSpatialFilter": True,
        "useGDF": False,
    }
    options.update(props)
    graph = RenderGraph(label)
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
    for channel in (
        "diffuseGI",
        "confidence",
        "probeInterpolated",
        "temporalFiltered",
        "temporalConfidence",
        "spatialFiltered",
        "filteredVariance",
        "gdfTrace",
    ):
        graph.markOutput("LumenGI." + channel)
    return graph


def _setup_scene():
    m.loadScene(SCENE)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0


def _render(graph, frames):
    samples = []
    for _ in range(frames):
        m.clock.frame += 1
        m.renderFrame()
        samples.append(
            {
                "frame": int(m.clock.frame),
                "temporal": _texture_stats(graph, "temporalFiltered"),
                "probe": _texture_stats(graph, "probeInterpolated"),
                "variance": _texture_stats(graph, "filteredVariance"),
                "gdf": _texture_stats(graph, "gdfTrace"),
            }
        )
    return samples


def _safe_remove(graph):
    if graph is not None:
        try:
            m.removeGraph(graph)
        except Exception as exc:
            print("I0_CLEANUP removeGraph EXC", repr(exc))


def _transition_probe_disable():
    """Attempt the real runtime transition; return evidence, never assume it."""
    graph = None
    try:
        graph = _build_graph(
            "LumenI0TemporalDisable",
            useScreenProbes=True,
            useTemporalFilter=True,
            useSpatialFilter=False,
        )
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene()
        _render(graph, WARMUP_FRAMES)
        pass_obj = _get_pass(graph)
        before = None
        after = None
        set_supported = False
        if pass_obj is not None:
            try:
                before = dict(pass_obj.properties)
            except Exception:
                before = None
            try:
                pass_obj.set_properties({"useScreenProbes": False})
                set_supported = True
            except Exception as exc:
                return {
                    "status": "BLOCKED",
                    "reason": "LumenGI runtime property transition unavailable",
                    "error": repr(exc),
                    "before_properties": before,
                }
            try:
                after = dict(pass_obj.properties)
            except Exception:
                after = None
        if not set_supported or not isinstance(before, dict) or not isinstance(after, dict):
            return {
                "status": "BLOCKED",
                "reason": "pass properties are not observable after set_properties",
                "before_properties": before,
                "after_properties": after,
            }
        changed = before.get("useScreenProbes") is True and after.get("useScreenProbes") is False
        if not changed:
            return {
                "status": "BLOCKED",
                "reason": "LumenGIPass did not expose a mutable useScreenProbes transition",
                "before_properties": before,
                "after_properties": after,
            }
        post = _render(graph, POST_CHANGE_FRAMES)
        probe_stats = _get_stats(pass_obj, "screenProbeStats")
        probe_cleared = all(
            s["probe"].get("available") and s["probe"].get("nonzero_fraction", 1.0) <= 0.0
            for s in post
        )
        directions_zero = isinstance(probe_stats, dict) and probe_stats.get("directionsTraced") == 0.0
        return {
            "status": "PASS" if probe_cleared and directions_zero else "FAIL",
            "property_transition": True,
            "before_properties": before,
            "after_properties": after,
            "probe_stats": probe_stats,
            "post_samples": post,
            "probe_output_cleared": probe_cleared,
            "directions_zero": directions_zero,
            "note": (
                "Pass requires a real runtime transition; cleared probe output and zero traced "
                "directions are the observable stale-input contract."
            ),
        }
    except Exception as exc:
        return {"status": "BLOCKED", "reason": "temporal transition setup failed", "error": repr(exc), "traceback": traceback.format_exc()}
    finally:
        _safe_remove(graph)


def _variance_mirror_gate():
    """Validate producer/clear behavior and state the exact-mirror limitation."""
    enabled_graph = None
    disabled_graph = None
    result = {"status": "BLOCKED", "verdicts": []}
    try:
        enabled_graph = _build_graph(
            "LumenI0VarianceEnabled",
            useScreenProbes=True,
            useTemporalFilter=True,
            useSpatialFilter=True,
        )
        m.addGraph(enabled_graph)
        m.setActiveGraph(enabled_graph)
        _setup_scene()
        enabled_samples = _render(enabled_graph, WARMUP_FRAMES)
        enabled = enabled_samples[-1]["variance"]
        producer_ok = bool(enabled.get("available") and enabled.get("finite") and enabled.get("nonnegative"))
        _safe_remove(enabled_graph)
        enabled_graph = None

        disabled_graph = _build_graph(
            "LumenI0VarianceDisabled",
            useScreenProbes=True,
            useTemporalFilter=True,
            useSpatialFilter=False,
        )
        m.addGraph(disabled_graph)
        m.setActiveGraph(disabled_graph)
        _setup_scene()
        disabled_samples = _render(disabled_graph, 1)
        disabled = disabled_samples[-1]["variance"]
        clear_ok = bool(
            disabled.get("available")
            and disabled.get("finite")
            and disabled.get("nonnegative")
            and disabled.get("nonzero_fraction", 1.0) <= 0.0
        )
        result.update(
            {
                "producer": enabled,
                "disabled_clear": disabled,
                "producer_ok": producer_ok,
                "clear_ok": clear_ok,
                "status": "BLOCKED",
                "verdicts": [
                    {"name": "filteredVariance producer finite/non-negative", "status": "PASS" if producer_ok else "FAIL"},
                    {"name": "filteredVariance disabled output is cleared", "status": "PASS" if clear_ok else "FAIL"},
                    {
                        "name": "filteredVariance equals internal variance resource",
                        "status": "BLOCKED",
                        "reason": "internal pVariance is not exposed as an independent graph output or host statistic",
                    },
                ],
                "reason": "producer and clear behavior are observable; exact internal-mirror equality is not",
            }
        )
        if not producer_ok or not clear_ok:
            result["status"] = "FAIL"
        return result
    except Exception as exc:
        result.update({"status": "BLOCKED", "reason": "variance graph/output unavailable", "error": repr(exc), "traceback": traceback.format_exc()})
        return result
    finally:
        _safe_remove(enabled_graph)
        _safe_remove(disabled_graph)


def _find_generation(stats):
    if not isinstance(stats, dict):
        return None
    for key, value in stats.items():
        lower = str(key).lower()
        if "generation" in lower or "invalidation" in lower or lower.endswith("reset"):
            if isinstance(value, (int, float)) and math.isfinite(float(value)):
                return {"key": str(key), "value": float(value)}
    return None


def _gdf_reset_gate():
    graph = None
    result = {"status": "BLOCKED"}
    try:
        graph = _build_graph(
            "LumenI0GDFReset",
            traceMode="MeshSDF",
            useGDF=True,
            useScreenProbes=False,
            useTemporalFilter=False,
            useSpatialFilter=False,
        )
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene()
        pass_obj = _get_pass(graph)
        pre_samples = []
        for sample in _render(graph, WARMUP_FRAMES):
            pre_samples.append(_get_stats(pass_obj, "gdfStats"))
        pre = pre_samples[-1] if pre_samples else None

        # Scene reload is the only geometry/mesh mutation guaranteed by the current
        # Python API.  It must invalidate MeshSDF/GDF resources even with a static camera.
        m.loadScene(SCENE)
        post_samples = []
        for sample in _render(graph, POST_CHANGE_FRAMES):
            post_samples.append(_get_stats(pass_obj, "gdfStats"))
        post = post_samples[-1] if post_samples else None
        pre_generation = _find_generation(pre)
        post_generation = _find_generation(post)
        generation_changed = bool(
            pre_generation and post_generation and pre_generation["value"] != post_generation["value"]
        )
        route_reset_observed = any(
            isinstance(s, dict)
            and (
                s.get("gdfExecuted") == 0.0
                or s.get("active") == 0.0
                or s.get("sphereTraced") == 0.0
            )
            for s in post_samples
        )
        stats_available = isinstance(pre, dict) and isinstance(post, dict) and "available" not in pre and "available" not in post
        if not stats_available:
            status = "BLOCKED"
            reason = "gdfStats host telemetry is unavailable"
        elif generation_changed or route_reset_observed:
            status = "PASS"
            reason = "generation/reset transition observed"
        else:
            status = "BLOCKED"
            reason = "gdfStats has no generation/reset key and no observable route reset transition"
        result.update(
            {
                "status": status,
                "reason": reason,
                "pre_stats": pre,
                "post_stats": post,
                "pre_generation": pre_generation,
                "post_generation": post_generation,
                "generation_changed": generation_changed,
                "route_reset_observed": route_reset_observed,
                "post_samples": post_samples,
                "scene_reload_used_as_geometry_change": True,
            }
        )
        return result
    except Exception as exc:
        result.update({"status": "BLOCKED", "reason": "GDF reset setup failed", "error": repr(exc), "traceback": traceback.format_exc()})
        return result
    finally:
        _safe_remove(graph)


def main():
    report = {
        "stage": "I0",
        "script": "run_i0_validity_regression.py",
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "warmup_frames": WARMUP_FRAMES,
        "post_change_frames": POST_CHANGE_FRAMES,
        "output_dir": OUT_DIR,
        "contract": {
            "no_fabricated_pass": True,
            "unobservable_host_stats": "BLOCKED",
            "exact_variance_mirror_requires_independent_internal_observation": True,
        },
    }
    results = {
        "temporal_screen_probe_disable": _transition_probe_disable(),
        "filtered_variance_mirror": _variance_mirror_gate(),
        "gdf_geometry_reset": _gdf_reset_gate(),
    }
    report["results"] = results
    statuses = [value.get("status") for value in results.values()]
    if any(status == "FAIL" for status in statuses):
        report["status"] = "FAIL"
    elif any(status == "BLOCKED" for status in statuses):
        report["status"] = "BLOCKED"
    elif any(status == "SKIP" for status in statuses):
        report["status"] = "SKIP"
    else:
        report["status"] = "PASS"
    print("I0_STATUS", report["status"])
    for name, result in results.items():
        print("I0_CASE", name, result.get("status"), result.get("reason", ""))
    _write_report(report)


try:
    main()
except Exception as exc:
    _write_report(
        {
            "stage": "I0",
            "script": "run_i0_validity_regression.py",
            "status": "BLOCKED",
            "fatal_error": repr(exc),
            "traceback": traceback.format_exc(),
        }
    )
exit()
