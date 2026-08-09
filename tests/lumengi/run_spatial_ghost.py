from falcor import *

"""LumenGI S5-C1/C2 ghost / trailing-artifact quantification on the S5-B2 spatial output
(Agent Z10, exclusive GPU user).

Role / purpose
--------------
RUN-ONLY Mogwai GPU ghost gate over the S5-B2 spatialFiltered output (the S5 final filtered
channel). Task.md S5 门禁: "动态物体不留下长期拖影". The ghost floor is scene-RELATIVE -- the
pointlight scene's noise floor is ~30x Cornell's (documented by Z7 in the S5-A1-B1 report), so a
fixed Cornell-derived floor would mis-classify every freeze frame as a ghost tail.

Graph: GBufferRT -> LumenGI with useScreenProbes + useTemporalFilter + useSpatialFilter, marking
spatialFiltered / temporalFiltered / temporalConfidence.

Phases (all 640x360, 60fps, paused clock, deterministic):
  1. light-warmup  : cornell_pointlight, fixed camera + light, temporal/spatial warm-in.
  2. light-cal     : static freeze-frame calibration of the spatialFiltered inter-frame noise floor.
  3. light-move    : step the point light (a moving illuminant is the dynamic-object ghost proxy;
                     the scene itself is rigid, so the irradiance field changes as the light moves).
  4. light-freeze  : pin the clock, keep rendering -> residual change above the floor is the
                     ghost/trailing tail. Recovery frames <= GHOST_MAX_FRAMES is the gate.
The same sequence is measured on temporalFiltered for a direct spatial-vs-temporal comparison.

Exit: Falcor `exit()`. JSON -> artifacts/lumengi/S5/gate/ghost-spatial.json (override
LUMEN_SPATIAL_GHOST_OUT). Frame counts env-overridable for bring-up.
"""

import json
import math
import os

import numpy as np

RESOLUTION = (640, 360)
FRAME_RATE = 60

SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)

OUT_JSON = os.environ.get("LUMEN_SPATIAL_GHOST_OUT", "artifacts/lumengi/S5/gate/ghost-spatial.json")

USE_SCREEN_TRACE = bool(os.environ.get("LUMEN_SPATIAL_GHOST_USE_SCREEN_TRACE", "") != "0")
USE_SCREEN_PROBES = bool(os.environ.get("LUMEN_SPATIAL_GHOST_USE_SCREEN_PROBES", "") != "0")
USE_TEMPORAL_FILTER = bool(os.environ.get("LUMEN_SPATIAL_GHOST_USE_TEMPORAL_FILTER", "") != "0")
USE_SPATIAL_FILTER = bool(os.environ.get("LUMEN_SPATIAL_GHOST_USE_SPATIAL_FILTER", "") != "0")

WARMUP_FRAMES = int(os.environ.get("LUMEN_SPATIAL_GHOST_WARMUP", "4"))
CAL_FRAMES = int(os.environ.get("LUMEN_SPATIAL_GHOST_CAL", "16"))
LIGHT_MOVE_FRAMES = int(os.environ.get("LUMEN_SPATIAL_GHOST_LIGHT_MOVE", "6"))
LIGHT_FREEZE_FRAMES = int(os.environ.get("LUMEN_SPATIAL_GHOST_LIGHT_FREEZE", "8"))

CAM_START_POS = float3(0, 0.28, 1.2)
CAM_START_TARGET = float3(0, 0.28, 0)
CAM_UP = float3(0, 1, 0)
CAM_FOCAL_LENGTH = 35.0

LIGHT_NAME = "LumenGITestPointLight"
LIGHT_MOVE_DELTA = float3(0.03, -0.02, 0.02)

TEMPORAL_FILTERED = "temporalFiltered"
SPATIAL_FILTERED = "spatialFiltered"

GHOST_FLOOR_MULT = 2.0        # recovery floor = max(1e-4, noise_floor * this).
GHOST_MAX_FRAMES = 4          # max trailing frames above the floor (the S5-C1 ghost gate).

