"""C8/C9 runtime gate for markOutput and FrameCapture export equivalence.

The gate runs one scene serially through the same frame-index schedule for
each declared policy.  It covers the two filter states that are currently
safe to compare (filters off and temporal-only/partial), and separates direct
LumenGI graph marking from FrameCapture export:

* mark_on_export_off: direct LumenGI outputs are marked; no EXR export.
* mark_on_export_on: direct outputs are marked; FrameCapture exports marked
  outputs only.
* mark_off_export_off: only two BlitPass readback sentinels are marked; no
  export.  This keeps the graph executable while probing whether direct
  LumenGI endpoints are available without markOutput; unavailable endpoints
  are reported BLOCKED and sentinel values remain diagnostic only.
* mark_off_export_on: direct outputs are not marked; FrameCapture uses
  captureAllOutputs to temporarily mark and export every graph output.

The same scene is reloaded for every case and the clock is reset to the same
frame sequence.  LumenGI has no host seed property; the seed value in the
manifest is therefore a deterministic protocol identifier, while the actual
runtime determinism comes from the fixed scene, camera, frame index and
driver.  The script never marks or samples LumenGI.finalColor.  A finalColor
record is emitted as SKIP so a resolvedDiffuseGI texture cannot masquerade as
a full-scene composite.

This file is a Mogwai run-only asset.  It does not build, spawn subprocesses,
or modify production C++/Slang.  Use a unique LUMEN_EXPORT_EQ_OUT directory
for each GPU run.  The JSON artifact schema is documented in REPORT_SCHEMA
below.
"""

from falcor import *

import json
import math
import os
import traceback


FRAME_RATE = 60
SCENE = os.environ.get("LUMEN_EXPORT_EQ_SCENE", "test_scenes/cornell_box.pyscene")
SEED = int(os.environ.get("LUMEN_EXPORT_EQ_SEED", "1337"))


def _parse_resolution(value):
    try:
        width, height = (int(token.strip()) for token in value.lower().replace("x", ",").split(",", 1))
        if width > 0 and height > 0:
            return width, height
    except Exception:
        pass
    print("EXPORTEQ_CONFIG invalid resolution", repr(value), "using 800x450")
    return 800, 450


WIDTH, HEIGHT = _parse_resolution(os.environ.get("LUMEN_EXPORT_EQ_RESOLUTION", "800x450"))
WARMUP_FRAMES = max(1, int(os.environ.get("LUMEN_EXPORT_EQ_WARMUP", "8")))
OUT_DIR = os.path.abspath(os.environ.get("LUMEN_EXPORT_EQ_OUT", "").strip())
BLACK_EPS = float(os.environ.get("LUMEN_EXPORT_EQ_BLACK_EPS", "1e-8"))
EQUIV_ABS_TOL = float(os.environ.get("LUMEN_EXPORT_EQ_EQUIV_ABS_TOL", "1e-5"))
EQUIV_REL_TOL = float(os.environ.get("LUMEN_EXPORT_EQ_EQUIV_REL_TOL", "1e-3"))

SENTINEL_OUTPUTS = ("ResolvedReadback.dst", "DiffuseReadback.dst")
DIRECT_OUTPUTS = ("LumenGI.resolvedDiffuseGI", "LumenGI.diffuseGI")

# ``mark_lumen`` controls direct LumenGI endpoints.  The two sentinel outputs
# remain marked in every graph so a no-mark/no-export policy still executes the
# LumenGI -> BlitPass consumer chain and has a legal resource to read back.
POLICIES = (
    {"name": "mark_on_export_off", "mark_lumen": True, "export": False, "capture_all": False},
    {"name": "mark_on_export_on", "mark_lumen": True, "export": True, "capture_all": False},
    {"name": "mark_off_export_off", "mark_lumen": False, "export": False, "capture_all": False},
    {"name": "mark_off_export_on", "mark_lumen": False, "export": True, "capture_all": True},
)

FILTERS = (
    {
        "name": "off",
        "properties": {
            "useScreenProbes": False,
            "useTemporalFilter": False,
            "useSpatialFilter": False,
        },
    },
    {
        "name": "partial",
        "properties": {
            "useScreenProbes": True,
            "useTemporalFilter": True,
            "useSpatialFilter": False,
        },
    },
)

