"""Strict offline gate for the C5 Surface-Cache card-grid lookup.

The gate compares two manifests captured from the same scene and resolution:
one with ``useCacheCardGrid=false`` (the full-card scan control) and one with
``useCacheCardGrid=true``.  It is intentionally offline: it never starts
Mogwai, changes a graph, or invents missing telemetry.

The result has three terminal states:

``PASS``
    All required evidence is present, the grid did not lose cache hits, and
    the two runs agree on candidate-index telemetry.  A high coverage-reject
    rate is reported separately as ``OPEN``; it is not silently converted into
    a pass or a failure.
``FAIL``
    Evidence is present but the grid loses hits, changes candidate coverage,
    has invalid values, or changes lookup-attempt cardinality beyond the
    bounded tolerance.
``BLOCKED``
    A required identity, output, counter, or candidate-index field is absent.

Examples::

    python tests/lumengi/run_c5_grid_equivalence.py \
        --fullscan artifacts/lumengi/C5/fullscan/manifest.json \
        --grid artifacts/lumengi/C5/grid/manifest.json \
        --output artifacts/lumengi/C5/grid-equivalence.json

    python tests/lumengi/run_c5_grid_equivalence.py --self-test
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Tuple


GATE_SCHEMA = "LumenGI.C5GridEquivalence.v2"
SCRIPT_NAME = "run_c5_grid_equivalence.py"
ATTEMPT_RELATIVE_TOLERANCE = 0.05
HIGH_COVERAGE_REJECT_RATIO = 0.50
DEFAULT_OUTPUT = Path(
    os.environ.get(
        "LUMEN_C5_GRID_EQUIVALENCE_OUT",
        "artifacts/lumengi/C5/grid-equivalence/grid-equivalence.json",
    )
).absolute()

# These are deliberately host-facing fields.  The first four are the
# completeness proof; the last two make the candidate-list density and the
# zero-page publication boundary observable.
CANDIDATE_FIELDS = (
    "cardGridCandidateCount",
    "cardGridOverflowCells",
    "cardGridCardCount",
    "cardGridIndexedCards",
    "cardGridMissingCards",
)
CACHE_FIELDS = (
    "cacheLookupAttempts",
    "cacheLookupHits",
    "cacheCoverageRejects",
    "cachePageRejects",
    "cacheMetadataRejects",
    "cacheVisibilityRejects",
)
OUTPUT_REQUIRED_FLAGS = ("finite", "nonnegative")


def _record(name: str, status: str, reason: str | None = None, **extra: Any) -> Dict[str, Any]:
    result: Dict[str, Any] = {"name": name, "status": status}
    if reason:
        result["reason"] = reason
    result.update(extra)
    return result


def _number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    value = float(value)
    return value if math.isfinite(value) else None


def _nonnegative(value: Any) -> float | None:
    value = _number(value)
    return value if value is not None and value >= 0.0 else None


def _flatten(value: Any) -> Iterable[Any]:
    if isinstance(value, (list, tuple)):
        for item in value:
            yield from _flatten(item)
    else:
        yield value


def _mapping_at(value: Any, *keys: str) -> Mapping[str, Any] | None:
    if not isinstance(value, Mapping):
        return None
    for key in keys:
        candidate = value.get(key)
        if isinstance(candidate, Mapping):
            return candidate
    return None


def _last_mapping(value: Any, key: str) -> Mapping[str, Any] | None:
    """Find the latest frame's mapping without recursively guessing fields."""
    if not isinstance(value, Mapping):
        return None
    direct = value.get(key)
    if isinstance(direct, Mapping):
        return direct
    frames = value.get("frames")
    if isinstance(frames, Sequence) and not isinstance(frames, (str, bytes)):
        for frame in reversed(frames):
            if isinstance(frame, Mapping) and isinstance(frame.get(key), Mapping):
                return frame[key]
    return None


def _scene(manifest: Mapping[str, Any]) -> Any:
    configuration = manifest.get("configuration")
    if "scene" in manifest:
        return manifest["scene"]
    if isinstance(configuration, Mapping) and "scene" in configuration:
        return configuration["scene"]
    if isinstance(configuration, Mapping):
        properties = configuration.get("pass_properties")
        if isinstance(properties, Mapping) and "scene" in properties:
            return properties["scene"]
    return None


def _resolution(manifest: Mapping[str, Any]) -> Tuple[int, int] | None:
    candidates: List[Any] = [manifest.get("resolution")]
    configuration = manifest.get("configuration")
    if isinstance(configuration, Mapping):
        candidates.append(configuration.get("resolution"))
        if "width" in configuration and "height" in configuration:
            candidates.append([configuration["width"], configuration["height"]])
    for candidate in candidates:
        if isinstance(candidate, Mapping):
            candidate = [candidate.get("width"), candidate.get("height")]
        if isinstance(candidate, (list, tuple)) and len(candidate) == 2:
            width, height = candidate
            if (
                isinstance(width, int)
                and not isinstance(width, bool)
                and isinstance(height, int)
                and not isinstance(height, bool)
                and width > 0
                and height > 0
            ):
                return int(width), int(height)
    return None


def _schema(manifest: Mapping[str, Any]) -> Any:
    for key in ("schema", "schemaVersion", "schema_version"):
        if key in manifest:
            return manifest[key]
    # The C6 surface-cache A/B runner stores the immutable telemetry schema
    # beside ``cases`` rather than repeating it at the report root.  Treat it
    # as capture-format identity, not as an inferred route.
    for key in ("c6FrozenTelemetry", "pageTelemetrySchema"):
        value = manifest.get(key)
        if isinstance(value, Mapping):
            for schema_key in ("schema", "schemaVersion", "schema_version"):
                if schema_key in value:
                    return value[schema_key]
    return None


def _identity_value(kind: str, value: Any) -> Any:
    if kind == "scene" and isinstance(value, str):
        # Scene path spelling is not semantic; slash/case differences between
        # two offline invocations must not hide a real scene mismatch.
        return value.replace("\\", "/").lower()
    if kind == "resolution" and isinstance(value, (list, tuple)):
        return tuple(value)
    return value


