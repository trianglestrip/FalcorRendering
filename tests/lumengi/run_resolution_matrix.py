"""C2 resolution/resource-lifecycle validation for LumenGI.

This Mogwai script intentionally uses one LumenGI graph while resizing the
same scene through the complete C2 matrix.  The transition
800x450 -> 799x449 -> 800x449 is especially important: all three dimensions
map to the same 8x8 probe-grid cardinality (100x57 = 5700 probes), while the
backing frame dimensions and aspect ratio differ.  A host implementation that
keys allocations only by probeCount can therefore reuse stale screen-probe,
HZB, or history resources.

The script is run-only and does not build the repository.  To avoid accidental
evidence collisions it requires the caller to provide the *only* output root:

    $env:LUMEN_RESOLUTION_MATRIX_OUT =
        'artifacts/lumengi/chain-closure/C2/resolution-matrix/run-001'

All JSON and FrameCapture outputs are written below that directory.  When the
variable is absent the script prints SKIP and exits without creating files.

The script deliberately records a missing ``screenProbeStats`` binding as a
host-contract gap.  Output shapes and finite values remain independently
checkable, but a production gate should expose the counters so probeCount,
resourceDim, and update state can be verified without reverse-engineering a
texture.
"""

from falcor import *

import json
import math
import os
import traceback


FRAME_RATE = 60
TILE_SIZE = 8
SETTLE_FRAMES = int(os.environ.get("LUMEN_RESOLUTION_MATRIX_SETTLE", "4"))
SCENE = os.environ.get("LUMEN_RESOLUTION_MATRIX_SCENE", "test_scenes/cornell_box.pyscene")
OUT_DIR = os.environ.get("LUMEN_RESOLUTION_MATRIX_OUT", "").strip()
CAPTURE_ENABLED = os.environ.get("LUMEN_RESOLUTION_MATRIX_CAPTURE", "1").strip().lower() not in (
    "0", "false", "off", "no"
)

# Required C2 coverage.  Do not remove the odd dimensions: they exercise
# ceil(tile) allocation and the same-probe-count/aspect transition.
RESOLUTION_MATRIX = (
    (640, 360),
    (800, 450),
    (641, 361),
    (799, 449),
    (800, 449),
    (801, 451),
    (1280, 720),
)

# Optional diagnostic subset. The default remains the complete required matrix;
# a bounded subset is useful when a driver/resource-view failure must be
# isolated to one resize transition without weakening the release gate.
_step_filter = os.environ.get("LUMEN_RESOLUTION_MATRIX_STEPS", "").strip()
if _step_filter:
    try:
        _indices = [int(token.strip()) for token in _step_filter.split(",") if token.strip()]
        RESOLUTION_MATRIX = tuple(RESOLUTION_MATRIX[index] for index in _indices)
    except Exception as exc:
        print("RESMATRIX_CONFIG invalid steps", repr(_step_filter), repr(exc), "using full matrix")

CAPTURE_OUTPUTS = (
    "LumenGI.diffuseGI",
    "LumenGI.confidence",
    "LumenGI.probeInterpolated",
    "LumenGI.temporalFiltered",
    "LumenGI.spatialFiltered",
)


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
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(json_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temp, path)


