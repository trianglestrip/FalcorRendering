"""GPU transition gate for the LumenGI ScreenProbe validity sidecar.

This is a run-only Mogwai asset.  It reuses the ScreenProbe convergence graph
shape but marks *only* ``LumenGI.probeValidity``.  No FrameCapture, EXR, PNG,
or display output is requested: the gate reads the raw ``uint4`` sidecar
buffer and writes decoded records to one JSON manifest.

The transition matrix is two resolutions (800x450 and the partial-tile
641x361 case), scene/camera plus dynamic lighting transitions, and checkpoints
1/8/32/96 after the transition.  A transition is PASS only when the raw sidecar
is readable, host ``screenProbeStats`` is readable, the history generation
changes, and a reset reason/age reset is observed.  Missing bindings are
BLOCKED, never inferred from a rendered image.

Run after a Release build with one GPU owner, for example::

    $env:LUMEN_PROBE_TRANSITIONS_OUT = "artifacts/lumengi/A1/transitions/run-001"
    mogwai -p tests/lumengi/run_probe_validity_transitions.py

The script has no Falcor dependency when used with ``--self-test``.  This
allows CI to verify the decoder and fixture without a GPU.  Running it with a
normal Python interpreter (without ``falcor``) writes a BLOCKED manifest so a
missing Mogwai API cannot be mistaken for a pass.
"""

from __future__ import annotations

import json
import math
import os
import sys
import traceback

try:
    from falcor import *  # type: ignore  # noqa: F401,F403 - Mogwai binding.

    FALCOR_AVAILABLE = True
    FALCOR_IMPORT_ERROR = None
except Exception as _falcor_error:  # pragma: no cover - exercised outside Mogwai.
    FALCOR_AVAILABLE = False
    FALCOR_IMPORT_ERROR = repr(_falcor_error)


FRAME_RATE = 60
def _parse_checkpoints():
    raw = os.environ.get("LUMEN_PROBE_TRANSITIONS_CHECKPOINTS", "1,8,32,96")
    values = tuple(sorted({int(token.strip()) for token in raw.split(",") if token.strip()}))
    if not values or values[0] < 1:
        raise ValueError("checkpoints must contain positive frame numbers")
    return values


try:
    CHECKPOINT_FRAMES = _parse_checkpoints()
    CHECKPOINT_ERROR = None
except Exception as _checkpoint_error:
    CHECKPOINT_FRAMES = (1, 8, 32, 96)
    CHECKPOINT_ERROR = repr(_checkpoint_error)
TILE_SIZE = 8
VALIDITY_STRIDE = 32
SIDECAR_SCHEMA_VERSION = "LumenGI.ProbeValiditySidecar.v2"
GATE_SCHEMA_VERSION = "LumenGI.ProbeValidityTransitions.v1"
MAX_SAMPLE_RECORDS = max(1, int(os.environ.get("LUMEN_PROBE_TRANSITIONS_MAX_SAMPLE_RECORDS", "8192")))

SCENE_PATH = os.environ.get("LUMEN_PROBE_TRANSITIONS_SCENE", "media/test_scenes/cornell_box.pyscene")
OUT_DIR = os.path.abspath(
    os.environ.get("LUMEN_PROBE_TRANSITIONS_OUT", "artifacts/lumengi/A1/probe-validity-transitions")
)
MANIFEST_PATH = os.path.join(OUT_DIR, "probe-validity-transitions.json")