def _validate_identity(full: Mapping[str, Any], grid: Mapping[str, Any]) -> List[Dict[str, Any]]:
    checks: List[Dict[str, Any]] = []
    values = {
        "scene": (_scene(full), _scene(grid)),
        "resolution": (_resolution(full), _resolution(grid)),
        "schema": (_schema(full), _schema(grid)),
    }
    for kind, (control, candidate) in values.items():
        if control is None or candidate is None:
            checks.append(
                _record(
                    "identity:%s" % kind,
                    "BLOCKED",
                    "missing %s in one or both manifests" % kind,
                    full=control,
                    grid=candidate,
                )
            )
            continue
        equal = _identity_value(kind, control) == _identity_value(kind, candidate)
        checks.append(
            _record(
                "identity:%s" % kind,
                "PASS" if equal else "FAIL",
                None if equal else "full-scan and grid values differ",
                full=control,
                grid=candidate,
            )
        )
    return checks


def _pass_property(manifest: Mapping[str, Any]) -> Any:
    if "useCacheCardGrid" in manifest:
        return manifest["useCacheCardGrid"]
    configuration = manifest.get("configuration")
    if isinstance(configuration, Mapping):
        if "useCacheCardGrid" in configuration:
            return configuration["useCacheCardGrid"]
        properties = configuration.get("pass_properties")
        if isinstance(properties, Mapping) and "useCacheCardGrid" in properties:
            return properties["useCacheCardGrid"]
    props = manifest.get("pass_properties")
    if isinstance(props, Mapping):
        return props.get("useCacheCardGrid")
    # Surface-cache A/B captures put pass properties under the case object.
    cases = manifest.get("cases")
    if isinstance(cases, Mapping):
        for case in cases.values():
            if not isinstance(case, Mapping):
                continue
            properties = case.get("properties")
            if isinstance(properties, Mapping) and "useCacheCardGrid" in properties:
                return properties["useCacheCardGrid"]
    # A final stats sample is authoritative for minimal captures that omit a
    # separate properties object.  This is intentionally a direct field read,
    # never a recursive guess through arbitrary JSON values.
    stats = _surface_stats(manifest)
    if isinstance(stats, Mapping) and "useCacheCardGrid" in stats:
        return stats["useCacheCardGrid"]
    return None


