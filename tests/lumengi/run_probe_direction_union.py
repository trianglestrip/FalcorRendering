"""C7 screen-probe direction-union diagnostic (read-only runtime asset).

This script samples only the currently reflected ``probeHistory`` output and
the scriptable ``screenProbeStats`` binding.  It can prove finite/nonnegative
history, monotonic accumulated direction-count growth, and history reset
shape at checkpoints 1/8/32/96.  It deliberately reports the actual
direction-union identity gate as SKIP: the shader computes ``sampleIndex`` but
the current hit record does not store it, ``gProbeDebug`` is not host-bound,
and no graph output exposes a direction vector or stable sample ID.

The minimum ABI needed to close the identity gate is one producer-valid
per-direction identity stream, for example a StructuredBuffer<uint>
``sampleIndex`` indexed by ``probeIndex * maxStride + dirIndex`` plus a frame
valid mask, or a reflected RG32_UINT texture with the same contract.  A
direction vector without a producer frame/valid mask is insufficient because
budget-skipped records are intentionally stale.

The script does not build, spawn subprocesses, or modify production files.
Run it through Mogwai after a Release build with a unique
``LUMEN_PROBE_UNION_OUT`` directory.  No GPU run is performed as part of the
source change; py_compile is the local validation gate.
"""

from falcor import *

import json
import math
import os
import traceback


FRAME_RATE = 60
SCENE = os.environ.get("LUMEN_PROBE_UNION_SCENE", "test_scenes/cornell_box.pyscene")
RESOLUTION = (640, 360)
DIRECTIONS_PER_PROBE = int(os.environ.get("LUMEN_PROBE_UNION_DIRECTIONS", "16"))
SEED = 0x51B8DC0D  # LumenScreenProbe::kSeed; fixed host/shader contract.
OUT_DIR = os.path.abspath(os.environ.get("LUMEN_PROBE_UNION_OUT", "").strip())
CHECKPOINTS = (1, 8, 32, 96)

REPORT_SCHEMA = {
    "protocol": "C7-probe-direction-union-v1",
    "historyGate": "PASS|FAIL|SKIP",
    "directionUnionGate": "SKIP|PASS|FAIL",
    "checkpoints": [
        {
            "frame": "int",
            "probeHistory": {"finite": True, "nonnegative": True, "maxAlpha": "float"},
            "screenProbeStats": {"status": "PASS|SKIP", "directionsTraced": "int"},
            "directionUnion": {"status": "SKIP", "reason": "identity telemetry unavailable"},
        }
    ],
    "status": "PASS|PARTIAL|FAIL|SKIP",
}


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


def _write_json(path, payload):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temp, path)


def _create_graph():
    graph = RenderGraph("LumenProbeDirectionUnion")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "enabled": True,
                "useSurfaceCache": False,
                "useCacheLighting": False,
                "useScreenTrace": False,
                "useScreenProbes": True,
                "probeDirectionsPerProbe": DIRECTIONS_PER_PROBE,
                "useTemporalFilter": False,
                "useSpatialFilter": False,
                "useGDF": False,
            },
        ),
        "LumenGI",
    )
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
    for output in ("LumenGI.diffuseGI", "LumenGI.probeInterpolated", "LumenGI.probeHistory"):
        graph.markOutput(output)
    return graph


def _texture_array(graph, output):
    import numpy as np

    data = np.asarray(graph.get_output(output).to_numpy()).copy()
    raw_shape = list(data.shape)
    if data.ndim == 1 and data.size == RESOLUTION[0] * RESOLUTION[1] * 4:
        data = data.reshape((RESOLUTION[1], RESOLUTION[0], 4))
    return data, raw_shape


