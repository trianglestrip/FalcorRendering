"""Capture one deterministic screen-probe convergence sequence.

This is intentionally a small, single-scene/single-view diagnostic harness.  It
renders the same graph at frames 1, 8, 32 and 96 and asks FrameCapture to write
the probe stages plus a fixed-exposure PNG at every checkpoint.  The manifest is
the authority for whether a checkpoint is usable: a missing graph output,
missing required RGBA alpha, non-finite value, or missing capture file is
``BLOCKED``.  The script never invents a substitute channel or reports a
missing resource as a pass.

Run through Mogwai after a Release build.  No build is performed here.

Optional environment variables (all keep the single-scene/single-view shape):

    LUMEN_SCREENPROBE_OUT=artifacts/lumengi/screenprobe-convergence/run-001
    LUMEN_SCREENPROBE_SCENE=test_scenes/cornell_box.pyscene
    LUMEN_SCREENPROBE_RESOLUTION=800x450

The front camera and checkpoint list are deliberately frozen. Resolution is
an explicit environment override so partial-tile validity can be tested in a
separate output directory without changing the comparison protocol.
"""

from falcor import *

import json
import math
import os
import time

import numpy as np


FRAME_RATE = 60


def _parse_resolution(value):
    if not value:
        return (800, 450)
    normalized = value.lower().replace(" ", "")
    for separator in ("x", ","):
        if separator in normalized:
            width, height = normalized.split(separator, 1)
            parsed = (int(width), int(height))
            if parsed[0] < 1 or parsed[1] < 1:
                raise ValueError("resolution must be positive: %s" % value)
            return parsed
    raise ValueError("resolution must be WxH or W,H: %s" % value)


RESOLUTION = _parse_resolution(os.environ.get("LUMEN_SCREENPROBE_RESOLUTION"))
CHECKPOINT_FRAMES = (1, 8, 32, 96)
# Keep Falcor's media resolver in charge of scene paths.  The repository stores
# this scene under media/test_scenes; converting the default to an absolute
# checkout path prevents Mogwai from resolving its referenced assets.
SCENE_PATH = os.environ.get(
    "LUMEN_SCREENPROBE_SCENE", "media/test_scenes/cornell_box.pyscene"
)
SCENE_LABEL = "cornell"
VIEW_NAME = "front"
OUT_DIR = os.path.abspath(
    os.environ.get("LUMEN_SCREENPROBE_OUT", "artifacts/lumengi/screenprobe-convergence/run-001")
)
MANIFEST_PATH = os.path.join(OUT_DIR, "screenprobe-convergence-manifest.json")
BASE_FILENAME = "screenprobe-%s-%s" % (SCENE_LABEL, VIEW_NAME)

FIXED_CAMERA_POSITION = float3(0, 0.28, 1.2)
FIXED_CAMERA_TARGET = float3(0, 0.28, 0)
FIXED_CAMERA_UP = float3(0, 1, 0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0

FIXED_TONE_MAPPER = {
    "autoExposure": False,
    "exposureCompensation": 0.0,
}

# Optional UE-style spatial-filter A/B knobs.  Defaults match the current
# LumenGI production preset; the values are clamped at graph construction just
# like the existing showcase harness.
SPATIAL_RADIUS_MIN = float(os.environ.get("LUMEN_SCREENPROBE_SPATIAL_RADIUS_MIN", "1.0"))
SPATIAL_RADIUS_MAX = float(os.environ.get("LUMEN_SCREENPROBE_SPATIAL_RADIUS_MAX", "4.0"))
SPATIAL_VARIANCE_LOW = float(os.environ.get("LUMEN_SCREENPROBE_SPATIAL_VARIANCE_LOW", "0.0"))
SPATIAL_VARIANCE_HIGH = float(os.environ.get("LUMEN_SCREENPROBE_SPATIAL_VARIANCE_HIGH", "0.20"))
USE_SOURCE_MOMENTS = os.environ.get("LUMEN_SCREENPROBE_USE_SOURCE_MOMENTS", "1") == "1"

# Channels requested by the convergence protocol.  RGBA outputs must retain
# alpha in both the graph resource and the frame-capture EXR.  Confidence is a
# deliberate R32F channel and therefore has no alpha requirement.
REQUIRED_OUTPUTS = {
    "probeInterpolated": {"channels": 4, "alpha": True},
    "probeHistory": {"channels": 4, "alpha": True},
    "temporalConfidence": {"channels": 1, "alpha": False},
    "temporalFiltered": {"channels": 4, "alpha": True},
    "spatialFiltered": {"channels": 4, "alpha": True},
    "resolvedDiffuseGI": {"channels": 4, "alpha": True},
    "screenRadianceHistoryAge": {"channels": 1, "alpha": False},
}
VALIDITY_CHANNEL = "probeValidity"
CAPTURE_VALIDITY = os.environ.get("LUMEN_SCREENPROBE_CAPTURE_VALIDITY", "0") == "1"
CAPTURE_IMAGES = os.environ.get("LUMEN_SCREENPROBE_CAPTURE_IMAGES", "1") == "1"


def _json_safe(value):
    """Return values accepted by json.dump, including numpy scalars."""

    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, (np.integer,)):
        return int(value)
    if isinstance(value, (np.floating,)):
        value = float(value)
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    return str(value)


