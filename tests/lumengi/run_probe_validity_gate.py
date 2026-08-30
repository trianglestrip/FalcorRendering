"""Offline A1 ScreenProbe validity/reset gate.

This gate is deliberately *not* a Mogwai asset and never imports ``falcor``.
It consumes JSON produced by a future/runtime probe-validity export and checks
the frozen A1 contract without starting a GPU process.  A missing or ambiguous
field is ``BLOCKED``; this script never treats a non-zero image, a history
length encoded in an unrelated channel, or a filename as validity evidence.

The required matrix is:

* 800x450 and 641x361 (the latter is the partial-tile resize case; both
  dimensions are checked against the ceil-sized 8x8 ScreenProbe grid),
* checkpoint frames 1, 8, 32 and 96, and
* an explicit ``scene_reload`` and ``camera_cut`` transition for every size.

Input is read from ``LUMEN_PROBE_VALIDITY_INPUT`` (a JSON file, directory, or
semicolon-separated list).  When it is unset, JSON files below
``artifacts/lumengi`` are inspected.  The only file written is
``probe-validity-gate.json`` below ``LUMEN_PROBE_VALIDITY_OUT``.  This default
is useful for dry runs, while a CI invocation should pass a dedicated input
manifest and output directory.

Expected record fields (aliases are accepted but recorded):

``resolution`` (or width/height), ``frame``/``frameIndex``, ``transition``,
``backendCode``/``sourceBackendCode``, ``sourceBackend``/``traceBackend``,
``geometryValid``, ``radianceValid``, ``producerFrame``, ``sampleIndex``,
``directionFingerprint``,
``generation``/``sceneGeneration``, ``age``/``historyAge``,
``resetReason``, and ``probeGrid``/``tileGrid``.  The sidecar must carry
``schemaVersion=LumenGI.ProbeValiditySidecar.v2`` and a four-way
``transitionCase`` object (camera/light/material/geometry).  A reset marker
(``resetObserved``, ``historyReset``, ``generationChanged`` or ``ageReset``)
or a matching reset reason is required to prove the transition itself;
merely having a different JSON file or a finite output is insufficient.
"""

from __future__ import annotations

import json
import math
import os
import re
import sys
from pathlib import Path


FRAME_CHECKPOINTS = (1, 8, 32, 96)
RESOLUTIONS = ((800, 450), (641, 361))
TRANSITIONS = ("scene_reload", "camera_cut")
TRANSITION_CASE_DIMENSIONS = ("camera", "light", "material", "geometry")
SIDECAR_SCHEMA_VERSION = "LumenGI.ProbeValiditySidecar.v2"
TILE_SIZE = max(1, int(os.environ.get("LUMEN_PROBE_VALIDITY_TILE_SIZE", "8")))
MAX_INPUT_FILES = max(1, int(os.environ.get("LUMEN_PROBE_VALIDITY_MAX_FILES", "512")))
MAX_INPUT_BYTES = max(1024, int(os.environ.get("LUMEN_PROBE_VALIDITY_MAX_BYTES", str(64 * 1024 * 1024))))

OUT_DIR = Path(
    os.environ.get("LUMEN_PROBE_VALIDITY_OUT", "artifacts/lumengi/A1/probe-validity-gate")
).absolute()
OUT_JSON = OUT_DIR / "probe-validity-gate.json"


def _key(value):
    return re.sub(r"[^a-z0-9]", "", str(value).lower())


