"""C0.2 trusted LumenGI capture/reference protocol.

This is a Mogwai headless-run skeleton for the production-chain closure plan.
It deliberately keeps the capture contract explicit:

* Lumen stages are linear-HDR ``.npy`` snapshots plus FrameCapture EXR
  evidence.  The semantic names are ``rawBaselineGI``, ``probeInterpolated``,
  ``temporalFiltered``, ``spatialFiltered``, ``resolvedDiffuseGI`` and
  ``finalColor``.
* ``resolvedDiffuseGI`` is the C9 production output and is probed with
  RenderGraph reflection. ``finalColor`` remains an optional full-scene
  composite; the script never wires ``spatialFiltered`` directly to ToneMapper
  as a fake final image.
* PathTracer references use a fixed seed schedule.  ``ptDirect`` is bounce 0,
  ``ptIndirect`` is the per-checkpoint mean of bounce 1 minus bounce 0, and
  ``ptFinal`` is a full reference (default four surface bounces).

The script does not build the project.  Run it only through Mogwai after a
Release build, with a unique ``LUMEN_CHAIN_OUT`` directory per evidence run.

Environment configuration (all optional):

    LUMEN_CHAIN_OUT=artifacts/lumengi/chain-closure/P0/captures/run-001
    LUMEN_CHAIN_SCENES=name=path,name=path
    LUMEN_CHAIN_RESOLUTIONS=640x360,800x450,1920x1080
    LUMEN_CHAIN_VIEWS=front,left,right
    LUMEN_CHAIN_WARMUP_FRAMES=1,8,32,96
    LUMEN_CHAIN_PT_BOUNCES=4
    LUMEN_CHAIN_SEED=1337

Scene paths are passed to ``m.loadScene`` unchanged so Falcor's asset resolver
can resolve media paths that are not present in this checkout.
"""

from falcor import *

import json
import math
import os
import traceback


FRAME_RATE = 60
DEFAULT_RESOLUTIONS = ((640, 360), (800, 450), (1920, 1080))
DEFAULT_WARMUP_FRAMES = (1, 8, 32, 96)
BASE_SEED = int(os.environ.get("LUMEN_CHAIN_SEED", "1337"))
PT_FULL_BOUNCES = int(os.environ.get("LUMEN_CHAIN_PT_BOUNCES", "4"))
SAMPLES_PER_FRAME = 16  # PathTracer's current runtime cap.

