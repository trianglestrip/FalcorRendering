"""Strict offline gate for the C10 GPU Radiance Cache producer contract.

This gate consumes a JSON export from a Mogwai smoke run.  It intentionally
keeps three pieces of evidence independent:

* radiance RGB (which is allowed to be black for a black environment),
* producer validity (hit and sky bits are reported separately; interpolated
  bitmask fractions may overlap only with explicit mask evidence), and
* hit distance (finite, with an explicit miss sentinel).

The report also emits a separate ``coverageQuality`` section.  It distinguishes
typed real hits and legal black-sky records from an explicit coverage miss;
all-zero hit/sky validity remains ``unclassifiedNoHitSky`` until query
hit/miss readback or a per-channel ``coverageMissFraction`` is exported.  This
quality section is intentionally ``OPEN`` on the current bounded runtime and
does not weaken the producer contract into a residual-pixel heuristic.

The legacy ``run_radiance_cache.py`` report is a useful diagnostic artifact,
but its ``status=skeleton``/``summary=SKIP`` result is never a production
pass.  A producer can also be healthy while still being disconnected from the
final resolve; that state is reported as ``PARTIAL`` until
``finalResolveConnected`` is explicitly true.

Canonical input evidence (aliases are accepted only for migration) is::

    {
      "status": "production",
      "finalResolveConnected": true,
      "gpu": {
        "radianceCacheStatsAfterWarmup": {
          "gpuProducerEnabled": 1,
          "gpuInterpolationEnabled": 1,
          "requestCount": 8, "rayCount": 8, "traceCount": 8,
          "commitCount": 8, "readyCount": 8,
          "staleWriteRejects": 0, "readyNextFrame": 1,
          "frames": [
            {"frame": 10, "requestCount": 8, "rayCount": 8,
             "traceCount": 8, "commitCount": 8, "readyCount": 0,
             "staleWriteRejects": 0},
            {"frame": 11, "requestCount": 0, "rayCount": 0,
             "traceCount": 0, "commitCount": 0, "readyCount": 8,
             "staleWriteRejects": 0}
          ]
        },
        "series": [{
          "frame": 11,
          "radianceCache": {
            "radianceCache": {
              "finite": true, "nonnegative": true,
              "rgbMean": 0.0, "nonzeroRgbFraction": 0.0,
              "validity": {"hitFraction": 0.25, "skyFraction": 0.75},
              "hitDistance": {
                "finite": true, "nonMissFraction": 0.25,
                "missSentinel": 65504.0
              }
            },
            "radianceCacheHitDist": { ... same evidence ... }
          }
        }]
      }
    }

The script is dependency-free and does not start a GPU process.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Tuple


SCHEMA = "LumenGI.C10.RadianceCacheProducerGate.v1"
SCRIPT_NAME = "run_c10_producer_gate.py"
MISS_SENTINEL = 65504.0
MISS_SENTINEL_TOLERANCE = 1.0
REQUIRED_CHANNELS = ("radianceCache", "radianceCacheHitDist")
CLASSIFICATION_EPSILON = 1e-6

# A residual (1 - hit - sky) is deliberately *not* accepted as a coverage
# miss.  The validity mask is an interpolated provenance OR and its residual
# can include pixels that were never queried, invalid G-buffer pixels, or
# probes outside the current indirection footprint.  Only an explicit miss
# fraction or reconciled query counters can prove a coverage miss.
COVERAGE_MISS_ALIASES: Tuple[str, ...] = (
    "coverageMissFraction",
    "coverageMissRate",
    "coverageMissesFraction",
    "queryMissFraction",
    "unmappedFraction",
)
QUERY_COVERAGE_ALIASES: Dict[str, Tuple[str, ...]] = {
    "queryAttempts": ("queryAttempts", "coverageQueryAttempts"),
    "queryHits": ("queryHits", "coverageQueryHits"),
    "queryMisses": ("queryMisses", "coverageQueryMisses"),
}
PROBE_PROJECTION_ALIASES: Dict[str, Tuple[str, ...]] = {
    "projectedProbeCount": ("projectedProbeCount",),
    "inBoundsProbeCount": ("inBoundsProbeCount",),
}

STAT_ALIASES: Dict[str, Tuple[str, ...]] = {
    "requestCount": ("requestCount", "requestedCount", "requests"),
    # A separate ray counter is intentional.  Falling back to traceCount would
    # make an API that never reports ray work look complete.
    "rayCount": ("rayCount", "raysTraced", "rays", "rayQueries"),
    "traceCount": ("traceCount", "tracedCount", "traces"),
    "commitCount": ("commitCount", "committedCount", "commits"),
    "readyCount": ("readyCount", "readyProbes", "ready"),
    "staleWriteRejects": ("staleWriteRejects", "staleRejects"),
}


def _record(name: str, status: str, reason: str | None = None, **extra: Any) -> Dict[str, Any]:
    result: Dict[str, Any] = {"name": name, "status": status}
    if reason:
        result["reason"] = reason
    result.update(extra)
    return result


def _mapping(value: Any) -> Mapping[str, Any] | None:
    return value if isinstance(value, Mapping) else None


def _finite(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    result = float(value)
    return result if math.isfinite(result) else None


def _integer(value: Any) -> int | None:
    """Read integral JSON numbers without accepting booleans or strings."""

    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    if not math.isfinite(number) or number < 0.0 or number != math.floor(number):
        return None
    return int(number)


def _one(value: Any) -> bool:
    if value is True:
        return True
    number = _finite(value)
    return number is not None and number == 1.0


def _fraction(value: Any) -> float | None:
    number = _finite(value)
    return number if number is not None and 0.0 <= number <= 1.0 else None


def _first(mapping: Mapping[str, Any], names: Iterable[str]) -> Any:
    for name in names:
        if name in mapping:
            return mapping[name]
    return None


def _root_and_gpu(manifest: Mapping[str, Any]) -> Tuple[Mapping[str, Any], Mapping[str, Any]]:
    gpu = _mapping(manifest.get("gpu"))
    return manifest, gpu if gpu is not None else manifest


def _validate_schema(manifest: Mapping[str, Any]) -> Dict[str, Any]:
    value = manifest.get("schema", manifest.get("schemaVersion"))
    # Existing C10 reports have a different schema; the producer gate accepts
    # them as input so that it can explain the missing contract rather than
    # silently treating them as a pass.
    if value is None:
        return _record("schema", "BLOCKED", "missing schema")
    return _record("schema", "PASS" if value == SCHEMA else "BLOCKED", observed=value, expected=SCHEMA)


def _validate_legacy_status(manifest: Mapping[str, Any]) -> Dict[str, Any]:
    status = str(manifest.get("status", "")).strip().lower()
    summary = str(manifest.get("summary", "")).strip().lower()
    if status in {"skeleton", "skip", "skipped"} or summary in {"skip", "skeleton"}:
        return _record(
            "legacy skeleton/SKIP status",
            "BLOCKED",
            "legacy diagnostic status cannot be a production PASS",
            observedStatus=status or None,
            observedSummary=summary or None,
        )
    return _record("legacy skeleton/SKIP status", "PASS", observedStatus=status or None)


def _stats_object(gpu: Mapping[str, Any], manifest: Mapping[str, Any]) -> Mapping[str, Any] | None:
    for container in (gpu, manifest):
        for key in ("radianceCacheStatsAfterWarmup", "radianceCacheStats", "stats"):
            value = _mapping(container.get(key))
            if value is not None:
                return value
    return None


def _canonical_counter(stats: Mapping[str, Any], name: str) -> Tuple[int | None, str | None]:
    for key in STAT_ALIASES[name]:
        if key in stats:
            return _integer(stats[key]), key
    return None, None


def _validate_stats(gpu: Mapping[str, Any], manifest: Mapping[str, Any]) -> Tuple[Dict[str, Any], Mapping[str, Any] | None]:
    stats = _stats_object(gpu, manifest)
    if stats is None:
        return _record("radianceCacheStats binding", "BLOCKED", "missing radianceCacheStats"), None

    missing: List[str] = []
    invalid: List[str] = []
    values: Dict[str, int] = {}
    aliases: Dict[str, str] = {}
    for name in STAT_ALIASES:
        value, alias = _canonical_counter(stats, name)
        if alias is None:
            missing.append(name)
        elif value is None:
            invalid.append(f"{name} must be a non-negative integral number")
        else:
            values[name] = value
            aliases[name] = alias

    ready_next = _first(stats, ("readyNextFrame", "nextFrameReady"))
    if ready_next is None:
        missing.append("readyNextFrame")
    elif not _one(ready_next):
        invalid.append("readyNextFrame must equal 1")

    for flag in ("gpuProducerEnabled", "gpuInterpolationEnabled"):
        if flag not in stats:
            missing.append(flag)
        elif not _one(stats[flag]):
            invalid.append(f"{flag} must equal 1")

    if not missing and not invalid:
        for name in ("requestCount", "rayCount", "traceCount", "commitCount", "readyCount"):
            if values[name] <= 0:
                invalid.append(f"{name} must be > 0 in the aggregate run")
        if values["traceCount"] < values["requestCount"]:
            invalid.append("traceCount is below requestCount")
        if values["rayCount"] < values["requestCount"]:
            invalid.append("rayCount is below requestCount")
        if values["commitCount"] < values["requestCount"]:
            invalid.append("commitCount is below requestCount")
        if values["readyCount"] < values["requestCount"]:
            invalid.append("readyCount is below requestCount")

    status = "BLOCKED" if missing else "FAIL" if invalid else "PASS"
    return (
        _record(
            "radianceCacheStats binding",
            status,
            "; ".join(invalid) if invalid else ("missing required producer counters" if missing else None),
            missing=missing,
            invalid=invalid,
            counters=values,
            aliases=aliases,
        ),
        stats,
    )


def _frame_records(stats: Mapping[str, Any]) -> Sequence[Any] | None:
    if not isinstance(stats, Mapping):
        return None
    for key in ("frames", "frameSamples", "perFrame"):
        value = stats.get(key)
        if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
            return value
    return None


def _frame_counter(frame: Mapping[str, Any], name: str) -> int | None:
    value, _ = _canonical_counter(frame, name)
    return value


def _validate_frame_pair(stats: Mapping[str, Any] | None) -> Dict[str, Any]:
    if stats is None:
        return _record("frame N request -> frame N+1 ready", "BLOCKED", "stats unavailable")
    frames = _frame_records(stats)
    if not frames:
        return _record("frame N request -> frame N+1 ready", "BLOCKED", "missing per-frame producer telemetry")

    parsed: List[Dict[str, int]] = []
    missing: List[str] = []
    invalid: List[str] = []
    for index, raw in enumerate(frames):
        frame = _mapping(raw)
        if frame is None:
            invalid.append(f"frames[{index}] is not an object")
            continue
        frame_index = _integer(frame.get("frame", frame.get("frameIndex")))
        if frame_index is None:
            missing.append(f"frames[{index}].frame")
            continue
        item = {"frame": frame_index}
        for name in STAT_ALIASES:
            value = _frame_counter(frame, name)
            if value is None:
                missing.append(f"frames[{index}].{name}")
            else:
                item[name] = value
        if len(item) == len(STAT_ALIASES) + 1:
            parsed.append(item)

    by_frame = {item["frame"]: item for item in parsed}
    pair: Dict[str, int] | None = None
    for item in parsed:
        next_item = by_frame.get(item["frame"] + 1)
        if next_item is None or item["requestCount"] <= 0:
            continue
        if next_item["readyCount"] >= item["requestCount"]:
            pair = {
                "requestFrame": item["frame"],
                "readyFrame": next_item["frame"],
                "requested": item["requestCount"],
                "rays": item["rayCount"],
                "traced": item["traceCount"],
                "committed": item["commitCount"],
                "ready": next_item["readyCount"],
            }
            break

    if missing:
        return _record("frame N request -> frame N+1 ready", "BLOCKED", "missing per-frame counters", missing=missing)
    if invalid:
        return _record("frame N request -> frame N+1 ready", "FAIL", "; ".join(invalid))
    if pair is None:
        return _record("frame N request -> frame N+1 ready", "FAIL", "no consecutive request/ready pair")
    return _record("frame N request -> frame N+1 ready", "PASS", pair=pair)


def _series(gpu: Mapping[str, Any], manifest: Mapping[str, Any]) -> Sequence[Any] | None:
    for container in (gpu, manifest):
        value = container.get("series", container.get("captures"))
        if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
            return value
    return None


def _channel_records(series: Sequence[Any], channel: str) -> List[Mapping[str, Any]]:
    records: List[Mapping[str, Any]] = []
    for raw in series:
        frame = _mapping(raw)
        if frame is None:
            continue
        cache_block = _mapping(frame.get("radianceCache"))
        if cache_block is None:
            cache_block = frame
        descriptor = _mapping(cache_block.get(channel))
        if descriptor is None:
            descriptor = _mapping(frame.get(channel))
        if descriptor is not None:
            # Keep the frame identity alongside the descriptor so the strict
            # classification report can point back to the exact runtime sample.
            record = dict(descriptor)
            # Interpolation exports a per-pixel provenance bitmask separately
            # from each channel.  Bits 0/1 are hit/sky and are OR-ed across
            # taps, so their fractions may overlap.  Carry the explicit mask
            # evidence into the channel descriptor; do not infer overlap from
            # RGB or from a residual fraction.
            mask_metrics = _mapping(cache_block.get("radianceCacheValidity"))
            if mask_metrics is None:
                mask_metrics = _mapping(frame.get("radianceCacheValidity"))
            validity = _mapping(record.get("validity"))
            if mask_metrics is not None and validity is not None:
                mask_max = _integer(mask_metrics.get("maskMax", mask_metrics.get("max")))
                if mask_max is not None:
                    validity_copy = dict(validity)
                    validity_copy.setdefault("maskMax", mask_max)
                    validity_copy.setdefault("encoding", "bitmask")
                    record["validity"] = validity_copy
            record["_frame"] = frame.get("frame", frame.get("frameIndex"))
            records.append(record)
    return records


def _validity_fraction(validity: Mapping[str, Any], names: Tuple[str, ...]) -> Tuple[float | None, str | None]:
    for name in names:
        if name in validity:
            return _fraction(validity[name]), name
    return None, None


def _explicit_coverage_miss(descriptor: Mapping[str, Any], validity: Mapping[str, Any]) -> Tuple[float | None, str | None]:
    """Read an explicit coverage-miss fraction without residual inference."""

    for container in (validity, descriptor):
        value, key = _validity_fraction(container, COVERAGE_MISS_ALIASES)
        if key is not None:
            return value, key
    return None, None


def _classify_descriptor(descriptor: Mapping[str, Any], index: int) -> Dict[str, Any]:
    """Classify one output summary using typed provenance, never RGB alone.

    ``legalBlackSky`` is a positive sky-bit observation with black RGB.  A
    ``realHit`` requires hit provenance and a matching non-miss distance.  A
    ``coverageMiss`` is reported only when the producer exports an explicit
    miss fraction; an all-zero validity mask otherwise remains
    ``unclassifiedNoHitSky`` rather than being relabeled as a miss.
    """

    validity = _mapping(descriptor.get("validity"))
    if validity is None:
        validity = descriptor
    hit, hit_key = _validity_fraction(validity, ("hitFraction", "hitValidFraction", "validHitFraction"))
    sky, sky_key = _validity_fraction(validity, ("skyFraction", "skyValidFraction", "validSkyFraction"))
    distance = _mapping(descriptor.get("hitDistance")) or {}
    nonmiss, nonmiss_key = _validity_fraction(
        distance, ("nonMissFraction", "hitDistanceValidFraction", "validFraction")
    )
    miss, miss_key = _explicit_coverage_miss(descriptor, validity)
    rgb_mean = _finite(descriptor.get("rgbMean"))
    black_rgb = rgb_mean is not None and abs(rgb_mean) <= CLASSIFICATION_EPSILON
    real_hit = (
        hit is not None
        and hit > CLASSIFICATION_EPSILON
        and nonmiss is not None
        and nonmiss + CLASSIFICATION_EPSILON >= hit
    )
    legal_black_sky = (
        sky is not None
        and sky > CLASSIFICATION_EPSILON
        and black_rgb
    )
    explicit_coverage_miss = miss is not None and miss > CLASSIFICATION_EPSILON
    no_hit_or_sky = (
        hit is not None
        and sky is not None
        and hit <= CLASSIFICATION_EPSILON
        and sky <= CLASSIFICATION_EPSILON
    )
    if real_hit:
        dominant = "realHit"
    elif legal_black_sky:
        dominant = "legalBlackSky"
    elif explicit_coverage_miss:
        dominant = "coverageMiss"
    elif no_hit_or_sky:
        dominant = "unclassifiedNoHitSky"
    elif sky is not None and sky > CLASSIFICATION_EPSILON:
        dominant = "skyRadiance"
    else:
        dominant = "unclassified"
    return {
        "sample": index,
        "frame": descriptor.get("_frame"),
        "dominant": dominant,
        "realHit": real_hit,
        "legalBlackSky": legal_black_sky,
        "coverageMiss": explicit_coverage_miss,
        "unclassifiedNoHitSky": no_hit_or_sky and not explicit_coverage_miss,
        "blackRgb": black_rgb,
        "hitFraction": hit,
        "skyFraction": sky,
        "nonMissFraction": nonmiss,
        "coverageMissFraction": miss,
        "sourceKeys": {
            "hit": hit_key,
            "sky": sky_key,
            "nonMiss": nonmiss_key,
            "coverageMiss": miss_key,
        },
    }


def _nonnegative_stat(mapping: Mapping[str, Any] | None, aliases: Tuple[str, ...]) -> Tuple[float | None, str | None]:
    if not isinstance(mapping, Mapping):
        return None, None
    for key in aliases:
        if key not in mapping:
            continue
        value = _finite(mapping[key])
        if value is not None and value >= 0.0:
            return value, key
        return None, key
    return None, None


def _coverage_quality(
    stats: Mapping[str, Any] | None,
    channel_checks: Sequence[Mapping[str, Any]],
) -> Dict[str, Any]:
    """Report coverage evidence separately from the bounded producer PASS.

    Existing C10 runtime stats count dispatched interpolation attempts but do
    not read back per-pixel query hit/miss totals.  In that state the result is
    ``OPEN`` with actionable next fields; it must not claim that every
    residual pixel is a coverage miss.
    """

    stats = stats if isinstance(stats, Mapping) else {}
    query_values: Dict[str, float | None] = {}
    query_sources: Dict[str, str | None] = {}
    for name, aliases in QUERY_COVERAGE_ALIASES.items():
        value, source = _nonnegative_stat(stats, aliases)
        query_values[name] = value
        query_sources[name] = source
    attempts = query_values["queryAttempts"]
    hits = query_values["queryHits"]
    misses = query_values["queryMisses"]
    query_complete = (
        attempts is not None
        and hits is not None
        and misses is not None
        and attempts > 0.0
        and abs((hits + misses) - attempts) <= 1e-5 * max(1.0, attempts)
    )
    query_miss_fraction = misses / attempts if query_complete else None

    projection: Dict[str, Any] = {}
    for name, aliases in PROBE_PROJECTION_ALIASES.items():
        value, source = _nonnegative_stat(stats, aliases)
        projection[name] = value
        projection[name + "SourceKey"] = source
    projected = projection.get("projectedProbeCount")
    in_bounds = projection.get("inBoundsProbeCount")
    projection["inBoundsFraction"] = (
        in_bounds / projected
        if projected is not None and projected > 0.0 and in_bounds is not None and in_bounds <= projected
        else None
    )

    explicit_miss_samples = 0
    unclassified_samples = 0
    classification_counts: Dict[str, int] = {
        "realHit": 0,
        "legalBlackSky": 0,
        "coverageMiss": 0,
        "unclassifiedNoHitSky": 0,
    }
    per_channel: Dict[str, Any] = {}
    explicit_miss_channels = 0
    for check in channel_checks:
        channel = str(check.get("name", ""))
        if not channel.startswith("channel:") or channel.endswith(" availability"):
            continue
        records = check.get("classification")
        if not isinstance(records, Mapping):
            continue
        per_channel[channel] = records
        channel_miss_samples = int(records.get("coverageMissSamples", 0) or 0)
        explicit_miss_samples += channel_miss_samples
        if channel_miss_samples > 0:
            explicit_miss_channels += 1
        unclassified_samples += int(records.get("unclassifiedNoHitSkySamples", 0) or 0)
        for key in classification_counts:
            classification_counts[key] += int(records.get(key + "Samples", 0) or 0)

    if query_complete:
        status = "PASS"
        reason = "query hit/miss counters reconcile with queryAttempts"
        next_fields: List[str] = []
    elif explicit_miss_samples > 0 and explicit_miss_channels >= len(REQUIRED_CHANNELS):
        status = "PASS"
        reason = "explicit coverageMissFraction evidence is present for every required channel"
        next_fields = []
    elif explicit_miss_samples > 0:
        status = "OPEN"
        reason = "explicit coverageMissFraction evidence is incomplete across required channels"
        next_fields = ["coverageMissFraction for every required channel/sample"]
    elif attempts is None or hits is None or misses is None:
        status = "OPEN"
        reason = "coverage miss fraction and/or query hit/miss readback is missing"
        next_fields = [
            "coverageMissFraction (per channel/sample)",
            "queryAttempts/queryHits/queryMisses readback from the same frame domain",
        ]
    else:
        status = "OPEN"
        reason = "queryAttempts is present but query hit/miss counters do not reconcile; likely unbound readback"
        next_fields = [
            "queryHits/queryMisses GPU readback fence",
            "coverageMissFraction (per channel/sample)",
        ]
    return {
        "status": status,
        "reason": reason,
        "queryCounters": {
            **query_values,
            "sourceKeys": query_sources,
            "reconciled": query_complete,
            "coverageMissFraction": query_miss_fraction,
        },
        "probeProjection": projection,
        "classificationCounts": classification_counts,
        "perChannel": per_channel,
        "explicitCoverageMissSamples": explicit_miss_samples,
        "explicitCoverageMissChannels": explicit_miss_channels,
        "unclassifiedNoHitSkySamples": unclassified_samples,
        "residualInference": False,
        "nextFields": next_fields,
    }


def _validate_channel(channel: str, descriptors: Sequence[Mapping[str, Any]]) -> Dict[str, Any]:
    if not descriptors:
        return _record(f"channel:{channel}", "BLOCKED", "missing channel samples")

    missing: List[str] = []
    invalid: List[str] = []
    observed = 0
    black_rgb_samples = 0
    evidence_frames = 0
    classifications: List[Dict[str, Any]] = []
    for index, descriptor in enumerate(descriptors):
        prefix = f"sample[{index}]"
        classification = _classify_descriptor(descriptor, index)
        classifications.append(classification)
        for key in ("finite", "nonnegative"):
            if key not in descriptor:
                missing.append(f"{prefix}.{key}")
            elif descriptor[key] is not True:
                invalid.append(f"{prefix}.{key} must be true")
        sample_count = _integer(descriptor.get("sampleCount"))
        if sample_count is None:
            missing.append(f"{prefix}.sampleCount")
        elif sample_count <= 0:
            invalid.append(f"{prefix}.sampleCount must be > 0")

        rgb_mean = _finite(descriptor.get("rgbMean"))
        rgb_max = _finite(descriptor.get("rgbMax"))
        if rgb_mean is not None and rgb_mean == 0.0:
            black_rgb_samples += 1
        if rgb_mean is not None and rgb_mean < 0.0:
            invalid.append(f"{prefix}.rgbMean must be non-negative")
        if rgb_max is not None and rgb_max < 0.0:
            invalid.append(f"{prefix}.rgbMax must be non-negative")

        validity = _mapping(descriptor.get("validity"))
        if validity is None:
            # Permit a flat migration form, but do not infer validity from RGB
            # or alpha.  This is the important distinction from the skeleton
            # report, which has only nonzeroRgbFraction/alphaMean.
            validity = descriptor
        hit, hit_key = _validity_fraction(validity, ("hitFraction", "hitValidFraction", "validHitFraction"))
        sky, sky_key = _validity_fraction(validity, ("skyFraction", "skyValidFraction", "validSkyFraction"))
        if hit_key is None:
            missing.append(f"{prefix}.validity.hitFraction")
        elif hit is None:
            invalid.append(f"{prefix}.{hit_key} must be a fraction in [0,1]")
        if sky_key is None:
            missing.append(f"{prefix}.validity.skyFraction")
        elif sky is None:
            invalid.append(f"{prefix}.{sky_key} must be a fraction in [0,1]")
        if hit is not None and sky is not None:
            mask_max = _integer(validity.get("maskMax"))
            mask_encoding = validity.get("encoding")
            bitmask_overlap = mask_encoding == "bitmask" and mask_max is not None and mask_max >= 3
            if hit + sky > 1.0 + 1e-5 and not bitmask_overlap:
                invalid.append(f"{prefix}.hitFraction + skyFraction exceeds 1")
            if hit + sky > 0.0:
                observed += 1

        distance = _mapping(descriptor.get("hitDistance"))
        if distance is None:
            missing.append(f"{prefix}.hitDistance")
            continue
        if distance.get("finite") is not True:
            if "finite" not in distance:
                missing.append(f"{prefix}.hitDistance.finite")
            else:
                invalid.append(f"{prefix}.hitDistance.finite must be true")
        nonmiss, nonmiss_key = _validity_fraction(
            distance, ("nonMissFraction", "hitDistanceValidFraction", "validFraction")
        )
        if nonmiss_key is None:
            missing.append(f"{prefix}.hitDistance.nonMissFraction")
        elif nonmiss is None:
            invalid.append(f"{prefix}.{nonmiss_key} must be a fraction in [0,1]")
        sentinel = _finite(distance.get("missSentinel"))
        if sentinel is None:
            missing.append(f"{prefix}.hitDistance.missSentinel")
        elif abs(sentinel - MISS_SENTINEL) > MISS_SENTINEL_TOLERANCE:
            invalid.append(f"{prefix}.hitDistance.missSentinel must be {MISS_SENTINEL:g}")
        if nonmiss is not None and hit is not None and nonmiss + 1e-5 < hit:
            invalid.append(f"{prefix}.hitDistance.nonMissFraction is below hitFraction")
        if hit is not None and sky is not None and (hit + sky > 0.0):
            evidence_frames += 1

    if not descriptors:
        missing.append("samples")
    explicit_coverage_miss_count = sum(
        1 for item in classifications if item.get("coverageMiss")
    )
    if observed == 0 and explicit_coverage_miss_count == 0 and not missing:
        invalid.append("no hit or sky validity was observed")

    status = "BLOCKED" if missing else "FAIL" if invalid else "PASS"
    classification_counts = {
        "realHitSamples": sum(1 for item in classifications if item.get("realHit")),
        "legalBlackSkySamples": sum(1 for item in classifications if item.get("legalBlackSky")),
        "coverageMissSamples": sum(1 for item in classifications if item.get("coverageMiss")),
        "unclassifiedNoHitSkySamples": sum(1 for item in classifications if item.get("unclassifiedNoHitSky")),
    }
    return _record(
        f"channel:{channel}",
        status,
        "; ".join(invalid) if invalid else ("missing explicit validity/hit-distance evidence" if missing else None),
        samples=len(descriptors),
        evidenceFrames=evidence_frames,
        blackRgbSamples=black_rgb_samples,
        rgbNonzeroNotUsedForValidity=True,
        missing=missing,
        invalid=invalid,
        classification={
            **classification_counts,
            "samples": classifications,
            "strictCoverageMissEvidence": classification_counts["coverageMissSamples"] > 0,
        },
    )


def _derive_frame_stats(gpu: Mapping[str, Any]) -> Mapping[str, Any] | None:
    """Normalize runner series into the strict per-frame producer contract."""
    series = gpu.get("series")
    if not isinstance(series, Sequence) or isinstance(series, (str, bytes)):
        return None
    frames: List[Dict[str, Any]] = []
    for raw in series:
        record = _mapping(raw)
        if record is None:
            continue
        stats = _mapping(record.get("radianceCacheStats"))
        if stats is None:
            continue
        item = dict(stats)
        item["frame"] = record.get("frame", stats.get("frameIndex"))
        frames.append(item)
    return {"frames": frames} if frames else None


def _validate_channels(gpu: Mapping[str, Any], manifest: Mapping[str, Any]) -> List[Dict[str, Any]]:
    available: Any = gpu.get("channels_available", manifest.get("channels_available"))
    checks: List[Dict[str, Any]] = []
    if not isinstance(available, Sequence) or isinstance(available, (str, bytes)):
        available = []
    for channel in REQUIRED_CHANNELS:
        if channel not in available:
            checks.append(_record(f"channel:{channel} availability", "BLOCKED", "channel not listed as available"))
            continue
        checks.append(_validate_channel(channel, _channel_records(_series(gpu, manifest) or [], channel)))
    if _series(gpu, manifest) is None:
        checks.append(_record("C10 output series", "BLOCKED", "missing per-frame output series"))
    return checks


def _validate_final_resolve(manifest: Mapping[str, Any], gpu: Mapping[str, Any]) -> Dict[str, Any]:
    value: Any = None
    found = False
    for container in (manifest, gpu):
        if "finalResolveConnected" in container:
            value = container["finalResolveConnected"]
            found = True
            break
    if not found:
        return _record("final resolve connected", "BLOCKED", "missing explicit finalResolveConnected evidence")
    if not isinstance(value, bool):
        return _record("final resolve connected", "FAIL", "finalResolveConnected must be boolean", observed=value)
    if not value:
        return _record(
            "final resolve connected",
            "PARTIAL",
            "producer/interpolation is not connected to the final resolve",
            observed=False,
        )
    return _record("final resolve connected", "PASS", observed=True)


def evaluate(manifest: Mapping[str, Any]) -> Dict[str, Any]:
    if not isinstance(manifest, Mapping):
        manifest = {}
    root, gpu = _root_and_gpu(manifest)
    checks: List[Dict[str, Any]] = [_validate_schema(root), _validate_legacy_status(root)]
    stats_check, stats = _validate_stats(gpu, root)
    checks.append(stats_check)
    frame_stats = stats if _frame_records(stats) else _derive_frame_stats(gpu)
    checks.append(_validate_frame_pair(frame_stats))
    checks.extend(_validate_channels(gpu, root))
    checks.append(_validate_final_resolve(root, gpu))
    coverage_quality = _coverage_quality(stats, checks)

    statuses = [str(check.get("status")) for check in checks]
    if "FAIL" in statuses:
        status = "FAIL"
    elif "BLOCKED" in statuses:
        status = "BLOCKED"
    elif "PARTIAL" in statuses:
        status = "PARTIAL"
    else:
        status = "PASS"
    return {
        "schema": SCHEMA,
        "schemaVersion": SCHEMA,
        "script": SCRIPT_NAME,
        "status": status,
        "productionPass": status == "PASS",
        "contract": {
            "requiredChannels": list(REQUIRED_CHANNELS),
            "requiredStats": [
                "requestCount", "rayCount", "traceCount", "commitCount",
                "readyCount", "staleWriteRejects", "readyNextFrame",
            ],
            "validity": "hitFraction and skyFraction are independent of RGB nonzero coverage",
            "hitDistance": {"missSentinel": MISS_SENTINEL, "requiresNonMissFraction": True},
            "classification": {
                "legalBlackSky": "skyFraction>0 and black RGB; never inferred from RGB alone",
                "realHit": "hitFraction>0 and nonMissFraction>=hitFraction",
                "coverageMiss": "explicit coverageMissFraction or reconciled queryHits/queryMisses only",
                "allZeroValidity": "unclassifiedNoHitSky until explicit miss telemetry is bound",
            },
            "coverageResidualInference": False,
            "legacySkeletonOrSkip": "BLOCKED",
            "finalResolveConnectedFalse": "PARTIAL",
        },
        "checks": checks,
        # Coverage quality is intentionally separate from the bounded producer
        # contract above.  A producer can have typed hit/sky records while the
        # GPU query hit/miss readback is still unavailable; that state is OPEN,
        # not a false PASS and not a reason to discard a legal black sky sample.
        "coverageQuality": coverage_quality,
        "summary": {name.lower(): statuses.count(name) for name in ("PASS", "PARTIAL", "BLOCKED", "FAIL")},
    }


def _descriptor(
    rgb_mean: float = 0.0,
    hit_fraction: float = 0.25,
    sky_fraction: float = 0.75,
    coverage_miss_fraction: float | None = None,
) -> Dict[str, Any]:
    validity: Dict[str, Any] = {
        "hitFraction": hit_fraction,
        "skyFraction": sky_fraction,
    }
    if coverage_miss_fraction is not None:
        validity["coverageMissFraction"] = coverage_miss_fraction
    return {
        "finite": True,
        "nonnegative": True,
        "sampleCount": 4,
        "rgbMean": rgb_mean,
        "rgbMax": rgb_mean,
        "nonzeroRgbFraction": 0.0,
        "validity": validity,
        "hitDistance": {"finite": True, "nonMissFraction": hit_fraction, "missSentinel": MISS_SENTINEL},
    }


def _pass_fixture(final_resolve: bool = True) -> Dict[str, Any]:
    frame_a = {"frame": 10, "requestCount": 2, "rayCount": 2, "traceCount": 2, "commitCount": 2, "readyCount": 0, "staleWriteRejects": 0}
    frame_b = {"frame": 11, "requestCount": 0, "rayCount": 0, "traceCount": 0, "commitCount": 0, "readyCount": 2, "staleWriteRejects": 0}
    stats = {
        "gpuProducerEnabled": 1,
        "gpuInterpolationEnabled": 1,
        "requestCount": 2,
        "rayCount": 2,
        "traceCount": 2,
        "commitCount": 2,
        "readyCount": 2,
        "staleWriteRejects": 0,
        "readyNextFrame": 1,
        "frames": [frame_a, frame_b],
    }
    descriptor = {"radianceCache": _descriptor(), "radianceCacheHitDist": _descriptor()}
    return {
        "schema": SCHEMA,
        "status": "production",
        "summary": "PASS",
        "finalResolveConnected": final_resolve,
        "gpu": {
            "channels_available": list(REQUIRED_CHANNELS),
            "radianceCacheStatsAfterWarmup": stats,
            "series": [{"frame": 11, "radianceCache": descriptor}],
        },
    }


def _blocked_fixture() -> Dict[str, Any]:
    return {
        "schema": SCHEMA,
        "status": "skeleton",
        "summary": "SKIP",
        "gpu": {
            "channels_available": list(REQUIRED_CHANNELS),
            "series": [{"frame": 1, "radianceCache": {"radianceCache": {"finite": True, "nonnegative": True}}}],
        },
    }


def _set_fixture_descriptors(
    fixture: Dict[str, Any],
    *,
    hit_fraction: float,
    sky_fraction: float,
    coverage_miss_fraction: float | None = None,
) -> None:
    """Replace both channel summaries in a self-test fixture."""

    series = fixture["gpu"]["series"]
    descriptor = {
        "radianceCache": _descriptor(
            hit_fraction=hit_fraction,
            sky_fraction=sky_fraction,
            coverage_miss_fraction=coverage_miss_fraction,
        ),
        "radianceCacheHitDist": _descriptor(
            hit_fraction=hit_fraction,
            sky_fraction=sky_fraction,
            coverage_miss_fraction=coverage_miss_fraction,
        ),
    }
    series[0]["radianceCache"] = descriptor


def _run_self_test() -> int:
    passed = evaluate(_pass_fixture(True))
    partial = evaluate(_pass_fixture(False))
    blocked = evaluate(_blocked_fixture())
    black_sky_fixture = _pass_fixture(True)
    _set_fixture_descriptors(black_sky_fixture, hit_fraction=0.0, sky_fraction=1.0)
    black_sky = evaluate(black_sky_fixture)
    explicit_miss_fixture = _pass_fixture(True)
    _set_fixture_descriptors(
        explicit_miss_fixture,
        hit_fraction=0.0,
        sky_fraction=0.0,
        coverage_miss_fraction=1.0,
    )
    explicit_miss = evaluate(explicit_miss_fixture)
    zero_validity_fixture = _pass_fixture(True)
    _set_fixture_descriptors(zero_validity_fixture, hit_fraction=0.0, sky_fraction=0.0)
    zero_validity = evaluate(zero_validity_fixture)
    bitmask_fixture = _pass_fixture(True)
    _set_fixture_descriptors(bitmask_fixture, hit_fraction=0.75, sky_fraction=0.75)
    bitmask_fixture["gpu"]["series"][0]["radianceCache"]["radianceCacheValidity"] = {
        "maskMax": 15,
        "hitFraction": 0.75,
        "skyFraction": 0.75,
        "validFraction": 0.9,
    }
    bitmask = evaluate(bitmask_fixture)
    black_channel = next(
        check for check in black_sky["checks"] if check.get("name") == "channel:radianceCache"
    )
    miss_channel = next(
        check for check in explicit_miss["checks"] if check.get("name") == "channel:radianceCache"
    )
    zero_channel = next(
        check for check in zero_validity["checks"] if check.get("name") == "channel:radianceCache"
    )
    ok = (
        passed["status"] == "PASS"
        and partial["status"] == "PARTIAL"
        and blocked["status"] == "BLOCKED"
        and all(check["status"] != "SKIP" for check in blocked["checks"])
        and black_sky["coverageQuality"]["status"] == "OPEN"
        and black_channel["classification"]["legalBlackSkySamples"] == 1
        and black_channel["classification"]["realHitSamples"] == 0
        and explicit_miss["coverageQuality"]["status"] == "PASS"
        and miss_channel["status"] == "PASS"
        and miss_channel["classification"]["coverageMissSamples"] == 1
        and zero_validity["coverageQuality"]["status"] == "OPEN"
        and zero_channel["classification"]["unclassifiedNoHitSkySamples"] == 1
        and bitmask["status"] == "PASS"
    )
    print("C10_PRODUCER_GATE_SELF_TEST_PASS", passed["status"])
    print("C10_PRODUCER_GATE_SELF_TEST_PARTIAL", partial["status"])
    print("C10_PRODUCER_GATE_SELF_TEST_BLOCKED", blocked["status"])
    print("C10_PRODUCER_GATE_SELF_TEST_BLACK_SKY", black_sky["coverageQuality"]["status"])
    print("C10_PRODUCER_GATE_SELF_TEST_COVERAGE_MISS", explicit_miss["coverageQuality"]["status"])
    print("C10_PRODUCER_GATE_SELF_TEST_ZERO_VALIDITY", zero_validity["coverageQuality"]["status"])
    print("C10_PRODUCER_GATE_SELF_TEST_BITMASK", bitmask["status"])
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
    parser.add_argument("--input", default=os.environ.get("LUMEN_C10_PRODUCER_GATE_INPUT", ""))
    parser.add_argument(
        "--output",
        default=os.environ.get(
            "LUMEN_C10_PRODUCER_GATE_OUT",
            "artifacts/lumengi/C10/c10-producer-gate.json",
        ),
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return _run_self_test()

    input_path = Path(args.input).absolute() if args.input else None
    input_error: str | None = None
    if input_path is None:
        manifest: Mapping[str, Any] = {}
        input_error = "missing input manifest"
    else:
        try:
            manifest = _read(input_path)
        except Exception as error:  # malformed runtime evidence is BLOCKED.
            manifest = {}
            input_error = repr(error)

    report = evaluate(manifest)
    report["input"] = {"path": str(input_path) if input_path else None, "error": input_error}
    if input_error:
        report["status"] = "BLOCKED"
        report["productionPass"] = False
        report["checks"].append(_record("input manifest", "BLOCKED", input_error))
        report["summary"]["blocked"] += 1
    output_path = Path(args.output).absolute()
    _write(output_path, report)
    print("C10_PRODUCER_GATE_STATUS", report["status"])
    print("C10_PRODUCER_GATE_WROTE", output_path)
    return {"PASS": 0, "PARTIAL": 3, "BLOCKED": 2, "FAIL": 1}.get(report["status"], 1)


if __name__ == "__main__":
    sys.exit(main())