def _finite_number(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        number = float(value)
        return number if math.isfinite(number) else None
    if isinstance(value, str):
        try:
            number = float(value.strip())
        except (TypeError, ValueError):
            return None
        return number if math.isfinite(number) else None
    return None


def _resolution(value):
    if isinstance(value, dict):
        width = next((value[k] for k in value if _key(k) in {"width", "w", "x"}), None)
        height = next((value[k] for k in value if _key(k) in {"height", "h", "y"}), None)
        if width is not None and height is not None:
            width_number = _finite_number(width)
            height_number = _finite_number(height)
            if width_number is not None and height_number is not None:
                return (int(width_number), int(height_number))
    if isinstance(value, (list, tuple)) and len(value) >= 2:
        width = _finite_number(value[0])
        height = _finite_number(value[1])
        if width is not None and height is not None:
            return (int(width), int(height))
    if isinstance(value, str):
        match = re.match(r"^\s*(\d+)\s*[xX,/]\s*(\d+)\s*$", value)
        if match:
            return (int(match.group(1)), int(match.group(2)))
    return None


def _grid(value):
    result = _resolution(value)
    return result if result and result[0] > 0 and result[1] > 0 else None


def _transition(value):
    if not isinstance(value, str):
        return None
    text = _key(value)
    if "scene" in text and ("reload" in text or "load" in text or "geometry" in text):
        return "scene_reload"
    if "camera" in text and ("cut" in text or "jump" in text or "teleport" in text):
        return "camera_cut"
    if text in {"reload", "scenereload", "geometryreload"}:
        return "scene_reload"
    if text in {"cut", "cameracut", "camerareset"}:
        return "camera_cut"
    return None


RESOLUTION_KEYS = {"resolution", "framesize", "renderresolution", "outputresolution", "size"}
FRAME_KEYS = {"frame", "frameindex", "checkpointframe", "renderedthrough", "renderedframe"}
TRANSITION_KEYS = {"transition", "event", "phase", "scenario", "case", "label", "name"}
GENERATION_KEYS = {
    "generation",
    "generationid",
    "scenegeneration",
    "lightinggeneration",
    "producergeneration",
    "historygeneration",
    "sceneepoch",
    "lightingepoch",
    "invalidationepoch",
}
AGE_KEYS = {
    "age",
    "historyage",
    "probeage",
    "screenprobeage",
    "historylength",
    "historycount",
    "samplecount",
}
SOURCE_KEYS = {
    "sourcebackend",
    "sourcebackendname",
    "tracebackend",
    "producerbackend",
    "backend",
    "backendname",
}
BACKEND_CODE_KEYS = {"backendcode", "sourcebackendcode", "tracebackendcode"}
GEOMETRY_VALID_KEYS = {"geometryvalid", "geometryisvalid", "validgeometry"}
RADIANCE_VALID_KEYS = {"radiancevalid", "radianceisvalid", "validradiance"}
PRODUCER_FRAME_KEYS = {"producerframe", "sourceframe", "radianceframe"}
SAMPLE_INDEX_KEYS = {"sampleindex", "directionslot", "sampleord"}
DIRECTION_FINGERPRINT_KEYS = {"directionfingerprint", "directionhash", "directionidentity", "octdirection"}
RESET_REASON_KEYS = {"resetreason", "invalidationreason", "historyresetreason"}
SCHEMA_VERSION_KEYS = {"schemaversion", "sidecarschemaversion", "validityschemaversion", "abiversion"}
GRID_KEYS = {"probegrid", "probegridsize", "tilegrid", "tilegridsize", "griddims", "gridsize"}
TRANSITION_CASE_KEYS = {"transitioncase", "transitionflags", "caseflags", "mutationcase"}
RESET_KEYS = {
    "resetobserved",
    "historyreset",
    "generationchanged",
    "agereset",
    "invalidationobserved",
    "cameracutapplied",
    "scenereloadapplied",
}


def _case_value(value):
    """Normalize a transition-case flag without guessing from a filename."""

    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value)):
        return float(value) != 0.0
    if isinstance(value, str):
        token = _key(value)
        if token in {"", "none", "unchanged", "static", "false", "off", "no", "0"}:
            return False
        if token in {"changed", "change", "true", "on", "yes", "1", "cut", "reload", "reset"}:
            return True
        return value.strip()
    return None


def _transition_case(value):
    if not isinstance(value, dict):
        return None
    result = {}
    for dimension in TRANSITION_CASE_DIMENSIONS:
        for raw_key, raw_value in value.items():
            if _key(raw_key) == dimension:
                parsed = _case_value(raw_value)
                if parsed is not None:
                    result[dimension] = parsed
                break
    return result or None


def _bool(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value) != 0.0
    if isinstance(value, str):
        return _key(value) in {"true", "yes", "on", "pass", "observed", "changed"}
    return None