FIXED_TONE_MAPPER = {"autoExposure": False, "exposureCompensation": 0.0}
FIXED_CAMERA_UP = float3(0, 1, 0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0

# The three view transforms are intentionally frozen.  They are valid for the
# Cornell-style baseline and can be overridden by adding a scene-specific
# wrapper in a later protocol revision; do not silently use scene animation.
VIEW_SPECS = {
    "front": (float3(0, 0.28, 1.2), float3(0, 0.28, 0)),
    "left": (float3(-1.2, 0.42, 0.72), float3(0, 0.28, 0)),
    "right": (float3(1.2, 0.42, 0.72), float3(0, 0.28, 0)),
}

# Current output names are stable in LumenGI.cpp.  finalColor remains a future
# full-scene composite; resolvedDiffuseGI is now the C9 production GI output.
LUMEN_OUTPUTS = {
    "rawBaselineGI": "diffuseGI",          # S1 HWRT, with probe/filter off.
    "rawHitRadiance": "diffuseRadianceHitDist",
    "probeInterpolated": "probeInterpolated",
    "probeHistory": "probeHistory",
    "temporalFiltered": "temporalFiltered",
    "temporalMoments": "temporalMoments",
    "spatialFiltered": "spatialFiltered",
    "filteredVariance": "filteredVariance",
    "resolvedDiffuseGI": "resolvedDiffuseGI",
    "finalColor": "finalColor",                # Full-scene composite; still SKIP.
}

# RenderGraph.markOutput() records a graph endpoint but does not validate the
# field name until compilation.  Keep the current reflection contract explicit
# so a planned C9 channel cannot accidentally alias another output and look like
# a successful capture.  Update this set only when LumenGI::reflect() exposes a
# new production channel and the corresponding consumer is wired.
CURRENT_EXPOSED_OUTPUTS = {
    "diffuseGI",
    "diffuseRadianceHitDist",
    "confidence",
    "bentNormal",
    "debugOutput",
    "cardCoverage",
    "screenTrace",
    "probeRadiance",
    "probeInterpolated",
    "probeHistory",
    "temporalFiltered",
    "temporalMoments",
    "temporalAlpha",
    "temporalConfidence",
    "filteredVariance",
    "spatialFiltered",
    "resolvedDiffuseGI",
    "gdfTrace",
    "cacheDirectRadiance",
}

# Outputs required to make each stage's internal producer execute.  They are
# not substitutes for the semantic stage output and are recorded as such.
STAGE_DEPENDENCIES = {
    "rawBaselineGI": ("diffuseGI", "diffuseRadianceHitDist"),
    "probeInterpolated": (
        "diffuseGI", "diffuseRadianceHitDist", "confidence", "probeInterpolated", "probeHistory",
    ),
    "temporalFiltered": (
        "diffuseGI", "diffuseRadianceHitDist", "confidence", "probeInterpolated",
        "temporalFiltered", "temporalAlpha", "temporalConfidence", "temporalMoments",
    ),
    "spatialFiltered": (
        "diffuseGI", "diffuseRadianceHitDist", "confidence", "probeInterpolated",
        "temporalFiltered", "temporalAlpha", "temporalConfidence", "spatialFiltered",
        "temporalMoments", "filteredVariance",
    ),
    "resolvedDiffuseGI": (
        "diffuseGI", "diffuseRadianceHitDist", "confidence", "probeInterpolated",
        "temporalFiltered", "temporalAlpha", "temporalConfidence", "spatialFiltered",
        "temporalMoments", "filteredVariance", "resolvedDiffuseGI",
    ),
}


def _parse_resolutions(value):
    if not value:
        return DEFAULT_RESOLUTIONS
    result = []
    for token in value.split(","):
        token = token.strip().lower().replace(" ", "")
        if not token:
            continue
        try:
            width, height = (int(part) for part in token.split("x", 1))
            if width <= 0 or height <= 0:
                raise ValueError("non-positive dimensions")
            result.append((width, height))
        except Exception:
            print("CHAINCAPTURE WARNING invalid resolution token", repr(token))
    return tuple(result) or DEFAULT_RESOLUTIONS


def _parse_frames(value):
    if not value:
        return DEFAULT_WARMUP_FRAMES
    values = set()
    for token in value.split(","):
        try:
            frame = int(token.strip())
            if frame > 0:
                values.add(frame)
        except Exception:
            print("CHAINCAPTURE WARNING invalid warmup frame token", repr(token))
    return tuple(sorted(values)) or DEFAULT_WARMUP_FRAMES


def _parse_views(value):
    if not value:
        return tuple(VIEW_SPECS.keys())
    views = tuple(token.strip() for token in value.split(",") if token.strip() in VIEW_SPECS)
    return views or tuple(VIEW_SPECS.keys())


def _parse_scenes(value):
    """Parse ``label=path`` pairs; bare paths receive a filesystem-safe label."""
    if not value:
        return (
            ("cornell", "test_scenes/cornell_box.pyscene"),
            ("arcade", "Arcade/Arcade.pyscene"),
            ("emissive_glow", os.path.join("tests", "lumengi", "scenes", "emissive_glow.pyscene")),
            ("black_room", os.path.join("tests", "lumengi", "scenes", "black_room.pyscene")),
            ("white_furnace", os.path.join("tests", "lumengi", "scenes", "white_furnace.pyscene")),
        )
    scenes = []
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        if "=" in token:
            label, path = token.split("=", 1)
        else:
            path = token
            label = os.path.splitext(os.path.basename(path))[0]
        scenes.append((label.strip(), path.strip()))
    return tuple(scenes)


def _parse_stages(value):
    default = ("rawBaselineGI", "probeInterpolated", "temporalFiltered", "spatialFiltered", "resolvedDiffuseGI")
    if not value:
        return default
    allowed = set(default)
    stages = tuple(token.strip() for token in value.split(",") if token.strip() in allowed)
    return stages or default


RESOLUTIONS = _parse_resolutions(os.environ.get("LUMEN_CHAIN_RESOLUTIONS"))
WARMUP_FRAMES = _parse_frames(os.environ.get("LUMEN_CHAIN_WARMUP_FRAMES"))
VIEWS = _parse_views(os.environ.get("LUMEN_CHAIN_VIEWS"))
SCENES = _parse_scenes(os.environ.get("LUMEN_CHAIN_SCENES"))
STAGES = _parse_stages(os.environ.get("LUMEN_CHAIN_STAGES"))
OUT_DIR = os.path.abspath(os.environ.get("LUMEN_CHAIN_OUT", "artifacts/lumengi/chain-closure/P0/captures/manual"))
MANIFEST_PATH = os.path.join(OUT_DIR, "capture-manifest.json")


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


def _vec3_list(value):
    """Serialize Falcor's non-iterable float3 binding for the manifest."""
    return [float(value.x), float(value.y), float(value.z)]


def _write_manifest(manifest):
    os.makedirs(OUT_DIR, exist_ok=True)
    temp_path = MANIFEST_PATH + ".tmp"
    with open(temp_path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(manifest), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temp_path, MANIFEST_PATH)


def _stem(scene_label, view_name, resolution, semantic, frame):
    width, height = resolution
    return "%s__%s__%dx%d__%s__f%03d" % (
        scene_label, view_name, width, height, semantic, frame
    )


def _set_camera(view_name):
    position, target = VIEW_SPECS[view_name]
    camera = m.scene.camera
    camera.position = position
    camera.target = target
    camera.up = FIXED_CAMERA_UP
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH


def _setup_scene(scene_path, resolution, view_name):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*resolution)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0
    _set_camera(view_name)