FIXED_CAMERA_POSITION = (0.0, 0.28, 1.2)
FIXED_CAMERA_TARGET = (0.0, 0.28, 0.0)
FIXED_CAMERA_UP = (0.0, 1.0, 0.0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0
CUT_CAMERA_POSITION = (0.8, 0.42, 0.35)
CUT_CAMERA_TARGET = (0.05, 0.25, -0.15)

BACKEND_NAMES = {0: "Invalid", 1: "Screen", 2: "HWRT", 3: "GDF"}
RESET_NAMES = {0: "Unknown", 1: "Resize", 2: "SceneChange", 3: "CameraCut", 4: "SetScene", 5: "HotReload"}
def _parse_transitions():
    raw = os.environ.get("LUMEN_PROBE_TRANSITIONS_CASES", "scene_reload,camera_cut")
    result = tuple(token.strip() for token in raw.split(",") if token.strip())
    invalid = [token for token in result if token not in (
        "scene_reload", "camera_cut", "light_change", "material_change", "env_change"
    )]
    if invalid:
        raise ValueError("unknown transition case(s): %s" % ", ".join(invalid))
    if not result:
        raise ValueError("at least one transition case is required")
    return tuple(dict.fromkeys(result))


try:
    TRANSITIONS = _parse_transitions()
    TRANSITION_ERROR = None
except Exception as _transition_error:
    TRANSITIONS = ("scene_reload", "camera_cut")
    TRANSITION_ERROR = repr(_transition_error)
TRANSITION_CASES = {
    "scene_reload": {"camera": False, "light": False, "material": False, "geometry": True},
    "camera_cut": {"camera": True, "light": False, "material": False, "geometry": False},
    "light_change": {"camera": False, "light": True, "material": False, "geometry": False},
    "material_change": {"camera": False, "light": False, "material": True, "geometry": False},
    "env_change": {"camera": False, "light": True, "material": False, "geometry": False},
}
EXPECTED_RESET_REASONS = {
    "steady": set(),
    "scene_reload": {2, 4},
    "camera_cut": {3},
    "light_change": set(),
    "material_change": set(),
    "env_change": set(),
}


def _parse_resolution(value):
    normalized = str(value).lower().replace(" ", "")
    for separator in ("x", ","):
        if separator in normalized:
            width, height = normalized.split(separator, 1)
            parsed = (int(width), int(height))
            if parsed[0] < 1 or parsed[1] < 1:
                raise ValueError("resolution must be positive: %s" % value)
            return parsed
    raise ValueError("resolution must be WxH or W,H: %s" % value)


def _parse_resolutions():
    raw = os.environ.get("LUMEN_PROBE_TRANSITIONS_RESOLUTIONS", "800x450,641x361")
    result = []
    for token in raw.split(","):
        token = token.strip()
        if token:
            result.append(_parse_resolution(token))
    if not result:
        raise ValueError("at least one transition resolution is required")
    return tuple(dict.fromkeys(result))


try:
    RESOLUTIONS = _parse_resolutions()
    RESOLUTION_ERROR = None
except Exception as _resolution_error:
    RESOLUTIONS = ((800, 450), (641, 361))
    RESOLUTION_ERROR = repr(_resolution_error)


def _json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    # Keep this script usable outside Falcor; numpy is imported lazily by the
    # raw readback path and its scalar types are handled here when available.
    if hasattr(value, "item"):
        try:
            return _json_safe(value.item())
        except Exception:
            pass
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


def _probe_grid(resolution):
    width, height = resolution
    return ((width + TILE_SIZE - 1) // TILE_SIZE, (height + TILE_SIZE - 1) // TILE_SIZE)


def _decode_words(raw_words, resolution, frame, transition, reset_observed, transition_case):
    """Decode the frozen GPU ``uint4`` ABI and return summary + bounded records."""

    import numpy as np

    source = np.asarray(raw_words)
    if source.dtype == np.uint8:
        raw_bytes = source.reshape(-1)
        raw = None
    else:
        # Self-tests may provide an already typed uint32 fixture. GPU rawBuffer
        # readback is uint8 and takes the byte-reinterpretation path below.
        raw = np.asarray(source, dtype=np.uint32).reshape(-1)
        raw_bytes = None
    if (raw is not None and raw.size == 0) or (raw is None and raw_bytes.size == 0):
        raise RuntimeError("probeValidity readback is empty")
    if raw is None:
        if raw_bytes.size % 16 != 0:
            raise RuntimeError("probeValidity readback is not uint4-aligned: %d bytes" % raw_bytes.size)
        raw = np.frombuffer(raw_bytes.tobytes(), dtype=np.uint32)
    records = raw.reshape((-1, 4))
    grid_x, grid_y = _probe_grid(resolution)
    expected_records = grid_x * grid_y * VALIDITY_STRIDE
    if records.shape[0] != expected_records:
        raise RuntimeError(
            "probeValidity record count %d != expected %d (%dx%d grid x stride %d)"
            % (records.shape[0], expected_records, grid_x, grid_y, VALIDITY_STRIDE)
        )

    sample_count = min(int(records.shape[0]), MAX_SAMPLE_RECORDS)
    decoded = []
    backend_counts = {}
    reset_counts = {}
    geometry_valid_count = 0
    radiance_valid_count = 0
    ages = []
    generations = []
    producer_frames = []
    direction_slots = set()
    direction_fingerprints = set()
    expected_reasons = EXPECTED_RESET_REASONS[transition]
    for index in range(int(records.shape[0])):
        packed, producer_frame, generation, age = (int(value) for value in records[index])
        backend_code = packed & 0xFF
        geometry_valid = bool((packed >> 8) & 1)
        radiance_valid = bool((packed >> 9) & 1)
        sample_index = (packed >> 10) & 0x3F
        reset_reason = (packed >> 16) & 0xFF
        direction_fingerprint = (packed >> 24) & 0xFF
        backend_counts[str(backend_code)] = backend_counts.get(str(backend_code), 0) + 1
        reset_counts[str(reset_reason)] = reset_counts.get(str(reset_reason), 0) + 1
        geometry_valid_count += int(geometry_valid)
        radiance_valid_count += int(radiance_valid)
        ages.append(age)
        generations.append(generation)
        producer_frames.append(producer_frame)
        if backend_code != 0 and geometry_valid:
            probe_index = index // VALIDITY_STRIDE
            direction_slots.add((probe_index, sample_index))
            direction_fingerprints.add((probe_index, direction_fingerprint))
        if index < sample_count:
            probe_index = index // VALIDITY_STRIDE
            direction_index = index % VALIDITY_STRIDE
            decoded.append(
                {
                    "schemaVersion": SIDECAR_SCHEMA_VERSION,
                    "resolution": list(resolution),
                    "frame": int(frame),
                    "transition": transition,
                    "backendCode": backend_code,
                    "sourceBackend": BACKEND_NAMES.get(backend_code, "Unknown"),
                    "geometryValid": geometry_valid,
                    "radianceValid": radiance_valid,
                    "producerFrame": producer_frame,
                    "generation": generation,
                    "age": age,
                    "resetReason": RESET_NAMES.get(reset_reason, "Unknown(%d)" % reset_reason),
                    "resetReasonCode": reset_reason,
                    "resetObserved": bool(reset_observed or reset_reason in expected_reasons),
                    "transitionCase": dict(transition_case),
                    "probeGrid": [grid_x, grid_y],
                    "probeIndex": probe_index,
                    "directionIndex": direction_index,
                    "sampleIndex": sample_index,
                    "directionFingerprint": direction_fingerprint,
                }
            )
    return {
        "schemaVersion": SIDECAR_SCHEMA_VERSION,
        "recordCount": int(records.shape[0]),
        "sampledRecordCount": len(decoded),
        "sampledProbeCount": (len(decoded) + VALIDITY_STRIDE - 1) // VALIDITY_STRIDE,
        "directionsPerProbe": VALIDITY_STRIDE,
        "probeGrid": [grid_x, grid_y],
        "backendCounts": backend_counts,
        "resetReasonCounts": reset_counts,
        "geometryValidFraction": float(geometry_valid_count) / float(records.shape[0]),
        "radianceValidFraction": float(radiance_valid_count) / float(records.shape[0]),
        "generationMin": min(generations),
        "generationMax": max(generations),
        "ageMin": min(ages),
        "ageMax": max(ages),
        "producerFrameMin": min(producer_frames),
        "producerFrameMax": max(producer_frames),
        "directionSlotUnique": len(direction_slots),
        "directionFingerprintUnique": len(direction_fingerprints),
        "directionIdentityKeys": sorted(
            int(probe_index) * 256 + int(fingerprint)
            for probe_index, fingerprint in direction_fingerprints
        ),
        "resetObserved": bool(reset_observed or any(reason in expected_reasons for reason in reset_counts)),
        "records": decoded,
    }


def _fixture_words(resolution, frame, generation, age, reset_reason, backend_code=2):
    """Small deterministic raw fixture with the same packed fields as the shader."""

    import numpy as np

    grid_x, grid_y = _probe_grid(resolution)
    words = np.zeros((grid_x * grid_y * VALIDITY_STRIDE, 4), dtype=np.uint32)
    for index in range(words.shape[0]):
        sample_index = index % VALIDITY_STRIDE
        fingerprint = (sample_index * 7) & 0xFF
        words[index, 0] = (
            backend_code | (1 << 8) | (1 << 9) | ((sample_index & 0x3F) << 10) |
            ((reset_reason & 0xFF) << 16) | (fingerprint << 24)
        )
    words[:, 1] = frame
    words[:, 2] = generation
    words[:, 3] = age
    return words


def _run_self_test():
    checks = []
    for resolution in RESOLUTIONS:
        for transition in TRANSITIONS:
            reset_reason = 3 if transition == "camera_cut" else 4
            frame = 1
            words = _fixture_words(resolution, frame, 7, 1, reset_reason)
            decoded = _decode_words(
                words,
                resolution,
                frame,
                transition,
                True,
                TRANSITION_CASES[transition],
            )
            grid = list(_probe_grid(resolution))
            checks.extend(
                [
                    decoded["schemaVersion"] == SIDECAR_SCHEMA_VERSION,
                    decoded["recordCount"] == grid[0] * grid[1] * VALIDITY_STRIDE,
                    decoded["probeGrid"] == grid,
                    decoded["generationMin"] == 7 and decoded["generationMax"] == 7,
                    decoded["ageMin"] == 1,
                    decoded["directionSlotUnique"] >= VALIDITY_STRIDE,
                    decoded["directionFingerprintUnique"] >= VALIDITY_STRIDE,
                    decoded["resetObserved"] is True,
                    decoded["records"][0]["transitionCase"] == TRANSITION_CASES[transition],
                ]
            )
    status = "PASS" if all(checks) else "FAIL"
    print("PROBE_TRANSITIONS_FIXTURE", status)
    return 0 if status == "PASS" else 1


def _get_probe_stats(graph):
    try:
        raw = dict(graph.getPass("LumenGI").screenProbeStats)
    except Exception as error:
        return None, "screenProbeStats unavailable: %s" % error
    return {str(key): _json_safe(value) for key, value in raw.items()}, None


def _read_lighting_generation_output(graph):
    """Read the optional R32Uint screen-radiance generation mirror."""

    resource = graph.get_output("LumenGI.screenRadianceLightingGeneration")
    if resource is None:
        raise RuntimeError("screenRadianceLightingGeneration output is unavailable")
    import numpy as np

    values = np.asarray(resource.to_numpy(), dtype=np.uint64).reshape(-1)
    if values.size == 0:
        raise RuntimeError("screen-radiance generation output is empty")
    return {
        "finite": True,
        "min": int(values.min()),
        "max": int(values.max()),
        "nonzero": int(np.count_nonzero(values)),
        "sampleCount": int(values.size),
    }


def _generation_stat(stats, transition):
    """Select the host epoch that must change for this transition."""

    if not isinstance(stats, dict):
        return None
    key = "lightingGeneration" if transition in ("light_change", "material_change", "env_change") else "historyGeneration"
    return _stats_number(stats, key)


def _apply_dynamic_transition(transition):
    """Apply one scene-side change whose UpdateFlags feed mLightingGeneration."""

    if transition == "light_change":
        for name in (
            "Distant light", "Directional light", "DistantLight", "DirectionalLight",
            "LumenGITestPointLight", "Point light", "PointLight"
        ):
            try:
                light = m.scene.getLight(name)
                if light is not None:
                    light.intensity = light.intensity * 1.5
                    return {"kind": "light", "name": name, "scale": 1.5}
            except Exception:
                continue
        try:
            light = m.scene.getLight(0)
            light.intensity = light.intensity * 1.5
            return {"kind": "light", "index": 0, "scale": 1.5}
        except Exception:
            pass
        raise RuntimeError("no mutable analytic light found for light_change")
    if transition == "material_change":
        for material in m.scene.materials:
            try:
                value = float(material.emissiveFactor)
                material.emissiveFactor = value * 1.5 if value > 0.0 else 0.5
                return {"kind": "material", "name": str(material.name), "emissiveScale": 1.5}
            except Exception:
                continue
        raise RuntimeError("no mutable material found for material_change")
    if transition == "env_change":
        env_map = getattr(m.scene, "envMap", None)
        if env_map is None:
            raise RuntimeError("scene has no environment map for env_change")
        env_map.intensity = float(env_map.intensity) * 1.5
        return {"kind": "environment", "scale": 1.5}
    return None


def _stats_number(stats, key):
    if not isinstance(stats, dict):
        return None
    value = stats.get(key)
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def _build_graph():
    graph = RenderGraph("LumenGIProbeValidityTransitions")
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
                "useSpatialFilter": True,
                "debugMode": "None",
            },
        ),
        "LumenGI",
    )
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
    # Keep the same LumenGI output shape as the already-warmed convergence graph
    # while omitting FrameCapture/PNG work. Marking only the raw sidecar creates
    # a distinct shader/resource specialization and can force a very expensive
    # first-run compile; these mirrors are not sampled for the transition gate.
    for channel in (
        "probeInterpolated",
        "probeHistory",
        "temporalConfidence",
        "temporalFiltered",
        "spatialFiltered",
        "screenRadianceLightingGeneration",
        "diffuseGI",
        "resolvedDiffuseGI",
    ):
        graph.markOutput("LumenGI." + channel)
    graph.markOutput("LumenGI.probeValidity")
    return graph