def _input_paths():
    raw = os.environ.get("LUMEN_PROBE_VALIDITY_INPUT", "").strip()
    if raw:
        tokens = [token.strip() for token in re.split(r"[;\n]+", raw) if token.strip()]
        paths = [Path(token).absolute() for token in tokens]
    else:
        paths = [Path("artifacts/lumengi").absolute()]

    files = []
    for path in paths:
        if path.is_file() and path.suffix.lower() == ".json":
            files.append(path)
        elif path.is_dir():
            files.extend(item for item in path.rglob("*.json") if item.is_file())
    unique = sorted(set(files), key=lambda item: str(item).lower())
    # Do not read a report emitted in the same output directory when the
    # default artifact root happens to contain it.
    excluded = OUT_DIR.resolve()
    result = []
    for path in unique:
        try:
            if path.resolve().is_relative_to(excluded):
                continue
        except AttributeError:  # Python 3.8 compatibility.
            if str(path.resolve()).lower().startswith(str(excluded).lower() + os.sep):
                continue
        if len(result) >= MAX_INPUT_FILES:
            break
        result.append(path)
    return result


def _local_fields(mapping):
    """Extract fields from one object; no values are inferred from images."""

    fields = {}
    for raw_key, value in mapping.items():
        key = _key(raw_key)
        if key in RESOLUTION_KEYS:
            parsed = _resolution(value)
            if parsed:
                fields["resolution"] = parsed
        elif key in {"width", "w"}:
            width = _finite_number(value)
            if width is not None:
                fields["width"] = int(width)
        elif key in {"height", "h"}:
            height = _finite_number(value)
            if height is not None:
                fields["height"] = int(height)
        elif key in FRAME_KEYS:
            frame = _finite_number(value)
            if frame is not None:
                fields["frame"] = int(frame)
        elif key in TRANSITION_KEYS:
            parsed = _transition(value)
            if parsed:
                fields["transition"] = parsed
        elif key in SCHEMA_VERSION_KEYS or key == "schema":
            if isinstance(value, str) and value.strip():
                fields["schemaVersion"] = value.strip()
                fields["schemaVersionKey"] = str(raw_key)
        elif key in {"scenereload", "scenereloaded", "reloadscene"} and _bool(value):
            fields["transition"] = "scene_reload"
        elif key in {"cameracut", "cameracuttaken", "camerareset"} and _bool(value):
            fields["transition"] = "camera_cut"
        elif key in BACKEND_CODE_KEYS:
            fields["backendCode"] = value
            fields["backendCodeKey"] = str(raw_key)
        elif key in GEOMETRY_VALID_KEYS:
            fields["geometryValid"] = value
            fields["geometryValidKey"] = str(raw_key)
        elif key in RADIANCE_VALID_KEYS:
            fields["radianceValid"] = value
            fields["radianceValidKey"] = str(raw_key)
        elif key in PRODUCER_FRAME_KEYS:
            fields["producerFrame"] = value
            fields["producerFrameKey"] = str(raw_key)
        elif key in SAMPLE_INDEX_KEYS:
            fields["sampleIndex"] = value
            fields["sampleIndexKey"] = str(raw_key)
        elif key in DIRECTION_FINGERPRINT_KEYS:
            fields["directionFingerprint"] = value
            fields["directionFingerprintKey"] = str(raw_key)
        elif key in GENERATION_KEYS:
            fields["generation"] = value
            fields["generationKey"] = str(raw_key)
        elif key in AGE_KEYS:
            fields["age"] = value
            fields["ageKey"] = str(raw_key)
        elif key in RESET_REASON_KEYS:
            fields["resetReason"] = value
            fields["resetReasonKey"] = str(raw_key)
        elif key in SOURCE_KEYS:
            fields["sourceBackend"] = value
            fields["sourceBackendKey"] = str(raw_key)
        elif key in TRANSITION_CASE_KEYS:
            parsed = _transition_case(value)
            if parsed:
                fields["transitionCase"] = parsed
                fields["transitionCaseKey"] = str(raw_key)
        elif key in {"cameracase", "cameratransition", "camerachanged", "cameracut"}:
            parsed = _case_value(value)
            if parsed is not None:
                fields.setdefault("transitionCase", {})["camera"] = parsed
        elif key in {"lightcase", "lighttransition", "lightchanged", "lightchange"}:
            parsed = _case_value(value)
            if parsed is not None:
                fields.setdefault("transitionCase", {})["light"] = parsed
        elif key in {"materialcase", "materialtransition", "materialchanged", "materialchange"}:
            parsed = _case_value(value)
            if parsed is not None:
                fields.setdefault("transitionCase", {})["material"] = parsed
        elif key in {"geometrycase", "geometrytransition", "geometrychanged", "geometrychange"}:
            parsed = _case_value(value)
            if parsed is not None:
                fields.setdefault("transitionCase", {})["geometry"] = parsed
        elif key in GRID_KEYS:
            parsed = _grid(value)
            if parsed:
                fields["probeGrid"] = parsed
            fields["probeGridKey"] = str(raw_key)
        elif key in RESET_KEYS:
            parsed = _bool(value)
            if parsed is not None:
                fields["resetObserved"] = parsed
                fields["resetKey"] = str(raw_key)
    if "resolution" not in fields and "width" in fields and "height" in fields:
        fields["resolution"] = (fields["width"], fields["height"])
    return fields


