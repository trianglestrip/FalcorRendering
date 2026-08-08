"""Stable manifest helpers for LumenGI benchmark runs.

This module intentionally depends only on the Python standard library so it can be
used both by Mogwai's embedded Python and by standalone regression tooling.
"""

from __future__ import annotations

import json
import math
import os
import platform
import statistics
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
BENCHMARK_NAME = "LumenGI"


def json_safe(value: Any) -> Any:
    """Convert pybind and other mapping/sequence values to strict JSON values."""

    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, Mapping):
        return {str(key): json_safe(item) for key, item in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        return [json_safe(item) for item in value]
    return str(value)


def percentile(values: Sequence[float], fraction: float) -> float | None:
    """Return a linearly interpolated percentile for a fraction in [0, 1]."""

    finite_values = sorted(float(value) for value in values if math.isfinite(float(value)))
    if not finite_values:
        return None
    if not 0.0 <= fraction <= 1.0:
        raise ValueError("fraction must be in the range [0, 1]")
    position = fraction * (len(finite_values) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return finite_values[lower]
    weight = position - lower
    return finite_values[lower] * (1.0 - weight) + finite_values[upper] * weight


def summarize_profiler_capture(capture: Mapping[str, Any] | None) -> dict[str, Any]:
    """Reduce Falcor Profiler capture records to stable benchmark fields."""

    if not capture:
        return {"frame_count": 0, "events": {}}

    safe_capture = json_safe(capture)
    events = safe_capture.get("events", {}) if isinstance(safe_capture, dict) else {}
    event_summary: dict[str, Any] = {}
    for event_name in sorted(events):
        event = events[event_name]
        records = event.get("records", []) if isinstance(event, dict) else []
        finite_records = [float(value) for value in records if value is not None and math.isfinite(float(value))]
        event_summary[event_name] = {
            "sample_count": len(finite_records),
            "min_ms": min(finite_records) if finite_records else None,
            "max_ms": max(finite_records) if finite_records else None,
            "mean_ms": statistics.fmean(finite_records) if finite_records else None,
            "p50_ms": percentile(finite_records, 0.50),
            "p95_ms": percentile(finite_records, 0.95),
            "p99_ms": percentile(finite_records, 0.99),
        }

    return {
        "frame_count": int(safe_capture.get("frame_count", 0)),
        "events": event_summary,
    }


def _run_git(repo_root: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=repo_root,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip()


def collect_git_manifest(repo_root: Path) -> dict[str, Any]:
    status = _run_git(repo_root, "status", "--porcelain")
    return {
        "commit": _run_git(repo_root, "rev-parse", "HEAD"),
        "branch": _run_git(repo_root, "branch", "--show-current"),
        "dirty": bool(status) if status is not None else None,
    }


def collect_system_manifest() -> dict[str, Any]:
    """Collect stable host fields; GPU/driver values are supplied by the runner."""

    return {
        "platform": platform.platform(),
        "python_version": platform.python_version(),
        "machine": platform.machine(),
        "gpu": os.getenv("LUMENGI_BENCHMARK_GPU"),
        "driver": os.getenv("LUMENGI_BENCHMARK_DRIVER"),
        "device_type": os.getenv("LUMENGI_BENCHMARK_DEVICE_TYPE", "d3d12"),
    }


def build_manifest(
    repo_root: Path,
    configuration: Mapping[str, Any],
    *,
    status: str,
    profiler_capture: Mapping[str, Any] | None = None,
    pass_properties: Mapping[str, Any] | None = None,
    resource_stats: Mapping[str, Any] | None = None,
    error: str | None = None,
) -> dict[str, Any]:
    """Build one schema-versioned benchmark manifest."""

    return {
        "schema_version": SCHEMA_VERSION,
        "benchmark": BENCHMARK_NAME,
        "status": status,
        "git": collect_git_manifest(repo_root),
        "system": collect_system_manifest(),
        "configuration": json_safe(configuration),
        "lumen_gi": {
            "pass_properties": json_safe(pass_properties or {}),
            "resource_stats": json_safe(resource_stats or {}),
        },
        "profiler": summarize_profiler_capture(profiler_capture),
        "error": error,
    }


def write_json(path: Path, payload: Mapping[str, Any]) -> None:
    """Atomically write deterministic strict JSON."""

    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = path.with_name(path.name + ".tmp")
    with temp_path.open("w", encoding="utf-8", newline="\n") as output:
        json.dump(json_safe(payload), output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")
    os.replace(temp_path, path)