def _gbuffer_edges(graph, lumen_pass="LumenGI"):
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
        graph.addEdge("GBufferRT." + source, lumen_pass + "." + destination)


def _lumen_properties(stage):
    return {
        "enabled": True,
        "useSurfaceCache": True,
        "useCacheLighting": True,
        "useScreenTrace": stage != "rawBaselineGI",
        "useScreenProbes": stage != "rawBaselineGI",
        "useTemporalFilter": stage in ("temporalFiltered", "spatialFiltered", "resolvedDiffuseGI"),
        "useSpatialFilter": stage in ("spatialFiltered", "resolvedDiffuseGI"),
        "debugMode": "None",
    }


def _make_lumen_graph(stage):
    graph = RenderGraph("ChainClosureLumen_%s" % stage)
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", _lumen_properties(stage)), "LumenGI")
    _gbuffer_edges(graph)

    marked = []
    for channel in STAGE_DEPENDENCIES.get(stage, (LUMEN_OUTPUTS[stage],)):
        if channel not in CURRENT_EXPOSED_OUTPUTS:
            print("CHAINCAPTURE INFO stage", stage, "channel", channel,
                  "SKIP: not exposed by current LumenGI reflection")
            continue
        try:
            graph.markOutput("LumenGI." + channel)
            marked.append(channel)
        except Exception as exc:
            # Optional channels (notably C9 resolvedDiffuseGI) are reflection
            # probed before the graph is added; no render is attempted here.
            print("CHAINCAPTURE INFO stage", stage, "channel", channel, "unavailable:", str(exc)[:180])

    target = LUMEN_OUTPUTS[stage]
    if target not in marked:
        return None, {
            "status": "SKIP",
            "reason": "required semantic output LumenGI.%s is not exposed by this build" % target,
            "attemptedChannels": list(STAGE_DEPENDENCIES.get(stage, (target,))),
        }

    return graph, {"status": "READY", "markedChannels": marked, "target": "LumenGI." + target}


def _make_final_graph():
    """Build finalColor only when a production final-color output is exposed.

    A ToneMapper over ``spatialFiltered`` or ``resolvedDiffuseGI`` would be an
    indirect-only debug view, so it is intentionally not used as a fallback.
    """
    candidates = []
    configured = os.environ.get("LUMEN_CHAIN_FINAL_CHANNEL", "").strip()
    if configured:
        candidates.append(configured)
    candidates.extend(("finalColor", "resolvedFinalColor"))

    graph = RenderGraph("ChainClosureLumen_finalColor")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", _lumen_properties("resolvedDiffuseGI")), "LumenGI")
    _gbuffer_edges(graph)
    for channel in STAGE_DEPENDENCIES["resolvedDiffuseGI"]:
        if channel not in CURRENT_EXPOSED_OUTPUTS:
            continue
        try:
            graph.markOutput("LumenGI." + channel)
        except Exception:
            pass

    final_channel = None
    for channel in candidates:
        if channel not in CURRENT_EXPOSED_OUTPUTS:
            continue
        try:
            graph.markOutput("LumenGI." + channel)
            final_channel = channel
            break
        except Exception:
            continue
    if final_channel is None:
        return None, {
            "status": "SKIP",
            "reason": "no production finalColor output exposed; refusing to tone-map an intermediate GI channel",
            "candidates": candidates,
        }

    graph.addPass(createPass("ToneMapper", dict(FIXED_TONE_MAPPER)), "ToneMapperDisplay")
    graph.addEdge("LumenGI." + final_channel, "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph, {
        "status": "READY",
        "target": "LumenGI." + final_channel,
        "displayTarget": "ToneMapperDisplay.dst",
    }


