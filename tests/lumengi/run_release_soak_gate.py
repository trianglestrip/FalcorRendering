"""Strict offline S2 release VRAM/soak contract gate.

This verifier consumes JSON written by :mod:`run_churn_short.py` and the
optional serial-launch manifest written by :mod:`run_release_soak_launcher.py`;
it never starts Mogwai, queries a GPU, or treats the 60-second proxy as a soak.  The
release contract has two independent evidence phases:

* ``dynamic``: at least 30 minutes / 60 Hz of material, reload, and resize
  churn;
* ``soak``: at least two hours / 60 Hz of the same dynamic workload.

Both phases must carry authoritative GPU-wide VRAM samples, authoritative
renderer/device provenance, complete Surface Cache counters, and a VRAM/cache
residency series that is not monotonically growing.  Missing evidence is
``BLOCKED``.  A complete phase with a violated invariant is ``FAIL``.  The
proxy artifact therefore cannot become ``PASS`` merely because its local
bounded ``proxy_gate`` says PASS.

Typical usage (from the repository root)::

    python -B tests/lumengi/run_release_soak_gate.py \
      --dynamic artifacts/lumengi/S2/churn-30min.json \
      --soak artifacts/lumengi/S2/churn-2h.json \
      --output artifacts/lumengi/S2/release-soak-gate.json

An optional manifest may instead contain ``dynamic`` and ``soak`` paths, or be
the S2 launcher manifest.  In the latter case the gate additionally verifies
the serial process records, logs, timeout outcomes and single-GPU identity.
This file is intentionally dependency-free so the contract can be reviewed
and self-tested on a machine without Falcor or a GPU.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile
from collections.abc import Mapping, Sequence


SCHEMA_VERSION = "S2-release-soak-gate-v1"
FRAME_RATE = 60.0
MIN_DYNAMIC_SECONDS = 30.0 * 60.0
MIN_SOAK_SECONDS = 2.0 * 60.0 * 60.0
MIN_VRAM_SAMPLES = 3
MIN_COUNTER_SAMPLES = 2
LAUNCHER_SCHEMA_PREFIX = "S2-release-soak-launcher-"

_VRAM_REQUIRED = (
    "gpu_index",
    "gpu_name",
    "driver_version",
    "total_bytes",
    "used_bytes",
    "free_bytes",
)


def _finite(value):
    """Return a finite float, or ``None`` for absent/invalid values."""
    if value is None or isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _is_true(value):
    return value is True or value == 1


def _number(value, *, integer=False):
    number = _finite(value)
    if number is None or number < 0:
        return None
    if integer and not number.is_integer():
        return None
    return int(number) if integer else number


def _mapping(value):
    return value if isinstance(value, Mapping) else None


def _first_mapping(root, *paths):
    """Read the first mapping at one of the dotted paths."""
    for path in paths:
        current = root
        for part in path.split("."):
            current = current.get(part) if isinstance(current, Mapping) else None
        if isinstance(current, Mapping):
            return current
    return None


def _status(checks, name, status, reason=None):
    item = {"status": status}
    if reason:
        item["reason"] = reason
    checks[name] = item


def _required_bool(mapping, key):
    return _is_true(mapping.get(key))


def _monotonic_non_decreasing(values):
    """Return true when values never decrease and at least one rises."""
    return len(values) >= 2 and any(b > a for a, b in zip(values, values[1:])) and all(
        b >= a for a, b in zip(values, values[1:])
    )


def _trend(values):
    if len(values) < 2:
        return "insufficient"
    if _monotonic_non_decreasing(values):
        return "monotonic-nondecreasing"
    if all(b == a for a, b in zip(values, values[1:])):
        return "stable"
    return "bounded-non-monotonic"


def _extract_vram(artifact):
    vram = _first_mapping(artifact, "vram", "telemetry_provenance.vram")
    samples = None
    if vram is not None:
        samples = vram.get("samples")
    if not isinstance(samples, Sequence) or isinstance(samples, (str, bytes)):
        samples = artifact.get("vram_samples")
    if not isinstance(samples, Sequence) or isinstance(samples, (str, bytes)):
        samples = []
    return vram, list(samples)


def _check_renderer(artifact, checks, blockers):
    renderer = _first_mapping(artifact, "renderer", "telemetry_provenance.renderer")
    required = ("adapter_name", "api_name", "device_type")
    if renderer is None:
        _status(checks, "renderer_provenance", "BLOCKED", "renderer provenance is absent")
        blockers.append("renderer provenance is absent")
        return None
    missing = [key for key in required if not str(renderer.get(key) or "").strip()]
    if not _required_bool(renderer, "available"):
        missing.append("renderer.available=true")
    if not _required_bool(renderer, "authoritative"):
        missing.append("renderer.authoritative=true")
    source = str(renderer.get("source") or "")
    if not source or source.lower().startswith("environment") or "hint" in source.lower():
        missing.append("authoritative renderer source")
    if missing:
        reason = "missing/unauthoritative renderer fields: " + ", ".join(missing)
        _status(checks, "renderer_provenance", "BLOCKED", reason)
        blockers.append(reason)
    else:
        _status(checks, "renderer_provenance", "PASS")
    return renderer


def _check_vram(artifact, checks, blockers, failures):
    vram, samples = _extract_vram(artifact)
    if vram is None:
        reason = "GPU-wide VRAM record is absent"
        _status(checks, "gpu_vram_telemetry", "BLOCKED", reason)
        blockers.append(reason)
        return

    start = _mapping(vram.get("start"))
    end = _mapping(vram.get("end"))
    if start is None or end is None:
        reason = "authoritative VRAM start/end snapshots are required"
        _status(checks, "gpu_vram_telemetry", "BLOCKED", reason)
        blockers.append(reason)
        return

    records = [start, end]
    missing = []
    for label, record in (("start", start), ("end", end)):
        if not _required_bool(record, "available") or not _required_bool(record, "authoritative"):
            missing.append("%s.available/authoritative" % label)
        source = str(record.get("source") or "")
        if not source or source.lower().startswith("environment"):
            missing.append("%s.authoritative source" % label)
        for key in _VRAM_REQUIRED:
            if record.get(key) in (None, ""):
                missing.append("%s.%s" % (label, key))
        total = _number(record.get("total_bytes"))
        used = _number(record.get("used_bytes"))
        free = _number(record.get("free_bytes"))
        if total is None or total <= 0 or used is None or free is None or used > total or free > total:
            missing.append("%s.valid memory values" % label)
    if start.get("gpu_index") != end.get("gpu_index"):
        missing.append("start/end GPU index mismatch")
    if start.get("gpu_name") != end.get("gpu_name"):
        missing.append("start/end GPU name mismatch")
    if start.get("driver_version") != end.get("driver_version"):
        missing.append("start/end driver mismatch")
    if missing:
        reason = "invalid authoritative GPU-wide VRAM evidence: " + ", ".join(missing)
        _status(checks, "gpu_vram_telemetry", "BLOCKED", reason)
        blockers.append(reason)
        return

    if len(samples) < MIN_VRAM_SAMPLES:
        reason = "VRAM sample series has %d records; at least %d authoritative records are required" % (
            len(samples),
            MIN_VRAM_SAMPLES,
        )
        _status(checks, "gpu_vram_telemetry", "BLOCKED", reason)
        blockers.append(reason)
        return

    sample_missing = []
    used_values = []
    frames = []
    for index, sample_value in enumerate(samples):
        sample = _mapping(sample_value)
        if sample is None:
            sample_missing.append("sample[%d] is not an object" % index)
            continue
        if not _required_bool(sample, "available") or not _required_bool(sample, "authoritative"):
            sample_missing.append("sample[%d] is not authoritative" % index)
        if str(sample.get("source") or "").lower().startswith("environment"):
            sample_missing.append("sample[%d] uses environment telemetry" % index)
        for key in _VRAM_REQUIRED:
            if sample.get(key) in (None, ""):
                sample_missing.append("sample[%d].%s" % (index, key))
        for key in ("gpu_index", "gpu_name", "driver_version", "total_bytes"):
            if sample.get(key) != start.get(key):
                sample_missing.append("sample[%d].%s identity mismatch" % (index, key))
        if sample.get("source") != start.get("source"):
            sample_missing.append("sample[%d].source identity mismatch" % index)
        used = _number(sample.get("used_bytes"))
        total = _number(sample.get("total_bytes"))
        free = _number(sample.get("free_bytes"))
        if used is None or total is None or free is None or total <= 0 or used > total or free > total:
            sample_missing.append("sample[%d] invalid memory values" % index)
        else:
            used_values.append(used)
        frame = _number(sample.get("frame"), integer=True)
        if frame is None:
            sample_missing.append("sample[%d].frame is missing/invalid" % index)
        else:
            frames.append(frame)
    if len(frames) == len(samples) and any(b <= a for a, b in zip(frames, frames[1:])):
        sample_missing.append("VRAM sample frames are not strictly increasing")
    if sample_missing:
        reason = "invalid VRAM sample series: " + "; ".join(sample_missing)
        _status(checks, "gpu_vram_telemetry", "BLOCKED", reason)
        blockers.append(reason)
        return

    vram_trend = _trend(used_values)
    if vram_trend == "monotonic-nondecreasing":
        reason = "GPU used VRAM is monotonically non-decreasing across the soak samples"
        _status(checks, "gpu_vram_no_monotonic_growth", "FAIL", reason)
        failures.append(reason)
    else:
        _status(checks, "gpu_vram_no_monotonic_growth", "PASS")
    _status(
        checks,
        "gpu_vram_telemetry",
        "PASS",
        "authoritative %d-sample %s series" % (len(samples), vram_trend),
    )


def _check_residency(artifact, checks, blockers, failures):
    series = artifact.get("series")
    if not isinstance(series, Sequence) or isinstance(series, (str, bytes)) or len(series) < MIN_COUNTER_SAMPLES:
        reason = "Surface Cache counter series is absent or too short"
        _status(checks, "surface_cache_series", "BLOCKED", reason)
        blockers.append(reason)
        return
    fields = ("residentBytes", "allocatedPages")
    values_by_field = {}
    missing = []
    for field in fields:
        values = []
        for index, sample_value in enumerate(series):
            sample = _mapping(sample_value)
            value = _number(sample.get(field)) if sample is not None else None
            if value is None:
                # Older artifacts expose the legacy spelling for both fields.
                legacy = {"residentBytes": "resident_bytes", "allocatedPages": "allocated_pages"}[field]
                value = _number(sample.get(legacy)) if sample is not None else None
            if value is None:
                missing.append("series[%d].%s" % (index, field))
            else:
                values.append(value)
        values_by_field[field] = values
    if missing:
        reason = "incomplete Surface Cache residency series: " + ", ".join(missing[:12])
        _status(checks, "surface_cache_series", "BLOCKED", reason)
        blockers.append(reason)
        return

    trend_names = []
    for field, values in values_by_field.items():
        trend = _trend(values)
        trend_names.append("%s=%s" % (field, trend))
        if trend == "monotonic-nondecreasing":
            reason = "%s is monotonically non-decreasing across the run" % field
            failures.append(reason)
    if failures and any("Surface Cache" not in item for item in failures):
        # The caller may already have a VRAM failure.  Add only the cache
        # failures here to keep the phase report deterministic and readable.
        pass
    cache_failures = [item for item in failures if item.endswith("across the run")]
    if cache_failures:
        _status(checks, "surface_cache_no_monotonic_growth", "FAIL", "; ".join(cache_failures))
    else:
        _status(checks, "surface_cache_no_monotonic_growth", "PASS", ", ".join(trend_names))
    # Non-zero allocator failure/lost counters are hard failures when the
    # required complete series exists; do not hide them as a proxy result.
    churn_failures = []
    for field in ("fail", "lost", "schedAllocFailures", "schedLostPages"):
        for sample_value in series:
            sample = _mapping(sample_value)
            value = _number(sample.get(field)) if sample is not None else None
            if value is not None and value > 0:
                churn_failures.append("%s=%s" % (field, value))
                break
    if churn_failures:
        reason = "non-zero Surface Cache failure/lost counters: " + ", ".join(churn_failures)
        _status(checks, "surface_cache_error_counters", "FAIL", reason)
        failures.append(reason)
    else:
        _status(checks, "surface_cache_error_counters", "PASS")


def _check_duration_and_dynamic(artifact, role, minimum_seconds, checks, blockers):
    seconds = _number(artifact.get("seconds"))
    frame_count = _number(artifact.get("frame_count"), integer=True)
    frame_rate = _number(artifact.get("frame_rate")) or FRAME_RATE
    if seconds is None or seconds < minimum_seconds:
        reason = "%s evidence requires at least %.0f seconds (got %s)" % (
            role,
            minimum_seconds,
            "missing" if seconds is None else seconds,
        )
        _status(checks, "duration", "BLOCKED", reason)
        blockers.append(reason)
    elif frame_count is None or frame_count < math.ceil(seconds * frame_rate):
        reason = "%s frame_count is shorter than its declared %.3f-second run" % (role, seconds)
        _status(checks, "duration", "BLOCKED", reason)
        blockers.append(reason)
    else:
        _status(checks, "duration", "PASS", "%.3f seconds / %d frames" % (seconds, frame_count))

    if str(artifact.get("mode") or "").strip().lower() == "60s-proxy":
        reason = "%s artifact is explicitly marked 60s-proxy" % role
        _status(checks, "proxy_separation", "BLOCKED", reason)
        blockers.append(reason)
    else:
        _status(checks, "proxy_separation", "PASS", "proxy artifacts are not accepted")

    dynamic_missing = []
    if artifact.get("material_toggle_available") is not True:
        dynamic_missing.append("material_toggle_available=true")
    if (_number(artifact.get("dirty_injections")) or 0) <= 0:
        dynamic_missing.append("dirty_injections>0")
    if (_number(artifact.get("reloads")) or 0) <= 0:
        dynamic_missing.append("reloads>0")
    if (_number(artifact.get("resizes")) or 0) <= 0:
        dynamic_missing.append("resizes>0")
    if not _is_true(artifact.get("stats_complete")):
        dynamic_missing.append("stats_complete=true")
    if dynamic_missing:
        reason = "%s dynamic workload evidence is incomplete: %s" % (role, ", ".join(dynamic_missing))
        _status(checks, "dynamic_workload", "BLOCKED", reason)
        blockers.append(reason)
    else:
        _status(checks, "dynamic_workload", "PASS")


def evaluate_artifact(artifact, role):
    """Evaluate one phase and return a serializable evidence report."""
    checks = {}
    blockers = []
    failures = []
    if not isinstance(artifact, Mapping):
        return {
            "role": role,
            "status": "BLOCKED",
            "checks": {"artifact": {"status": "BLOCKED", "reason": "JSON root is not an object"}},
            "blocking_reasons": ["JSON root is not an object"],
            "failure_reasons": [],
        }
    minimum = MIN_DYNAMIC_SECONDS if role == "dynamic" else MIN_SOAK_SECONDS
    _check_duration_and_dynamic(artifact, role, minimum, checks, blockers)
    _check_renderer(artifact, checks, blockers)
    _check_vram(artifact, checks, blockers, failures)
    _check_residency(artifact, checks, blockers, failures)
    if failures:
        status = "FAIL"
    elif blockers:
        status = "BLOCKED"
    else:
        status = "PASS"
    return {
        "role": role,
        "status": status,
        "seconds": artifact.get("seconds"),
        "frame_count": artifact.get("frame_count"),
        "checks": checks,
        "blocking_reasons": blockers,
        "failure_reasons": failures,
    }


def _load_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, Mapping):
        raise ValueError("JSON root must be an object")
    return value


def _is_launcher_manifest(manifest):
    return str(manifest.get("schema_version") or "").startswith(LAUNCHER_SCHEMA_PREFIX)


def _manifest_info(manifest_path):
    """Resolve generic paths and preserve launcher metadata when present."""
    manifest = _load_json(manifest_path)
    container = manifest.get("artifacts") if isinstance(manifest.get("artifacts"), Mapping) else manifest
    aliases = {
        "dynamic": ("dynamic", "dynamic_30m", "dynamic30m", "dynamicArtifact"),
        "soak": ("soak", "soak_2h", "soak2h", "soakArtifact"),
    }
    result = {}
    for role, names in aliases.items():
        for name in names:
            value = container.get(name)
            if isinstance(value, str) and value.strip():
                candidate = value.strip()
                if not os.path.isabs(candidate):
                    candidate = os.path.join(os.path.dirname(os.path.abspath(manifest_path)), candidate)
                result[role] = os.path.abspath(candidate)
                break
    # v1 launcher manifests predate the additive ``artifacts`` map.  Preserve
    # their paths too so they evaluate BLOCKED on missing/invalid evidence
    # rather than being silently treated as a generic empty manifest.
    if _is_launcher_manifest(manifest):
        phase_records = manifest.get("phases")
        if isinstance(phase_records, Mapping):
            for role in ("dynamic", "soak"):
                if role in result:
                    continue
                phase = _mapping(phase_records.get(role))
                value = phase.get("output_json") if phase else None
                if isinstance(value, str) and value.strip():
                    candidate = value.strip()
                    if not os.path.isabs(candidate):
                        candidate = os.path.join(os.path.dirname(os.path.abspath(manifest_path)), candidate)
                    result[role] = os.path.abspath(candidate)
    return result, manifest


def _manifest_paths(manifest_path):
    return _manifest_info(manifest_path)[0]


def _same_gpu_identity(actual, expected):
    if not isinstance(actual, Mapping) or not isinstance(expected, Mapping):
        return False
    return all(
        str(actual.get(key) or "").strip() == str(expected.get(key) or "").strip()
        for key in ("gpu_index", "gpu_name", "driver_version")
    )


def _launcher_manifest_report(manifest):
    """Return launch-level blockers without ever certifying a release pass."""
    checks = {}
    blockers = []
    if not _is_launcher_manifest(manifest):
        return checks, blockers, None, {}
    requested = _mapping(manifest.get("requested"))
    selected_gpu = _mapping(manifest.get("selected_gpu"))
    contract = _mapping(manifest.get("contract"))
    phases = _mapping(manifest.get("phases")) or {}
    if str(manifest.get("status") or "") != "READY_FOR_OFFLINE_GATE":
        blockers.append("launcher manifest status is not READY_FOR_OFFLINE_GATE")
    if requested is None:
        blockers.append("launcher requested-duration record is missing")
    else:
        if (_number(requested.get("dynamic_seconds")) or 0) < MIN_DYNAMIC_SECONDS:
            blockers.append("launcher dynamic request is shorter than 30 minutes")
        if (_number(requested.get("soak_seconds")) or 0) < MIN_SOAK_SECONDS:
            blockers.append("launcher soak request is shorter than 2 hours")
        if not str(requested.get("gpu_index") or "").strip():
            blockers.append("launcher requested GPU index is missing")
    if selected_gpu is None or any(not str(selected_gpu.get(key) or "").strip() for key in ("gpu_index", "gpu_name", "driver_version")):
        blockers.append("launcher selected-GPU identity is incomplete")
    elif requested is not None and str(selected_gpu.get("gpu_index")) != str(requested.get("gpu_index")):
        blockers.append("launcher selected GPU does not match requested GPU index")
    if contract is None or contract.get("single_gpu") is not True:
        blockers.append("launcher does not declare single_gpu=true")
    elif (
        contract.get("require_authoritative_renderer_provenance") is not True
        or contract.get("require_authoritative_gpu_wide_vram") is not True
        or contract.get("proxy_is_not_soak") is not True
    ):
        blockers.append("launcher provenance/proxy contract is incomplete")
    for role in ("dynamic", "soak"):
        if not isinstance(_mapping(phases.get(role)), Mapping):
            blockers.append("launcher %s phase record is missing" % role)
    _status(
        checks,
        "launcher_manifest",
        "PASS" if not blockers else "BLOCKED",
        None if not blockers else "; ".join(blockers),
    )
    return checks, blockers, selected_gpu, phases


def _check_launcher_phase(artifact, artifact_path, role, record, selected_gpu, checks, blockers):
    """Verify the serial launcher record against one child evidence artifact."""
    prefix = "launcher_%s" % role
    reasons = []
    if not isinstance(record, Mapping):
        reasons.append("phase record is missing")
    else:
        if record.get("role") != role:
            reasons.append("phase role mismatch")
        expected = _number(record.get("expected_seconds"))
        minimum = MIN_DYNAMIC_SECONDS if role == "dynamic" else MIN_SOAK_SECONDS
        if expected is None or expected < minimum:
            reasons.append("expected duration is below release minimum")
        output = record.get("output_json")
        if not isinstance(output, str) or os.path.abspath(output) != os.path.abspath(artifact_path):
            reasons.append("phase output_json does not match consumed artifact")
        process = _mapping(record.get("process"))
        timeout = _number(record.get("timeout_seconds"))
        if process is None:
            reasons.append("process record is missing")
        else:
            intentional_termination = process.get("intentional_termination") is True
            if intentional_termination and process.get("termination_reason") != "child_artifact_complete":
                reasons.append("intentional process termination reason is invalid")
            if process.get("exit_code") != 0 and not intentional_termination:
                reasons.append("Mogwai exit code is not zero")
            if process.get("timed_out") is not False:
                reasons.append("Mogwai phase timed out or timeout state is missing")
            started = _finite(process.get("started_at_unix"))
            finished = _finite(process.get("finished_at_unix"))
            elapsed = _finite(process.get("elapsed_seconds"))
            if started is None or finished is None or finished < started or elapsed is None or elapsed <= 0:
                reasons.append("process timing record is invalid")
            if timeout is None or timeout < expected:
                reasons.append("process timeout is missing or shorter than the requested phase")
            elif elapsed is not None and elapsed > timeout:
                reasons.append("process elapsed time exceeds the recorded timeout")
        if record.get("status") != "READY_FOR_OFFLINE_GATE":
            reasons.append("launcher phase was not READY_FOR_OFFLINE_GATE")
        if record.get("child_contract_status") != "READY":
            reasons.append("launcher child contract was not READY")
        log = _mapping(record.get("log"))
        if log is None or log.get("exists") is not True or (_number(log.get("bytes")) or 0) <= 0:
            reasons.append("Mogwai logfile is missing or empty")
        log_path = record.get("logfile")
        if not isinstance(log_path, str) or not log_path.strip() or not os.path.isfile(log_path):
            reasons.append("Mogwai logfile path is absent on disk")
        elif os.path.getsize(log_path) <= 0:
            reasons.append("Mogwai logfile path is empty on disk")
        command = record.get("command")
        if not isinstance(command, Sequence) or isinstance(command, (str, bytes)):
            reasons.append("launcher command record is missing")
        else:
            command = [str(item) for item in command]
            required = ("--device-type", "d3d12", "--headless", "--precise", "--script", "--logfile")
            if any(item not in command for item in required):
                reasons.append("launcher command is not the required D3D12 headless precise script invocation")
            elif command.index("--logfile") + 1 >= len(command) or command[command.index("--logfile") + 1] != str(record.get("logfile")):
                reasons.append("launcher command logfile differs from phase logfile")
        phase_gpu = _mapping(record.get("selected_gpu"))
        if not _same_gpu_identity(phase_gpu, selected_gpu):
            reasons.append("phase selected GPU identity differs from launcher identity")
        launcher_samples = record.get("launcher_vram_samples")
        if not isinstance(launcher_samples, Sequence) or isinstance(launcher_samples, (str, bytes)) or len(launcher_samples) < 2:
            reasons.append("launcher GPU-wide VRAM boundary samples are missing")
        else:
            for index, sample in enumerate(launcher_samples):
                sample = _mapping(sample)
                if (
                    sample is None
                    or not _required_bool(sample, "available")
                    or not _required_bool(sample, "authoritative")
                    or str(sample.get("source") or "").lower().startswith("environment")
                    or not _same_gpu_identity(sample, selected_gpu)
                    or _finite(sample.get("timestamp_unix")) is None
                ):
                    reasons.append("launcher VRAM sample[%d] is incomplete, non-authoritative, or on another GPU" % index)
                    break
        renderer = _first_mapping(artifact, "renderer", "telemetry_provenance.renderer") if isinstance(artifact, Mapping) else None
        if renderer is None or str(renderer.get("adapter_name") or "").strip().casefold() != str(selected_gpu.get("gpu_name") or "").strip().casefold():
            reasons.append("child renderer adapter does not match selected GPU")
        vram, _ = _extract_vram(artifact) if isinstance(artifact, Mapping) else (None, [])
        start = _mapping(vram.get("start")) if vram else None
        if not _same_gpu_identity(start, selected_gpu):
            reasons.append("child GPU-wide VRAM identity does not match selected GPU")
    _status(checks, prefix, "PASS" if not reasons else "BLOCKED", None if not reasons else "; ".join(reasons))
    blockers.extend("%s: %s" % (prefix, reason) for reason in reasons)


def _write_json(path, value):
    path = os.path.abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, indent=2, sort_keys=True, allow_nan=False)
        handle.write("\n")
    os.replace(temporary, path)


def evaluate_paths(dynamic_path=None, soak_path=None, manifest_path=None):
    paths = {}
    manifest = None
    if manifest_path:
        paths_from_manifest, manifest = _manifest_info(manifest_path)
        paths.update(paths_from_manifest)
    if dynamic_path:
        paths["dynamic"] = os.path.abspath(dynamic_path)
    if soak_path:
        paths["soak"] = os.path.abspath(soak_path)
    launcher_checks, launcher_blockers, selected_gpu, launcher_phases = (
        _launcher_manifest_report(manifest) if isinstance(manifest, Mapping) else ({}, [], None, {})
    )
    phases = {}
    for role in ("dynamic", "soak"):
        path = paths.get(role)
        if not path:
            phases[role] = {
                "role": role,
                "status": "BLOCKED",
                "checks": {"artifact": {"status": "BLOCKED", "reason": "phase artifact path is missing"}},
                "blocking_reasons": ["%s phase artifact path is missing" % role],
                "failure_reasons": [],
            }
            continue
        try:
            artifact = _load_json(path)
        except Exception as exc:
            phases[role] = {
                "role": role,
                "status": "BLOCKED",
                "path": path,
                "checks": {"artifact": {"status": "BLOCKED", "reason": str(exc)}},
                "blocking_reasons": ["cannot load %s artifact: %s" % (role, exc)],
                "failure_reasons": [],
            }
            continue
        report = evaluate_artifact(artifact, role)
        report["path"] = path
        if selected_gpu is not None:
            _check_launcher_phase(
                artifact,
                path,
                role,
                _mapping(launcher_phases.get(role)),
                selected_gpu,
                report["checks"],
                report["blocking_reasons"],
            )
            if report["status"] != "FAIL" and report["blocking_reasons"]:
                report["status"] = "BLOCKED"
        phases[role] = report
    statuses = [phases[role]["status"] for role in ("dynamic", "soak")]
    if all(status == "PASS" for status in statuses):
        status = "PASS"
    elif any(status == "FAIL" for status in statuses):
        status = "FAIL"
    else:
        status = "BLOCKED"
    if launcher_blockers and status != "FAIL":
        status = "BLOCKED"
    return {
        "schema_version": SCHEMA_VERSION,
        "status": status,
        "contract": {
            "dynamic_min_seconds": MIN_DYNAMIC_SECONDS,
            "soak_min_seconds": MIN_SOAK_SECONDS,
            "frame_rate_hz": FRAME_RATE,
            "min_vram_samples": MIN_VRAM_SAMPLES,
            "require_authoritative_gpu_wide_vram": True,
            "require_authoritative_renderer_provenance": True,
            "reject_proxy_as_soak": True,
            "reject_monotonic_vram_or_cache_residency_growth": True,
        },
        "launcher": {
            "recognized": isinstance(manifest, Mapping) and _is_launcher_manifest(manifest),
            "checks": launcher_checks,
            "blocking_reasons": launcher_blockers,
        },
        "phases": phases,
        "overall_reason": (
            "dynamic AND soak phases passed"
            if status == "PASS"
            else "one or more phase contracts failed"
            if status == "FAIL"
            else "required release evidence is missing or unauthoritative"
        ),
    }


def _fixture(seconds, *, monotonic=False, proxy=False, renderer=True):
    values = [1000, 1020, 990] if not monotonic else [1000, 1020, 1040]
    vram_samples = [
        {
            "frame": index * int(seconds * FRAME_RATE / 2),
            "available": True,
            "authoritative": True,
            "status": "PASS",
            "source": "nvidia-smi",
            "gpu_index": "0",
            "gpu_name": "fixture-gpu",
            "driver_version": "fixture-driver",
            "total_bytes": 8000,
            "used_bytes": value,
            "free_bytes": 8000 - value,
        }
        for index, value in enumerate(values)
    ]
    return {
        "mode": "60s-proxy" if proxy else "soak",
        "seconds": seconds,
        "frame_rate": FRAME_RATE,
        "frame_count": int(seconds * FRAME_RATE),
        "material_toggle_available": True,
        "dirty_injections": int(seconds * FRAME_RATE),
        "reloads": 4,
        "resizes": 8,
        "stats_complete": True,
        "renderer": {
            "available": renderer,
            "authoritative": renderer,
            "status": "PASS" if renderer else "BLOCKED",
            "source": "falcor.Device.info" if renderer else "environment:GPU",
            "adapter_name": "fixture-gpu" if renderer else None,
            "api_name": "D3D12" if renderer else None,
            "device_type": "d3d12" if renderer else None,
        },
        "vram": {
            "start": vram_samples[0],
            "end": vram_samples[-1],
            "samples": vram_samples,
        },
        "series": [
            {"frame": 0, "residentBytes": 100, "allocatedPages": 1, "fail": 0, "lost": 0},
            {"frame": int(seconds * FRAME_RATE / 2), "residentBytes": 120, "allocatedPages": 2, "fail": 0, "lost": 0},
            {"frame": int(seconds * FRAME_RATE), "residentBytes": 110, "allocatedPages": 1, "fail": 0, "lost": 0},
        ],
    }


def _self_test():
    dynamic = _fixture(MIN_DYNAMIC_SECONDS)
    soak = _fixture(MIN_SOAK_SECONDS)
    result = evaluate_artifact(dynamic, "dynamic")
    assert result["status"] == "PASS", result
    result = evaluate_artifact(soak, "soak")
    assert result["status"] == "PASS", result
    assert evaluate_artifact(_fixture(60.0, proxy=True), "dynamic")["status"] == "BLOCKED"
    assert evaluate_artifact(_fixture(MIN_DYNAMIC_SECONDS, renderer=False), "dynamic")["status"] == "BLOCKED"
    assert evaluate_artifact(_fixture(MIN_DYNAMIC_SECONDS, monotonic=True), "dynamic")["status"] == "FAIL"
    aggregate = evaluate_paths()
    assert aggregate["status"] == "BLOCKED"
    # Exercise the launcher-manifest path without running Mogwai.  This proves
    # that a prospective release launch is evaluated as one serial evidence
    # chain rather than merely extracting child JSON paths from the manifest.
    with tempfile.TemporaryDirectory(prefix="s2-release-soak-selftest-") as directory:
        dynamic_path = os.path.join(directory, "dynamic.json")
        soak_path = os.path.join(directory, "soak.json")
        dynamic_log = os.path.join(directory, "dynamic.log")
        soak_log = os.path.join(directory, "soak.log")
        manifest_path = os.path.join(directory, "launcher-manifest.json")
        for path, artifact in ((dynamic_path, dynamic), (soak_path, soak)):
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                json.dump(artifact, handle)
        for path in (dynamic_log, soak_log):
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("Mogwai test log\n")
        selected_gpu = {"gpu_index": "0", "gpu_name": "fixture-gpu", "driver_version": "fixture-driver"}
        launcher_samples = [
            dict(dynamic["vram"]["start"], timestamp_unix=1.0),
            dict(dynamic["vram"]["end"], timestamp_unix=2.0),
        ]
        def phase(role, seconds, output_path, log_path):
            return {
                "role": role,
                "expected_seconds": seconds,
                "output_json": output_path,
                "logfile": log_path,
                "command": [
                    "Mogwai.exe", "--device-type", "d3d12", "--headless", "--precise",
                    "--script", "tests/lumengi/run_churn_short.py", "--logfile", log_path,
                ],
                "timeout_seconds": seconds + 300.0,
                "process": {
                    "exit_code": 0,
                    "timed_out": False,
                    "started_at_unix": 1.0,
                    "finished_at_unix": 2.0,
                    "elapsed_seconds": 1.0,
                },
                "log": {"exists": True, "bytes": 16},
                "selected_gpu": selected_gpu,
                "launcher_vram_samples": launcher_samples,
                "child_contract_status": "READY",
                "status": "READY_FOR_OFFLINE_GATE",
            }
        launcher = {
            "schema_version": "S2-release-soak-launcher-v2",
            "status": "READY_FOR_OFFLINE_GATE",
            "requested": {"dynamic_seconds": MIN_DYNAMIC_SECONDS, "soak_seconds": MIN_SOAK_SECONDS, "gpu_index": "0"},
            "contract": {
                "single_gpu": True,
                "require_authoritative_renderer_provenance": True,
                "require_authoritative_gpu_wide_vram": True,
                "proxy_is_not_soak": True,
            },
            "selected_gpu": selected_gpu,
            "artifacts": {"dynamic": dynamic_path, "soak": soak_path},
            "phases": {
                "dynamic": phase("dynamic", MIN_DYNAMIC_SECONDS, dynamic_path, dynamic_log),
                "soak": phase("soak", MIN_SOAK_SECONDS, soak_path, soak_log),
            },
        }
        with open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(launcher, handle)
        launcher_result = evaluate_paths(manifest_path=manifest_path)
        assert launcher_result["status"] == "PASS", launcher_result
        assert launcher_result["launcher"]["recognized"] is True, launcher_result
        launcher["phases"]["soak"]["process"]["timed_out"] = True
        with open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(launcher, handle)
        assert evaluate_paths(manifest_path=manifest_path)["status"] == "BLOCKED"
    print(
        "S2_RELEASE_SOAK_SELF_TEST PASS strict-duration=PASS proxy-separation=PASS "
        "authoritative-vram=PASS launcher-chain=PASS monotonic-growth=FAIL missing-evidence=BLOCKED"
    )
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dynamic", help="30-minute dynamic churn JSON")
    parser.add_argument("--soak", help="2-hour dynamic soak JSON")
    parser.add_argument("--manifest", help="JSON manifest containing dynamic and soak paths")
    parser.add_argument("--output", help="gate JSON output path")
    parser.add_argument("--self-test", action="store_true", help="run dependency-free contract fixtures")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    if not args.output:
        parser.error("--output is required unless --self-test is used")
    if not args.manifest and not args.dynamic and not args.soak:
        parser.error("provide --manifest or at least one phase artifact")
    result = evaluate_paths(args.dynamic, args.soak, args.manifest)
    _write_json(args.output, result)
    print("S2_RELEASE_SOAK_GATE", result["status"], os.path.abspath(args.output))
    return 0 if result["status"] == "PASS" else 1 if result["status"] == "FAIL" else 2


if __name__ == "__main__":
    sys.exit(main())
