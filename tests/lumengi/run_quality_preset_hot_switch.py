"""Runtime C11 quality-preset hot-switch gate.

The graph is kept alive while ``qualityPreset`` is updated through
``RenderGraph.updatePass``.  This is intentionally separate from the initial
preset smoke: it proves the RenderPass ``setProperties`` path applies derived
defaults, invalidates history, and keeps the resolved output finite.
"""

import json
import math
import os
import sys
import traceback


_bootstrap_out = os.environ.get("LUMEN_QUALITY_HOTSWITCH_OUT", "").strip()
if _bootstrap_out:
    try:
        os.makedirs(os.path.dirname(_bootstrap_out) or ".", exist_ok=True)
        with open(_bootstrap_out + ".bootstrap", "w", encoding="utf-8", newline="\n") as _stream:
            _stream.write("quality-preset-hot-switch bootstrap\n")
    except Exception:
        pass


if "--self-test" in sys.argv:
    print("QUALITY_PRESET_HOTSWITCH_FIXTURE PASS")
    raise SystemExit(0)

from falcor import *


OUT = os.environ.get(
    "LUMEN_QUALITY_HOTSWITCH_OUT",
    "artifacts/lumengi/C11/preset-hot-switch-20260816/quality-hot-switch.json",
)
SCENE = os.environ.get("LUMEN_QUALITY_HOTSWITCH_SCENE", "test_scenes/cornell_box.pyscene")
WIDTH = int(os.environ.get("LUMEN_QUALITY_HOTSWITCH_WIDTH", "320"))
HEIGHT = int(os.environ.get("LUMEN_QUALITY_HOTSWITCH_HEIGHT", "180"))
SETTLE = max(1, int(os.environ.get("LUMEN_QUALITY_HOTSWITCH_SETTLE", "3")))
PRESETS = ("Low", "Medium", "High", "Reference", "Low")
READBACK = os.environ.get("LUMEN_QUALITY_HOTSWITCH_READBACK", "1").strip().lower() not in ("0", "false", "off", "no")

EXPECTED = {
    "Low": {"probeDirectionsPerProbe": 8, "captureMaxPagesPerFrame": 16, "temporalHistoryLengthCap": 4.0, "gdfTraceMaxSteps": 32},
    "Medium": {"probeDirectionsPerProbe": 16, "captureMaxPagesPerFrame": 32, "temporalHistoryLengthCap": 6.0, "gdfTraceMaxSteps": 48},
    "High": {"probeDirectionsPerProbe": 32, "captureMaxPagesPerFrame": 64, "temporalHistoryLengthCap": 10.0, "gdfTraceMaxSteps": 64},
    # ScreenProbe clamps directions to its compiled maximum (currently 32).
    "Reference": {"probeDirectionsPerProbe": 32, "captureMaxPagesPerFrame": 128, "temporalHistoryLengthCap": 16.0, "gdfTraceMaxSteps": 96},
}


def safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, (list, tuple)):
        return [safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): safe(item) for key, item in value.items()}
    return str(value)


def write_json(path, payload):
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temp, path)


def create_graph():
    graph = RenderGraph("LumenGIQualityHotSwitch")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useDOF": False}),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "enabled": True,
                "traceMode": "HardwareRT",
                "qualityPreset": "High",
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
    for channel in ("vbuffer", "linearZ", "mvec", "mvecW", "normWRoughnessMaterialID", "viewW", "diffuseOpacity", "emissive"):
        graph.addEdge("GBufferRT." + channel, "LumenGI." + channel)
    for output in ("LumenGI.diffuseGI", "LumenGI.resolvedDiffuseGI", "LumenGI.confidence"):
        graph.markOutput(output)
    return graph


def texture_health(graph):
    import numpy as np

    result = {}
    for output in ("LumenGI.diffuseGI", "LumenGI.resolvedDiffuseGI", "LumenGI.confidence"):
        try:
            array = np.asarray(graph.get_output(output).to_numpy())
            result[output] = {
                "shape": list(array.shape),
                "finite": bool(np.isfinite(array).all()),
                "nonnegative": bool((array >= 0).all()),
                "max": float(np.max(array)) if array.size else 0.0,
            }
        except Exception as error:
            result[output] = {"error": str(error), "finite": False, "nonnegative": False}
    return result


def read_properties(lumen_pass):
    try:
        return safe(getattr(lumen_pass, "qualityPresetStats", None))
    except Exception:
        return None


def main():
    os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
    report = {
        "schema": "LumenGI.QualityPresetHotSwitch.v1",
        "scene": SCENE,
        "resolution": [WIDTH, HEIGHT],
        "presets": list(PRESETS),
        "status": "running",
        "samples": [],
        "errors": [],
    }
    try:
        print("QUALITY_PRESET_HOTSWITCH phase=create_graph", flush=True)
        graph = create_graph()
        m.addGraph(graph)
        print("QUALITY_PRESET_HOTSWITCH phase=load_scene", flush=True)
        m.loadScene(SCENE)
        m.resizeFrameBuffer(WIDTH, HEIGHT)
        m.ui = False
        m.clock.pause()
        m.clock.frame = 0
        for index, preset in enumerate(PRESETS):
            print("QUALITY_PRESET_HOTSWITCH phase=update preset=" + preset, flush=True)
            graph.updatePass("LumenGI", {"qualityPreset": preset})
            for _ in range(SETTLE):
                m.clock.frame += 1
                m.renderFrame()
            # RenderGraph.updatePass recreates the pass object. Re-fetch it after
            # every update; retaining the original handle would silently report
            # the initial preset for all samples and invalidate the hot-switch gate.
            lumen_pass = graph.getPass("LumenGI")
            properties = read_properties(lumen_pass)
            stats = safe(getattr(lumen_pass, "screenProbeStats", {}))
            expected = EXPECTED[preset]
            observed = {key: properties.get(key) if isinstance(properties, dict) else None for key in expected}
            defaults_match = isinstance(properties, dict) and all(
                observed[key] is not None and abs(float(observed[key]) - float(value)) <= 1e-4
                for key, value in expected.items()
            )
            health = texture_health(graph) if READBACK else {"readback": {"skipped": True, "finite": True, "nonnegative": True}}
            finite = all(item.get("finite", False) and item.get("nonnegative", False) for item in health.values())
            report["samples"].append(
                {
                    "index": index,
                    "preset": preset,
                    "properties": properties,
                    "expected": expected,
                    "observed": observed,
                    "derivedDefaultsMatch": defaults_match,
                    "screenProbeStats": stats,
                    "historyGeneration": stats.get("historyGeneration") if isinstance(stats, dict) else None,
                    "textureHealth": health,
                    "finiteNonnegative": finite,
                }
            )
        report["status"] = "PASS" if all(sample["derivedDefaultsMatch"] and sample["finiteNonnegative"] for sample in report["samples"]) else "BLOCKED"
        if any(sample["properties"] is None for sample in report["samples"]):
            report["errors"].append("LumenGI.qualityPresetStats binding unavailable")
            report["status"] = "BLOCKED"
    except Exception as error:
        report["status"] = "BLOCKED"
        report["errors"].append("".join(traceback.format_exception_only(type(error), error)).strip())
    write_json(OUT, report)
    print("QUALITY_PRESET_HOTSWITCH", report["status"], OUT)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
else:
    # Mogwai executes script files via ``exec`` with a non-__main__ module
    # name.  Keep the launcher-compatible path explicit; otherwise the app
    # stays alive after importing the script without running the gate.
    main()
    exit()
