"""C4 E1/E2 GDF compose descriptor-bisect runner.

This is a deliberately tiny single-GPU gate.  It creates a fresh Cornell graph,
enables MeshSDF/GDF and selects the opt-in ``gdfDiagnosticStage`` property:
stage 1 binds only the level UAV (E1), while stage 2 binds the complete compose
descriptor contract (E2).  The host log is the authoritative dispatch result;
the JSON records script exceptions and the reflected gdfStats when available.

Environment:
``LUMEN_GDF_DIAG_STAGE``  0..6 (default 1): production, E1, E2, E2a buffers, E2b atlas, E2c buffers+uniform, E2d CB+uniform
``LUMEN_GDF_DIAG_OUT``    unique output directory
``LUMEN_GDF_DIAG_RESOLUTION`` WIDTHxHEIGHT (default 320x180)
``LUMEN_GDF_DIAG_WARMUP`` frames (default 1; a fresh first frame is the gate)
"""

from falcor import *

import json
import os
import traceback


SCENE = "test_scenes/cornell_box.pyscene"
LABEL = "c4-gdf-diagnostic"


def _resolution(value):
    if not value:
        return (320, 180)
    try:
        tokens = value.lower().replace("x", ",").split(",")
        if len(tokens) != 2:
            raise ValueError("expected WIDTHxHEIGHT")
        width, height = (int(token.strip()) for token in tokens)
        if width <= 0 or height <= 0:
            raise ValueError("dimensions must be positive")
        return (width, height)
    except Exception as exc:
        print("C4_CONFIG invalid resolution", repr(value), repr(exc))
        return (320, 180)


def _positive_int(value, fallback):
    try:
        parsed = int(value)
        return parsed if parsed > 0 else fallback
    except Exception:
        return fallback


def _stage(value):
    try:
        parsed = int(value)
        return parsed if 0 <= parsed <= 6 else 1
    except Exception:
        return 1


STAGE = _stage(os.environ.get("LUMEN_GDF_DIAG_STAGE"))
RESOLUTION = _resolution(os.environ.get("LUMEN_GDF_DIAG_RESOLUTION"))
WARMUP = _positive_int(os.environ.get("LUMEN_GDF_DIAG_WARMUP"), 1)
OUT_DIR = os.path.abspath(
    os.environ.get("LUMEN_GDF_DIAG_OUT") or "artifacts/lumengi/C4/gdf-diagnostic"
)


def _graph():
    options = {
        "enabled": True,
        "traceMode": "HardwareRT",
        "useGDF": True,
        "useSurfaceCache": False,
        "useCacheLighting": False,
        "useScreenProbes": False,
        "useTemporalFilter": False,
        "useSpatialFilter": False,
        "gdfDiagnosticStage": STAGE,
        "meshSDFResolution": 32,
        "gdfLevelCount": 2,
        "gdfResolution": 64,
        "gdfBaseExtent": 4.0,
        "gdfTraceMaxSteps": 32,
        "gdfTraceMaxDistance": 20.0,
        "gdfEmptyDistanceScale": 8.0,
    }
    graph = RenderGraph(LABEL + "Graph")
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
    # Keep the public GI resource alive for Final Resolve without enabling the
    # optional gdfTrace output (which would run the separate trace pass).
    graph.markOutput("LumenGI.diffuseGI")
    return graph


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    record = {
        "script": "run_gdf_diag_dispatch.py",
        "stage": STAGE,
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "warmup_frames": WARMUP,
        "output_dir": OUT_DIR,
        "dispatch_contract": {
            "stage0": "production compose path",
            "stage1": "E1 single UAV, logical threads (1,1,1)",
            "stage2": "E2 all compose descriptors, logical threads (1,1,1)",
            "stage3": "E2a CB + GDF buffers, logical threads (1,1,1)",
            "stage4": "E2b atlas descriptors + scalars, logical threads (1,1,1)",
            "stage5": "E2c CB + GDF buffers + one uniform, logical threads (1,1,1)",
            "stage6": "E2d CB + one uniform, logical threads (1,1,1)",
        },
    }
    graph = None
    try:
        m.ui = False
        m.clock.framerate = 60
        m.clock.time = 0
        m.clock.pause()
        m.resizeFrameBuffer(*RESOLUTION)
        graph = _graph()
        m.addGraph(graph)
        m.setActiveGraph(graph)
        m.loadScene(SCENE)
        for frame in range(1, WARMUP + 1):
            m.clock.frame = frame
            m.renderFrame()
        try:
            record["gdf_stats"] = m.activeGraph.getPass("LumenGI").gdfStats
        except Exception as exc:
            record["gdf_stats_error"] = repr(exc)
        record["status"] = "OK"
    except Exception as exc:
        record["status"] = "EXC"
        record["exception"] = repr(exc)
        record["traceback"] = traceback.format_exc()
        traceback.print_exc()
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as exc:
                record["cleanup_error"] = repr(exc)
    path = os.path.join(OUT_DIR, "gdf-diagnostic.json")
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(record, stream, indent=2, sort_keys=True, default=str)
        stream.write("\n")
    print("C4_DIAG_DONE", json.dumps(record, sort_keys=True))


main()
exit()