def _read_output(graph, output_name, keep_alpha=False):
    import numpy as np

    arr = np.asarray(graph.get_output(output_name).to_numpy(), dtype=np.float32)
    if not keep_alpha and arr.ndim == 3 and arr.shape[-1] >= 3:
        return arr[..., :3]
    return arr


def _read_rgb(graph, output_name):
    return _read_output(graph, output_name, keep_alpha=False)


def _save_linear_array(path, arr):
    import numpy as np

    array = np.asarray(arr, dtype=np.float32)
    finite = bool(np.isfinite(array).all())
    nonnegative = bool(float(array.min()) >= 0.0) if array.size else True
    np.save(path, array)
    return {
        "path": path,
        "shape": list(array.shape),
        "dtype": str(array.dtype),
        "finite": finite,
        "nonnegative": nonnegative,
        "min": float(array.min()) if array.size else 0.0,
        "max": float(array.max()) if array.size else 0.0,
        "mean": float(array.mean()) if array.size else 0.0,
    }


def _capture_frame(graph, semantic, target, scene_label, view_name, resolution, frame, manifest):
    stem = _stem(scene_label, view_name, resolution, semantic, frame)
    npy_path = os.path.join(OUT_DIR, stem + ".npy")
    record = {"semantic": semantic, "frame": frame, "target": target, "npy": npy_path}
    try:
        # Keep RGBA for probeHistory so the C7 accumulated-direction count in
        # alpha remains measurable; all other semantic outputs are RGB HDR.
        record.update(_save_linear_array(
            npy_path, _read_output(graph, target, keep_alpha=(semantic == "probeHistory"))
        ))
    except Exception as exc:
        record["npyError"] = str(exc)
        print("CHAINCAPTURE WARNING readback", semantic, target, str(exc)[:180])

    try:
        m.frameCapture.outputDir = OUT_DIR
        m.frameCapture.baseFilename = stem + "__exr"
        m.frameCapture.capture()
        record["frameCapture"] = "requested"
    except Exception as exc:
        # Readback remains useful even if an optional non-texture output makes
        # FrameCapture unhappy.  Never call this a successful screenshot.
        record["frameCaptureError"] = str(exc)
        print("CHAINCAPTURE WARNING FrameCapture", semantic, str(exc)[:180])
    manifest.setdefault("captures", []).append(record)


def _render_lumen_stage(scene_label, scene_path, resolution, view_name, stage, manifest):
    graph, info = _make_lumen_graph(stage)
    if graph is None:
        return info
    try:
        _setup_scene(scene_path, resolution, view_name)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        target = info["target"]
        max_frame = max(WARMUP_FRAMES)
        for frame in range(1, max_frame + 1):
            m.clock.frame = frame
            m.renderFrame()
            if frame in WARMUP_FRAMES:
                _capture_frame(graph, stage, target, scene_label, view_name, resolution, frame, manifest)
                if stage == "probeInterpolated" and "probeHistory" in info.get("markedChannels", []):
                    _capture_frame(
                        graph,
                        "probeHistory",
                        "LumenGI.probeHistory",
                        scene_label,
                        view_name,
                        resolution,
                        frame,
                        manifest,
                    )
                if stage == "temporalFiltered" and "temporalMoments" in info.get("markedChannels", []):
                    _capture_frame(
                        graph,
                        "temporalMoments",
                        "LumenGI.temporalMoments",
                        scene_label,
                        view_name,
                        resolution,
                        frame,
                        manifest,
                    )
                if stage in ("spatialFiltered", "resolvedDiffuseGI") and "filteredVariance" in info.get("markedChannels", []):
                    _capture_frame(
                        graph,
                        "filteredVariance",
                        "LumenGI.filteredVariance",
                        scene_label,
                        view_name,
                        resolution,
                        frame,
                        manifest,
                    )
        return {"status": "PASS", "target": target, "markedChannels": info.get("markedChannels", [])}
    except Exception as exc:
        return {"status": "FAIL", "reason": str(exc), "traceback": traceback.format_exc(limit=4)}
    finally:
        try:
            m.removeGraph(graph)
        except Exception:
            pass