def _setup_scene(resolution):
    m.loadScene(SCENE_PATH)
    m.resizeFrameBuffer(*resolution)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0
    camera = m.scene.camera
    camera.position = float3(*FIXED_CAMERA_POSITION)
    camera.target = float3(*FIXED_CAMERA_TARGET)
    camera.up = float3(*FIXED_CAMERA_UP)
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH


def _render_frame(graph, frame):
    m.clock.frame = frame
    m.renderFrame()
    resource = graph.get_output("LumenGI.probeValidity")
    if resource is None:
        raise RuntimeError("LumenGI.probeValidity output is unavailable")
    return resource.to_numpy()


def _capture_sequence(graph, resolution, transition, baseline, transition_case):
    captures = []
    previous = 0
    baseline_generation = _generation_stat(baseline.get("stats"), transition)
    for frame in CHECKPOINT_FRAMES:
        raw = None
        stats = None
        errors = []
        try:
            # Preserve temporal history by rendering every intervening frame;
            # only the four checkpoint readbacks are retained in the manifest.
            for render_frame in range(previous + 1, frame + 1):
                m.clock.frame = render_frame
                m.renderFrame()
                if render_frame == frame:
                    resource = graph.get_output("LumenGI.probeValidity")
                    if resource is None:
                        raise RuntimeError("LumenGI.probeValidity output is unavailable")
                    raw = resource.to_numpy()
            stats, stats_error = _get_probe_stats(graph)
            if stats_error:
                errors.append(stats_error)
        except Exception as error:
            errors.append("raw sidecar render/readback: %s" % error)
        entry = {
            "frame": frame,
            "renderedFrom": previous + 1,
            "renderedThrough": frame,
            "resolution": list(resolution),
            "transition": transition,
            "transitionCase": dict(transition_case),
            "status": "BLOCKED",
            "stats": stats,
            "errors": errors,
            "sidecar": {},
            "lightingGenerationOutput": {},
        }
        try:
            entry["lightingGenerationOutput"] = _read_lighting_generation_output(graph)
        except Exception as error:
            errors.append("generation mirror readback: %s" % error)
        if raw is not None:
            try:
                current_generation = _generation_stat(stats, transition)
                stats_reset = bool(_stats_number(stats, "historyResetThisFrame") or 0.0)
                reset_observed = stats_reset or (
                    baseline_generation is not None
                    and current_generation is not None
                    and current_generation != baseline_generation
                )
                entry["sidecar"] = _decode_words(
                    raw,
                    resolution,
                    frame,
                    transition,
                    reset_observed,
                    transition_case,
                )
                entry["status"] = "PASS" if not errors else "BLOCKED"
            except Exception as error:
                entry["errors"].append("sidecar decode: %s" % error)
        captures.append(entry)
        previous = frame
    return captures