def _history_record(graph):
    import numpy as np

    try:
        data, raw_shape = _texture_array(graph, "LumenGI.probeHistory")
        if data.ndim < 3 or data.shape[-1] < 4:
            return {
                "status": "SKIP",
                "reason": "probeHistory is not an RGBA texture",
                "shape": list(data.shape),
                "rawShape": raw_shape,
            }
        rgb = data[..., :3]
        alpha = data[..., 3]
        finite = bool(np.isfinite(data).all())
        nonnegative = bool(float(np.min(data)) >= 0.0) if data.size else True
        return {
            "status": "PASS" if finite and nonnegative else "FAIL",
            "shape": list(data.shape),
            "rawShape": raw_shape,
            "finite": finite,
            "nonnegative": nonnegative,
            "maxAlpha": float(np.max(alpha)) if alpha.size else 0.0,
            "meanAlpha": float(np.mean(alpha)) if alpha.size else 0.0,
            "nonzeroAlphaPixels": int(np.count_nonzero(alpha > 0.0)),
            "rgbMean": float(np.mean(rgb)) if rgb.size else 0.0,
        }
    except Exception as exc:
        return {"status": "SKIP", "reason": str(exc)}


def _screen_probe_stats(graph):
    try:
        value = getattr(graph.getPass("LumenGI"), "screenProbeStats")
        if isinstance(value, dict):
            return {"status": "PASS", "value": dict(value)}
        return {"status": "PASS", "value": str(value)}
    except Exception as exc:
        return {
            "status": "SKIP",
            "reason": "screenProbeStats binding unavailable: %s" % str(exc),
        }


def _direction_union_skip():
    return {
        "status": "SKIP",
        "reason": "no per-direction sampleIndex/vector identity is reflected; probeHistory alpha is count-only",
        "requiredTelemetry": {
            "sampleIndex": "uint per probe*direction record or RG32_UINT texture",
            "producerFrame": "frame index or valid mask per record",
            "source": "LumenProbeHit currently lacks sampleIndex; gProbeDebug is not host-bound",
        },
    }


def main():
    if not OUT_DIR:
        print("PROBEUNION SKIP: set LUMEN_PROBE_UNION_OUT to a unique output directory")
        return
    os.makedirs(OUT_DIR, exist_ok=True)
    report_path = os.path.join(OUT_DIR, "probe-direction-union.json")
    report = {
        "protocol": "C7-probe-direction-union-v1",
        "status": "RUNNING",
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "directionsPerProbe": DIRECTIONS_PER_PROBE,
        "seed": SEED,
        "checkpoints": list(CHECKPOINTS),
        "historyGate": "RUNNING",
        "directionUnionGate": "SKIP",
        "identityTelemetry": _direction_union_skip(),
        "runs": [],
        "reportSchema": REPORT_SCHEMA,
    }
    _write_json(report_path, report)

    graph = None
    try:
        m.ui = False
        m.clock.framerate = FRAME_RATE
        m.clock.time = 0
        m.clock.pause()
        m.clock.frame = 0
        m.loadScene(SCENE)
        m.resizeFrameBuffer(*RESOLUTION)
        graph = _create_graph()
        m.addGraph(graph)
        m.setActiveGraph(graph)
        previous_max_alpha = None
        history_fail = False
        for frame in range(1, max(CHECKPOINTS) + 1):
            m.clock.frame = frame
            m.renderFrame()
            if frame not in CHECKPOINTS:
                continue
            history = _history_record(graph)
            stats = _screen_probe_stats(graph)
            max_alpha = history.get("maxAlpha")
            monotonic = previous_max_alpha is None or (
                max_alpha is not None and max_alpha + 1.0e-5 >= previous_max_alpha
            )
            history["monotonicMaxAlpha"] = monotonic
            if history.get("status") == "FAIL" or not monotonic:
                history_fail = True
            if max_alpha is not None:
                previous_max_alpha = max_alpha
            record = {
                "frame": frame,
                "probeHistory": history,
                "screenProbeStats": stats,
                "directionUnion": _direction_union_skip(),
            }
            report["runs"].append(record)
            _write_json(report_path, report)
            print(
                "PROBEUNION frame",
                frame,
                "historyMaxAlpha",
                history.get("maxAlpha"),
                "stats",
                stats.get("value"),
            )
        report["historyGate"] = "FAIL" if history_fail else "PASS"
        report["status"] = "FAIL" if history_fail else "PARTIAL"
    except Exception as exc:
        report["status"] = "FAIL"
        report.setdefault("errors", []).append(str(exc))
        report["traceback"] = traceback.format_exc(limit=8)
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as exc:
                report.setdefault("errors", []).append("removeGraph: " + str(exc))
        _write_json(report_path, report)
    print("PROBEUNION done", report["status"], "report", report_path)


main()
exit()