def _make_pt_graph(max_bounces):
    graph = RenderGraph("ChainClosurePT_b%d" % max_bounces)
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "PathTracer",
            {
                "samplesPerPixel": SAMPLES_PER_FRAME,
                "maxSurfaceBounces": max_bounces,
                "useRussianRoulette": False,
                "fixedSeed": BASE_SEED,
            },
        ),
        "PT",
    )
    graph.addPass(createPass("ToneMapper", dict(FIXED_TONE_MAPPER)), "ToneMapperDisplay")
    graph.addEdge("GBufferRT.vbuffer", "PT.vbuffer")
    graph.addEdge("GBufferRT.mvec", "PT.mvec")
    graph.addEdge("GBufferRT.viewW", "PT.viewW")
    graph.markOutput("PT.color")
    graph.addEdge("PT.color", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph


def _render_pt_variant(scene_label, scene_path, resolution, view_name, bounces, semantic, manifest):
    """Return checkpoint means for a fixed-seed PT variant."""
    import numpy as np

    graph = _make_pt_graph(bounces)
    sums = None
    checkpoints = {}
    try:
        _setup_scene(scene_path, resolution, view_name)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        pt = graph.getPass("PT")
        for frame in range(1, max(WARMUP_FRAMES) + 1):
            pt.fixedSeed = BASE_SEED + frame
            m.clock.frame = frame
            m.renderFrame()
            color = _read_rgb(graph, "PT.color")
            sums = color.copy() if sums is None else sums + color
            if frame in WARMUP_FRAMES:
                mean = sums / float(frame)
                stem = _stem(scene_label, view_name, resolution, semantic, frame)
                path = os.path.join(OUT_DIR, stem + ".npy")
                record = _save_linear_array(path, mean)
                record.update({"semantic": semantic, "frame": frame, "target": "PT.color", "arrayMode": "mean over frames"})
                try:
                    m.frameCapture.outputDir = OUT_DIR
                    m.frameCapture.baseFilename = stem + "__exr"
                    m.frameCapture.capture()
                    record["frameCapture"] = "requested"
                except Exception as exc:
                    record["frameCaptureError"] = str(exc)
                    print("CHAINCAPTURE WARNING PT FrameCapture", semantic, str(exc)[:180])
                manifest.setdefault("captures", []).append(record)
                checkpoints[frame] = mean.copy()
        return checkpoints
    finally:
        try:
            m.removeGraph(graph)
        except Exception:
            pass


def _render_pt_references(scene_label, scene_path, resolution, view_name, manifest):
    import numpy as np

    result = {"status": "PASS", "checkpoints": list(WARMUP_FRAMES), "indirectDefinition": "mean(PT bounce1) - mean(PT bounce0)"}
    try:
        direct = _render_pt_variant(scene_label, scene_path, resolution, view_name, 0, "ptDirect", manifest)
        one_bounce = _render_pt_variant(scene_label, scene_path, resolution, view_name, 1, "ptOneBounce", manifest)
        final = _render_pt_variant(scene_label, scene_path, resolution, view_name, PT_FULL_BOUNCES, "ptFinal", manifest)
        for frame in WARMUP_FRAMES:
            if frame not in direct or frame not in one_bounce or frame not in final:
                continue
            indirect = one_bounce[frame] - direct[frame]
            stem = _stem(scene_label, view_name, resolution, "ptIndirect", frame)
            path = os.path.join(OUT_DIR, stem + ".npy")
            record = _save_linear_array(path, indirect)
            record.update({
                "semantic": "ptIndirect",
                "frame": frame,
                "target": "PT.bounce1 - PT.bounce0",
                "arrayMode": "mean(PT bounce1) - mean(PT bounce0)",
            })
            manifest.setdefault("captures", []).append(record)
        return result
    except Exception as exc:
        return {"status": "FAIL", "reason": str(exc), "traceback": traceback.format_exc(limit=4)}


def main():
    manifest = {
        "protocol": "C0.2-chain-closure-capture-v1",
        "status": "RUNNING",
        "outputDirectory": OUT_DIR,
        "resolutionMatrix": [list(resolution) for resolution in RESOLUTIONS],
        "warmupFrames": list(WARMUP_FRAMES),
        "views": list(VIEWS),
        "scenes": [{"label": label, "path": path} for label, path in SCENES],
        "stages": list(STAGES),
        "fixedCamera": {
            name: {
                "position": _vec3_list(spec[0]),
                "target": _vec3_list(spec[1]),
                "up": _vec3_list(FIXED_CAMERA_UP),
                "focalLength": FIXED_CAMERA_FOCAL_LENGTH,
            }
            for name, spec in VIEW_SPECS.items()
            if name in VIEWS
        },
        "fixedToneMapper": dict(FIXED_TONE_MAPPER),
        "semanticOutputs": dict(LUMEN_OUTPUTS),
        "pt": {
            "baseSeed": BASE_SEED,
            "samplesPerFrame": SAMPLES_PER_FRAME,
            "fullBounces": PT_FULL_BOUNCES,
            "indirect": "PT bounce1 minus PT bounce0 with identical seed schedule",
        },
        "runs": [],
        "captures": [],
        "skips": [],
        "notes": [
            "All numeric arrays are linear HDR float32. PNG/EXR are visual evidence only.",
            "resolvedDiffuseGI is the production GI output; finalColor is SKIP until a full-scene composite is exposed.",
            "rawBaselineGI is LumenGI.diffuseGI with probe/temporal/spatial features disabled.",
            "No intermediate irradiance channel is used as finalColor.",
        ],
    }
    os.makedirs(OUT_DIR, exist_ok=True)
    _write_manifest(manifest)
    print("CHAINCAPTURE protocol", manifest["protocol"])
    print("CHAINCAPTURE output", OUT_DIR)
    print("CHAINCAPTURE resolutions", RESOLUTIONS, "frames", WARMUP_FRAMES, "views", VIEWS)

    for scene_label, scene_path in SCENES:
        for resolution in RESOLUTIONS:
            for view_name in VIEWS:
                unit = {
                    "scene": scene_label,
                    "scenePath": scene_path,
                    "resolution": list(resolution),
                    "view": view_name,
                    "lumen": {},
                    "pt": None,
                }
                for stage in STAGES:
                    result = _render_lumen_stage(scene_label, scene_path, resolution, view_name, stage, manifest)
                    unit["lumen"][stage] = result
                    if result.get("status") == "SKIP":
                        manifest["skips"].append({"kind": "lumen", "stage": stage, **unit})
                    _write_manifest(manifest)

                # finalColor is deliberately independent: a ToneMapper over
                # spatial/resolved output is not a final composite.
                final_graph, final_info = _make_final_graph()
                if final_graph is None:
                    unit["lumen"]["finalColor"] = final_info
                    manifest["skips"].append({"kind": "lumen", "stage": "finalColor", **unit})
                else:
                    # This graph is already validated by reflection.  Render
                    # using the same helper shape but with its display target.
                    try:
                        _setup_scene(scene_path, resolution, view_name)
                        m.addGraph(final_graph)
                        m.setActiveGraph(final_graph)
                        target = final_info["displayTarget"]
                        for frame in range(1, max(WARMUP_FRAMES) + 1):
                            m.clock.frame = frame
                            m.renderFrame()
                            if frame in WARMUP_FRAMES:
                                _capture_frame(final_graph, "finalColor", target, scene_label, view_name, resolution, frame, manifest)
                        unit["lumen"]["finalColor"] = final_info
                    except Exception as exc:
                        unit["lumen"]["finalColor"] = {"status": "FAIL", "reason": str(exc), "traceback": traceback.format_exc(limit=4)}
                    finally:
                        try:
                            m.removeGraph(final_graph)
                        except Exception:
                            pass
                _write_manifest(manifest)

                # PT is expensive, but it is part of the correctness reference
                # and uses a separate graph from the Lumen stage captures.
                unit["pt"] = _render_pt_references(scene_label, scene_path, resolution, view_name, manifest)
                manifest["runs"].append(unit)
                _write_manifest(manifest)
                print("CHAINCAPTURE RUN", scene_label, resolution, view_name, unit["pt"].get("status"))

    has_failure = any(
        run.get("pt", {}).get("status") == "FAIL"
        or any(stage.get("status") == "FAIL" for stage in run.get("lumen", {}).values())
        for run in manifest["runs"]
    )
    manifest["status"] = "PASS" if not has_failure else "PARTIAL"
    _write_manifest(manifest)
    print("CHAINCAPTURE done status", manifest["status"], "manifest", MANIFEST_PATH)


main()
exit()