def _as_bool(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and value in (0, 1):
        return bool(value)
    if isinstance(value, float) and value in (0.0, 1.0):
        return bool(value)
    return None


def _validate_switches(full: Mapping[str, Any], grid: Mapping[str, Any]) -> List[Dict[str, Any]]:
    checks: List[Dict[str, Any]] = []
    control, candidate = _pass_property(full), _pass_property(grid)
    if control is None or candidate is None:
        return [
            _record(
                "route:useCacheCardGrid",
                "BLOCKED",
                "missing useCacheCardGrid in one or both manifests",
                full=control,
                grid=candidate,
            )
        ]
    control_bool, candidate_bool = _as_bool(control), _as_bool(candidate)
    if control_bool is None or candidate_bool is None:
        return [
            _record(
                "route:useCacheCardGrid",
                "BLOCKED",
                "useCacheCardGrid must be boolean or 0/1",
                full=control,
                grid=candidate,
            )
        ]
    if control_bool or not candidate_bool:
        checks.append(
            _record(
                "route:useCacheCardGrid",
                "FAIL",
                "control must be false and candidate must be true",
                full=control_bool,
                grid=candidate_bool,
            )
        )
    else:
        checks.append(
            _record(
                "route:useCacheCardGrid",
                "PASS",
                full=False,
                grid=True,
            )
        )
    return checks


def _screen_stats(manifest: Mapping[str, Any]) -> Mapping[str, Any] | None:
    # Current benchmark manifests use lumen_gi.resource_stats.screen_probe;
    # router manifests use the latest frame's screenProbeStats.  Both are
    # explicit producers, not a recursive search through arbitrary numbers.
    lumen = manifest.get("lumen_gi")
    if isinstance(lumen, Mapping):
        stats = lumen.get("resource_stats")
        if isinstance(stats, Mapping) and isinstance(stats.get("screen_probe"), Mapping):
            return stats["screen_probe"]
    return (
        _last_mapping(manifest, "screenProbeStats")
        or _mapping_at(manifest, "screenProbeStats", "screen_probe")
        or _mapping_at(manifest, "stats")
        or _surface_stats(manifest)
    )


def _surface_stats(manifest: Mapping[str, Any]) -> Mapping[str, Any] | None:
    lumen = manifest.get("lumen_gi")
    if isinstance(lumen, Mapping):
        stats = lumen.get("resource_stats")
        if isinstance(stats, Mapping) and isinstance(stats.get("surface_cache"), Mapping):
            return stats["surface_cache"]
    direct = (
        _last_mapping(manifest, "surfaceCacheStats")
        or _last_mapping(manifest, "surface_cache")
        or _mapping_at(manifest, "surfaceCacheStats", "surface_cache")
    )
    if direct is not None:
        return direct

    # C6's A/B runner records one stats mapping in every case sample.  Select
    # the latest sample explicitly; frame-1 warm-up has zero lookup attempts
    # and must not mask the settled frame.  Keeping the selection local also
    # avoids accidentally summing cumulative and per-frame counters together.
    cases = manifest.get("cases")
    if isinstance(cases, Mapping):
        latest: Mapping[str, Any] | None = None
        latest_frame = -math.inf
        for case in cases.values():
            if not isinstance(case, Mapping):
                continue
            samples = case.get("samples")
            if not isinstance(samples, Sequence) or isinstance(samples, (str, bytes)):
                continue
            for sample in samples:
                if not isinstance(sample, Mapping) or not isinstance(sample.get("stats"), Mapping):
                    continue
                frame = _number(sample.get("frame"))
                frame_value = frame if frame is not None else latest_frame + 1.0
                if frame_value >= latest_frame:
                    latest = sample["stats"]
                    latest_frame = frame_value
        if latest is not None:
            return latest

    direct_stats = manifest.get("stats")
    if isinstance(direct_stats, Mapping):
        return direct_stats
    return None


def _value_with_aliases(mapping: Mapping[str, Any] | None, aliases: Sequence[str]) -> Any:
    if mapping is None:
        return None
    for key in aliases:
        if key in mapping:
            return mapping[key]
    return None


def _candidate_values(manifest: Mapping[str, Any]) -> Tuple[Dict[str, float] | None, List[str]]:
    stats = _surface_stats(manifest)
    if stats is None:
        return None, ["surface-cache stats object"]
    aliases = {
        "cardGridCandidateCount": ("cardGridCandidateCount", "candidateCount"),
        "cardGridOverflowCells": ("cardGridOverflowCells", "overflowCells"),
        "cardGridCardCount": ("cardGridCardCount", "cardCount", "cards"),
        "cardGridIndexedCards": ("cardGridIndexedCards", "cardGridCardsIndexed", "indexedCards"),
        "cardGridMissingCards": ("cardGridMissingCards", "missingCards"),
    }
    values: Dict[str, float] = {}
    missing: List[str] = []
    invalid: List[str] = []
    for field in CANDIDATE_FIELDS:
        raw = _value_with_aliases(stats, aliases[field])
        if raw is None:
            missing.append(field)
            continue
        value = _nonnegative(raw)
        if value is None:
            invalid.append(field)
        else:
            values[field] = value
    if invalid:
        missing.extend("%s (non-finite/negative)" % field for field in invalid)
    return values if not missing else None, missing


def _stats_samples(manifest: Mapping[str, Any]) -> List[Mapping[str, Any]]:
    """Return explicit frame stats in capture order.

    C5 router captures use ``frames[*].surfaceCacheStats`` while the C6 A/B
    capture stores ``cases[*].samples[*].stats``.  Both are per-frame
    snapshots.  This helper deliberately does not recursively walk arbitrary
    JSON, because doing so would mix final-gate summaries with producer data.
    """
    frames = manifest.get("frames")
    if isinstance(frames, Sequence) and not isinstance(frames, (str, bytes)):
        result: List[Mapping[str, Any]] = []
        for frame in frames:
            if not isinstance(frame, Mapping):
                continue
            stats = frame.get("surfaceCacheStats")
            if not isinstance(stats, Mapping):
                stats = frame.get("stats")
            if isinstance(stats, Mapping):
                result.append(stats)
        if result:
            return result

    cases = manifest.get("cases")
    if isinstance(cases, Mapping):
        result = []
        for case in cases.values():
            if not isinstance(case, Mapping):
                continue
            samples = case.get("samples")
            if not isinstance(samples, Sequence) or isinstance(samples, (str, bytes)):
                continue
            for sample in samples:
                if isinstance(sample, Mapping) and isinstance(sample.get("stats"), Mapping):
                    result.append(sample["stats"])
        if result:
            return result

    direct = _mapping_at(manifest, "surfaceCacheStats", "surface_cache")
    if direct is not None:
        return [direct]
    stats = manifest.get("stats")
    return [stats] if isinstance(stats, Mapping) else []


def _latest_stat_value(stats: Mapping[str, Any], aliases: Sequence[str]) -> Any:
    for key in aliases:
        if key in stats:
            return stats[key]
    return None


_LIFECYCLE_ALIASES: Dict[str, Tuple[str, ...]] = {
    "cardGridCandidateCount": ("cardGridCandidateCount", "candidateCount"),
    "cardGridOverflowCells": ("cardGridOverflowCells", "overflowCells"),
    "cardGridCardCount": ("cardGridCardCount", "cardCount", "cards"),
    "cardGridIndexedCards": ("cardGridIndexedCards", "cardGridCardsIndexed", "indexedCards"),
    "cardGridMissingCards": ("cardGridMissingCards", "missingCards"),
    "cardGridDim": ("cardGridDim", "gridDim"),
    "cardGridMaxCandidates": ("cardGridMaxCandidates", "maxCandidates"),
    "allocatedPages": ("allocatedPages",),
    "pageMetadataAllocated": ("pageMetadataAllocated",),
    "totalPages": ("totalPages",),
    "freePages": ("freePages",),
    "pageMetadataPending": ("pageMetadataPending",),
    "pageMetadataReady": ("pageMetadataReady",),
    "cacheLookupHits": ("cacheLookupHits",),
    "staleOwnerRejects": ("staleOwnerRejects",),
    "generationMismatchRejects": ("generationMismatchRejects", "generationRejects"),
    "stateMismatchRejects": ("stateMismatchRejects",),
    "surfaceCacheFrameIndex": ("surfaceCacheFrameIndex",),
    "schedulerFrameIndex": ("schedulerFrameIndex", "frameIndex"),
}


def _grid_snapshot(manifest: Mapping[str, Any]) -> Tuple[Dict[str, float] | None, List[str]]:
    stats = _surface_stats(manifest)
    if stats is None:
        return None, list(_LIFECYCLE_ALIASES)
    values: Dict[str, float] = {}
    missing: List[str] = []
    for field, aliases in _LIFECYCLE_ALIASES.items():
        raw = _latest_stat_value(stats, aliases)
        if raw is None:
            # Lifecycle clocks and optional stale counters are useful when
            # present, but older C5 manifests predate them.  Candidate/page
            # publication fields remain required below.
            if field in {
                "pageMetadataPending",
                "pageMetadataReady",
                "staleOwnerRejects",
                "generationMismatchRejects",
                "stateMismatchRejects",
                "surfaceCacheFrameIndex",
                "schedulerFrameIndex",
            }:
                continue
            missing.append(field)
            continue
        value = _nonnegative(raw)
        if value is None:
            missing.append("%s (non-finite/negative)" % field)
        else:
            values[field] = value
    return values if not missing else None, missing


def _validate_grid_lifecycle(full: Mapping[str, Any], grid: Mapping[str, Any]) -> Dict[str, Any]:
    """Validate publication invariants without treating reject cardinality as failure.

    ``cacheLookupAttempts`` is counted once per probe direction.  Coverage,
    page, metadata and visibility rejects are counted once per candidate card
    visited; therefore rejects can legitimately exceed attempts.  This check
    only fails impossible resource/index invariants and records zero-page,
    overflow and stale-index observations explicitly.
    """
    full_values, full_missing = _grid_snapshot(full)
    grid_values, grid_missing = _grid_snapshot(grid)
    if full_values is None or grid_values is None:
        return _record(
            "grid lifecycle",
            "BLOCKED",
            "required grid/page publication telemetry is missing or invalid",
            fullMissing=full_missing,
            gridMissing=grid_missing,
        )

    invalid: List[str] = []
    open_quality: List[str] = []
    per_run: Dict[str, Any] = {}
    for label, values in (("full", full_values), ("grid", grid_values)):
        candidate_count = values["cardGridCandidateCount"]
        overflow = values["cardGridOverflowCells"]
        card_count = values["cardGridCardCount"]
        indexed = values["cardGridIndexedCards"]
        missing = values["cardGridMissingCards"]
        allocated = values["allocatedPages"]
        metadata_allocated = values["pageMetadataAllocated"]
        total = values["totalPages"]
        free = values["freePages"]
        run_invalid: List[str] = []
        run_open: List[str] = []

        if indexed > card_count:
            run_invalid.append("indexed cards %.3f exceed card count %.3f" % (indexed, card_count))
        if missing > card_count:
            run_invalid.append("missing cards %.3f exceed card count %.3f" % (missing, card_count))
        # Missing cards are the complement of indexed cards in the current
        # host contract.  Permit a conservative over-report (for a card that
        # is both missing and intentionally omitted) but never under-report.
        if indexed + missing < card_count:
            run_invalid.append("indexed+missing cards under-report card count")
        if allocated > total or free > total or abs((allocated + free) - total) > 0.0:
            run_invalid.append("allocated/free pages do not partition total pages")
        if metadata_allocated != allocated:
            run_invalid.append("page metadata allocation differs from resident allocation")

        dim = values.get("cardGridDim")
        max_candidates = values.get("cardGridMaxCandidates")
        if dim is not None and max_candidates is not None:
            if dim <= 0.0 or max_candidates <= 0.0:
                run_invalid.append("grid dimension/max-candidates must be positive")
            elif overflow <= dim * dim * dim:
                # CandidateCount is bounded per non-overflow cell.  Overflow
                # cells intentionally use the authoritative full-card scan;
                # they do not justify a failure or a reject/attempt ratio.
                bounded_capacity = dim * dim * dim * max_candidates
                if candidate_count > bounded_capacity:
                    run_invalid.append("candidate count exceeds grid capacity without overflow")
            if overflow > dim * dim * dim:
                run_invalid.append("overflow-cell count exceeds grid cell count")

        hits = values.get("cacheLookupHits", 0.0)
        if allocated == 0.0:
            # A zero-page capture is a valid bounded state.  It is only a
            # correctness failure if the consumer claims a cache hit while no
            # page can have been published.
            if hits > 0.0:
                run_invalid.append("cache hit observed with zero resident pages")
            if candidate_count > 0.0 or indexed > 0.0 or card_count > 0.0:
                run_open.append("zero resident pages with published card candidates")
            zero_page_status = "PASS"
        else:
            zero_page_status = "NOT_APPLICABLE"
        if overflow > 0.0:
            run_open.append("overflow cells invoke full-card fallback")
        if missing > 0.0:
            run_open.append("missing cards indicate stale/incomplete grid publication")

        per_run[label] = {
            "candidateCount": candidate_count,
            "cardCount": card_count,
            "indexedCards": indexed,
            "missingCards": missing,
            "overflowCells": overflow,
            "allocatedPages": allocated,
            "pageMetadataAllocated": metadata_allocated,
            "totalPages": total,
            "freePages": free,
            "cacheLookupHits": hits,
            "zeroPage": zero_page_status,
            "overflowFallback": overflow > 0.0,
            "staleGrid": missing > 0.0,
            "invalid": run_invalid,
            "open": run_open,
        }
        invalid.extend("%s: %s" % (label, item) for item in run_invalid)
        open_quality.extend("%s: %s" % (label, item) for item in run_open)

    # Route equivalence includes residency/publication, not just output
    # finiteness.  These values should be route-independent; stale owner and
    # coverage counters are intentionally excluded because the grid can visit
    # a different number/order of candidate cards before falling back.
    equivalent_fields = (
        "cardGridCandidateCount",
        "cardGridOverflowCells",
        "cardGridCardCount",
        "cardGridIndexedCards",
        "cardGridMissingCards",
        "allocatedPages",
        "pageMetadataAllocated",
        "totalPages",
        "freePages",
    )
    delta = {field: grid_values[field] - full_values[field] for field in equivalent_fields}
    different = [field for field, value in delta.items() if value != 0.0]
    if different:
        invalid.append("full-scan and grid publication differs: %s" % ", ".join(different))

    # A changing candidate publication during one capture is not automatically
    # wrong (the cache may be allocating pages), but it is evidence that a
    # broad steady-state equivalence claim needs a longer runtime capture.
    sequence_notes: Dict[str, Any] = {}
    for label, manifest in (("full", full), ("grid", grid)):
        samples = _stats_samples(manifest)
        candidate_series = []
        for stats in samples:
            values = {
                field: _nonnegative(_latest_stat_value(stats, aliases))
                for field, aliases in _LIFECYCLE_ALIASES.items()
            }
            if all(values[field] is not None for field in CANDIDATE_FIELDS):
                candidate_series.append({field: values[field] for field in CANDIDATE_FIELDS})
        changes: Dict[str, List[Tuple[float, float]]] = {}
        if candidate_series:
            baseline = candidate_series[0]
            for field in CANDIDATE_FIELDS:
                pairs = [(baseline[field], item[field]) for item in candidate_series[1:] if item[field] != baseline[field]]
                if pairs:
                    changes[field] = pairs
        if changes:
            open_quality.append("%s: candidate publication changes across frames" % label)
        sequence_notes[label] = {
            "samples": len(candidate_series),
            "candidateChanges": changes,
        }

    status = "FAIL" if invalid else "OPEN" if open_quality else "PASS"
    return _record(
        "grid lifecycle",
        status,
        "; ".join(invalid or open_quality) if (invalid or open_quality) else None,
        full=per_run["full"],
        grid=per_run["grid"],
        equivalentDelta=delta,
        sequence=sequence_notes,
        rejectUnitContract={
            "cacheLookupAttempts": "probe directions",
            "cacheCoverageRejects": "candidate card visits",
            "cachePageRejects": "candidate card visits",
            "cacheMetadataRejects": "candidate card visits",
            "cacheVisibilityRejects": "candidate card visits",
            "rejectsMayExceedAttempts": True,
        },
    )


def _validate_candidate_telemetry(full: Mapping[str, Any], grid: Mapping[str, Any]) -> Dict[str, Any]:
    control, control_missing = _candidate_values(full)
    candidate, candidate_missing = _candidate_values(grid)
    if control is None or candidate is None:
        return _record(
            "candidate telemetry",
            "BLOCKED",
            "required candidate-index telemetry is missing or invalid",
            fullMissing=control_missing,
            gridMissing=candidate_missing,
        )
    deltas = {field: candidate[field] - control[field] for field in CANDIDATE_FIELDS}
    different = [field for field, delta in deltas.items() if delta != 0.0]
    if different:
        return _record(
            "candidate telemetry",
            "FAIL",
            "full-scan and grid candidate telemetry differs",
            full=control,
            grid=candidate,
            delta=deltas,
            different=different,
        )
    return _record(
        "candidate telemetry",
        "PASS",
        full=control,
        grid=candidate,
        delta=deltas,
    )


def _cache_values(manifest: Mapping[str, Any]) -> Tuple[Dict[str, float] | None, List[str]]:
    stats = _screen_stats(manifest)
    if stats is None:
        return None, list(CACHE_FIELDS)
    values: Dict[str, float] = {}
    missing: List[str] = []
    for field in CACHE_FIELDS:
        raw = stats.get(field)
        if raw is None:
            missing.append(field)
            continue
        value = _nonnegative(raw)
        if value is None:
            missing.append("%s (non-finite/negative)" % field)
        else:
            values[field] = value
    return values if not missing else None, missing


def _validate_cache_comparison(full: Mapping[str, Any], grid: Mapping[str, Any]) -> Dict[str, Any]:
    control, control_missing = _cache_values(full)
    candidate, candidate_missing = _cache_values(grid)
    if control is None or candidate is None:
        return _record(
            "cache lookup counters",
            "BLOCKED",
            "cache hit/attempt/coverage telemetry is missing or invalid",
            fullMissing=control_missing,
            gridMissing=candidate_missing,
        )
    attempts = control["cacheLookupAttempts"]
    grid_attempts = candidate["cacheLookupAttempts"]
    hits = control["cacheLookupHits"]
    grid_hits = candidate["cacheLookupHits"]
    delta_attempts = grid_attempts - attempts
    tolerance = max(1.0, attempts * ATTEMPT_RELATIVE_TOLERANCE)
    invalid: List[str] = []
    if attempts <= 0.0 or grid_attempts <= 0.0:
        invalid.append("lookup attempts must be positive in both runs")
    if abs(delta_attempts) > tolerance:
        invalid.append(
            "lookup-attempt delta %.3f exceeds tolerance %.3f" % (delta_attempts, tolerance)
        )
    # A grid that loses one cache hit is a correctness regression even if its
    # image remains finite.  This is intentionally stricter than coverage.
    if grid_hits < hits:
        invalid.append("grid lost cache hits (%.3f < %.3f)" % (grid_hits, hits))
    if invalid:
        status = "FAIL"
        reason = "; ".join(invalid)
    else:
        status = "PASS"
        reason = None
    return _record(
        "cache lookup counters",
        status,
        reason,
        full=control,
        grid=candidate,
        delta={
            "cacheLookupAttempts": delta_attempts,
            "cacheLookupHits": grid_hits - hits,
            "cacheCoverageRejects": candidate["cacheCoverageRejects"] - control["cacheCoverageRejects"],
        },
        attemptTolerance=tolerance,
    )


def _output_maps(manifest: Mapping[str, Any]) -> List[Mapping[str, Any]]:
    maps: List[Mapping[str, Any]] = []
    direct = manifest.get("outputs")
    if isinstance(direct, Mapping):
        maps.append(direct)
    frames = manifest.get("frames")
    if isinstance(frames, Sequence) and not isinstance(frames, (str, bytes)):
        for frame in reversed(frames):
            if isinstance(frame, Mapping) and isinstance(frame.get("outputs"), Mapping):
                maps.append(frame["outputs"])
                break
    lumen = manifest.get("lumen_gi")
    if isinstance(lumen, Mapping) and isinstance(lumen.get("outputs"), Mapping):
        maps.append(lumen["outputs"])
    # C6 A/B captures store output health under the latest case sample.
    cases = manifest.get("cases")
    if isinstance(cases, Mapping):
        latest: Mapping[str, Any] | None = None
        latest_frame = -math.inf
        for case in cases.values():
            if not isinstance(case, Mapping):
                continue
            samples = case.get("samples")
            if not isinstance(samples, Sequence) or isinstance(samples, (str, bytes)):
                continue
            for sample in samples:
                if not isinstance(sample, Mapping) or not isinstance(sample.get("outputs"), Mapping):
                    continue
                frame = _number(sample.get("frame"))
                frame_value = frame if frame is not None else latest_frame + 1.0
                if frame_value >= latest_frame:
                    latest = sample["outputs"]
                    latest_frame = frame_value
        if latest is not None:
            maps.append(latest)
    return maps


def _validate_outputs(full: Mapping[str, Any], grid: Mapping[str, Any]) -> Dict[str, Any]:
    full_maps, grid_maps = _output_maps(full), _output_maps(grid)
    full_outputs = full_maps[0] if full_maps else None
    grid_outputs = grid_maps[0] if grid_maps else None
    if not isinstance(full_outputs, Mapping) or not isinstance(grid_outputs, Mapping):
        return _record("finite outputs", "BLOCKED", "missing output evidence in one or both manifests")
    common = sorted(set(full_outputs.keys()) & set(grid_outputs.keys()))
    if not common:
        return _record("finite outputs", "BLOCKED", "no common output channels")
    invalid: List[str] = []
    different: List[str] = []
    channel_summary: Dict[str, Any] = {}
    for channel in common:
        channel_summary[channel] = {}
        for label, outputs in (("full", full_outputs), ("grid", grid_outputs)):
            descriptor = outputs[channel]
            if not isinstance(descriptor, Mapping):
                invalid.append("%s:%s descriptor is not an object" % (label, channel))
                continue
            missing = [key for key in OUTPUT_REQUIRED_FLAGS if key not in descriptor]
            if missing:
                invalid.append("%s:%s missing %s" % (label, channel, ",".join(missing)))
                continue
            if descriptor["finite"] is not True:
                invalid.append("%s:%s finite=false" % (label, channel))
            if descriptor["nonnegative"] is not True:
                invalid.append("%s:%s nonnegative=false" % (label, channel))
            for key in ("mean", "min", "max"):
                if key in descriptor and _nonnegative(descriptor[key]) is None:
                    invalid.append("%s:%s %s is non-finite/negative" % (label, channel, key))
            if "samples" in descriptor:
                samples = list(_flatten(descriptor["samples"]))
                if not samples:
                    invalid.append("%s:%s samples are empty" % (label, channel))
                elif any(_nonnegative(sample) is None for sample in samples):
                    invalid.append("%s:%s samples contain non-finite/negative values" % (label, channel))
            channel_summary[channel][label] = {
                "finite": descriptor.get("finite"),
                "nonnegative": descriptor.get("nonnegative"),
                "mean": descriptor.get("mean"),
                "max": descriptor.get("max"),
            }
        # A/B captures usually expose scalar health summaries rather than raw
        # images.  Compare any common scalar metrics when present.  This is a
        # bounded equivalence check: tiny floating-point/dispatch ordering
        # differences are tolerated, while a route-induced image change is not
        # hidden behind merely finite/nonnegative flags.
        full_descriptor = full_outputs.get(channel)
        grid_descriptor = grid_outputs.get(channel)
        if isinstance(full_descriptor, Mapping) and isinstance(grid_descriptor, Mapping):
            for metric in ("mean", "min", "max", "nonzeroFraction"):
                full_value = _number(full_descriptor.get(metric))
                grid_value = _number(grid_descriptor.get(metric))
                if full_value is None or grid_value is None:
                    continue
                tolerance = max(1e-6, 1e-4 * max(1.0, abs(full_value), abs(grid_value)))
                if abs(grid_value - full_value) > tolerance:
                    different.append(
                        "%s:%s delta %.6g exceeds %.6g"
                        % (channel, metric, grid_value - full_value, tolerance)
                    )
    return _record(
        "finite outputs",
        "FAIL" if invalid or different else "PASS",
        "; ".join(invalid + different) if invalid or different else None,
        channels=common,
        evidence=channel_summary,
        equivalence={"status": "FAIL" if different else "PASS", "differences": different},
    )


def _validate_coverage_open(full: Mapping[str, Any], grid: Mapping[str, Any]) -> Dict[str, Any]:
    control, control_missing = _cache_values(full)
    candidate, candidate_missing = _cache_values(grid)
    if control is None or candidate is None:
        return _record(
            "coverage reject rate",
            "BLOCKED",
            "coverage telemetry unavailable",
            fullMissing=control_missing,
            gridMissing=candidate_missing,
        )
    records: Dict[str, Any] = {}
    open_quality: List[str] = []
    for label, values in (("full", control), ("grid", candidate)):
        attempts = values["cacheLookupAttempts"]
        coverage = values["cacheCoverageRejects"]
        page = values["cachePageRejects"]
        metadata = values["cacheMetadataRejects"]
        visibility = values["cacheVisibilityRejects"]
        hits = values["cacheLookupHits"]
        # Coverage rejects are counted per candidate card, while attempts are
        # counted per probe direction.  Dividing them directly can exceed 1
        # (and was misleading for Cornell's 12-card candidate set).  The
        # outcome counters form the actual candidate-evaluation denominator:
        # exactly one terminal outcome is recorded for each evaluated card.
        evaluations = coverage + page + metadata + visibility + hits
        coverage_fraction = coverage / evaluations if evaluations > 0.0 else None
        hit_fraction = hits / evaluations if evaluations > 0.0 else None
        records[label] = {
            "coverageRejects": coverage,
            "pageRejects": page,
            "metadataRejects": metadata,
            "visibilityRejects": visibility,
            "hits": hits,
            "attempts": attempts,
            "candidateEvaluations": evaluations,
            "coverageFraction": coverage_fraction,
            "hitFraction": hit_fraction,
            # ``attempts`` counts probe directions, whereas each reject/hit
            # below counts one candidate card visit.  A candidate list can
            # therefore produce rejects >> attempts without being impossible.
            "counterUnits": {
                "cacheLookupAttempts": "probe directions",
                "cacheCoverageRejects": "candidate card visits",
                "cachePageRejects": "candidate card visits",
                "cacheMetadataRejects": "candidate card visits",
                "cacheVisibilityRejects": "candidate card visits",
                "cacheLookupHits": "probe directions",
            },
            "rejectToAttemptRatio": coverage / attempts if attempts > 0.0 else None,
        }
        if evaluations <= 0.0 or hit_fraction is None or hit_fraction <= 0.0:
            open_quality.append(label)
    # OPEN is deliberately not folded into the overall failure calculation.
    # It keeps the known producer-coverage debt visible while allowing grid
    # equivalence to be measured independently from broad lighting quality.
    return _record(
        "coverage reject rate",
        "OPEN" if open_quality else "PASS",
        "candidate evaluation has no valid cache hit" if open_quality else None,
        legacyThreshold=HIGH_COVERAGE_REJECT_RATIO,
        denominator="candidateEvaluations = coverageRejects + pageRejects + metadataRejects + visibilityRejects + hits",
        rejectsMayExceedAttempts=True,
        records=records,
        openFor=open_quality,
    )


def evaluate(full: Mapping[str, Any], grid: Mapping[str, Any]) -> Dict[str, Any]:
    if not isinstance(full, Mapping):
        full = {}
    if not isinstance(grid, Mapping):
        grid = {}
    checks: List[Dict[str, Any]] = []
    checks.extend(_validate_identity(full, grid))
    checks.extend(_validate_switches(full, grid))
    checks.append(_validate_candidate_telemetry(full, grid))
    checks.append(_validate_cache_comparison(full, grid))
    checks.append(_validate_outputs(full, grid))
    checks.append(_validate_grid_lifecycle(full, grid))
    checks.append(_validate_coverage_open(full, grid))
    statuses = [check["status"] for check in checks]
    if "FAIL" in statuses:
        status = "FAIL"
    elif "BLOCKED" in statuses:
        status = "BLOCKED"
    else:
        status = "PASS"
    return {
        "schema": GATE_SCHEMA,
        "schemaVersion": GATE_SCHEMA,
        "script": SCRIPT_NAME,
        "status": status,
        "contract": {
            "control": "useCacheCardGrid=false",
            "candidate": "useCacheCardGrid=true",
            "sameIdentity": ["scene", "resolution", "schema"],
            "requiredCacheFields": list(CACHE_FIELDS),
            "requiredCandidateFields": list(CANDIDATE_FIELDS),
            "requiredGridLifecycleFields": [
                "allocatedPages",
                "pageMetadataAllocated",
                "totalPages",
                "freePages",
            ],
            "hitLossVerdict": "FAIL",
            "missingEvidenceVerdict": "BLOCKED",
            "highCoverageVerdict": "OPEN",
            "rejectUnitContract": {
                "cacheLookupAttempts": "probe directions",
                "cacheCoverageRejects": "candidate card visits",
                "cachePageRejects": "candidate card visits",
                "cacheMetadataRejects": "candidate card visits",
                "cacheVisibilityRejects": "candidate card visits",
                "rejectsMayExceedAttempts": True,
            },
            "attemptRelativeTolerance": ATTEMPT_RELATIVE_TOLERANCE,
        },
        "checks": checks,
        "summary": {
            "pass": statuses.count("PASS"),
            "open": statuses.count("OPEN"),
            "blocked": statuses.count("BLOCKED"),
            "fail": statuses.count("FAIL"),
        },
    }


def _fixture(*, grid_hits: float = 12.0, include_outputs: bool = True, include_candidate: bool = True) -> Dict[str, Any]:
    surface = {
        "cardGridCandidateCount": 128.0,
        "cardGridOverflowCells": 0.0,
        "cardGridCardCount": 16.0,
        "cardGridIndexedCards": 16.0,
        "cardGridMissingCards": 0.0,
        "cardGridDim": 4.0,
        "cardGridMaxCandidates": 8.0,
        "allocatedPages": 16.0,
        "pageMetadataAllocated": 16.0,
        "totalPages": 64.0,
        "freePages": 48.0,
        "pageMetadataPending": 0.0,
        "pageMetadataReady": 16.0,
        "staleOwnerRejects": 0.0,
        "generationMismatchRejects": 0.0,
        "stateMismatchRejects": 0.0,
        "surfaceCacheFrameIndex": 1.0,
        "schedulerFrameIndex": 1.0,
        "cacheLookupHits": grid_hits,
    }
    screen = {
        "cacheLookupAttempts": 100.0,
        "cacheLookupHits": grid_hits,
        "cacheCoverageRejects": 60.0,
        "cachePageRejects": 20.0,
        "cacheMetadataRejects": 8.0,
        "cacheVisibilityRejects": 4.0,
    }
    manifest: Dict[str, Any] = {
        "schema": "LumenGI.GDFProbeRouter.v2",
        "scene": "media/test_scenes/cornell_box.pyscene",
        "resolution": [320, 180],
        "useCacheCardGrid": False,
        "lumen_gi": {
            "resource_stats": {"surface_cache": surface, "screen_probe": screen}
        },
    }
    if include_outputs:
        manifest["outputs"] = {
            "resolvedDiffuseGI": {
                "finite": True,
                "nonnegative": True,
                "mean": 0.25,
                "max": 1.0,
                "samples": [0.0, 0.25, 1.0],
            }
        }
    if not include_candidate:
        for key in CANDIDATE_FIELDS:
            surface.pop(key, None)
    return manifest


def _self_test() -> int:
    full = _fixture(grid_hits=12.0)
    grid = _fixture(grid_hits=12.0)
    grid["useCacheCardGrid"] = True
    passed = evaluate(full, grid)

    blocked_full = _fixture(include_outputs=False, include_candidate=False)
    blocked_grid = _fixture(include_outputs=False, include_candidate=False)
    blocked_grid["useCacheCardGrid"] = True
    blocked = evaluate(blocked_full, blocked_grid)

    failed_full = _fixture(grid_hits=12.0)
    failed_grid = _fixture(grid_hits=11.0)
    failed_grid["useCacheCardGrid"] = True
    failed = evaluate(failed_full, failed_grid)

    # A zero-page publication is a valid bounded state.  It must not be
    # rejected merely because page/coverage outcomes outnumber probe attempts.
    zero_full = _fixture(grid_hits=0.0)
    zero_grid = _fixture(grid_hits=0.0)
    for manifest in (zero_full, zero_grid):
        stats = manifest["lumen_gi"]["resource_stats"]["surface_cache"]
        stats.update({
            "cardGridCandidateCount": 0.0,
            "cardGridCardCount": 0.0,
            "cardGridIndexedCards": 0.0,
            "cardGridMissingCards": 0.0,
            "allocatedPages": 0.0,
            "pageMetadataAllocated": 0.0,
            "totalPages": 64.0,
            "freePages": 64.0,
            "cacheLookupAttempts": 100.0,
            "cacheLookupHits": 0.0,
            "cacheCoverageRejects": 0.0,
            "cachePageRejects": 100.0,
        })
    zero_grid["useCacheCardGrid"] = True
    zero = evaluate(zero_full, zero_grid)

    # Overflow is an explicit conservative fallback to full-card scan.  It is
    # evidence to report, not a correctness failure.
    overflow_full = _fixture(grid_hits=1.0)
    overflow_grid = _fixture(grid_hits=1.0)
    overflow_full["lumen_gi"]["resource_stats"]["surface_cache"]["cardGridOverflowCells"] = 1.0
    overflow_grid["useCacheCardGrid"] = True
    overflow_grid["lumen_gi"]["resource_stats"]["surface_cache"]["cardGridOverflowCells"] = 1.0
    overflow = evaluate(overflow_full, overflow_grid)

    # A stale/incomplete index remains OPEN until a runtime capture proves
    # fallback coverage; it does not get silently promoted to FAIL.
    stale_full = _fixture(grid_hits=1.0)
    stale_grid = _fixture(grid_hits=1.0)
    stale_grid["useCacheCardGrid"] = True
    stale_full_stats = stale_full["lumen_gi"]["resource_stats"]["surface_cache"]
    stale_full_stats["cardGridIndexedCards"] = 15.0
    stale_full_stats["cardGridMissingCards"] = 1.0
    stale_stats = stale_grid["lumen_gi"]["resource_stats"]["surface_cache"]
    stale_stats["cardGridIndexedCards"] = 15.0
    stale_stats["cardGridMissingCards"] = 1.0
    stale = evaluate(stale_full, stale_grid)

    # The pass fixture includes a non-zero valid-hit fraction, so the producer
    # coverage observation is closed for this synthetic case. Real manifests
    # with zero valid hits remain OPEN without failing grid equivalence.
    pass_ok = passed["status"] == "PASS" and passed["summary"]["open"] == 0
    blocked_ok = blocked["status"] == "BLOCKED"
    fail_ok = failed["status"] == "FAIL"
    zero_ok = zero["status"] == "PASS" and any(
        check.get("name") == "grid lifecycle"
        and check.get("status") == "PASS"
        and check.get("full", {}).get("zeroPage") == "PASS"
        for check in zero["checks"]
    )
    overflow_ok = overflow["status"] == "PASS" and any(
        check.get("name") == "grid lifecycle"
        and check.get("grid", {}).get("overflowFallback") is True
        for check in overflow["checks"]
    )
    stale_ok = stale["status"] == "PASS" and any(
        check.get("name") == "grid lifecycle" and check.get("status") == "OPEN"
        for check in stale["checks"]
    )
    print("C5_GRID_EQUIVALENCE_SELF_TEST_PASS", passed["status"])
    print("C5_GRID_EQUIVALENCE_SELF_TEST_BLOCKED", blocked["status"])
    print("C5_GRID_EQUIVALENCE_SELF_TEST_FAIL", failed["status"])
    print("C5_GRID_EQUIVALENCE_SELF_TEST_ZERO_PAGE", zero["status"])
    print("C5_GRID_EQUIVALENCE_SELF_TEST_OVERFLOW", overflow["status"])
    print("C5_GRID_EQUIVALENCE_SELF_TEST_STALE_GRID", stale["status"])
    return 0 if pass_ok and blocked_ok and fail_ok and zero_ok and overflow_ok and stale_ok else 1


def _read(path: Path) -> Mapping[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, Mapping):
        raise ValueError("manifest must be a JSON object")
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
    parser.add_argument("--fullscan", help="full-scan control manifest JSON")
    parser.add_argument("--grid", help="grid-on candidate manifest JSON")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT), help="gate report JSON path")
    parser.add_argument("--self-test", action="store_true", help="run embedded PASS/BLOCKED/FAIL fixtures")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()

    errors: List[str] = []
    full: Mapping[str, Any] = {}
    grid: Mapping[str, Any] = {}
    if not args.fullscan:
        errors.append("missing --fullscan")
    else:
        try:
            full = _read(Path(args.fullscan).absolute())
        except Exception as error:
            errors.append("fullscan: %r" % (error,))
    if not args.grid:
        errors.append("missing --grid")
    else:
        try:
            grid = _read(Path(args.grid).absolute())
        except Exception as error:
            errors.append("grid: %r" % (error,))

    report = evaluate(full, grid)
    report["inputs"] = {
        "fullscan": str(Path(args.fullscan).absolute()) if args.fullscan else None,
        "grid": str(Path(args.grid).absolute()) if args.grid else None,
        "errors": errors,
    }
    if errors:
        report["status"] = "BLOCKED"
        report["checks"].append(_record("input manifests", "BLOCKED", "; ".join(errors)))
        report["summary"]["blocked"] += 1
    output = Path(args.output).absolute()
    _write(output, report)
    print("C5_GRID_EQUIVALENCE_STATUS", report["status"])
    print("C5_GRID_EQUIVALENCE_WROTE", str(output))
    return 0 if report["status"] == "PASS" else 1 if report["status"] == "FAIL" else 2


if __name__ == "__main__":
    sys.exit(main())