def _write_manifest(manifest):
    os.makedirs(OUT_DIR, exist_ok=True)
    temporary = MANIFEST_PATH + ".tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(manifest), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temporary, MANIFEST_PATH)


def _texture_array(graph, channel):
    """Read a graph output without hiding a missing/invalid channel."""

    resource = graph.get_output("LumenGI." + channel)
    if resource is None:
        raise RuntimeError("LumenGI.%s returned no resource" % channel)
    # Falcor may expose RGBA16F here.  Promote before reductions so a
    # half-float accumulation (notably history/count alpha) cannot overflow to
    # inf and then serialize as JSON null.
    array = np.asarray(resource.to_numpy(), dtype=np.float64)
    if array.ndim == 2:
        channel_count = 1
    elif array.ndim == 3:
        channel_count = int(array.shape[-1])
    else:
        raise RuntimeError("LumenGI.%s has unsupported shape %s" % (channel, tuple(array.shape)))
    return array, channel_count


def _texture_summary(graph, channel, contract):
    array, channel_count = _texture_array(graph, channel)
    finite = bool(np.isfinite(array).all())
    nonnegative = bool(np.nanmin(array) >= 0.0) if array.size else False
    info = {
        "status": "PASS",
        "shape": list(array.shape),
        "channels": channel_count,
        "expectedChannels": contract["channels"],
        "alphaPresent": channel_count >= 4,
        "finite": finite,
        "nonnegative": nonnegative,
        "mean": float(np.nanmean(array)) if array.size else None,
        "min": float(np.nanmin(array)) if array.size else None,
        "max": float(np.nanmax(array)) if array.size else None,
    }
    reasons = []
    if channel_count != contract["channels"]:
        reasons.append("expected %d channels, got %d" % (contract["channels"], channel_count))
    if contract["alpha"] and channel_count < 4:
        reasons.append("required alpha channel is missing")
    if not finite:
        reasons.append("non-finite value")
    if not nonnegative:
        reasons.append("negative value")
    if reasons:
        info["status"] = "BLOCKED"
        info["reason"] = "; ".join(reasons)
    if contract["alpha"] and channel_count >= 4:
        info["alphaMean"] = float(np.nanmean(array[..., 3]))
        info["alphaMin"] = float(np.nanmin(array[..., 3]))
        info["alphaMax"] = float(np.nanmax(array[..., 3]))
    return info


def _screen_probe_stats(graph):
    """Read the host telemetry without treating an unavailable binding as pass."""

    try:
        raw = dict(graph.getPass("LumenGI").screenProbeStats)
    except Exception as error:
        return {"status": "BLOCKED", "reason": "screenProbeStats: %s" % error}
    stats = {str(key): _json_safe(value) for key, value in raw.items()}
    stats["status"] = "PASS"
    return stats