def _walk(value, path, inherited, source, out):
    if isinstance(value, dict):
        fields = dict(inherited)
        fields.update(_local_fields(value))
        if "resolution" in fields and "frame" in fields:
            # Keep only objects that have at least one validity/layout field.
            evidence_keys = {
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
                "sourceBackend",
                "probeGrid",
                "transitionCase",
                "resetObserved",
            }
            if evidence_keys.intersection(fields):
                record = {key: value for key, value in fields.items() if key not in {"width", "height"}}
                record["path"] = path
                record["source"] = str(source)
                out.append(record)
        for key, child in value.items():
            _walk(child, path + (str(key),), fields, source, out)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _walk(child, path + (str(index),), inherited, source, out)


def _load_records(paths):
    records = []
    inventory = []
    for path in paths:
        item = {"path": str(path), "status": "BLOCKED"}
        try:
            size = path.stat().st_size
            item["bytes"] = size
            if size > MAX_INPUT_BYTES:
                item["reason"] = "input exceeds max bytes"
                inventory.append(item)
                continue
            with path.open("r", encoding="utf-8") as stream:
                document = json.load(stream)
            local = []
            _walk(document, (), {}, path, local)
            records.extend(local)
            item.update({"status": "PASS", "records": len(local)})
        except Exception as error:
            item["reason"] = "%s: %s" % (type(error).__name__, error)
        inventory.append(item)
    return records, inventory


def _record_identity(record):
    def freeze(value):
        if isinstance(value, dict):
            return tuple(sorted((str(key), freeze(item)) for key, item in value.items()))
        if isinstance(value, (list, tuple)):
            return tuple(freeze(item) for item in value)
        try:
            hash(value)
            return value
        except TypeError:
            return repr(value)

    return (
        freeze(record.get("source")),
        tuple(record.get("path", ())),
        freeze(record.get("resolution")),
        freeze(record.get("frame")),
        freeze(record.get("transition")),
        freeze(record.get("generation")),
        freeze(record.get("age")),
        freeze(record.get("backendCode")),
        freeze(record.get("geometryValid")),
        freeze(record.get("radianceValid")),
        freeze(record.get("producerFrame")),
        freeze(record.get("resetReason")),
        freeze(record.get("schemaVersion")),
        freeze(record.get("sourceBackend")),
        freeze(record.get("probeGrid")),
        freeze(record.get("transitionCase")),
        freeze(record.get("resetObserved")),
    )


def _deduplicate(records):
    seen = set()
    unique = []
    for record in records:
        identity = _record_identity(record)
        if identity in seen:
            continue
        seen.add(identity)
        unique.append(record)
    return unique


