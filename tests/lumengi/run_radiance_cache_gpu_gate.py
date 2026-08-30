"""Strict offline GPU gate for the future LumenGI Radiance Cache ABI.

The current LumenGI pass only parses ``useRadianceCache``.  This runner is the
consumer-side contract to use once the GPU producer is wired.  A future Mogwai
capture (or a small readback helper) writes one JSON object with the fields
described by :data:`CONTRACT`; this script validates that object without
starting a GPU process.  It deliberately does not infer cache residency from
``diffuseGI`` or from a non-zero image.

The gate is strict:

* ``radianceCache`` and ``radianceCacheHitDist`` must be exposed and have
  finite, non-negative sampled output;
* ``radianceCacheValidity`` must expose a finite integer hit/sky validity mask;
* ``gpuProducerEnabled`` and ``gpuInterpolationEnabled`` must be exactly true
  (or integer ``1``);
* the ``radianceCacheStats`` binding must expose all counters, including
  ``staleWriteRejects``;
* a request on frame N must be ready on frame N+1; and
* a finite, non-negative, non-black fallback sample must be observable.

Missing channels, telemetry, the GPU API, or temporal evidence are
``BLOCKED``.  A present but invalid value is ``FAIL``.  There is no ``SKIP``
verdict, so an absent future integration can never be mistaken for a pass.

Normal/offline usage::

    python tests/lumengi/run_radiance_cache_gpu_gate.py \
        --input artifacts/lumengi/C10/radiance-cache-gpu.json

The dependency-free smoke test is::

    python tests/lumengi/run_radiance_cache_gpu_gate.py --self-test

No build or GPU is started by this module.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Tuple


GATE_SCHEMA_VERSION = "LumenGI.RadianceCacheGpuGate.v1"
SCRIPT_NAME = "run_radiance_cache_gpu_gate.py"
REQUIRED_CHANNELS = ("radianceCache", "radianceCacheHitDist", "radianceCacheValidity")
REQUIRED_COUNTERS = (
    "requestCount",
    "traceCount",
    "commitCount",
    "readyCount",
    "staleWriteRejects",
)
FRAME_COUNTERS = REQUIRED_COUNTERS
DEFAULT_OUT = Path(
    os.environ.get(
        "LUMEN_RADIANCE_CACHE_GPU_GATE_OUT",
        "artifacts/lumengi/C10/radiance-cache-gpu-gate",
    )
).absolute()
DEFAULT_MANIFEST = DEFAULT_OUT / "radiance-cache-gpu-gate.json"


# This is intentionally embedded in the runner: it is the frozen ABI that the
# future GPU producer must export.  Keeping it in the emitted manifest makes
# artifacts self-describing without adding a second mutable schema file.
CONTRACT: Dict[str, Any] = {
    "schemaVersion": GATE_SCHEMA_VERSION,
    "gpuApiAvailable": True,
    "requiredChannels": list(REQUIRED_CHANNELS),
    "requiredStatsBinding": "radianceCacheStats",
    "requiredProducerFlags": ["gpuProducerEnabled", "gpuInterpolationEnabled"],
    "requiredCounters": list(REQUIRED_COUNTERS),
    "framePair": "requestCount(frame=N)>0 and readyCount(frame=N+1)>=requestCount(frame=N)",
    "outputEvidence": ["available", "sampleCount", "finite", "nonnegative", "min", "max"],
    "fallbackEvidence": [
        "observed",
        "sampleCount",
        "finite",
        "nonnegative",
        "nonblack",
        "max",
    ],
    "missingTelemetryVerdict": "BLOCKED",
    "noSkipVerdict": True,
}


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _finite_number(value: Any) -> float | None:
    if not _is_number(value):
        return None
    result = float(value)
    return result if math.isfinite(result) else None


def _nonnegative_integer(value: Any) -> int | None:
    # Counts are deliberately not accepted as strings or 1.0: telemetry type
    # drift should block/fail loudly rather than silently changing semantics.
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return None
    return int(value)


def _is_one(value: Any) -> bool:
    return value is True or (isinstance(value, int) and not isinstance(value, bool) and value == 1)


def _flatten_numbers(value: Any) -> Iterable[Any]:
    if isinstance(value, (list, tuple)):
        for item in value:
            yield from _flatten_numbers(item)
    else:
        yield value


def _check_record(name: str, status: str, *, reason: str | None = None, **extra: Any) -> Dict[str, Any]:
    result: Dict[str, Any] = {"name": name, "status": status}
    if reason:
        result["reason"] = reason
    result.update(extra)
    return result


def _validate_api(manifest: Mapping[str, Any]) -> Dict[str, Any]:
    if "gpuApiAvailable" not in manifest:
        return _check_record("gpu API available", "BLOCKED", reason="missing gpuApiAvailable")
    if not _is_one(manifest["gpuApiAvailable"]):
        return _check_record("gpu API available", "BLOCKED", reason="GPU API unavailable")
    return _check_record("gpu API available", "PASS", observed=True)


def _validate_schema(manifest: Mapping[str, Any]) -> Dict[str, Any]:
    value = manifest.get("schemaVersion", manifest.get("schema"))
    if value is None:
        return _check_record(
            "manifest schema",
            "BLOCKED",
            reason="missing schemaVersion",
        )
    if value != GATE_SCHEMA_VERSION:
        return _check_record(
            "manifest schema",
            "FAIL",
            reason="expected %s, got %s" % (GATE_SCHEMA_VERSION, value),
        )
    return _check_record("manifest schema", "PASS", schemaVersion=value)


def _channel_descriptor(manifest: Mapping[str, Any], channel: str) -> Any:
    channels = manifest.get("channels")
    if isinstance(channels, Mapping):
        return channels.get(channel)
    # A top-level channel map is accepted as a migration convenience, but the
    # emitted report always records the canonical ``channels`` contract.
    return manifest.get(channel)


def _validate_channel(manifest: Mapping[str, Any], channel: str) -> Dict[str, Any]:
    descriptor = _channel_descriptor(manifest, channel)
    if descriptor is None:
        return _check_record(
            "channel:%s" % channel,
            "BLOCKED",
            reason="missing channel",
            channel=channel,
        )
    if not isinstance(descriptor, Mapping):
        return _check_record(
            "channel:%s" % channel,
            "BLOCKED",
            reason="channel descriptor is not an object",
            channel=channel,
        )
    missing = [
        key
        for key in ("available", "sampleCount", "finite", "nonnegative", "min", "max")
        if key not in descriptor
    ]
    if missing:
        return _check_record(
            "channel:%s" % channel,
            "BLOCKED",
            reason="missing channel evidence",
            channel=channel,
            missing=missing,
        )
    if not _is_one(descriptor["available"]):
        return _check_record(
            "channel:%s" % channel,
            "BLOCKED",
            reason="channel unavailable",
            channel=channel,
        )

    invalid: List[str] = []
    sample_count = _nonnegative_integer(descriptor["sampleCount"])
    if sample_count is None or sample_count == 0:
        invalid.append("sampleCount must be a positive integer")
    if descriptor["finite"] is not True:
        invalid.append("finite must be true")
    if descriptor["nonnegative"] is not True:
        invalid.append("nonnegative must be true")
    minimum = _finite_number(descriptor["min"])
    maximum = _finite_number(descriptor["max"])
    if minimum is None:
        invalid.append("min must be finite")
    if maximum is None:
        invalid.append("max must be finite")
    if minimum is not None and minimum < 0.0:
        invalid.append("min must be non-negative")
    if maximum is not None and maximum < 0.0:
        invalid.append("max must be non-negative")
    if minimum is not None and maximum is not None and maximum < minimum:
        invalid.append("max must be >= min")

    # ``samples`` is bounded evidence rather than a full-resolution image.  If
    # present, validate every scalar so a claimed summary cannot hide NaN/Inf
    # or negative radiance/hit distance values.
    if "samples" in descriptor:
        sample_values = list(_flatten_numbers(descriptor["samples"]))
        if not sample_values:
            invalid.append("samples must not be empty")
        for value in sample_values:
            number = _finite_number(value)
            if number is None:
                invalid.append("samples contain a non-finite value")
                break
            if number < 0.0:
                invalid.append("samples contain a negative value")
                break

    status = "FAIL" if invalid else "PASS"
    return _check_record(
        "channel:%s" % channel,
        status,
        reason="; ".join(invalid) if invalid else None,
        channel=channel,
        sampleCount=sample_count,
        finite=descriptor["finite"],
        nonnegative=descriptor["nonnegative"],
        min=minimum,
        max=maximum,
    )


def _stats_object(manifest: Mapping[str, Any]) -> Mapping[str, Any] | None:
    # ``radianceCacheStats`` is the binding name.  ``stats`` is accepted only
    # for offline manifests produced before the binding was named; both paths
    # are reported as the same canonical check.
    value = manifest.get("radianceCacheStats")
    if value is None:
        value = manifest.get("stats")
    return value if isinstance(value, Mapping) else None


def _validate_stats(manifest: Mapping[str, Any]) -> Tuple[Dict[str, Any], Mapping[str, Any] | None]:
    stats = _stats_object(manifest)
    if stats is None:
        return (
            _check_record(
                "radianceCacheStats binding",
                "BLOCKED",
                reason="missing radianceCacheStats binding or object",
            ),
            None,
        )

    missing: List[str] = []
    invalid: List[str] = []
    for flag in ("gpuProducerEnabled", "gpuInterpolationEnabled"):
        if flag not in stats:
            missing.append(flag)
        elif not _is_one(stats[flag]):
            invalid.append("%s must equal 1" % flag)
    for counter in REQUIRED_COUNTERS:
        if counter not in stats:
            missing.append(counter)
        elif _nonnegative_integer(stats[counter]) is None:
            invalid.append("%s must be a non-negative integer" % counter)

    # A nested counters object is allowed only to fill fields that are not
    # already present at the binding root.  This keeps the schema compatible
    # with a raw reflection dump while still requiring every counter.
    nested = stats.get("counters")
    if isinstance(nested, Mapping):
        for key in REQUIRED_COUNTERS:
            if key in missing and key in nested:
                missing.remove(key)
                if _nonnegative_integer(nested[key]) is None:
                    invalid.append("counters.%s must be a non-negative integer" % key)

    values = {key: _nonnegative_integer(stats.get(key)) for key in REQUIRED_COUNTERS}
    if isinstance(nested, Mapping):
        for key in REQUIRED_COUNTERS:
            if values[key] is None and _nonnegative_integer(nested.get(key)) is not None:
                values[key] = _nonnegative_integer(nested[key])

    if not missing and not invalid:
        if values["traceCount"] < values["requestCount"]:
            invalid.append("traceCount is below requestCount")
        if values["commitCount"] < values["requestCount"]:
            invalid.append("commitCount is below requestCount")
        if values["readyCount"] < values["requestCount"]:
            invalid.append("readyCount is below requestCount")

    if missing:
        status = "BLOCKED"
    elif invalid:
        status = "FAIL"
    else:
        status = "PASS"
    return (
        _check_record(
            "radianceCacheStats binding",
            status,
            reason="; ".join(invalid) if invalid else None,
            missing=missing,
            counters=values,
            staleWriteRejects=values.get("staleWriteRejects"),
        ),
        stats,
    )


def _frame_records(stats: Mapping[str, Any]) -> Sequence[Any] | None:
    frames = stats.get("frames")
    if frames is None:
        frames = stats.get("frameSamples")
    return frames if isinstance(frames, Sequence) and not isinstance(frames, (str, bytes)) else None


def _validate_frame_pair(stats: Mapping[str, Any] | None) -> Dict[str, Any]:
    if stats is None:
        return _check_record("frame N request -> frame N+1 ready", "BLOCKED", reason="stats unavailable")
    frames = _frame_records(stats)
    if frames is None or not frames:
        return _check_record(
            "frame N request -> frame N+1 ready",
            "BLOCKED",
            reason="missing per-frame telemetry",
        )

    records: List[Dict[str, int]] = []
    missing: List[str] = []
    invalid: List[str] = []
    for index, raw in enumerate(frames):
        if not isinstance(raw, Mapping):
            invalid.append("frames[%d] is not an object" % index)
            continue
        frame = _nonnegative_integer(raw.get("frame", raw.get("frameIndex")))
        if frame is None:
            missing.append("frames[%d].frame" % index)
            continue
        record: Dict[str, int] = {"frame": frame}
        for counter in FRAME_COUNTERS:
            if counter not in raw:
                missing.append("frames[%d].%s" % (index, counter))
                continue
            value = _nonnegative_integer(raw[counter])
            if value is None:
                invalid.append("frames[%d].%s must be a non-negative integer" % (index, counter))
            else:
                record[counter] = value
        if len(record) == len(FRAME_COUNTERS) + 1:
            records.append(record)

    pair: Dict[str, Any] | None = None
    by_frame = {record["frame"]: record for record in records}
    for record in records:
        next_record = by_frame.get(record["frame"] + 1)
        if next_record is None:
            continue
        requested = record["requestCount"]
        if requested > 0 and next_record["readyCount"] >= requested:
            pair = {
                "requestFrame": record["frame"],
                "readyFrame": next_record["frame"],
                "requested": requested,
                "ready": next_record["readyCount"],
            }
            break

    if missing:
        return _check_record(
            "frame N request -> frame N+1 ready",
            "BLOCKED",
            reason="missing per-frame counter",
            missing=missing,
        )
    if invalid:
        return _check_record(
            "frame N request -> frame N+1 ready",
            "FAIL",
            reason="; ".join(invalid),
        )
    if pair is None:
        return _check_record(
            "frame N request -> frame N+1 ready",
            "FAIL",
            reason="no consecutive request/ready pair",
            frames=len(records),
        )
    return _check_record(
        "frame N request -> frame N+1 ready",
        "PASS",
        pair=pair,
    )


def _validate_fallback(manifest: Mapping[str, Any]) -> Dict[str, Any]:
    fallback = manifest.get("fallback")
    if not isinstance(fallback, Mapping):
        return _check_record("nonblack fallback", "BLOCKED", reason="missing fallback evidence")
    required = ("observed", "sampleCount", "finite", "nonnegative", "nonblack", "max")
    missing = [key for key in required if key not in fallback]
    if missing:
        return _check_record("nonblack fallback", "BLOCKED", reason="missing fallback evidence", missing=missing)
    invalid: List[str] = []
    if not _is_one(fallback["observed"]):
        invalid.append("observed must be true")
    count = _nonnegative_integer(fallback["sampleCount"])
    if count is None or count == 0:
        invalid.append("sampleCount must be a positive integer")
    if fallback["finite"] is not True:
        invalid.append("finite must be true")
    if fallback["nonnegative"] is not True:
        invalid.append("nonnegative must be true")
    if fallback["nonblack"] is not True:
        invalid.append("nonblack must be true")
    maximum = _finite_number(fallback["max"])
    if maximum is None:
        invalid.append("max must be finite")
    elif maximum <= 0.0:
        invalid.append("max must be > 0 for nonblack fallback")
    if "samples" in fallback:
        for value in _flatten_numbers(fallback["samples"]):
            number = _finite_number(value)
            if number is None:
                invalid.append("samples contain a non-finite value")
                break
            if number < 0.0:
                invalid.append("samples contain a negative value")
                break
    return _check_record(
        "nonblack fallback",
        "FAIL" if invalid else "PASS",
        reason="; ".join(invalid) if invalid else None,
        sampleCount=count,
        max=maximum,
    )


def evaluate_manifest(manifest: Mapping[str, Any]) -> Dict[str, Any]:
    """Evaluate one future GPU manifest and return a JSON-safe gate report."""

    if not isinstance(manifest, Mapping):
        manifest = {}
    # run_radiance_cache.py keeps its producer report and the frozen consumer
    # contract side by side. Validate the explicitly named projection when it
    # is present; do not infer this shape from diffuseGI or legacy fields.
    nested_gate = manifest.get("gpuGate")
    if isinstance(nested_gate, Mapping):
        manifest = nested_gate
    checks: List[Dict[str, Any]] = [_validate_schema(manifest), _validate_api(manifest)]
    checks.extend(_validate_channel(manifest, channel) for channel in REQUIRED_CHANNELS)
    stats_check, stats = _validate_stats(manifest)
    checks.append(stats_check)
    checks.append(_validate_frame_pair(stats))
    checks.append(_validate_fallback(manifest))

    statuses = [check["status"] for check in checks]
    if any(status == "FAIL" for status in statuses):
        status = "FAIL"
    elif any(status == "BLOCKED" for status in statuses):
        status = "BLOCKED"
    else:
        status = "PASS"

    return {
        "schema": GATE_SCHEMA_VERSION,
        "schemaVersion": GATE_SCHEMA_VERSION,
        "script": SCRIPT_NAME,
        "status": status,
        "gpu": True,
        "contract": CONTRACT,
        "checks": checks,
        "summary": {
            "pass": statuses.count("PASS"),
            "blocked": statuses.count("BLOCKED"),
            "fail": statuses.count("FAIL"),
        },
    }


def _pass_fixture() -> Dict[str, Any]:
    return {
        "schema": GATE_SCHEMA_VERSION,
        "gpuApiAvailable": True,
        "channels": {
            "radianceCache": {
                "available": True,
                "sampleCount": 4,
                "finite": True,
                "nonnegative": True,
                "min": 0.0,
                "max": 1.25,
                "samples": [0.0, 0.25, 0.5, 1.25],
            },
            "radianceCacheHitDist": {
                "available": True,
                "sampleCount": 4,
                "finite": True,
                "nonnegative": True,
                "min": 0.0,
                "max": 3.0,
                "samples": [0.0, 1.0, 2.0, 3.0],
            },
            "radianceCacheValidity": {
                "available": True,
                "sampleCount": 4,
                "finite": True,
                "nonnegative": True,
                "min": 0.0,
                "max": 7.0,
                "samples": [0.0, 1.0, 2.0, 7.0],
            },
        },
        "radianceCacheStats": {
            "gpuProducerEnabled": 1,
            "gpuInterpolationEnabled": 1,
            "requestCount": 2,
            "traceCount": 2,
            "commitCount": 2,
            "readyCount": 2,
            "staleWriteRejects": 1,
            "frames": [
                {
                    "frame": 10,
                    "requestCount": 2,
                    "traceCount": 2,
                    "commitCount": 2,
                    "readyCount": 0,
                    "staleWriteRejects": 1,
                },
                {
                    "frame": 11,
                    "requestCount": 0,
                    "traceCount": 0,
                    "commitCount": 0,
                    "readyCount": 2,
                    "staleWriteRejects": 0,
                },
            ],
        },
        "fallback": {
            "observed": True,
            "sampleCount": 2,
            "finite": True,
            "nonnegative": True,
            "nonblack": True,
            "max": 0.35,
            "samples": [0.1, 0.35],
        },
    }


def _blocked_fixture() -> Dict[str, Any]:
    # Deliberately missing the hit-distance channel, stats binding, GPU API,
    # and fallback evidence.  Every missing integration point must remain
    # BLOCKED (never SKIP/PASS).
    return {
        "schema": GATE_SCHEMA_VERSION,
        "channels": {
            "radianceCache": {
                "available": True,
                "sampleCount": 1,
                "finite": True,
                "nonnegative": True,
                "min": 0.0,
                "max": 0.0,
            }
        },
    }


def _run_self_test() -> int:
    passed = evaluate_manifest(_pass_fixture())
    blocked = evaluate_manifest(_blocked_fixture())
    pass_ok = passed["status"] == "PASS"
    blocked_ok = blocked["status"] == "BLOCKED" and all(
        check["status"] != "SKIP" for check in blocked["checks"]
    )
    print("RADIANCE_CACHE_GPU_GATE_SELF_TEST_PASS", passed["status"])
    print("RADIANCE_CACHE_GPU_GATE_SELF_TEST_BLOCKED", blocked["status"])
    return 0 if pass_ok and blocked_ok else 1


def _read_manifest(path: Path) -> Mapping[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, Mapping):
        raise ValueError("input manifest must be a JSON object")
    return value


def _write_report(path: Path, report: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        dest="input_path",
        default=os.environ.get("LUMEN_RADIANCE_CACHE_GPU_GATE_INPUT", ""),
        help="future GPU manifest JSON (missing input is BLOCKED)",
    )
    parser.add_argument(
        "--output",
        dest="output_path",
        default=str(DEFAULT_MANIFEST),
        help="gate report JSON path",
    )
    parser.add_argument("--self-test", action="store_true", help="run dependency-free PASS/BLOCKED fixtures")
    args = parser.parse_args(argv)
    if args.self_test:
        return _run_self_test()

    input_path = Path(args.input_path).absolute() if args.input_path else None
    if input_path is None:
        manifest: Mapping[str, Any] = {}
        input_error = "missing input manifest (GPU runtime export not provided)"
    else:
        try:
            manifest = _read_manifest(input_path)
            input_error = None
        except Exception as error:  # malformed/unavailable API evidence is BLOCKED.
            manifest = {}
            input_error = repr(error)

    report = evaluate_manifest(manifest)
    report["input"] = {
        "path": str(input_path) if input_path else None,
        "error": input_error,
    }
    if input_error:
        report["status"] = "BLOCKED"
        report["checks"].append(
            _check_record("GPU runtime manifest input", "BLOCKED", reason=input_error)
        )
        report["summary"]["blocked"] += 1
    output_path = Path(args.output_path).absolute()
    _write_report(output_path, report)
    print("RADIANCE_CACHE_GPU_GATE_STATUS", report["status"])
    print("RADIANCE_CACHE_GPU_GATE_WROTE", str(output_path))
    return 0 if report["status"] == "PASS" else 2 if report["status"] == "BLOCKED" else 1


if __name__ == "__main__":
    sys.exit(main())