# Stable schema summary for downstream validators and handoff documents.
REPORT_SCHEMA = {
    "protocol": "C8-C9-export-equivalence-v1",
    "runs": [
        {
            "filter": "off|partial",
            "policy": "mark_on_export_off|mark_on_export_on|mark_off_export_off|mark_off_export_on",
            "resolvedDiffuseGI": {"status": "PASS|SKIP|FAIL", "shape": ["H", "W", "C"], "finite": True, "nonnegative": True, "blackFrame": False},
            "diffuseGI": {"status": "PASS|SKIP|FAIL", "shape": ["H", "W", "C"], "finite": True, "nonnegative": True, "blackFrame": False},
            "directOutputAvailability": {"LumenGI.resolvedDiffuseGI": True, "LumenGI.diffuseGI": True},
            "sentinelDiagnostics": {"ResolvedReadback.dst": "optional health record"},
            "resolvedVsDiffuse": {"status": "PASS|SKIP|FAIL", "sameShape": True, "meanAbs": "float", "maxAbs": "float"},
            "equivalenceToMarkOnExportOff": {"status": "PASS|FAIL|BASELINE|SKIP"},
            "finalColor": {"status": "SKIP", "used": False},
            "status": "PASS|FAIL|BLOCKED|EXC",
        }
    ],
    "status": "PASS|FAIL|PARTIAL",
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
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temp, path)


def _create_graph(filter_spec, policy, index):
    graph_name = "LumenExportEq_%02d_%s_%s" % (index, filter_spec["name"], policy["name"])
    graph = RenderGraph(graph_name)
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )

    lumen_properties = {
        "enabled": True,
        "useSurfaceCache": False,
        "useCacheLighting": False,
        "useScreenTrace": False,
        "useGDF": False,
    }
    lumen_properties.update(filter_spec["properties"])
    graph.addPass(createPass("LumenGI", lumen_properties), "LumenGI")

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

    # BlitPass outputs are marked in all policies.  They are neutral readback
    # sentinels, not a final-color composite and not replacements for the
    # resolvedDiffuseGI production channel.
    # Preserve the linear HDR contract in the diagnostic readback sentinels.
    # BlitPass defaults to RGBA8 when outputFormat is omitted, which quantizes
    # GI and makes the sentinel unsuitable even for health/equivalence checks.
    sentinel_options = {"filter": "Linear", "outputFormat": "RGBA16Float"}
    graph.addPass(createPass("BlitPass", sentinel_options), "ResolvedReadback")
    graph.addPass(createPass("BlitPass", sentinel_options), "DiffuseReadback")
    graph.addEdge("LumenGI.resolvedDiffuseGI", "ResolvedReadback.src")
    graph.addEdge("LumenGI.diffuseGI", "DiffuseReadback.src")
    for output in SENTINEL_OUTPUTS:
        graph.markOutput(output)
    if policy["mark_lumen"]:
        for output in DIRECT_OUTPUTS:
            graph.markOutput(output)
    return graph


def _set_runtime():
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0


def _health(arr, expected_width, expected_height):
    import numpy as np

    data = np.asarray(arr)
    shape = list(data.shape)
    shape_ok = data.ndim >= 2 and data.shape[0] == expected_height and data.shape[1] == expected_width
    values = data[..., : min(3, data.shape[-1])] if data.ndim >= 3 else data
    if values.size:
        minimum = float(values.min())
        maximum = float(values.max())
        mean = float(values.mean())
    else:
        minimum = maximum = mean = 0.0
    finite = bool(np.isfinite(data).all())
    nonnegative = bool(minimum >= 0.0)
    black_frame = bool(values.size == 0 or maximum <= BLACK_EPS)
    return {
        "status": "PASS" if shape_ok and finite and nonnegative and not black_frame else "FAIL",
        "shape": shape,
        "expectedShapePrefix": [expected_height, expected_width],
        "shapeOk": shape_ok,
        "finite": finite,
        "nonnegative": nonnegative,
        "blackFrame": black_frame,
        "min": minimum,
        "max": maximum,
        "mean": mean,
    }