def _expected_grid(resolution):
    width, height = resolution
    return ((width + TILE_SIZE - 1) // TILE_SIZE, (height + TILE_SIZE - 1) // TILE_SIZE)


def _field_status(record, resolution, transition):
    missing = []
    invalid = []
    schema_version = record.get("schemaVersion")
    if schema_version is None:
        missing.append("schemaVersion")
    elif schema_version != SIDECAR_SCHEMA_VERSION:
        invalid.append("schemaVersion=%r expected=%r" % (schema_version, SIDECAR_SCHEMA_VERSION))

    backend_code = _finite_number(record.get("backendCode"))
    if backend_code is None:
        missing.append("backendCode") if "backendCode" not in record else invalid.append("backendCode")
    elif backend_code < 0.0 or backend_code != math.floor(backend_code):
        invalid.append("backendCode is not a non-negative integer")

    geometry_valid = _bool(record.get("geometryValid"))
    if geometry_valid is None:
        missing.append("geometryValid") if "geometryValid" not in record else invalid.append("geometryValid")
    radiance_valid = _bool(record.get("radianceValid"))
    if radiance_valid is None:
        missing.append("radianceValid") if "radianceValid" not in record else invalid.append("radianceValid")

    producer_frame = _finite_number(record.get("producerFrame"))
    if producer_frame is None:
        missing.append("producerFrame") if "producerFrame" not in record else invalid.append("producerFrame")
    elif producer_frame < 0.0 or producer_frame != math.floor(producer_frame):
        invalid.append("producerFrame is not a non-negative integer")

    sample_index = _finite_number(record.get("sampleIndex"))
    if sample_index is None:
        missing.append("sampleIndex") if "sampleIndex" not in record else invalid.append("sampleIndex")
    elif sample_index < 0.0 or sample_index != math.floor(sample_index) or sample_index >= 64.0:
        invalid.append("sampleIndex is outside the 6-bit range")

    direction_fingerprint = _finite_number(record.get("directionFingerprint"))
    if direction_fingerprint is None:
        missing.append("directionFingerprint") if "directionFingerprint" not in record else invalid.append("directionFingerprint")
    elif direction_fingerprint < 0.0 or direction_fingerprint != math.floor(direction_fingerprint) or direction_fingerprint >= 256.0:
        invalid.append("directionFingerprint is outside the 8-bit range")

    generation = _finite_number(record.get("generation"))
    if generation is None:
        missing.append("generation") if "generation" not in record else invalid.append("generation")
    age = _finite_number(record.get("age"))
    if age is None:
        missing.append("age") if "age" not in record else invalid.append("age")
    elif age < 0.0:
        invalid.append("age<0")
    source = record.get("sourceBackend")
    source_scalar = isinstance(source, (str, int, float)) and not isinstance(source, bool)
    if source is None or (isinstance(source, str) and not source.strip()):
        missing.append("sourceBackend") if source is None else invalid.append("sourceBackend")
    elif not source_scalar:
        invalid.append("sourceBackend is not scalar")

    reset_reason = record.get("resetReason")
    if reset_reason is None:
        missing.append("resetReason")
    elif not isinstance(reset_reason, (str, int, float)) or (
        isinstance(reset_reason, str) and not reset_reason.strip()
    ):
        invalid.append("resetReason is not a non-empty scalar")

    transition_case = record.get("transitionCase")
    if not isinstance(transition_case, dict):
        missing.append("transitionCase")
        transition_case = {}
    missing_case = [dimension for dimension in TRANSITION_CASE_DIMENSIONS if dimension not in transition_case]
    if missing_case:
        missing.append("transitionCase." + ",".join(missing_case))
    expected_dimension = "geometry" if transition == "scene_reload" else "camera"
    if expected_dimension in transition_case and not bool(transition_case[expected_dimension]):
        invalid.append("transitionCase.%s is false for %s" % (expected_dimension, transition))

    grid = record.get("probeGrid")
    expected = _expected_grid(resolution)
    if grid is None:
        missing.append("probeGrid")
    elif tuple(grid) != expected:
        invalid.append("probeGrid=%s expected=%s" % (tuple(grid), expected))
    return {
        "status": "PASS" if not missing and not invalid else "BLOCKED" if missing else "FAIL",
        "missing": missing,
        "invalid": invalid,
        "generation": generation,
        "age": age,
        "schemaVersion": schema_version,
        "backendCode": int(backend_code) if backend_code is not None else None,
        "geometryValid": geometry_valid,
        "radianceValid": radiance_valid,
        "producerFrame": int(producer_frame) if producer_frame is not None else None,
        "sampleIndex": int(sample_index) if sample_index is not None else None,
        "directionFingerprint": int(direction_fingerprint) if direction_fingerprint is not None else None,
        "resetReason": reset_reason,
        "sourceBackend": source,
        "transitionCase": transition_case,
        "probeGrid": list(grid) if grid else None,
        "expectedProbeGrid": list(expected),
    }


def _transition_evidence(records, transition):
    explicit = [record for record in records if record.get("resetObserved") is True]
    if explicit:
        return {"status": "PASS", "kind": "explicit", "records": len(explicit)}
    reason_tokens = {
        "scene_reload": {"scenereload", "geometryreload", "reload", "geometrychange"},
        "camera_cut": {"cameracut", "camerajump", "camerateleport", "camerareset"},
    }
    reason_records = []
    for record in records:
        reason = record.get("resetReason")
        if isinstance(reason, str) and _key(reason) in reason_tokens.get(transition, set()):
            reason_records.append(record)
    if reason_records:
        return {"status": "PASS", "kind": "resetReason", "records": len(reason_records)}
    # A future exporter may provide before/after generations or ages without a
    # dedicated bool.  Only accept an actual observed change, never a missing
    # value or a change between unrelated files.
    ordered = sorted(records, key=lambda record: int(record.get("frame", -1)))
    generations = [
        _finite_number(record.get("generation"))
        for record in ordered
        if _finite_number(record.get("generation")) is not None
    ]
    ages = [
        _finite_number(record.get("age"))
        for record in ordered
        if _finite_number(record.get("age")) is not None
    ]
    if transition == "scene_reload" and len(generations) >= 2 and len(set(generations)) > 1:
        return {"status": "PASS", "kind": "generation_delta", "values": generations}
    if transition == "camera_cut" and len(ages) >= 2 and any(
        ages[index] <= 1.0 and ages[index - 1] > 1.0 for index in range(1, len(ages))
    ):
        return {"status": "PASS", "kind": "age_reset", "values": ages}
    return {
        "status": "BLOCKED",
        "reason": "no explicit reset marker or observable %s transition" % transition,
    }


def _case(resolution, transition, frame, records, group):
    case_id = "%dx%d-%s-frame%d" % (resolution[0], resolution[1], transition, frame)
    matches = [
        record
        for record in records
        if tuple(record.get("resolution", ())) == resolution
        and record.get("transition") == transition
        and int(record.get("frame", -1)) == frame
    ]
    if not matches:
        return {
            "id": case_id,
            "status": "BLOCKED",
            "reason": "no input record for required resolution/frame/transition",
            "resolution": list(resolution),
            "partialTile": True,
            "frame": frame,
            "transition": transition,
        }
    # Prefer the record with the most required fields when a manifest contains
    # both a summary and a nested per-output view.
    record = max(
        matches,
        key=lambda item: sum(
            key in item
            for key in (
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
                "sourceBackend",
                "probeGrid",
                "transitionCase",
            )
        ),
    )
    fields = _field_status(record, resolution, transition)
    transition_status = group["transitionEvidence"]
    status = fields["status"]
    reasons = list(fields["missing"]) + list(fields["invalid"])
    if transition_status.get("status") != "PASS":
        status = "BLOCKED" if status != "FAIL" else status
        reasons.append(transition_status.get("reason", "transition evidence unavailable"))
    return {
        "id": case_id,
        "status": status,
        "resolution": list(resolution),
        "partialTile": True,
        "tileSize": TILE_SIZE,
        "expectedProbeGrid": fields["expectedProbeGrid"],
        "frame": frame,
        "transition": transition,
        "source": record.get("source"),
        "recordPath": list(record.get("path", ())),
        "fields": fields,
        "transitionEvidence": transition_status,
        "reason": "; ".join(reasons) if reasons else None,
    }


def _static_fixture_records():
    """Build an in-memory ABI fixture for the non-GPU smoke test."""

    records = []
    for resolution in RESOLUTIONS:
        for transition in TRANSITIONS:
            transition_case = {
                "camera": transition == "camera_cut",
                "light": False,
                "material": False,
                "geometry": transition == "scene_reload",
            }
            for frame in FRAME_CHECKPOINTS:
                records.append(
                    {
                        "schemaVersion": SIDECAR_SCHEMA_VERSION,
                        "resolution": resolution,
                        "frame": frame,
                        "transition": transition,
                        "backendCode": 1,
                        "sourceBackend": "HWRT",
                        "geometryValid": True,
                        "radianceValid": True,
                        "producerFrame": frame,
                        "sampleIndex": frame % 32,
                        "directionFingerprint": (frame * 7) % 256,
                        "generation": 9 if transition == "scene_reload" else 4,
                        "age": 1 if transition == "camera_cut" and frame == 1 else frame,
                        "resetObserved": True,
                        "resetReason": "SceneReload" if transition == "scene_reload" else "CameraCut",
                        "transitionCase": transition_case,
                        "probeGrid": _expected_grid(resolution),
                        "source": "<in-memory-fixture>",
                        "path": ("fixture", "%dx%d" % resolution, transition, str(frame)),
                    }
                )
    return records


def _run_static_fixture():
    records = _static_fixture_records()
    statuses = []
    for resolution in RESOLUTIONS:
        for transition in TRANSITIONS:
            group_records = [
                record
                for record in records
                if record["resolution"] == resolution and record["transition"] == transition
            ]
            group = {"transitionEvidence": _transition_evidence(group_records, transition)}
            statuses.extend(
                _case(resolution, transition, frame, records, group)["status"]
                for frame in FRAME_CHECKPOINTS
            )
    status = "PASS" if len(statuses) == 16 and all(value == "PASS" for value in statuses) else "FAIL"
    print("PROBE_VALIDITY_FIXTURE", status)
    return 0 if status == "PASS" else 1


def _write(payload):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    temporary = OUT_JSON.with_suffix(OUT_JSON.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    temporary.replace(OUT_JSON)


def main():
    paths = _input_paths()
    records, inventory = _load_records(paths)
    records = _deduplicate(records)
    cases = []
    groups = []
    for resolution in RESOLUTIONS:
        for transition in TRANSITIONS:
            group_records = [
                record
                for record in records
                if tuple(record.get("resolution", ())) == resolution and record.get("transition") == transition
            ]
            group = {
                "resolution": list(resolution),
                "transition": transition,
                "recordCount": len(group_records),
                "transitionEvidence": _transition_evidence(group_records, transition),
            }
            groups.append(group)
            for frame in FRAME_CHECKPOINTS:
                cases.append(_case(resolution, transition, frame, records, group))

    statuses = [case["status"] for case in cases]
    if any(status == "FAIL" for status in statuses):
        status = "FAIL"
    elif any(status == "BLOCKED" for status in statuses) or not records:
        status = "BLOCKED"
    else:
        status = "PASS"

    payload = {
        "schema": "LumenGI.ProbeValidityGate.v2",
        "script": "run_probe_validity_gate.py",
        "status": status,
        "gpu": False,
        "contract": {
            "sidecarSchemaVersion": SIDECAR_SCHEMA_VERSION,
            "noFabricatedPass": True,
            "requiredFields": [
                "schemaVersion",
                "backendCode",
                "sourceBackend",
                "geometryValid",
                "radianceValid",
                "producerFrame",
                "generation",
                "age",
                "resetReason",
                "transitionCase.camera",
                "transitionCase.light",
                "transitionCase.material",
                "transitionCase.geometry",
                "probeGrid",
            ],
            "missingTelemetry": "BLOCKED",
            "missingProbeGrid": "BLOCKED",
            "transitionEvidenceRequired": True,
            "tileSize": TILE_SIZE,
            "partialTileResolutions": [list(resolution) for resolution in RESOLUTIONS],
        },
        "input": {
            "environment": os.environ.get("LUMEN_PROBE_VALIDITY_INPUT", ""),
            "filesConsidered": len(paths),
            "inventory": inventory,
            "recordsDiscovered": len(records),
        },
        "matrix": {
            "resolutions": [list(resolution) for resolution in RESOLUTIONS],
            "frames": list(FRAME_CHECKPOINTS),
            "transitions": list(TRANSITIONS),
            "caseCount": len(cases),
        },
        "groups": groups,
        "cases": cases,
        "summary": {
            "pass": statuses.count("PASS"),
            "blocked": statuses.count("BLOCKED"),
            "fail": statuses.count("FAIL"),
        },
    }
    _write(payload)
    print("PROBE_VALIDITY_STATUS", status)
    print("PROBE_VALIDITY_RECORDS", len(records))
    print("PROBE_VALIDITY_WROTE", str(OUT_JSON))
    return 0 if status == "PASS" else 2 if status == "BLOCKED" else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(_run_static_fixture())
    sys.exit(main())
