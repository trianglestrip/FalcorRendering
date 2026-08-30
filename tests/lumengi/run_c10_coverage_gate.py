"""Strict C10 Radiance Cache coverage-quality gate.

The producer and consumer gates prove that the GPU resources are wired and that
the frame fence is legal.  They do not prove that the world-space probe
clipmap covers the scene.  This gate is intentionally narrower and stronger:

* query hit/miss counters are reconciled against their own ``queryAttempts``
  denominator (never against image RGB or frame dimensions);
* every CPU clipmap level reported by ``levelCount`` must have explicit
  level-local probe-position and query evidence;
* hit-distance validity is checked independently of radiance, and the final
  GI fallback must be observable and finite; and
* missing level telemetry is ``BLOCKED`` rather than a guessed pass.

The canonical optional telemetry payload is accepted under
``radianceCacheStats.coverageByLevel`` (``levelCoverage`` and
``perLevelCoverage`` are migration aliases)::

    {
      "level": 0,
      "projectedProbeCount": 32,
      "inBoundsProbeCount": 28,
      "queryAttempts": 1000,
      "queryHits": 700,
      "queryMisses": 300,
      "sampleCount": 1000,
      "validHitDistanceCount": 700,
      "fallbackSampleCount": 300
    }

No GPU process is started.  The input is normally the JSON emitted by
``run_radiance_cache.py``; a future Host telemetry extension can add the
per-level object without changing this gate's denominator semantics.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Tuple


SCHEMA = "LumenGI.C10.RadianceCacheCoverageGate.v1"
SCRIPT_NAME = "run_c10_coverage_gate.py"
MISS_SENTINEL = 65504.0
EPSILON = 1e-6

# These are release-quality defaults, not pass/fail claims about every scene.
# They are exposed in the report and can be overridden for a deliberately
# different target while preserving all structural checks.
DEFAULT_MIN_QUERY_HIT_FRACTION = 0.05
DEFAULT_MIN_IN_BOUNDS_FRACTION = 0.50
DEFAULT_MIN_HIT_DISTANCE_FRACTION = 0.01

LEVEL_COVERAGE_ALIASES = ("coverageByLevel", "levelCoverage", "perLevelCoverage")
QUERY_KEYS = ("queryAttempts", "queryHits", "queryMisses")


def _record(name: str, status: str, reason: str | None = None, **extra: Any) -> Dict[str, Any]:
    result: Dict[str, Any] = {"name": name, "status": status}
    if reason:
        result["reason"] = reason
    result.update(extra)
    return result


def _mapping(value: Any) -> Mapping[str, Any] | None:
    return value if isinstance(value, Mapping) else None


def _number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    result = float(value)
    return result if math.isfinite(result) else None


def _nonnegative_number(value: Any) -> float | None:
    result = _number(value)
    return result if result is not None and result >= 0.0 else None


def _count(value: Any) -> int | None:
    result = _number(value)
    if result is None or result < 0.0 or abs(result - round(result)) > EPSILON:
        return None
    return int(round(result))


def _fraction(value: Any) -> float | None:
    result = _number(value)
    return result if result is not None and -EPSILON <= result <= 1.0 + EPSILON else None


def _root_and_gpu(manifest: Mapping[str, Any]) -> Tuple[Mapping[str, Any], Mapping[str, Any]]:
    gpu = _mapping(manifest.get("gpu"))
    return manifest, gpu if gpu is not None else {}


def _stats(gpu: Mapping[str, Any], root: Mapping[str, Any]) -> Mapping[str, Any] | None:
    for container in (gpu, root):
        for key in ("radianceCacheStatsAfterStatic", "radianceCacheStatsAfterWarmup", "radianceCacheStats", "stats"):
            value = _mapping(container.get(key))
            if value is not None:
                return value
    return None


def _series(gpu: Mapping[str, Any], root: Mapping[str, Any]) -> List[Mapping[str, Any]]:
    for container in (gpu, root):
        value = container.get("series")
        if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
            return [item for item in value if isinstance(item, Mapping)]
    return []


def _level_payload(stats: Mapping[str, Any]) -> Any:
    for key in LEVEL_COVERAGE_ALIASES:
        if key in stats:
            return stats[key]
    return None


def _level_records(stats: Mapping[str, Any]) -> Tuple[List[Mapping[str, Any]], List[str]]:
    payload = _level_payload(stats)
    if isinstance(payload, Sequence) and not isinstance(payload, (str, bytes)):
        return [item for item in payload if isinstance(item, Mapping)], []
    if isinstance(payload, Mapping):
        records: List[Mapping[str, Any]] = []
        malformed: List[str] = []
        for key, value in payload.items():
            if not str(key).lstrip("-").isdigit() or not isinstance(value, Mapping):
                malformed.append(str(key))
                continue
            item = dict(value)
            item.setdefault("level", int(key))
            records.append(item)
        return records, malformed
    return [], []


def _validate_counter_snapshot(stats: Mapping[str, Any]) -> Tuple[Dict[str, int] | None, str | None]:
    values: Dict[str, int] = {}
    for key in QUERY_KEYS:
        if key not in stats:
            return None, f"missing {key}"
        value = _count(stats[key])
        if value is None:
            return None, f"{key} must be a finite non-negative integer"
        values[key] = value
    if values["queryHits"] + values["queryMisses"] != values["queryAttempts"]:
        return None, "queryHits + queryMisses does not equal queryAttempts"
    return values, None


def _aggregate_query_series(records: Sequence[Mapping[str, Any]]) -> Dict[str, Any]:
    """Aggregate cumulative counters across resettable phase segments.

    Mogwai trajectories reset the cache on scene/camera changes.  A decrease
    starts a new segment; using the maximum in each monotonic segment avoids
    counting every cumulative snapshot as a new sample while still retaining
    every reset domain in the denominator.
    """

    segments: List[Dict[str, Any]] = []
    current: Dict[str, Any] | None = None
    errors: List[str] = []
    for index, record in enumerate(records):
        stats = _mapping(record.get("radianceCacheStats")) or record
        snapshot, error = _validate_counter_snapshot(stats)
        if error:
            # Frames before the first readback commonly have no counters.  A
            # partial stats record is still useful evidence, but never turns
            # into a quality pass.
            if any(key in stats for key in QUERY_KEYS):
                errors.append(f"series[{index}]: {error}")
            continue
        frame = _count(record.get("frame", stats.get("frameIndex", 0)))
        if current is None or any(snapshot[key] < current[key] for key in QUERY_KEYS):
            current = {**snapshot, "firstFrame": frame, "lastFrame": frame}
            segments.append(current)
        else:
            for key in QUERY_KEYS:
                current[key] = snapshot[key]
            current["lastFrame"] = frame

    totals = {key: sum(int(segment[key]) for segment in segments) for key in QUERY_KEYS}
    attempts = totals["queryAttempts"]
    hits = totals["queryHits"]
    misses = totals["queryMisses"]
    return {
        "totals": totals,
        "segments": segments,
        "segmentCount": len(segments),
        "reconciled": attempts == hits + misses and attempts > 0,
        "hitFraction": hits / attempts if attempts else None,
        "missFraction": misses / attempts if attempts else None,
        "errors": errors,
    }


def _latest_channel_descriptors(records: Sequence[Mapping[str, Any]]) -> Dict[str, Mapping[str, Any]]:
    result: Dict[str, Mapping[str, Any]] = {}
    # Last complete sample is the conservative settled observation.  The
    # runner nests output descriptors beneath ``series[*].radianceCache``.
    for record in records:
        container = _mapping(record.get("radianceCache")) or {}
        for channel in ("radianceCache", "radianceCacheHitDist", "radianceCacheValidity"):
            value = _mapping(container.get(channel))
            if value is not None:
                result[channel] = value
    return result


def _validate_channels(records: Sequence[Mapping[str, Any]]) -> Dict[str, Any]:
    channels = _latest_channel_descriptors(records)
    missing = [
        channel for channel in ("radianceCache", "radianceCacheHitDist", "radianceCacheValidity")
        if channel not in channels
    ]
    invalid: List[str] = []
    observations: Dict[str, Any] = {}
    for channel, descriptor in channels.items():
        finite = descriptor.get("finite") is True
        nonnegative = descriptor.get("nonnegative") is True
        sample_count = _count(descriptor.get("sampleCount"))
        if not finite:
            invalid.append(f"{channel}.finite must be true")
        if not nonnegative:
            invalid.append(f"{channel}.nonnegative must be true")
        if sample_count is None or sample_count <= 0:
            invalid.append(f"{channel}.sampleCount must be a positive integer")
        observations[channel] = {
            "finite": finite,
            "nonnegative": nonnegative,
            "sampleCount": sample_count,
            "validFraction": _fraction(
                descriptor.get("validHitFraction", descriptor.get("validFraction"))
            ),
            "hitFraction": _fraction(
                (_mapping(descriptor.get("validity")) or {}).get("hitFraction")
            ),
            "skyFraction": _fraction(
                (_mapping(descriptor.get("validity")) or {}).get("skyFraction")
            ),
        }
    status = "BLOCKED" if missing else "FAIL" if invalid else "PASS"
    reason = f"missing channels: {', '.join(missing)}" if missing else "; ".join(invalid) or None
    return _record("C10 output channel invariants", status, reason, missing=missing, invalid=invalid, observations=observations)


def _validate_fallback(gpu: Mapping[str, Any], root: Mapping[str, Any]) -> Dict[str, Any]:
    fallback: Mapping[str, Any] | None = None
    for container in (gpu, root):
        gate = _mapping(container.get("gpuGate"))
        if gate is not None:
            fallback = _mapping(gate.get("fallback"))
        if fallback is None:
            fallback = _mapping(container.get("fallback"))
        if fallback is not None:
            break
    if fallback is None:
        return _record("final GI fallback evidence", "BLOCKED", "missing explicit fallback descriptor")
    missing = [key for key in ("observed", "finite", "nonnegative", "nonblack", "sampleCount") if key not in fallback]
    invalid: List[str] = []
    for key in ("observed", "finite", "nonnegative", "nonblack"):
        if key in fallback and fallback[key] is not True:
            invalid.append(f"{key} must be true")
    count = _count(fallback.get("sampleCount"))
    if count is not None and count <= 0:
        invalid.append("sampleCount must be positive")
    if count is None and "sampleCount" not in missing:
        invalid.append("sampleCount must be a finite non-negative integer")
    status = "BLOCKED" if missing else "FAIL" if invalid else "PASS"
    reason = ", ".join(missing) if missing else "; ".join(invalid) or None
    return _record("final GI fallback evidence", status, reason, observed=dict(fallback))


def _validate_levels(
    stats: Mapping[str, Any] | None,
    *,
    min_query_hit_fraction: float,
    min_in_bounds_fraction: float,
    min_hit_distance_fraction: float,
    expected_levels: Sequence[int] | None = None,
) -> Dict[str, Any]:
    if stats is None:
        return _record("per-level clipmap coverage", "BLOCKED", "missing radianceCacheStats")
    raw_level_count = stats.get("levelCount")
    level_count = _count(raw_level_count)
    if level_count is None or level_count <= 0:
        return _record("per-level clipmap coverage", "BLOCKED", "levelCount is missing or invalid")
    records, malformed = _level_records(stats)
    selected_levels = sorted(set(int(level) for level in expected_levels)) if expected_levels is not None else list(range(level_count))
    invalid_selected = [level for level in selected_levels if level < 0 or level >= level_count]
    if invalid_selected:
        return _record(
            "per-level clipmap coverage",
            "FAIL",
            "expected level outside levelCount",
            levelCount=level_count,
            expectedLevels=selected_levels,
            invalidExpectedLevels=invalid_selected,
        )
    if not selected_levels:
        return _record("per-level clipmap coverage", "BLOCKED", "expectedLevels is empty")
    if _level_payload(stats) is None:
        return _record(
            "per-level clipmap coverage",
            "BLOCKED",
            "missing coverageByLevel telemetry; projectedProbeCount is not a level denominator",
            levelCount=level_count,
            expectedLevels=selected_levels,
            missingFields=["coverageByLevel"],
        )
    if malformed:
        return _record("per-level clipmap coverage", "FAIL", "malformed level entries", malformed=malformed)

    by_level: Dict[int, Mapping[str, Any]] = {}
    duplicate: List[int] = []
    invalid: List[str] = []
    missing_fields: List[str] = []
    for index, raw in enumerate(records):
        level = _count(raw.get("level"))
        if level is None:
            invalid.append(f"level[{index}].level must be a non-negative integer")
            continue
        if level in by_level:
            duplicate.append(level)
        by_level[level] = raw
    expected = set(selected_levels)
    missing_levels = sorted(expected - set(by_level))
    unexpected_levels = sorted(set(by_level) - set(range(level_count)))
    if missing_levels:
        missing_fields.extend([f"coverageByLevel[{level}]" for level in missing_levels])
    if unexpected_levels:
        invalid.extend([f"unexpected level {level}" for level in unexpected_levels])
    if duplicate:
        invalid.append(f"duplicate levels: {sorted(set(duplicate))}")

    observations: List[Dict[str, Any]] = []
    for level in sorted(expected & set(by_level)):
        raw = by_level[level]
        values: Dict[str, int] = {}
        for key in (
            "projectedProbeCount",
            "inBoundsProbeCount",
            "queryAttempts",
            "queryHits",
            "queryMisses",
            "sampleCount",
            "validHitDistanceCount",
            "fallbackSampleCount",
        ):
            value = _count(raw.get(key))
            if value is None:
                missing_fields.append(f"coverageByLevel[{level}].{key}")
            else:
                values[key] = value
        if any(key not in values for key in (
            "projectedProbeCount", "inBoundsProbeCount", "queryAttempts", "queryHits",
            "queryMisses", "sampleCount", "validHitDistanceCount", "fallbackSampleCount",
        )):
            continue
        if values["inBoundsProbeCount"] > values["projectedProbeCount"]:
            invalid.append(f"level {level}: inBoundsProbeCount exceeds projectedProbeCount")
        if values["queryHits"] + values["queryMisses"] != values["queryAttempts"]:
            invalid.append(f"level {level}: query hit/miss denominator mismatch")
        if values["validHitDistanceCount"] > values["sampleCount"]:
            invalid.append(f"level {level}: validHitDistanceCount exceeds sampleCount")
        if values["fallbackSampleCount"] > values["sampleCount"]:
            invalid.append(f"level {level}: fallbackSampleCount exceeds sampleCount")
        query_fraction = values["queryHits"] / values["queryAttempts"] if values["queryAttempts"] else 0.0
        in_bounds_fraction = (
            values["inBoundsProbeCount"] / values["projectedProbeCount"]
            if values["projectedProbeCount"] else 0.0
        )
        hit_distance_fraction = values["validHitDistanceCount"] / values["sampleCount"] if values["sampleCount"] else 0.0
        observations.append({
            "level": level,
            **values,
            "queryHitFraction": query_fraction,
            "inBoundsFraction": in_bounds_fraction,
            "hitDistanceFraction": hit_distance_fraction,
            "fallbackFraction": values["fallbackSampleCount"] / values["sampleCount"] if values["sampleCount"] else 0.0,
        })

    below: List[str] = []
    for item in observations:
        level = item["level"]
        if item["queryHitFraction"] < min_query_hit_fraction:
            below.append(f"level {level} queryHitFraction {item['queryHitFraction']:.6f} < {min_query_hit_fraction:.6f}")
        if item["inBoundsFraction"] < min_in_bounds_fraction:
            below.append(f"level {level} inBoundsFraction {item['inBoundsFraction']:.6f} < {min_in_bounds_fraction:.6f}")
        if item["hitDistanceFraction"] < min_hit_distance_fraction:
            below.append(f"level {level} hitDistanceFraction {item['hitDistanceFraction']:.6f} < {min_hit_distance_fraction:.6f}")

    if invalid:
        status = "FAIL"
    elif missing_fields:
        status = "BLOCKED"
    elif below:
        status = "OPEN"
    else:
        status = "PASS"
    reason = "; ".join(invalid) if invalid else (
        "missing per-level evidence: " + ", ".join(missing_fields) if missing_fields else (
            "quality thresholds not met: " + "; ".join(below) if below else None
        )
    )
    return _record(
        "per-level clipmap coverage",
        status,
        reason,
        levelCount=level_count,
        expectedLevels=selected_levels,
        observedLevels=sorted(by_level),
        ignoredLevels=sorted(set(by_level) - expected),
        missingFields=missing_fields,
        observations=observations,
        thresholds={
            "minQueryHitFraction": min_query_hit_fraction,
            "minInBoundsFraction": min_in_bounds_fraction,
            "minHitDistanceFraction": min_hit_distance_fraction,
        },
    )


def evaluate(
    manifest: Mapping[str, Any],
    *,
    min_query_hit_fraction: float = DEFAULT_MIN_QUERY_HIT_FRACTION,
    min_in_bounds_fraction: float = DEFAULT_MIN_IN_BOUNDS_FRACTION,
    min_hit_distance_fraction: float = DEFAULT_MIN_HIT_DISTANCE_FRACTION,
    expected_levels: Sequence[int] | None = None,
) -> Dict[str, Any]:
    root, gpu = _root_and_gpu(manifest if isinstance(manifest, Mapping) else {})
    if expected_levels is None:
        phase = _mapping(root.get("phaseMetadata"))
        candidate = root.get("expectedLevels")
        if candidate is None and phase is not None:
            candidate = phase.get("expectedLevels")
        if isinstance(candidate, Sequence) and not isinstance(candidate, (str, bytes)):
            parsed = [_count(item) for item in candidate]
            if all(item is not None for item in parsed):
                expected_levels = [int(item) for item in parsed if item is not None]
    stats = _stats(gpu, root)
    records = _series(gpu, root)
    query = _aggregate_query_series(records)
    query_status = "FAIL" if query["errors"] else "PASS" if query["reconciled"] else "BLOCKED"
    checks: List[Dict[str, Any]] = [
        _record(
            "query denominator reconciliation",
            query_status,
            "; ".join(query["errors"]) if query["errors"] else (
                "queryAttempts is missing or zero" if query_status == "BLOCKED" else None
            ),
            **query,
        ),
        _validate_channels(records),
        _validate_fallback(gpu, root),
        _validate_levels(
            stats,
            min_query_hit_fraction=min_query_hit_fraction,
            min_in_bounds_fraction=min_in_bounds_fraction,
            min_hit_distance_fraction=min_hit_distance_fraction,
            expected_levels=expected_levels,
        ),
    ]
    statuses = [str(check.get("status")) for check in checks]
    if "FAIL" in statuses:
        status = "FAIL"
    elif "BLOCKED" in statuses:
        status = "BLOCKED"
    elif "OPEN" in statuses:
        status = "OPEN"
    else:
        status = "PASS"
    projection = {}
    if stats is not None:
        for key in ("projectedProbeCount", "inBoundsProbeCount", "levelCount", "resolution"):
            if key in stats:
                projection[key] = stats[key]
        projected = _count(stats.get("projectedProbeCount"))
        in_bounds = _count(stats.get("inBoundsProbeCount"))
        projection["inBoundsFraction"] = in_bounds / projected if projected and in_bounds is not None and in_bounds <= projected else None
    return {
        "schema": SCHEMA,
        "schemaVersion": SCHEMA,
        "script": SCRIPT_NAME,
        "status": status,
        "productionPass": status == "PASS",
        "contract": {
            "queryDenominator": "queryAttempts; queryHits + queryMisses must equal it per snapshot",
            "levelDenominator": "levelCount; expected levels are [0, levelCount)",
            "probePosition": "projectedProbeCount/inBoundsProbeCount per level",
            "hitDistance": {"denominator": "validHitDistanceCount/sampleCount", "missSentinel": MISS_SENTINEL},
            "fallback": "explicit finite/non-negative/non-black fallback sample",
            "missingLevelTelemetry": "BLOCKED",
        },
        "checks": checks,
        "queryCoverage": query,
        "probeProjection": projection,
        "thresholds": {
            "minQueryHitFraction": min_query_hit_fraction,
            "minInBoundsFraction": min_in_bounds_fraction,
            "minHitDistanceFraction": min_hit_distance_fraction,
        },
        "phase": {
            "phaseId": root.get("phaseId") or (_mapping(root.get("phaseMetadata")) or {}).get("phaseId"),
            "expectedLevels": list(expected_levels) if expected_levels is not None else list(range(int(projection.get("levelCount", 0)))) if projection.get("levelCount") else None,
        },
        "summary": {name.lower(): statuses.count(name) for name in ("PASS", "OPEN", "BLOCKED", "FAIL")},
    }


def _descriptor(hit_fraction: float = 0.7, sky_fraction: float = 0.1) -> Dict[str, Any]:
    return {
        "finite": True,
        "nonnegative": True,
        "sampleCount": 100,
        "validHitFraction": hit_fraction,
        "validity": {"hitFraction": hit_fraction, "skyFraction": sky_fraction},
    }


def _fixture() -> Dict[str, Any]:
    levels = []
    for level in range(2):
        levels.append({
            "level": level,
            "projectedProbeCount": 10,
            "inBoundsProbeCount": 8,
            "queryAttempts": 100,
            "queryHits": 70,
            "queryMisses": 30,
            "sampleCount": 100,
            "validHitDistanceCount": 70,
            "fallbackSampleCount": 30,
        })
    descriptor = {
        "radianceCache": _descriptor(),
        "radianceCacheHitDist": _descriptor(),
        "radianceCacheValidity": {
            "finite": True,
            "nonnegative": True,
            "sampleCount": 100,
        },
    }
    return {
        "gpu": {
            "radianceCacheStatsAfterWarmup": {
                "levelCount": 2,
                "resolution": 8,
                "coverageByLevel": levels,
            },
            "series": [
                {
                    "frame": 1,
                    "radianceCacheStats": {"queryAttempts": 100, "queryHits": 70, "queryMisses": 30},
                    "radianceCache": descriptor,
                }
            ],
            "gpuGate": {
                "fallback": {
                    "observed": True,
                    "finite": True,
                    "nonnegative": True,
                    "nonblack": True,
                    "sampleCount": 30,
                }
            },
        }
    }


def _far_field_fixture() -> Dict[str, Any]:
    """Return a deterministic six-level fixture with an unserved far field.

    This is deliberately not a passing fixture.  Levels 0--4 have valid query
    and hit-distance denominators, while level 5 has projected/in-bounds probes
    but no query or payload samples.  Keeping the level record (instead of
    deleting it) exercises the important distinction between *telemetry
    missing* (BLOCKED) and *level observed but unserved* (OPEN).
    """

    manifest = _fixture()
    stats = manifest["gpu"]["radianceCacheStatsAfterWarmup"]
    levels = list(stats["coverageByLevel"])
    for level in range(2, 5):
        levels.append({
            "level": level,
            "projectedProbeCount": 16,
            "inBoundsProbeCount": 8,
            "queryAttempts": 100,
            "queryHits": 70,
            "queryMisses": 30,
            "sampleCount": 100,
            "validHitDistanceCount": 70,
            "fallbackSampleCount": 30,
        })
    levels.append({
        "level": 5,
        "projectedProbeCount": 16,
        "inBoundsProbeCount": 8,
        "queryAttempts": 0,
        "queryHits": 0,
        "queryMisses": 0,
        "sampleCount": 0,
        "validHitDistanceCount": 0,
        "fallbackSampleCount": 0,
    })
    stats["levelCount"] = 6
    stats["coverageByLevel"] = levels
    manifest["farFieldFixture"] = {
        "targetLevel": 5,
        "projectedProbeCount": 16,
        "inBoundsProbeCount": 8,
        "queryAttempts": 0,
        "sampleCount": 0,
        "expectedStatus": "OPEN",
        "reason": "level-5 probes are projected/in-bounds but no query or hit-distance samples exist",
        "recommendedCapturePlan": {
            "scene": "test_scenes/sphere_array.pyscene",
            "cameraPosition": [0.0, 0.28, 50.0],
            "cameraTarget": [0.0, 0.28, 0.0],
            "focalLength": 35.0,
            "requiredTelemetry": [
                "coverageLevel5ProjectedProbeCount",
                "coverageLevel5InBoundsProbeCount",
                "coverageLevel5QueryAttempts",
                "coverageLevel5QueryHits",
                "coverageLevel5QueryMisses",
                "coverageLevel5SampleCount",
                "coverageLevel5ValidHitDistanceCount",
            ],
            "note": "run one GPU trajectory at an authored far-field camera; this fixture does not claim runtime coverage",
        },
    }
    return manifest


def _run_self_test() -> int:
    passed = evaluate(_fixture())
    phase_selected = evaluate(_fixture(), expected_levels=[0, 1])
    missing = _fixture()
    del missing["gpu"]["radianceCacheStatsAfterWarmup"]["coverageByLevel"]
    blocked = evaluate(missing)
    mismatch = _fixture()
    mismatch["gpu"]["series"][0]["radianceCacheStats"]["queryMisses"] = 31
    failed = evaluate(mismatch)
    threshold = _fixture()
    threshold["gpu"]["radianceCacheStatsAfterWarmup"]["coverageByLevel"][0]["queryHits"] = 1
    threshold["gpu"]["radianceCacheStatsAfterWarmup"]["coverageByLevel"][0]["queryMisses"] = 99
    open_report = evaluate(threshold)
    far_field_manifest = _far_field_fixture()
    far_field = evaluate(far_field_manifest)
    far_field_check = next(
        check for check in far_field["checks"] if check.get("name") == "per-level clipmap coverage"
    )
    far_field_level = next(
        item for item in far_field_check.get("observations", []) if item.get("level") == 5
    )
    ok = (
        passed["status"] == "PASS"
        and phase_selected["status"] == "PASS"
        and blocked["status"] == "BLOCKED"
        and failed["status"] == "FAIL"
        and open_report["status"] == "OPEN"
        and far_field["status"] == "OPEN"
        and far_field_check["status"] == "OPEN"
        and far_field_level["queryAttempts"] == 0
        and far_field_level["sampleCount"] == 0
        and far_field_manifest["farFieldFixture"]["recommendedCapturePlan"]["cameraPosition"][2] == 50.0
    )
    print("C10_COVERAGE_GATE_SELF_TEST_PASS", passed["status"])
    print("C10_COVERAGE_GATE_SELF_TEST_PHASE", phase_selected["status"])
    print("C10_COVERAGE_GATE_SELF_TEST_BLOCKED", blocked["status"])
    print("C10_COVERAGE_GATE_SELF_TEST_FAIL", failed["status"])
    print("C10_COVERAGE_GATE_SELF_TEST_OPEN", open_report["status"])
    print("C10_COVERAGE_GATE_SELF_TEST_FAR_FIELD_OPEN", far_field["status"])
    return 0 if ok else 1


def _read(path: Path) -> Mapping[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, Mapping):
        raise ValueError("input manifest must be a JSON object")
    return value


def _write(path: Path, report: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", default=os.environ.get("LUMEN_C10_COVERAGE_INPUT", ""))
    parser.add_argument(
        "--output",
        default=os.environ.get("LUMEN_C10_COVERAGE_OUT", "artifacts/lumengi/C10/c10-coverage-gate.json"),
    )
    parser.add_argument("--min-query-hit-fraction", type=float, default=DEFAULT_MIN_QUERY_HIT_FRACTION)
    parser.add_argument("--min-in-bounds-fraction", type=float, default=DEFAULT_MIN_IN_BOUNDS_FRACTION)
    parser.add_argument("--min-hit-distance-fraction", type=float, default=DEFAULT_MIN_HIT_DISTANCE_FRACTION)
    parser.add_argument(
        "--expected-levels",
        default=os.environ.get("LUMEN_C10_EXPECTED_LEVELS", ""),
        help="comma-separated real clipmap levels to validate; default validates every level",
    )
    parser.add_argument(
        "--phase-manifest",
        default=os.environ.get("LUMEN_C10_PHASE_MANIFEST", ""),
        help="JSON manifest with phases [{input, phaseId, expectedLevels}]; overall status is AND",
    )
    parser.add_argument(
        "--far-field-fixture",
        action="store_true",
        help="write a deterministic level-5 unserved-far-field fixture (expected status OPEN)",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return _run_self_test()

    input_path = Path(args.input).absolute() if args.input else None
    input_error: str | None = None
    phase_path = Path(args.phase_manifest).absolute() if args.phase_manifest else None
    if phase_path is not None:
        try:
            phase_payload = _read(phase_path)
            raw_phases = phase_payload.get("phases")
            if not isinstance(raw_phases, Sequence) or isinstance(raw_phases, (str, bytes)) or not raw_phases:
                raise ValueError("phase manifest phases must be a non-empty array")
            phase_reports: List[Dict[str, Any]] = []
            for index, phase in enumerate(raw_phases):
                if not isinstance(phase, Mapping):
                    raise ValueError(f"phase[{index}] must be an object")
                phase_input = phase.get("input")
                if not isinstance(phase_input, str) or not phase_input:
                    raise ValueError(f"phase[{index}].input is required")
                phase_input_path = Path(phase_input)
                if not phase_input_path.is_absolute():
                    phase_input_path = (phase_path.parent / phase_input).absolute()
                phase_data = _read(phase_input_path)
                levels = phase.get("expectedLevels")
                parsed_levels = None
                if isinstance(levels, Sequence) and not isinstance(levels, (str, bytes)):
                    parsed_levels = [int(value) for value in levels]
                phase_report = evaluate(
                    phase_data,
                    min_query_hit_fraction=args.min_query_hit_fraction,
                    min_in_bounds_fraction=args.min_in_bounds_fraction,
                    min_hit_distance_fraction=args.min_hit_distance_fraction,
                    expected_levels=parsed_levels,
                )
                phase_report["phaseId"] = phase.get("phaseId") or phase_data.get("phaseId")
                phase_report["input"] = {"path": str(phase_input_path), "error": None}
                phase_reports.append(phase_report)
            phase_statuses = [str(report.get("status")) for report in phase_reports]
            if "FAIL" in phase_statuses:
                combined_status = "FAIL"
            elif "BLOCKED" in phase_statuses:
                combined_status = "BLOCKED"
            elif "OPEN" in phase_statuses:
                combined_status = "OPEN"
            else:
                combined_status = "PASS"
            report = {
                "schema": SCHEMA,
                "schemaVersion": SCHEMA,
                "script": SCRIPT_NAME,
                "status": combined_status,
                "productionPass": combined_status == "PASS",
                "phaseManifest": str(phase_path),
                "phases": phase_reports,
                "phaseSummary": {name.lower(): phase_statuses.count(name) for name in ("PASS", "OPEN", "BLOCKED", "FAIL")},
                "thresholds": {
                    "minQueryHitFraction": args.min_query_hit_fraction,
                    "minInBoundsFraction": args.min_in_bounds_fraction,
                    "minHitDistanceFraction": args.min_hit_distance_fraction,
                },
            }
            input_path = phase_path
            input_error = None
        except Exception as error:
            report = {
                "schema": SCHEMA,
                "schemaVersion": SCHEMA,
                "script": SCRIPT_NAME,
                "status": "BLOCKED",
                "productionPass": False,
                "phaseManifest": str(phase_path),
                "phases": [],
                "phaseSummary": {"pass": 0, "open": 0, "blocked": 1, "fail": 0},
            }
            input_path = phase_path
            input_error = repr(error)
    elif args.far_field_fixture:
        manifest = _far_field_fixture()
        input_path = None
        input_error = None
    elif input_path is None:
        manifest: Mapping[str, Any] = {}
        input_error = "missing input manifest"
    else:
        try:
            manifest = _read(input_path)
        except Exception as error:
            manifest = {}
            input_error = repr(error)

    expected_levels = None
    if args.expected_levels.strip():
        tokens = [token.strip() for token in args.expected_levels.split(",") if token.strip()]
        if not tokens or any(not token.lstrip("-").isdigit() for token in tokens):
            expected_levels = []
        else:
            expected_levels = [int(token) for token in tokens]
    if phase_path is None:
        report = evaluate(
            manifest,
            min_query_hit_fraction=args.min_query_hit_fraction,
            min_in_bounds_fraction=args.min_in_bounds_fraction,
            min_hit_distance_fraction=args.min_hit_distance_fraction,
            expected_levels=expected_levels,
        )
    report["input"] = {"path": str(input_path) if input_path else None, "error": input_error}
    if args.far_field_fixture:
        report["input"]["kind"] = "synthetic-far-field-fixture"
        report["fixture"] = manifest.get("farFieldFixture", {})
    if input_error:
        report["status"] = "BLOCKED"
        report["productionPass"] = False
        report["checks"].append(_record("input manifest", "BLOCKED", input_error))
        report["summary"]["blocked"] += 1
    output_path = Path(args.output).absolute()
    _write(output_path, report)
    print("C10_COVERAGE_GATE_STATUS", report["status"])
    print("C10_COVERAGE_GATE_WROTE", output_path)
    return {"PASS": 0, "OPEN": 3, "BLOCKED": 2, "FAIL": 1}.get(report["status"], 1)


if __name__ == "__main__":
    sys.exit(main())