def _sample(graph, output):
    import numpy as np

    try:
        data = np.asarray(graph.get_output(output).to_numpy()).copy()
        raw_shape = list(data.shape)
        # Some Mogwai/BlitPass readbacks expose a tightly packed RGBA texture
        # as a flat vector.  Restore the image shape before applying the
        # dimensional and black-frame checks; retain the raw shape in the
        # health record so the adapter is visible in the artifact.
        reshaped_from_flat = False
        if data.ndim == 1 and data.size == WIDTH * HEIGHT * 4:
            data = data.reshape((HEIGHT, WIDTH, 4))
            reshaped_from_flat = True
        health = _health(data, WIDTH, HEIGHT)
        health["rawShape"] = raw_shape
        health["reshapedFromFlatRGBA"] = reshaped_from_flat
        return data, health
    except Exception as exc:
        return None, {"status": "FAIL", "error": str(exc), "output": output}


def _sample_production(graph, direct_output, sentinel_output, case, label):
    """Probe a LumenGI endpoint before considering a BlitPass diagnostic.

    RenderGraph.get_output() is intentionally strict: an unmarked endpoint
    normally raises.  That is evidence that the endpoint is not available to
    this policy, not permission to reinterpret an arbitrary sentinel texture
    as production GI.  Sentinel values are retained only under
    ``sentinelDiagnostics`` for debugging the graph/export path.
    """
    direct, direct_health = _sample(graph, direct_output)
    available = direct is not None
    case["directOutputAvailability"][direct_output] = available
    case.setdefault("directOutputProbes", {})[direct_output] = direct_health
    if available:
        case[label + "Source"] = direct_output
        return direct, direct_health, False

    sentinel, sentinel_health = _sample(graph, sentinel_output)
    case["sentinelDiagnostics"][sentinel_output] = sentinel_health
    return None, {
        "status": "SKIP",
        "reason": "direct LumenGI output is unavailable without markOutput; sentinel is diagnostic only",
        "directOutput": direct_output,
        "directProbe": direct_health,
        "sentinelOutput": sentinel_output,
        "sentinelDiagnostic": sentinel_health,
    }, True


def _pair_summary(left, right):
    import numpy as np

    if left is None or right is None:
        return {"status": "FAIL", "reason": "one or both samples unavailable"}
    if left.shape != right.shape:
        return {"status": "FAIL", "sameShape": False, "leftShape": list(left.shape), "rightShape": list(right.shape)}
    diff = np.abs(left.astype(np.float64) - right.astype(np.float64))
    mean_abs = float(diff.mean()) if diff.size else 0.0
    max_abs = float(diff.max()) if diff.size else 0.0
    return {
        "status": "PASS" if np.isfinite(diff).all() else "FAIL",
        "sameShape": True,
        "shape": list(left.shape),
        "meanAbs": mean_abs,
        "maxAbs": max_abs,
        "leftMean": float(left.mean()) if left.size else 0.0,
        "rightMean": float(right.mean()) if right.size else 0.0,
    }


def _equivalent(left, right):
    import numpy as np

    summary = _pair_summary(left, right)
    if summary.get("status") != "PASS":
        summary["equivalent"] = False
        return summary
    scale = max(abs(summary["leftMean"]), abs(summary["rightMean"]), 1.0e-6)
    relative = summary["meanAbs"] / scale
    summary["relativeMeanAbs"] = relative
    summary["absoluteTolerance"] = EQUIV_ABS_TOL
    summary["relativeTolerance"] = EQUIV_REL_TOL
    summary["equivalent"] = bool(summary["maxAbs"] <= EQUIV_ABS_TOL or relative <= EQUIV_REL_TOL)
    summary["status"] = "PASS" if summary["equivalent"] else "FAIL"
    return summary


def _capture_files(directory):
    if not os.path.isdir(directory):
        return []
    files = []
    for root, _, names in os.walk(directory):
        for name in names:
            files.append(os.path.relpath(os.path.join(root, name), directory))
    return sorted(files)