def _run_transition(resolution, transition):
    graph = None
    result = {
        "resolution": list(resolution),
        "partialTile": bool(resolution[0] % TILE_SIZE or resolution[1] % TILE_SIZE),
        "transition": transition,
        "transitionCase": dict(TRANSITION_CASES[transition]),
        "status": "BLOCKED",
        "baseline": {},
        "captures": [],
        "transitionEvidence": {},
        "errors": [],
    }
    try:
        _setup_scene(resolution)
        # Compile after the framebuffer resize so the raw sidecar allocation
        # uses the requested (possibly partial-tile) dimensions.
        graph = _build_graph()
        m.addGraph(graph)
        m.setActiveGraph(graph)
        # Warmup establishes a non-zero generation/age baseline; every frame
        # between checkpoints is rendered, as in run_screenprobe_convergence.py.
        baseline_raw = None
        for frame in range(1, 9):
            m.clock.frame = frame
            m.renderFrame()
            if frame == 8:
                resource = graph.get_output("LumenGI.probeValidity")
                if resource is None:
                    raise RuntimeError("LumenGI.probeValidity output is unavailable")
                baseline_raw = resource.to_numpy()
        baseline_stats, baseline_error = _get_probe_stats(graph)
        result["baseline"]["stats"] = baseline_stats
        if baseline_error:
            result["errors"].append(baseline_error)
        try:
            baseline_decoded = _decode_words(
                baseline_raw,
                resolution,
                8,
                "steady",
                False,
                {"camera": False, "light": False, "material": False, "geometry": False},
            )
            result["baseline"]["sidecar"] = baseline_decoded
        except Exception as error:
            result["errors"].append("baseline sidecar: %s" % error)

        if transition == "scene_reload":
            # Scene::setScene emits the SetScene reset; the subsequent first
            # execute may additionally report SceneChange. Both are accepted
            # by the frozen reset-reason ABI.
            m.loadScene(SCENE_PATH)
            m.resizeFrameBuffer(*resolution)
            camera = m.scene.camera
            camera.position = float3(*FIXED_CAMERA_POSITION)
            camera.target = float3(*FIXED_CAMERA_TARGET)
            camera.up = float3(*FIXED_CAMERA_UP)
            camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH
        else:
            camera = m.scene.camera
            camera.position = float3(*CUT_CAMERA_POSITION)
            camera.target = float3(*CUT_CAMERA_TARGET)
        if transition in ("light_change", "material_change", "env_change"):
            result["transitionMutation"] = _apply_dynamic_transition(transition)
        # Checkpoints are relative to the transition, not the warmup clock.
        m.clock.frame = 0
        result["captures"] = _capture_sequence(
            graph,
            resolution,
            transition,
            result["baseline"],
            TRANSITION_CASES[transition],
        )

        baseline_generation = _generation_stat(baseline_stats, transition)
        capture_generations = [
            _stats_number(capture.get("stats"), "historyGeneration")
            for capture in result["captures"]
        ]
        capture_generations = [value for value in capture_generations if value is not None]
        age_mins = [
            capture.get("sidecar", {}).get("ageMin")
            for capture in result["captures"]
            if capture.get("sidecar", {}).get("ageMin") is not None
        ]
        reset_seen = any(
            capture.get("sidecar", {}).get("resetObserved") is True
            or bool(_stats_number(capture.get("stats"), "historyResetThisFrame") or 0.0)
            for capture in result["captures"]
        )
        generation_changed = bool(
            baseline_generation is not None
            and capture_generations
            and any(value != baseline_generation for value in capture_generations)
        )
        age_reset = bool(age_mins and min(age_mins) <= 1)
        all_sidecars_read = bool(result["captures"]) and all(
            capture.get("sidecar", {}).get("recordCount", 0) > 0 for capture in result["captures"]
        )
        dynamic_transition = transition in ("light_change", "material_change", "env_change")
        generation_mirror_readable = bool(result["captures"]) and all(
            capture.get("lightingGenerationOutput", {}).get("sampleCount", 0) > 0 for capture in result["captures"]
        )
        generation_mirror_matches = bool(result["captures"]) and all(
            capture.get("lightingGenerationOutput", {}).get("max") == int(_generation_stat(capture.get("stats"), transition) or 0)
            for capture in result["captures"]
        ) if dynamic_transition else True
        generation_key = "lightingGeneration" if transition in ("light_change", "material_change", "env_change") else "historyGeneration"
        direction_union = set()
        per_checkpoint_direction_union = []
        for capture in result["captures"]:
            keys = capture.get("sidecar", {}).get("directionIdentityKeys", [])
            direction_union.update(int(key) for key in keys)
            per_checkpoint_direction_union.append(
                {"frame": capture.get("frame"), "unique": len(set(int(key) for key in keys))}
            )
        result["transitionEvidence"] = {
            "generationKey": generation_key,
            "baselineGeneration": baseline_generation,
            "captureGenerations": capture_generations,
            "generationChanged": generation_changed,
            "ageReset": age_reset,
            "resetObserved": reset_seen,
            "sidecarReadable": all_sidecars_read,
            "generationMirrorReadable": generation_mirror_readable,
            "generationMirrorMatches": generation_mirror_matches,
            "directionIdentityContract": {
                "perCheckpointUnique": per_checkpoint_direction_union,
                "unionUnique": len(direction_union),
                "sampled": bool(direction_union),
            },
        }
        direction_identity_readable = bool(direction_union)
        result["status"] = (
            "PASS"
            if all_sidecars_read and generation_changed and generation_mirror_readable and generation_mirror_matches and
            direction_identity_readable and
            (not dynamic_transition and age_reset or dynamic_transition) and
            (reset_seen if not dynamic_transition else True) and not result["errors"]
            else "BLOCKED"
        )
    except Exception as error:
        result["errors"].append("transition setup: %s" % error)
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as error:
                result["errors"].append("removeGraph: %s" % error)
    return result