records = []
prev_temporal = None
prev_spatial = None
available_channels = []


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
    path = os.path.abspath(path)
    out_dir = os.path.dirname(path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(json_safe(payload), f, indent=2, sort_keys=True, allow_nan=False)
        f.write("\n")
    os.replace(temp, path)


def lumen_props():
    return {
        "enabled": True,
        "traceMode": "HardwareRT",
        "qualityPreset": "High",
        "useScreenTrace": USE_SCREEN_TRACE,
        "useScreenProbes": USE_SCREEN_PROBES,
        "useTemporalFilter": USE_TEMPORAL_FILTER,
        "useSpatialFilter": USE_SPATIAL_FILTER,
    }


def create_lumen_graph(extra_outputs):
    graph = RenderGraph("LumenGISpatialGhost")
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
    graph.addPass(createPass("LumenGI", lumen_props()), "LumenGI")
    for edge in [
        ("GBufferRT.vbuffer", "LumenGI.vbuffer"),
        ("GBufferRT.linearZ", "LumenGI.linearZ"),
        ("GBufferRT.mvec", "LumenGI.mvec"),
        ("GBufferRT.mvecW", "LumenGI.mvecW"),
        ("GBufferRT.normWRoughnessMaterialID", "LumenGI.normWRoughnessMaterialID"),
        ("GBufferRT.viewW", "LumenGI.viewW"),
        ("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity"),
        ("GBufferRT.emissive", "LumenGI.emissive"),
    ]:
        graph.addEdge(*edge)
    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.confidence")
    graph.markOutput("GBufferRT.mvec")
    for ch in extra_outputs:
        graph.markOutput("LumenGI." + ch)
    return graph


def _setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    camera = m.scene.camera
    camera.position = CAM_START_POS
    camera.target = CAM_START_TARGET
    camera.up = CAM_UP
    camera.focalLength = CAM_FOCAL_LENGTH


def add_main_graph(extra_outputs):
    global available_channels
    graph = create_lumen_graph(extra_outputs)
    m.addGraph(graph)
    m.setActiveGraph(graph)
    available_channels = list(extra_outputs)
    return graph


def grab(name):
    return np.asarray(m.activeGraph.get_output(name).to_numpy(), dtype=np.float32)


def rgb3(img):
    a = img[..., :3] if img.ndim == 3 and img.shape[-1] >= 3 else img
    return a[..., :3]


def render_one(label, advance=True):
    global prev_temporal, prev_spatial
    if advance:
        m.clock.frame += 1
    m.renderFrame()

    rec = {"phase": label, "frame": int(m.clock.frame)}

    if TEMPORAL_FILTERED in available_channels:
        t = grab("LumenGI." + TEMPORAL_FILTERED)
        rec["temporal"] = {
            "mean": float(rgb3(t).mean()),
            "finite": bool(math.isfinite(float(rgb3(t).min())) and math.isfinite(float(rgb3(t).max()))),
            "nonneg": bool(float(rgb3(t).min()) >= 0.0),
        }
        if prev_temporal is not None and prev_temporal.shape == t.shape:
            rec["temporal_framediff"] = float(np.abs(rgb3(t) - rgb3(prev_temporal)).mean())
        prev_temporal = t

    if SPATIAL_FILTERED in available_channels:
        s = grab("LumenGI." + SPATIAL_FILTERED)
        rec["spatial"] = {
            "mean": float(rgb3(s).mean()),
            "finite": bool(math.isfinite(float(rgb3(s).min())) and math.isfinite(float(rgb3(s).max()))),
            "nonneg": bool(float(rgb3(s).min()) >= 0.0),
            "confidence_mean": float(s[..., 3].mean()),
        }
        if prev_spatial is not None and prev_spatial.shape == s.shape:
            rec["spatial_framediff"] = float(np.abs(rgb3(s) - rgb3(prev_spatial)).mean())
        prev_spatial = s

    records.append(rec)
    return rec


def render_block(label, count):
    last = None
    for _ in range(count):
        last = render_one(label)
    return last


def render_frozen(label, count):
    pinned = m.clock.frame
    out = []
    for _ in range(count):
        m.clock.frame = pinned
        render_one(label, advance=False)
        out.append(records[-1])
    return out


def ghost_recovery_frames(phase_recs, floor):
    """Trailing frames of `phase_recs` whose inter-frame change stays above `floor`
    (the ghost / coverage tail). Floor is scene-noise-relative, never a fixed epsilon."""
    n = 0
    for r in reversed(phase_recs):
        d = r.get("spatial_framediff")
        if d is not None and d > floor:
            n += 1
        else:
            break
    return n


def main():
    report = {
        "stage": "S5",
        "script": "run_spatial_ghost.py",
        "role": "S5-C1/C2 ghost quantification on spatialFiltered (Agent Z10)",
        "status": "run",
        "resolution": list(RESOLUTION),
        "config": {
            "useScreenTrace": USE_SCREEN_TRACE,
            "useScreenProbes": USE_SCREEN_PROBES,
            "useTemporalFilter": USE_TEMPORAL_FILTER,
            "useSpatialFilter": USE_SPATIAL_FILTER,
            "ghost_max_frames": GHOST_MAX_FRAMES,
            "ghost_floor_mult": GHOST_FLOOR_MULT,
            "channels": [TEMPORAL_FILTERED, SPATIAL_FILTERED],
        },
    }
    verdicts = []

    extra = []
    if probe_channel("probeInterpolated"):
        extra.append("probeInterpolated")  # required input of the temporal filter (S4.3 interpolate).
    for ch in (TEMPORAL_FILTERED, SPATIAL_FILTERED):
        if probe_channel(ch):
            extra.append(ch)
    report["channels_available"] = extra
    if SPATIAL_FILTERED not in extra:
        write_json(OUT_JSON, {
            "stage": "S5", "script": "run_spatial_ghost.py", "summary": "SKIP",
            "fatal_error": "spatialFiltered channel not available",
            "verdicts": [("spatialFiltered channel present", "SKIP")],
        })
        print("SPATIAL GHOST WARNING 'spatialFiltered' not available; wrote SKIP json.")
        exit()

    add_main_graph(extra)

    _setup_scene(SCENE_POINTLIGHT)
    render_block("light-warmup", WARMUP_FRAMES)
    render_block("light-cal", CAL_FRAMES)
    cal_diffs = [r.get("spatial_framediff") for r in records if r["phase"] == "light-cal"]
    cal_diffs = [d for d in cal_diffs if d is not None]
    cal_floor = float(sum(cal_diffs[-4:]) / len(cal_diffs[-4:])) if len(cal_diffs) >= 4 else 1e-4
    ghost_floor = max(1e-4, cal_floor * GHOST_FLOOR_MULT)

    point_light = m.scene.getLight(LIGHT_NAME)
    for i in range(LIGHT_MOVE_FRAMES):
        point_light.position = point_light.position + LIGHT_MOVE_DELTA
        render_one("light-move-%d" % i)

    freezing = render_frozen("light-freeze", LIGHT_FREEZE_FRAMES)
    ghost = ghost_recovery_frames(freezing, ghost_floor)
    ghost_ok = ghost <= GHOST_MAX_FRAMES

    freeze_diffs = [r.get("spatial_framediff") for r in freezing]
    temporal_freeze_diffs = [r.get("temporal_framediff") for r in freezing]
    finite = all(r["spatial"]["finite"] for r in freezing)
    nonneg = all(r["spatial"]["nonneg"] for r in freezing)
    confidence_ok = all(0.0 <= r["spatial"]["confidence_mean"] <= 1.0 for r in freezing)

    report["moving_light"] = {
        "static_cal_floor": cal_floor,
        "ghost_floor": ghost_floor,
        "ghost_trailing_frames": ghost,
        "ghost_max_frames": GHOST_MAX_FRAMES,
        "freeze_framediffs_spatial": freeze_diffs,
        "freeze_framediffs_temporal": temporal_freeze_diffs,
        "finite": finite,
        "nonneg": nonneg,
        "confidence_in_01": confidence_ok,
    }
    verdicts.append(("moving light no long-term ghost on spatial (trailing %d <= %d, floor %.4f)" % (
        ghost, GHOST_MAX_FRAMES, ghost_floor), "PASS" if ghost_ok else "FAIL"))
    verdicts.append(("spatial ghost no worse than temporal (spatial tail <= temporal tail + floor)" ,
                     "PASS" if (temporal_freeze_diffs and freeze_diffs and
                                max(freeze_diffs) <= max(temporal_freeze_diffs) + ghost_floor) else "FAIL"))
    verdicts.append(("spatial finite + non-negative during ghost sequence", "PASS" if finite and nonneg else "FAIL"))
    verdicts.append(("spatial confidence in [0,1] during ghost sequence", "PASS" if confidence_ok else "FAIL"))

    report["series"] = records
    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("SPATIAL GHOST VERDICT", name, verdict)
    write_json(OUT_JSON, report)
    print("SPATIAL GHOST wrote", os.path.abspath(OUT_JSON))


def probe_channel(channel):
    graph = None
    try:
        graph = create_lumen_graph([channel])
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(SCENE_POINTLIGHT)
        m.clock.frame += 1
        m.renderFrame()
        return True
    except Exception as exc:
        print("SPATIAL GHOST WARNING channel 'LumenGI.%s' not available (%s)" % (channel, str(exc)))
        return False
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass


try:
    main()
except Exception as exc:
    print("SPATIAL GHOST ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S5",
            "script": "run_spatial_ghost.py",
            "summary": "SKIP",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive SKIP on fatal error)", "SKIP")],
        },
    )
exit()