def _run_case(filter_spec, policy, index, frame_cursor, report, baselines):
    case_dir = os.path.join(OUT_DIR, "%02d_%s_%s" % (index, filter_spec["name"], policy["name"]))
    os.makedirs(case_dir, exist_ok=True)
    case = {
        "index": index,
        "scene": SCENE,
        "seed": SEED,
        "filter": filter_spec["name"],
        "filterProperties": dict(filter_spec["properties"]),
        "policy": dict(policy),
        "sentinelOutputs": list(SENTINEL_OUTPUTS),
        "directOutputsMarked": bool(policy["mark_lumen"]),
        # These fields are populated with a production source only after the
        # direct-output probe below succeeds.  A sentinel is never reported as
        # resolvedDiffuseGI/diffuseGI production data.
        "resolvedSource": DIRECT_OUTPUTS[0] if policy["mark_lumen"] else None,
        "diffuseSource": DIRECT_OUTPUTS[1] if policy["mark_lumen"] else None,
        "sentinelResolvedSource": SENTINEL_OUTPUTS[0],
        "sentinelDiffuseSource": SENTINEL_OUTPUTS[1],
        "directOutputAvailability": {},
        "sentinelDiagnostics": {},
        "finalColor": {
            "status": "SKIP",
            "used": False,
            "reason": "LumenGI.finalColor is not reflected or wired; resolvedDiffuseGI is not a full-scene composite",
        },
        # Frame indices are local to the case.  Every policy therefore starts
        # from the identical frame-1..WARMUP schedule after scene reload.
        "frameStart": 0,
        "warmupFrames": WARMUP_FRAMES,
        "status": "RUNNING",
        "errors": [],
    }
    graph = None
    resolved = diffuse = None
    try:
        # Reloading the identical scene before every case prevents histories or
        # resource residency from making export policy look like a filter change.
        _set_runtime()
        m.loadScene(SCENE)
        m.resizeFrameBuffer(WIDTH, HEIGHT)
        graph = _create_graph(filter_spec, policy, index)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        m.frameCapture.captureAllOutputs = bool(policy["capture_all"])
        m.frameCapture.outputDir = case_dir
        m.frameCapture.baseFilename = "export-equivalence-%02d" % index

        local_frame = 0
        for _ in range(WARMUP_FRAMES):
            local_frame += 1
            m.clock.frame = local_frame
            m.renderFrame()

        resolved, case["resolvedDiffuseGI"], resolved_blocked = _sample_production(
            graph, DIRECT_OUTPUTS[0], SENTINEL_OUTPUTS[0], case, "resolved"
        )
        diffuse, case["diffuseGI"], diffuse_blocked = _sample_production(
            graph, DIRECT_OUTPUTS[1], SENTINEL_OUTPUTS[1], case, "diffuse"
        )
        direct_blocked = bool(resolved_blocked or diffuse_blocked)
        if direct_blocked:
            case["resolvedVsDiffuse"] = {
                "status": "SKIP",
                "reason": "one or both direct LumenGI endpoints are unavailable; sentinel values are not production GI",
            }
            case["equivalenceToMarkOnExportOff"] = {
                "status": "SKIP",
                "reason": "direct output unavailable without markOutput",
            }
        else:
            case["resolvedVsDiffuse"] = _pair_summary(resolved, diffuse)
            key = filter_spec["name"]
            if key not in baselines:
                baselines[key] = {"resolved": resolved, "diffuse": diffuse}
                case["equivalenceToMarkOnExportOff"] = {"status": "BASELINE"}
            else:
                baseline = baselines[key]
                case["equivalenceToMarkOnExportOff"] = {
                    "status": "PASS",
                    "resolved": _equivalent(baseline["resolved"], resolved),
                    "diffuse": _equivalent(baseline["diffuse"], diffuse),
                }
                eq = case["equivalenceToMarkOnExportOff"]
                if eq["resolved"]["status"] != "PASS" or eq["diffuse"]["status"] != "PASS":
                    eq["status"] = "FAIL"

        if policy["export"]:
            try:
                m.frameCapture.capture()
                case["export"] = "PASS"
                case["exportFiles"] = _capture_files(case_dir)
                case["exportFileCount"] = len(case["exportFiles"])
            except Exception as exc:
                case["export"] = "FAIL"
                case["errors"].append("FrameCapture: " + str(exc))
                case["exportFiles"] = _capture_files(case_dir)
        else:
            case["export"] = "DISABLED"
            case["exportFiles"] = []
            case["exportFileCount"] = 0

        direct_health_fail = any(
            case.get(name, {}).get("status") != "PASS" for name in ("resolvedDiffuseGI", "diffuseGI")
        )
        black_fail = any(
            case.get(name, {}).get("blackFrame", True) for name in ("resolvedDiffuseGI", "diffuseGI")
        )
        pair_fail = case.get("resolvedVsDiffuse", {}).get("status") != "PASS"
        eq_fail = case.get("equivalenceToMarkOnExportOff", {}).get("status") == "FAIL"
        export_fail = policy["export"] and case.get("export") != "PASS"
        if direct_blocked:
            # A no-mark policy cannot expose a production LumenGI endpoint in
            # the current RenderGraph contract.  Keep this distinct from a
            # numeric failure and do not promote sentinel diagnostics into a
            # false C8/C9 PASS.
            case["status"] = "BLOCKED"
        else:
            case["status"] = "FAIL" if direct_health_fail or black_fail or pair_fail or eq_fail or export_fail else "PASS"
    except Exception as exc:
        case["status"] = "EXC"
        case["errors"].append(str(exc))
        case["traceback"] = traceback.format_exc(limit=8)
    finally:
        try:
            m.frameCapture.captureAllOutputs = False
        except Exception:
            pass
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as exc:
                case["errors"].append("removeGraph: " + str(exc))

    case["frameEnd"] = WARMUP_FRAMES
    report["runs"].append(case)
    _write_json(os.path.join(OUT_DIR, "export-equivalence.json"), report)
    print(
        "EXPORTEQ case",
        index,
        filter_spec["name"],
        policy["name"],
        "status",
        case["status"],
        "resolvedMean",
        case.get("resolvedDiffuseGI", {}).get("mean"),
        "diffuseMean",
        case.get("diffuseGI", {}).get("mean"),
    )
    return frame_cursor