def probe_grid(width, height):
    grid_x = max(1, (width + TILE_SIZE - 1) // TILE_SIZE)
    grid_y = max(1, (height + TILE_SIZE - 1) // TILE_SIZE)
    return grid_x, grid_y, grid_x * grid_y


def aspect(width, height):
    return float(width) / float(height) if height else None


def create_lumen_graph():
    graph = RenderGraph("LumenGIResolutionMatrix")
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
    # Cache/GDF are intentionally disabled.  C2 owns resize/resource lifetime;
    # C1/C3 own cache-lighting and GDF backend failures respectively.
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "enabled": True,
                "useSurfaceCache": False,
                "useCacheLighting": False,
                "useScreenTrace": True,
                "useScreenProbes": True,
                "useTemporalFilter": True,
                "useSpatialFilter": True,
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

    for output in CAPTURE_OUTPUTS:
        graph.markOutput(output)
    return graph


def setup_runtime():
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0


def to_numpy(graph, output):
    import numpy as np

    return np.asarray(graph.get_output(output).to_numpy())


def texture_record(graph, output, width, height):
    """Return shape/numeric health for one marked graph texture."""
    import numpy as np

    try:
        arr = to_numpy(graph, output)
        shape = list(arr.shape)
        shape_ok = len(shape) >= 2 and shape[0] == height and shape[1] == width
        finite = bool(np.isfinite(arr).all())
        if arr.ndim >= 3 and arr.shape[-1] > 0:
            values = arr[..., : min(3, arr.shape[-1])]
        else:
            values = arr
        nonnegative = bool(float(values.min()) >= 0.0) if values.size else True
        return {
            "status": "PASS" if shape_ok and finite and nonnegative else "FAIL",
            "shape": shape,
            "expectedShapePrefix": [height, width],
            "shapeOk": shape_ok,
            "finite": finite,
            "nonnegative": nonnegative,
            "min": float(values.min()) if values.size else 0.0,
            "max": float(values.max()) if values.size else 0.0,
            "mean": float(values.mean()) if values.size else 0.0,
        }
    except Exception as exc:
        return {"status": "FAIL", "error": str(exc)}


def read_probe_stats(graph):
    """Read a future/current stats binding without requiring it pre-C2."""
    try:
        pass_obj = graph.getPass("LumenGI")
    except Exception as exc:
        return {"status": "SKIP", "reason": "LumenGI pass lookup failed: %s" % str(exc)}

    for attr in ("screenProbeStats", "probeStats"):
        try:
            value = getattr(pass_obj, attr)
            if isinstance(value, dict):
                return {"status": "PASS", "binding": attr, "value": dict(value)}
            return {"status": "PASS", "binding": attr, "value": str(value)}
        except Exception:
            continue
    return {
        "status": "SKIP",
        "reason": "screenProbeStats/probeStats Python binding is not exposed",
        "hostGap": "Expose probeCount, resourceDim, and counters for a production C2 gate.",
    }


def step_stem(index, width, height, grid_x, grid_y, probe_count):
    return "step%02d__%dx%d__grid%dx%d__probes%d" % (
        index, width, height, grid_x, grid_y, probe_count
    )


def run_step(graph, index, resolution, frame_cursor, report):
    import numpy as np

    width, height = resolution
    grid_x, grid_y, expected_probe_count = probe_grid(width, height)
    stem = step_stem(index, width, height, grid_x, grid_y, expected_probe_count)
    step = {
        "index": index,
        "resolution": [width, height],
        "aspect": aspect(width, height),
        "tileSize": TILE_SIZE,
        "expectedProbeGrid": [grid_x, grid_y],
        "expectedProbeCount": expected_probe_count,
        "frameStart": frame_cursor,
        "settleFrames": SETTLE_FRAMES,
        "status": "RUNNING",
        "captureBaseFilename": stem,
        "outputs": {},
        "errors": [],
    }
    try:
        # Keep the graph and scene alive across all steps.  resizeFrameBuffer()
        # must invalidate dimensions and histories without relying on graph
        # destruction/recreation to hide stale resources.
        m.resizeFrameBuffer(width, height)
        m.frameCapture.outputDir = OUT_DIR
        m.frameCapture.baseFilename = stem
        for _ in range(SETTLE_FRAMES):
            frame_cursor += 1
            m.clock.frame = frame_cursor
            m.renderFrame()

        for output in CAPTURE_OUTPUTS:
            step["outputs"][output] = texture_record(graph, output, width, height)
        step["probeStats"] = read_probe_stats(graph)

        if CAPTURE_ENABLED:
            try:
                m.frameCapture.capture()
                step["frameCapture"] = "requested"
            except Exception as exc:
                step["frameCapture"] = "FAIL"
                step["errors"].append("FrameCapture: %s" % str(exc))
        else:
            step["frameCapture"] = "disabled"

        output_failures = [
            name for name, value in step["outputs"].items()
            if value.get("status") == "FAIL"
        ]
        step["outputFailures"] = output_failures
        step["status"] = "PASS" if not output_failures else "FAIL"
    except Exception as exc:
        step["status"] = "FAIL"
        step["errors"].append(str(exc))
        step["traceback"] = traceback.format_exc(limit=5)

    step["frameEnd"] = frame_cursor
    report["steps"].append(step)
    return frame_cursor, step


def same_probe_count_aspect_pairs(steps):
    pairs = []
    for left_index, left in enumerate(steps):
        for right in steps[left_index + 1:]:
            if left.get("expectedProbeCount") != right.get("expectedProbeCount"):
                continue
            left_aspect = left.get("aspect")
            right_aspect = right.get("aspect")
            if left_aspect is None or right_aspect is None or abs(left_aspect - right_aspect) < 1e-7:
                continue
            shape_ok = True
            for output in CAPTURE_OUTPUTS:
                a = left.get("outputs", {}).get(output, {})
                b = right.get("outputs", {}).get(output, {})
                shape_ok = shape_ok and bool(a.get("shapeOk")) and bool(b.get("shapeOk"))
            pairs.append({
                "left": left.get("resolution"),
                "right": right.get("resolution"),
                "probeCount": left.get("expectedProbeCount"),
                "leftAspect": left_aspect,
                "rightAspect": right_aspect,
                "frameDimReallocationObserved": left.get("resolution") != right.get("resolution"),
                "status": "PASS" if shape_ok else "FAIL",
            })
    return pairs


def main():
    if not OUT_DIR:
        print("RESMATRIX SKIP: set LUMEN_RESOLUTION_MATRIX_OUT to a unique output directory")
        exit()

    os.makedirs(OUT_DIR, exist_ok=True)
    report_path = os.path.join(OUT_DIR, "resolution-matrix.json")
    report = {
        "protocol": "C2-resolution-resource-lifecycle-v1",
        "status": "RUNNING",
        "scene": SCENE,
        "outputDirectory": os.path.abspath(OUT_DIR),
        "tileSize": TILE_SIZE,
        "settleFrames": SETTLE_FRAMES,
        "requiredResolutions": [list(resolution) for resolution in RESOLUTION_MATRIX],
        "sameProbeCountDifferentAspect": "required; compare all pairs with equal ceil(8px tile) count",
        "hostContracts": [
            "LumenGI screen-probe resources must key allocation by frameDim as well as probeCount.",
            "Resize must recreate/clear metadata, hit records, HZB, integrated radiance, counters, and history resources.",
            "Resource dimensions and probe counters should be exposed through a scriptable screenProbeStats binding.",
            "A resize must not leave stale output dimensions, NaN/Inf, negative radiance, or a device error.",
        ],
        "steps": [],
        "sameProbeCountPairs": [],
        "errors": [],
    }
    write_json(report_path, report)

    graph = None
    frame_cursor = 0
    try:
        setup_runtime()
        m.loadScene(SCENE)
        graph = create_lumen_graph()
        m.addGraph(graph)
        m.setActiveGraph(graph)
        for index, resolution in enumerate(RESOLUTION_MATRIX):
            frame_cursor, step = run_step(graph, index, resolution, frame_cursor, report)
            write_json(report_path, report)
            print(
                "RESMATRIX step", index, "%dx%d" % resolution,
                "grid", step["expectedProbeGrid"],
                "probes", step["expectedProbeCount"],
                "status", step["status"],
            )

        report["sameProbeCountPairs"] = same_probe_count_aspect_pairs(report["steps"])
        pair_failures = [pair for pair in report["sameProbeCountPairs"] if pair["status"] == "FAIL"]
        step_failures = [step for step in report["steps"] if step["status"] == "FAIL"]
        report["status"] = "PASS" if not pair_failures and not step_failures else "FAIL"
    except Exception as exc:
        report["status"] = "FAIL"
        report["errors"].append(str(exc))
        report["traceback"] = traceback.format_exc(limit=6)
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as exc:
                report["errors"].append("removeGraph: %s" % str(exc))
        write_json(report_path, report)

    print("RESMATRIX done", report["status"], "report", report_path)


main()
exit()