def _probe_validity_summary(graph, max_probes=256):
    """Decode an actual GPU uint4 validity sidecar for a bounded audit sample.

    The raw buffer is sampled at evenly distributed probe indices to keep the JSON manifest
    reviewable without biasing the result toward the first screen tiles. ``coverage`` records
    the exact number of GPU records and the sample bound; this is telemetry, not an inferred
    image statistic.
    """
    resource = graph.get_output("LumenGI." + VALIDITY_CHANNEL)
    # RenderPassReflection::Field::rawBuffer exposes a byte-address buffer to
    # Python (uint8), not a typed uint4 array. Reinterpret the bytes instead of
    # numerically casting each byte; the latter creates bogus backend codes.
    raw_bytes = np.asarray(resource.to_numpy(), dtype=np.uint8).reshape(-1)
    if raw_bytes.size % 16 != 0:
        raise RuntimeError("probeValidity raw buffer is not uint4-aligned: %d bytes" % raw_bytes.size)
    raw = np.frombuffer(raw_bytes.tobytes(), dtype=np.uint32)
    records = raw.reshape((-1, 4))
    stride = 32
    probe_count = records.shape[0] // stride
    sample_count = min(max(0, int(max_probes)), probe_count)
    if sample_count == 0:
        probe_indices = []
    elif sample_count == probe_count:
        probe_indices = list(range(probe_count))
    else:
        # Include both endpoints and spread the sample over the whole grid. This catches
        # backend changes that are spatially localized (for example screen hits near walls).
        probe_indices = sorted({int(round(i * (probe_count - 1) / (sample_count - 1))) for i in range(sample_count)})
    decoded = []
    for probe in probe_indices:
        for direction in range(stride):
            packed, producer_frame, generation, age = (int(v) for v in records[probe * stride + direction])
            decoded.append({
                "resolution": list(RESOLUTION),
                "frame": int(producer_frame),
                "transition": "steady",
                "backendCode": int(packed & 0xff),
                "sourceBackend": {0: "Invalid", 1: "Screen", 2: "HWRT", 3: "GDF"}.get(int(packed & 0xff), "Unknown"),
                "geometryValid": bool((packed >> 8) & 1),
                "radianceValid": bool((packed >> 9) & 1),
                "producerFrame": int(producer_frame),
                "generation": int(generation),
                "age": int(age),
                "resetReason": int((packed >> 16) & 0xff),
                "transitionCase": {"camera": False, "light": False, "material": False, "geometry": False},
                "probeGrid": [int((RESOLUTION[0] + 7) // 8), int((RESOLUTION[1] + 7) // 8)],
                "probeIndex": int(probe),
                "directionIndex": int(direction),
            })
    return {
        "schemaVersion": "LumenGI.ProbeValiditySidecar.v1",
        "recordCount": int(records.shape[0]),
        "sampledRecordCount": len(decoded),
        "sampledProbeCount": len(probe_indices),
        "directionsPerProbe": stride,
        "records": decoded,
    }


def _capture_file(base, channel, frame, extension, timeout_seconds=1.0):
    """Resolve one FrameCapture file, tolerating asynchronous writeback.

    Callers pass a semantic channel (for example ``probeInterpolated`` or
    ``ToneMapperDisplay.dst``).  Lumen graph outputs receive exactly one
    ``LumenGI.`` prefix here; accepting an already-qualified name as well keeps
    this helper safe for future callers without producing a double prefix.
    """

    if channel.startswith("LumenGI."):
        output_name = channel
    elif channel in REQUIRED_OUTPUTS:
        output_name = "LumenGI." + channel
    else:
        output_name = channel

    prefix = base + "." + output_name + "."
    suffix = "." + extension
    deadline = time.monotonic() + max(0.0, float(timeout_seconds))
    while True:
        try:
            filenames = os.listdir(OUT_DIR)
        except OSError:
            filenames = ()
        for filename in filenames:
            if not filename.startswith(prefix) or not filename.endswith(suffix):
                continue
            frame_token = filename[len(prefix) : -len(suffix)]
            if frame_token == str(frame):
                return os.path.join(OUT_DIR, filename)
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.02)


def _graph():
    graph = RenderGraph("ScreenProbeConvergence")
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
                "useScreenRadianceMoments": USE_SOURCE_MOMENTS,
                "useSpatialFilter": True,
                "spatialRadiusMin": max(0.0, SPATIAL_RADIUS_MIN),
                "spatialRadiusMax": max(0.0, SPATIAL_RADIUS_MAX),
                "spatialVarianceThresholdLow": max(0.0, SPATIAL_VARIANCE_LOW),
                "spatialVarianceThresholdHigh": max(0.0, SPATIAL_VARIANCE_HIGH),
                "debugMode": "None",
            },
        ),
        "LumenGI",
    )
    graph.addPass(createPass("ToneMapper", dict(FIXED_TONE_MAPPER)), "ToneMapperDisplay")

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
    if CAPTURE_VALIDITY:
        graph.markOutput("LumenGI." + VALIDITY_CHANNEL)
    graph.addEdge("LumenGI.resolvedDiffuseGI", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph


def _setup_scene():
    m.loadScene(SCENE_PATH)
    m.resizeFrameBuffer(*RESOLUTION)
    m.scene.camera.position = FIXED_CAMERA_POSITION
    m.scene.camera.target = FIXED_CAMERA_TARGET
    m.scene.camera.up = FIXED_CAMERA_UP
    m.scene.camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0


def _capture_checkpoint(graph, frame, previous_frame):
    entry = {
        "frame": frame,
        "renderedFrom": previous_frame + 1,
        "renderedThrough": frame,
        "status": "PASS",
        "outputs": {},
        "screenProbeStats": {},
        "probeValidity": {},
        "files": {},
        "errors": [],
    }
    try:
        # Do not jump the clock directly from 1 to 96: temporal history is
        # produced by every intervening frame. Only the four checkpoints are
        # captured, while all frames in-between are still rendered.
        for render_frame in range(previous_frame + 1, frame + 1):
            m.clock.frame = render_frame
            m.renderFrame()
    except Exception as error:
        entry["status"] = "BLOCKED"
        entry["errors"].append("renderFrame: %s" % error)
        return entry

    for channel, contract in REQUIRED_OUTPUTS.items():
        try:
            info = _texture_summary(graph, channel, contract)
        except Exception as error:
            info = {"status": "BLOCKED", "reason": str(error)}
        entry["outputs"][channel] = info
        if info.get("status") != "PASS":
            entry["status"] = "BLOCKED"

    entry["screenProbeStats"] = _screen_probe_stats(graph)
    if entry["screenProbeStats"].get("status") != "PASS":
        entry["status"] = "BLOCKED"

    if CAPTURE_VALIDITY:
        try:
            entry["probeValidity"] = _probe_validity_summary(graph)
        except Exception as error:
            entry["status"] = "BLOCKED"
            entry["errors"].append("probeValidity: %s" % error)

    base = BASE_FILENAME
    if CAPTURE_IMAGES:
        m.frameCapture.outputDir = OUT_DIR
        m.frameCapture.baseFilename = base
        try:
            m.frameCapture.capture()
        except Exception as error:
            entry["status"] = "BLOCKED"
            entry["errors"].append("frameCapture.capture: %s" % error)

        for channel, contract in REQUIRED_OUTPUTS.items():
            path = _capture_file(base, channel, frame, "exr")
            entry["files"][channel] = {
                "path": path,
                "exists": path is not None and os.path.isfile(path),
                "format": "EXR",
                "alphaRequired": contract["alpha"],
            }
            if not entry["files"][channel]["exists"]:
                entry["status"] = "BLOCKED"
                entry["errors"].append("missing FrameCapture EXR for LumenGI.%s" % channel)

        png_path = _capture_file(base, "ToneMapperDisplay.dst", frame, "png")
        entry["files"]["ToneMapperDisplay.dst"] = {
            "path": png_path,
            "exists": png_path is not None and os.path.isfile(png_path),
            "format": "PNG",
        }
        if not entry["files"]["ToneMapperDisplay.dst"]["exists"]:
            entry["status"] = "BLOCKED"
            entry["errors"].append("missing ToneMapperDisplay.dst PNG")
    return entry


def _initial_manifest():
    return {
        "schema": "LumenGI.ScreenProbeConvergence.v1",
        "status": "BLOCKED",
        "scene": {"label": SCENE_LABEL, "path": SCENE_PATH},
        "view": {
            "name": VIEW_NAME,
            "position": [0.0, 0.28, 1.2],
            "target": [0.0, 0.28, 0.0],
            "up": [0.0, 1.0, 0.0],
            "focalLength": FIXED_CAMERA_FOCAL_LENGTH,
        },
        "resolution": list(RESOLUTION),
        "frameRate": FRAME_RATE,
        "checkpointFrames": list(CHECKPOINT_FRAMES),
        "probeDirectionsPerProbe": 32,
        "useScreenRadianceMoments": USE_SOURCE_MOMENTS,
        "spatialFilter": {
            "radiusMin": max(0.0, SPATIAL_RADIUS_MIN),
            "radiusMax": max(0.0, SPATIAL_RADIUS_MAX),
            "varianceThresholdLow": max(0.0, SPATIAL_VARIANCE_LOW),
            "varianceThresholdHigh": max(0.0, SPATIAL_VARIANCE_HIGH),
        },
        "fixedToneMapper": dict(FIXED_TONE_MAPPER),
        "capturePrefix": BASE_FILENAME,
        "captureDirectory": OUT_DIR,
        "requiredOutputs": list(REQUIRED_OUTPUTS),
        "requiredTelemetry": ["screenProbeStats", "probeValidity"],
        "captureValidity": CAPTURE_VALIDITY,
        "captureImages": CAPTURE_IMAGES,
        "captures": [],
        "errors": [],
    }


def main():
    manifest = _initial_manifest()
    os.makedirs(OUT_DIR, exist_ok=True)
    graph = None
    try:
        _setup_scene()
        graph = _graph()
        m.addGraph(graph)
        m.setActiveGraph(graph)
        previous_frame = 0
        for frame in CHECKPOINT_FRAMES:
            manifest["captures"].append(_capture_checkpoint(graph, frame, previous_frame))
            previous_frame = frame
    except Exception as error:
        manifest["errors"].append("setup/graph: %s" % error)
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as error:
                manifest["errors"].append("removeGraph: %s" % error)

    manifest["status"] = (
        "PASS"
        if not manifest["errors"]
        and len(manifest["captures"]) == len(CHECKPOINT_FRAMES)
        and all(item.get("status") == "PASS" for item in manifest["captures"])
        else "BLOCKED"
    )
    _write_manifest(manifest)
    print("SCREENPROBE_CONVERGENCE", manifest["status"], MANIFEST_PATH)


m.ui = False
main()
exit()