def main():
    if not OUT_DIR:
        print("EXPORTEQ SKIP: set LUMEN_EXPORT_EQ_OUT to a unique output directory")
        return
    os.makedirs(OUT_DIR, exist_ok=True)
    report = {
        "protocol": "C8-C9-export-equivalence-v1",
        "status": "RUNNING",
        "scene": SCENE,
        "seed": SEED,
        "seedContract": "LumenGI has no host seed; fixed frame index + scene + camera are the deterministic schedule",
        "resolution": [WIDTH, HEIGHT],
        "warmupFrames": WARMUP_FRAMES,
        "blackEpsilon": BLACK_EPS,
        "equivalenceTolerance": {"absolute": EQUIV_ABS_TOL, "relativeMeanAbs": EQUIV_REL_TOL},
        "policies": [dict(policy) for policy in POLICIES],
        "filters": [{"name": spec["name"], "properties": dict(spec["properties"])} for spec in FILTERS],
        "sentinelOutputs": list(SENTINEL_OUTPUTS),
        "directOutputs": list(DIRECT_OUTPUTS),
        "finalColor": {
            "status": "SKIP",
            "used": False,
            "reason": "No LumenGI.finalColor output is marked, sampled, or used as a C8/C9 gate",
        },
        "runs": [],
        "errors": [],
        "reportSchema": REPORT_SCHEMA,
    }
    report_path = os.path.join(OUT_DIR, "export-equivalence.json")
    _write_json(report_path, report)
    baselines = {}
    frame_cursor = 0
    for filter_spec in FILTERS:
        for policy in POLICIES:
            frame_cursor = _run_case(filter_spec, policy, len(report["runs"]), frame_cursor, report, baselines)

    failures = [run for run in report["runs"] if run.get("status") in ("FAIL", "EXC")]
    blocked = [run for run in report["runs"] if run.get("status") == "BLOCKED"]
    expected_runs = len(FILTERS) * len(POLICIES)
    if failures:
        report["status"] = "FAIL"
    elif blocked or len(report["runs"]) != expected_runs:
        report["status"] = "PARTIAL"
    else:
        report["status"] = "PASS"
    _write_json(report_path, report)
    print("EXPORTEQ done", report["status"], "report", report_path)


main()
exit()