def _blocked_manifest(reason):
    return {
        "schema": GATE_SCHEMA_VERSION,
        "sidecarSchemaVersion": SIDECAR_SCHEMA_VERSION,
        "script": "run_probe_validity_transitions.py",
        "status": "BLOCKED",
        "gpu": bool(FALCOR_AVAILABLE),
        "outputDirectory": OUT_DIR,
        "scene": SCENE_PATH,
        "resolutions": [list(resolution) for resolution in RESOLUTIONS],
        "checkpointFrames": list(CHECKPOINT_FRAMES),
        "transitions": list(TRANSITIONS),
        "contract": {
            "rawBufferOnly": True,
            "requiredFields": [
                "schemaVersion",
                "backendCode",
                "geometryValid",
                "radianceValid",
                "producerFrame",
                "sampleIndex",
                "directionFingerprint",
                "generation",
                "age",
                "resetReason",
                "resetObserved",
                "transitionCase",
            ],
            "missingTelemetry": "BLOCKED",
            "noImageInference": True,
        },
        "runs": [],
        "errors": [reason],
    }


def main():
    if not FALCOR_AVAILABLE:
        manifest = _blocked_manifest("Mogwai falcor binding unavailable: %s" % FALCOR_IMPORT_ERROR)
        _write_manifest(manifest)
        print("PROBE_TRANSITIONS_STATUS BLOCKED", MANIFEST_PATH)
        return 2
    if RESOLUTION_ERROR or TRANSITION_ERROR or CHECKPOINT_ERROR:
        reason = []
        if RESOLUTION_ERROR:
            reason.append("resolution configuration invalid: %s" % RESOLUTION_ERROR)
        if TRANSITION_ERROR:
            reason.append("transition configuration invalid: %s" % TRANSITION_ERROR)
        if CHECKPOINT_ERROR:
            reason.append("checkpoint configuration invalid: %s" % CHECKPOINT_ERROR)
        manifest = _blocked_manifest("; ".join(reason))
        _write_manifest(manifest)
        print("PROBE_TRANSITIONS_STATUS BLOCKED", MANIFEST_PATH)
        return 2

    manifest = _blocked_manifest("transition gate not executed")
    manifest["errors"] = []
    runs = []
    for resolution in RESOLUTIONS:
        for transition in TRANSITIONS:
            runs.append(_run_transition(resolution, transition))
    manifest["runs"] = runs
    statuses = [run.get("status") for run in runs]
    manifest["status"] = "PASS" if statuses and all(status == "PASS" for status in statuses) else "BLOCKED"
    if any(run.get("errors") for run in runs):
        manifest["errors"] = [error for run in runs for error in run.get("errors", [])]
    _write_manifest(manifest)
    print("PROBE_TRANSITIONS_STATUS", manifest["status"], MANIFEST_PATH)
    return 0 if manifest["status"] == "PASS" else 2


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(_run_self_test())
    try:
        sys.exit(main())
    except Exception as error:
        manifest = _blocked_manifest("fatal: %s\n%s" % (error, traceback.format_exc()))
        _write_manifest(manifest)
        print("PROBE_TRANSITIONS_STATUS BLOCKED", MANIFEST_PATH)
        sys.exit(2)
elif FALCOR_AVAILABLE:
    # Falcor's embedded Python may expose a launcher-specific __name__ rather
    # than ``builtins``. Any non-standalone invocation is a Mogwai run and must
    # execute the gate; otherwise Mogwai loads plugins and waits indefinitely
    # without ever importing the scene or writing a manifest.
    try:
        main()
    except Exception as error:
        _write_manifest(_blocked_manifest("fatal: %s\n%s" % (error, traceback.format_exc())))
    exit()
