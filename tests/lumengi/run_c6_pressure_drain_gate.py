"""Strict C6 pressure drain-window gate.

This checker complements ``run_c6_nextframe_gate.py``.  The next-frame gate
proves an individual request is not published on the request frame; this
gate proves that the host actually observed a bounded drain window after the
last newly introduced event sequence.  It consumes the per-frame
``surfaceCacheEvents`` ledger
from ``surfacecache-effect.json`` and, when supplied, cross-checks the
serialized ``nextframe-gate.json`` ledger.

The gate intentionally has three outcomes:

``PASS``
    Two or more scheduler samples follow the sample that introduced the final
    event sequence and every event is either a valid request -> capture -> ready (and
    optionally first-hit) lifecycle or an explicit terminal stale/rejected
    event.
``BLOCKED``
    Evidence is incomplete: the drain window is too short, an event is still
    requested/captured at the end of the report, or a ledger/sample is absent.
``FAIL``
    An explicit invariant is contradicted: a lifecycle frame regresses, a
    capture occurs on the request frame (or before it), a terminal event lacks
    its stale/rejected marker, or two ledgers disagree.

Aggregate counters are reported only as context.  They never settle an event
and never satisfy the two-sample drain requirement.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
import sys
from typing import Any, Dict, Iterable, List, Optional, Tuple


SCHEMA_VERSION = "C6-pressure-drain-v1"
MIN_DRAIN_SCHEDULER_SAMPLES = 2
STALE_OWNER_REASON_BIT = 2

EVENT_FIELDS = (
    "sceneGeneration",
    "cardID",
    "pageID",
    "generation",
    "requestFrame",
    "captureFrame",
    "readyFrame",
    "firstHitFrame",
    "reasonBits",
    "requestCount",
    "lookupHits",
    "state",
)
IDENTITY_FIELDS = ("sceneGeneration", "cardID", "requestFrame")
PROGRESS_FIELDS = ("captureFrame", "readyFrame", "firstHitFrame", "lookupHits", "requestCount")


def _finite_nonnegative(value: Any) -> Optional[float]:
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number) or number < 0.0:
        return None
    return number


def _integer(value: Any) -> Optional[int]:
    number = _finite_nonnegative(value)
    if number is None or number != float(int(number)):
        return None
    return int(number)


def _phase_name(sample: Dict[str, Any]) -> str:
    return str(sample.get("phase", "default"))


def _sample_frame(sample: Dict[str, Any]) -> Optional[int]:
    value = _integer(sample.get("frame"))
    return value if value is not None else None


def _stats(sample: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    value = sample.get("stats")
    return value if isinstance(value, dict) else None


def _clock(sample: Dict[str, Any]) -> Tuple[Optional[Dict[str, int]], List[str]]:
    """Read the monotonic scheduler clock and request marker from a sample."""

    stats = _stats(sample)
    if stats is None:
        return None, ["stats"]
    values: Dict[str, int] = {}
    missing: List[str] = []
    # schedulerFrameIndex is the clock used for the drain sample count.  The
    # Surface Cache clock is retained as provenance and must be in the same
    # domain; it is not replaced by GI/history frameIndex.
    for name in ("schedulerFrameIndex", "surfaceCacheFrameIndex", "requestObservedFrame"):
        value = _integer(stats.get(name))
        if value is None:
            missing.append("stats.%s" % name)
        else:
            values[name] = value
    if missing:
        return None, missing
    if values["schedulerFrameIndex"] < values["surfaceCacheFrameIndex"]:
        missing.append("schedulerFrameIndex must not precede surfaceCacheFrameIndex")
    return (values if not missing else None), missing


def _ordered_samples(samples: Iterable[Any]) -> Tuple[Dict[str, List[Dict[str, Any]]], List[Dict[str, Any]]]:
    """Partition samples by phase and check host-frame ordering."""

    grouped: Dict[str, List[Dict[str, Any]]] = {}
    violations: List[Dict[str, Any]] = []
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            violations.append({"index": index, "reason": "sample is not an object"})
            continue
        grouped.setdefault(_phase_name(sample), []).append(sample)

    ordered: Dict[str, List[Dict[str, Any]]] = {}
    for phase, phase_samples in grouped.items():
        # Preserve the producer order for the monotonicity check.  Sorting is
        # only a deterministic presentation step; it must not hide a
        # descending/duplicate scheduler sample in the source ledger.
        previous_source_frame: Optional[int] = None
        for sample in phase_samples:
            source_frame = _sample_frame(sample)
            if source_frame is None:
                violations.append({"phase": phase, "reason": "sample frame is missing or invalid"})
                continue
            if previous_source_frame is not None and source_frame <= previous_source_frame:
                violations.append({
                    "phase": phase,
                    "frame": source_frame,
                    "previousFrame": previous_source_frame,
                    "reason": "duplicate or descending source sample frame",
                })
            previous_source_frame = source_frame

        # Sorting makes a report deterministic.  The source-order check above
        # prevents sorting from turning an ambiguous ledger into a pass.
        phase_samples = sorted(
            phase_samples,
            key=lambda item: (_sample_frame(item) is None, _sample_frame(item) or 0),
        )
        ordered[phase] = [sample for sample in phase_samples if _sample_frame(sample) is not None]
    return ordered, violations


def _normalize_event(raw: Any) -> Tuple[Optional[Dict[str, int]], Optional[str]]:
    if not isinstance(raw, dict):
        return None, "event is not an object"
    normalized: Dict[str, int] = {}
    for name in EVENT_FIELDS:
        value = _integer(raw.get(name))
        if value is None:
            return None, "event field is missing or invalid: %s" % name
        normalized[name] = value
    sequence = _integer(raw.get("sequence"))
    if sequence is None or sequence <= 0:
        return None, "event sequence is missing or invalid"
    normalized["sequence"] = sequence
    if normalized["sceneGeneration"] <= 0:
        return None, "event sceneGeneration must be positive"
    if normalized["requestFrame"] <= 0:
        return None, "event requestFrame must be positive"
    if normalized["requestCount"] <= 0:
        return None, "event requestCount must be positive"
    if normalized["state"] not in {1, 2, 3, 4, 5}:
        return None, "event state is outside the Surface Cache lifecycle"
    return normalized, None


def _enabled(case: Dict[str, Any]) -> bool:
    properties = case.get("properties")
    if not isinstance(properties, dict):
        return True
    return bool(properties.get("useSurfaceCache", True) and properties.get("useCacheLighting", True))


def _event_frame_violation(
    event: Dict[str, int],
    *,
    sample_frame: int,
    surface_cache_frame: int,
    phase: str,
) -> List[Dict[str, Any]]:
    """Validate one snapshot's cross-field frame relations."""

    sequence = event["sequence"]
    violations: List[Dict[str, Any]] = []
    request = event["requestFrame"]
    capture = event["captureFrame"]
    ready = event["readyFrame"]
    hit = event["firstHitFrame"]
    for name, value in (
        ("requestFrame", request),
        ("captureFrame", capture),
        ("readyFrame", ready),
        ("firstHitFrame", hit),
    ):
        if value > sample_frame:
            violations.append({
                "phase": phase,
                "sequence": sequence,
                "field": name,
                "reason": "lifecycle frame is in the future of its sample",
                "value": value,
                "sampleFrame": sample_frame,
            })
        if value > surface_cache_frame:
            violations.append({
                "phase": phase,
                "sequence": sequence,
                "field": name,
                "reason": "lifecycle frame is ahead of Surface Cache clock",
                "value": value,
                "surfaceCacheFrameIndex": surface_cache_frame,
            })

    if capture and capture <= request:
        violations.append({
            "phase": phase,
            "sequence": sequence,
            "reason": "capture is not strictly after request (N+1 required)",
            "requestFrame": request,
            "captureFrame": capture,
        })
    elif capture and capture != request + 1:
        # Keep the frozen N+1 threshold exact.  A later frame is not silently
        # accepted as a weaker deferred publication proof.
        violations.append({
            "phase": phase,
            "sequence": sequence,
            "reason": "capture is not on explicit request N+1",
            "requestFrame": request,
            "captureFrame": capture,
        })
    if ready and capture and ready <= capture:
        violations.append({
            "phase": phase,
            "sequence": sequence,
            "reason": "ready is not strictly after capture",
            "captureFrame": capture,
            "readyFrame": ready,
        })
    if hit and ready and hit < ready:
        violations.append({
            "phase": phase,
            "sequence": sequence,
            "reason": "firstHitFrame precedes readyFrame",
            "readyFrame": ready,
            "firstHitFrame": hit,
        })
    if hit and event["lookupHits"] <= 0:
        violations.append({
            "phase": phase,
            "sequence": sequence,
            "reason": "firstHitFrame has no lookupHits",
        })
    if event["lookupHits"] and not hit:
        violations.append({
            "phase": phase,
            "sequence": sequence,
            "reason": "lookupHits has no firstHitFrame",
        })
    return violations


def _lifecycle_status(event: Dict[str, int], *, last_sample_frame: int) -> Tuple[str, List[Dict[str, Any]]]:
    """Classify the final event without using aggregate counters."""

    sequence = event["sequence"]
    state = event["state"]
    violations: List[Dict[str, Any]] = []
    if state == 5:
        # Host state 5 is the only terminal stale/rejected outcome.  The
        # reason bit is mandatory provenance; publication/lookup fields are
        # forbidden.  pageID/generation may remain as the old owner identity.
        if not (event["reasonBits"] & STALE_OWNER_REASON_BIT):
            violations.append({
                "sequence": sequence,
                "reason": "state=5 terminal outcome lacks stale/rejected reason bit",
            })
        if event["captureFrame"] or event["readyFrame"] or event["firstHitFrame"] or event["lookupHits"]:
            violations.append({
                "sequence": sequence,
                "reason": "state=5 terminal outcome has publication or lookup fields",
            })
        return ("FAIL" if violations else "PASS_TERMINAL"), violations

    if state == 1:
        violations.append({
            "sequence": sequence,
            "reason": "request remains unresolved at end of drain window",
            "state": state,
            "lastSampleFrame": last_sample_frame,
        })
        return "BLOCKED", violations
    if state == 2:
        if event["captureFrame"] <= 0:
            violations.append({"sequence": sequence, "reason": "captured state lacks captureFrame"})
        violations.append({
            "sequence": sequence,
            "reason": "captured event remains unready at end of drain window",
            "state": state,
            "lastSampleFrame": last_sample_frame,
        })
        return "BLOCKED", violations
    if state == 3:
        if event["pageID"] <= 0 or event["generation"] <= 0:
            violations.append({"sequence": sequence, "reason": "ready event lacks pageID/generation"})
        if event["captureFrame"] <= 0 or event["readyFrame"] <= 0:
            violations.append({"sequence": sequence, "reason": "ready event lacks capture/ready frame"})
        if event["firstHitFrame"] or event["lookupHits"]:
            violations.append({"sequence": sequence, "reason": "state=3 ready/no-hit event has lookup fields"})
        return ("FAIL" if violations else "PASS_READY"), violations
    if state == 4:
        if event["pageID"] <= 0 or event["generation"] <= 0:
            violations.append({"sequence": sequence, "reason": "hit event lacks pageID/generation"})
        if event["captureFrame"] <= 0 or event["readyFrame"] <= 0:
            violations.append({"sequence": sequence, "reason": "hit event lacks capture/ready frame"})
        if event["firstHitFrame"] <= 0 or event["lookupHits"] <= 0:
            violations.append({"sequence": sequence, "reason": "hit event lacks firstHitFrame/lookupHits"})
        return ("FAIL" if violations else "PASS_HIT"), violations
    return "FAIL", [{"sequence": sequence, "reason": "unsupported lifecycle state", "state": state}]


def _evaluate_phase(phase: str, samples: List[Dict[str, Any]]) -> Dict[str, Any]:
    """Evaluate one phase, including its explicit scheduler drain window."""

    violations: List[Dict[str, Any]] = []
    blocked: List[Dict[str, Any]] = []
    snapshots: List[Dict[str, Any]] = []
    previous_clock: Optional[Dict[str, int]] = None
    previous_sample_frame: Optional[int] = None
    last_observed: Optional[int] = None
    last_new_request_scheduler: Optional[int] = None
    last_new_request_sample_frame: Optional[int] = None
    last_new_request_sequence: Optional[int] = None
    last_new_request_event: Optional[Dict[str, int]] = None
    seen_sequences: set[int] = set()
    current_events: Dict[int, Dict[str, int]] = {}
    previous_events: Dict[int, Dict[str, int]] = {}
    previous_sequences: Optional[set[int]] = None
    dropped_max = 0.0

    for sample in samples:
        frame = _sample_frame(sample)
        # _ordered_samples filtered invalid frames; this is defensive for
        # callers importing evaluate_case directly.
        if frame is None:
            blocked.append({"reason": "sample frame is missing or invalid"})
            continue
        clock, missing = _clock(sample)
        if clock is None:
            blocked.append({"frame": frame, "reason": "missing scheduler/request clock", "fields": missing})
            continue
        if previous_sample_frame is not None and frame <= previous_sample_frame:
            violations.append({"phase": phase, "frame": frame, "reason": "sample frame regressed"})
        if previous_clock is not None:
            if clock["schedulerFrameIndex"] <= previous_clock["schedulerFrameIndex"]:
                violations.append({
                    "phase": phase,
                    "frame": frame,
                    "reason": "schedulerFrameIndex is not strictly increasing",
                    "previous": previous_clock["schedulerFrameIndex"],
                    "current": clock["schedulerFrameIndex"],
                })
            if clock["surfaceCacheFrameIndex"] < previous_clock["surfaceCacheFrameIndex"]:
                violations.append({
                    "phase": phase,
                    "frame": frame,
                    "reason": "surfaceCacheFrameIndex regressed",
                })
        previous_sample_frame = frame
        previous_clock = clock
        last_observed = clock["requestObservedFrame"] if last_observed is None else max(last_observed, clock["requestObservedFrame"])
        stats = _stats(sample) or {}
        dropped = _finite_nonnegative(stats.get("surfaceCacheEventDropped"))
        if dropped is not None:
            dropped_max = max(dropped_max, dropped)

        records = sample.get("surfaceCacheEvents")
        if records is None:
            # A sample before the first request may omit the ledger, but once
            # it appears every later sample must carry it to prove no event was
            # silently skipped during the drain window.
            if current_events:
                blocked.append({"phase": phase, "frame": frame, "reason": "surfaceCacheEvents missing"})
            snapshots.append({"frame": frame, "schedulerFrameIndex": clock["schedulerFrameIndex"], "eventCount": 0})
            continue
        if not isinstance(records, list):
            violations.append({"phase": phase, "frame": frame, "reason": "surfaceCacheEvents is not a list"})
            continue
        event_count = _finite_nonnegative(stats.get("surfaceCacheEventCount"))
        if event_count is not None and event_count != float(len(records)):
            violations.append({
                "phase": phase,
                "frame": frame,
                "reason": "surfaceCacheEventCount does not match ledger length",
                "reported": event_count,
                "exported": len(records),
            })
        normalized_records: Dict[int, Dict[str, int]] = {}
        for raw in records:
            event, error = _normalize_event(raw)
            if error is not None:
                violations.append({"phase": phase, "frame": frame, "reason": error})
                continue
            assert event is not None
            sequence = event["sequence"]
            if sequence in normalized_records:
                violations.append({"phase": phase, "frame": frame, "sequence": sequence, "reason": "duplicate event sequence in sample"})
                continue
            normalized_records[sequence] = event
            violations.extend(_event_frame_violation(
                event,
                sample_frame=frame,
                surface_cache_frame=clock["surfaceCacheFrameIndex"],
                phase=phase,
            ))
            old = previous_events.get(sequence)
            if old is not None:
                for name in IDENTITY_FIELDS:
                    if event[name] != old[name]:
                        violations.append({
                            "phase": phase,
                            "frame": frame,
                            "sequence": sequence,
                            "field": name,
                            "reason": "event identity changed",
                            "old": old[name],
                            "new": event[name],
                        })
                for name in PROGRESS_FIELDS:
                    if event[name] < old[name]:
                        violations.append({
                            "phase": phase,
                            "frame": frame,
                            "sequence": sequence,
                            "field": name,
                            "reason": "event progress regressed",
                            "old": old[name],
                            "new": event[name],
                        })
                for name in ("pageID", "generation"):
                    if old[name] and event[name] != old[name]:
                        violations.append({
                            "phase": phase,
                            "frame": frame,
                            "sequence": sequence,
                            "field": name,
                            "reason": "assigned page identity changed",
                            "old": old[name],
                            "new": event[name],
                        })
                if (event["reasonBits"] | old["reasonBits"]) != event["reasonBits"]:
                    violations.append({
                        "phase": phase,
                        "frame": frame,
                        "sequence": sequence,
                        "field": "reasonBits",
                        "reason": "reason bits regressed",
                    })
                if old["state"] == 5 and event["state"] != 5:
                    violations.append({
                        "phase": phase,
                        "frame": frame,
                        "sequence": sequence,
                        "field": "state",
                        "reason": "terminal stale/rejected event reopened",
                    })
                elif old["state"] != 5 and event["state"] < old["state"]:
                    violations.append({
                        "phase": phase,
                        "frame": frame,
                        "sequence": sequence,
                        "field": "state",
                        "reason": "lifecycle state regressed",
                    })
        current_sequence_set = set(normalized_records)
        if previous_sequences is not None and not previous_sequences.issubset(current_sequence_set):
            if dropped_max <= 0.0:
                blocked.append({
                    "phase": phase,
                    "frame": frame,
                    "reason": "event ring lost records without explicit drop telemetry",
                })
        # A request marker is a newly introduced event sequence, not the
        # requestObservedFrame readback stamp.  The latter can advance every
        # tail frame while the ledger is unchanged (GPU readback timing), so
        # using it as the drain anchor would make a quiet tail look like an
        # endless stream of new requests.
        new_sequences = current_sequence_set - seen_sequences
        if new_sequences:
            last_new_request_sequence = max(new_sequences)
            last_new_request_event = normalized_records[last_new_request_sequence]
            last_new_request_scheduler = clock["schedulerFrameIndex"]
            last_new_request_sample_frame = frame
            seen_sequences.update(new_sequences)
        previous_sequences = current_sequence_set
        previous_events = normalized_records
        current_events.update(normalized_records)
        snapshots.append({
            "frame": frame,
            "schedulerFrameIndex": clock["schedulerFrameIndex"],
            "requestObservedFrame": clock["requestObservedFrame"],
            "eventCount": len(normalized_records),
            "tailSample": bool(sample.get("tailSample", False)),
        })

    scheduler_after = []
    if last_new_request_scheduler is not None:
        scheduler_after = [
            item for item in snapshots
            if item.get("schedulerFrameIndex") is not None
            and int(item["schedulerFrameIndex"]) > last_new_request_scheduler
        ]
    scheduler_after_observed = []
    if last_observed is not None:
        scheduler_after_observed = [
            item for item in snapshots
            if item.get("schedulerFrameIndex") is not None
            and int(item["schedulerFrameIndex"]) > last_observed
        ]
    drain_status = "PASS" if len(scheduler_after) >= MIN_DRAIN_SCHEDULER_SAMPLES else "BLOCKED"
    drain_record = {
        "status": drain_status,
        "requiredSchedulerSamples": MIN_DRAIN_SCHEDULER_SAMPLES,
        "lastNewRequestSequence": last_new_request_sequence,
        "lastNewRequestRequestFrame": (
            last_new_request_event.get("requestFrame") if last_new_request_event else None
        ),
        "lastNewRequestSampleFrame": last_new_request_sample_frame,
        "lastNewRequestSchedulerFrame": last_new_request_scheduler,
        "lastRequestObservedFrame": last_observed,
        "schedulerSamplesAfterLastNewRequest": scheduler_after,
        # requestObservedFrame is retained as a diagnostic readback stamp.
        # It is intentionally not the drain anchor because it can advance
        # while the event sequence remains unchanged.
        "schedulerSamplesAfterLastRequestObserved": scheduler_after_observed,
        "observedSchedulerSampleCount": len(scheduler_after),
    }
    if last_new_request_scheduler is None:
        blocked.append({"phase": phase, "reason": "no newly observed event sequence for drain anchor"})
    elif len(scheduler_after) < MIN_DRAIN_SCHEDULER_SAMPLES:
        blocked.append({
            "phase": phase,
            "reason": "drain window has fewer than two scheduler samples after last new event sequence",
            "lastNewRequestSequence": last_new_request_sequence,
            "lastNewRequestSchedulerFrame": last_new_request_scheduler,
            "lastRequestObservedFrame": last_observed,
            "observedSchedulerSampleCount": len(scheduler_after),
        })
    if dropped_max > 0.0:
        # A nonzero ring-drop counter is itself evidence that at least one
        # request is unavailable to this per-event gate.  Aggregate drop
        # telemetry cannot settle that missing lifecycle.
        blocked.append({
            "phase": phase,
            "reason": "surfaceCacheEventDropped is nonzero; per-event ledger is incomplete",
            "surfaceCacheEventDroppedMax": dropped_max,
        })

    checked: List[Dict[str, Any]] = []
    pending: List[int] = []
    statuses: List[str] = []
    last_sample_frame = max((_sample_frame(sample) or 0 for sample in samples), default=0)
    for sequence, event in sorted(current_events.items()):
        status, event_violations = _lifecycle_status(event, last_sample_frame=last_sample_frame)
        checked.append({"sequence": sequence, **event, "status": status})
        statuses.append(status)
        for violation in event_violations:
            if status == "BLOCKED":
                blocked.append({"phase": phase, **violation})
            else:
                violations.append({"phase": phase, **violation})
        if status == "BLOCKED":
            pending.append(sequence)

    if not current_events:
        blocked.append({"phase": phase, "reason": "no per-event surfaceCacheEvents records"})
    if any(status == "FAIL" for status in statuses) or violations:
        status = "FAIL"
    elif blocked or drain_status != "PASS" or any(status == "BLOCKED" for status in statuses):
        status = "BLOCKED"
    elif all(item in {"PASS_TERMINAL", "PASS_READY", "PASS_HIT"} for item in statuses):
        status = "PASS"
    else:
        status = "BLOCKED"
    return {
        "status": status,
        "sampleCount": len(samples),
        "lastSampleFrame": last_sample_frame,
        "drainWindow": drain_record,
        "events": checked,
        "pendingSequences": pending,
        "violations": violations,
        "blockedReasons": blocked,
        "aggregateCountersUsedForCompletion": False,
        "aggregateContext": {"surfaceCacheEventDroppedMax": dropped_max},
    }


def _cross_check_nextframe(
    case_report: Dict[str, Any],
    next_case: Optional[Dict[str, Any]],
    *,
    phase: str,
) -> Dict[str, Any]:
    """Cross-check event fields against an optional next-frame ledger."""

    if next_case is None:
        return {"provided": False, "status": "NOT_PROVIDED"}
    next_events = next_case.get("events")
    if not isinstance(next_events, list) or not next_events:
        return {
            "provided": True,
            "status": "BLOCKED",
            "reason": "nextframe ledger has no per-event records",
        }
    source_map = {int(item["sequence"]): item for item in case_report.get("events", []) if "sequence" in item}
    ledger_map: Dict[int, Dict[str, Any]] = {}
    mismatches: List[Dict[str, Any]] = []
    for item in next_events:
        if not isinstance(item, dict):
            mismatches.append({"reason": "nextframe event is not an object"})
            continue
        sequence = _integer(item.get("sequence"))
        if sequence is None:
            mismatches.append({"reason": "nextframe event sequence is invalid"})
            continue
        ledger_map[sequence] = item
    for sequence, source in source_map.items():
        other = ledger_map.get(sequence)
        if other is None:
            mismatches.append({"sequence": sequence, "reason": "nextframe ledger is missing source event"})
            continue
        for name in ("sceneGeneration", "cardID", "pageID", "generation", "requestFrame", "captureFrame", "readyFrame", "firstHitFrame", "reasonBits", "requestCount", "lookupHits", "state"):
            left = _integer(source.get(name))
            right = _integer(other.get(name))
            if left is None or right is None or left != right:
                mismatches.append({
                    "sequence": sequence,
                    "field": name,
                    "reason": "surfacecache and nextframe ledgers disagree",
                    "surfacecache": source.get(name),
                    "nextframe": other.get(name),
                })
    for sequence in sorted(set(ledger_map) - set(source_map)):
        mismatches.append({"sequence": sequence, "reason": "nextframe ledger has event absent from surfacecache ledger"})
    result = {
        "provided": True,
        "status": "FAIL" if mismatches else "PASS",
        "eventCount": len(ledger_map),
        "mismatches": mismatches,
        "nextframeCaseStatus": next_case.get("status"),
    }
    # When the caller supplies the next-frame ledger explicitly, its own
    # status is part of the frozen dependency.  A BLOCKED next-frame result
    # cannot be promoted by this drain checker, even if the surviving event
    # records happen to compare equal.
    if not mismatches and next_case.get("status") == "FAIL":
        result["status"] = "FAIL"
        result["reason"] = "supplied nextframe case is FAIL"
    elif not mismatches and next_case.get("status") == "BLOCKED":
        result["status"] = "BLOCKED"
        result["reason"] = "supplied nextframe case is BLOCKED"
    return result


def evaluate_report(report: Dict[str, Any], nextframe: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Evaluate a surfacecache-effect report and optional nextframe ledger."""

    cases = report.get("cases") if isinstance(report, dict) else None
    if not isinstance(cases, dict) or not cases:
        return {
            "schemaVersion": SCHEMA_VERSION,
            "status": "BLOCKED",
            "reason": "missing cases object with per-frame samples",
            "cases": {},
            "drainWindow": {"requiredSchedulerSamplesAfterLastNewRequest": MIN_DRAIN_SCHEDULER_SAMPLES},
            "hostTelemetryOnly": True,
            "imageInference": False,
        }

    case_reports: Dict[str, Any] = {}
    enabled_statuses: List[str] = []
    cross_checks: Dict[str, Any] = {}
    next_cases = nextframe.get("cases") if isinstance(nextframe, dict) else {}
    if nextframe is not None and not isinstance(next_cases, dict):
        return {
            "schemaVersion": SCHEMA_VERSION,
            "status": "FAIL",
            "reason": "nextframe ledger cases is not an object",
            "cases": {},
            "hostTelemetryOnly": True,
            "imageInference": False,
        }

    for name, case in cases.items():
        if not isinstance(case, dict):
            case_reports[name] = {"status": "BLOCKED", "reason": "case is not an object"}
            enabled_statuses.append("BLOCKED")
            continue
        if not _enabled(case):
            case_reports[name] = {"status": "NOT_APPLICABLE", "reason": "cache disabled"}
            continue
        samples = case.get("samples")
        if not isinstance(samples, list) or not samples:
            case_reports[name] = {"status": "BLOCKED", "reason": "missing per-frame sample ledger"}
            enabled_statuses.append("BLOCKED")
            continue
        ordered, ordering_violations = _ordered_samples(samples)
        phase_reports: Dict[str, Any] = {}
        for phase, phase_samples in ordered.items():
            phase_reports[phase] = _evaluate_phase(phase, phase_samples)
            phase_reports[phase]["structuralViolations"] = [
                item for item in ordering_violations if item.get("phase") == phase
            ]
            if phase_reports[phase]["structuralViolations"]:
                phase_reports[phase]["status"] = "FAIL"
                phase_reports[phase]["violations"].extend(phase_reports[phase]["structuralViolations"])
        if not phase_reports:
            result = {"status": "BLOCKED", "reason": "no valid phase samples"}
        elif any(item["status"] == "FAIL" for item in phase_reports.values()):
            result = {"status": "FAIL", "reason": "one or more phases contain an explicit invariant violation"}
        elif any(item["status"] == "BLOCKED" for item in phase_reports.values()):
            result = {"status": "BLOCKED", "reason": "one or more phases lack a complete drain window or lifecycle"}
        else:
            result = {"status": "PASS", "reason": None}
        result["phases"] = phase_reports
        result["aggregateCountersUsedForCompletion"] = False
        next_case = next_cases.get(name) if isinstance(next_cases, dict) else None
        cross = _cross_check_nextframe(
            # The cross-check is over all phases' final event reports.
            {"events": [event for phase in phase_reports.values() for event in phase.get("events", [])]},
            next_case,
            phase="*",
        )
        cross_checks[name] = cross
        result["nextframeCrossCheck"] = cross
        if cross["status"] == "FAIL":
            result["status"] = "FAIL"
            result["reason"] = "surfacecache and nextframe event ledgers disagree"
        elif cross["status"] == "BLOCKED" and result["status"] == "PASS":
            result["status"] = "BLOCKED"
            result["reason"] = "nextframe ledger was supplied but lacks per-event evidence"
        case_reports[name] = result
        enabled_statuses.append(result["status"])

    if not enabled_statuses:
        status = "BLOCKED"
        reason = "no cache-enabled case"
    elif any(item == "FAIL" for item in enabled_statuses):
        status = "FAIL"
        reason = "explicit C6 drain invariant failed"
    elif any(item == "BLOCKED" for item in enabled_statuses):
        status = "BLOCKED"
        reason = "C6 drain evidence is incomplete"
    else:
        status = "PASS"
        reason = None
    return {
        "schemaVersion": SCHEMA_VERSION,
        "status": status,
        "reason": reason,
        "cases": case_reports,
        "nextframeCrossCheck": cross_checks,
        "required": {
            "minSchedulerSamplesAfterLastNewRequestSequence": MIN_DRAIN_SCHEDULER_SAMPLES,
            "captureFrame": "requestFrame + 1",
            "terminalState": 5,
            "terminalReasonBit": STALE_OWNER_REASON_BIT,
        },
        "aggregateCountersUsedForCompletion": False,
        "hostTelemetryOnly": True,
        "imageInference": False,
    }


def _event(sequence: int, *, state: int, request: int, capture: int = 0, ready: int = 0, hit: int = 0, lookup: int = 0, card: int = 1, page: int = 1, generation: int = 1, reason: int = 4) -> Dict[str, int]:
    return {
        "sequence": sequence,
        "sceneGeneration": 1,
        "cardID": card,
        "pageID": page,
        "generation": generation,
        "requestFrame": request,
        "captureFrame": capture,
        "readyFrame": ready,
        "firstHitFrame": hit,
        "reasonBits": reason,
        "requestCount": 1,
        "lookupHits": lookup,
        "state": state,
    }


def _fixture_sample(frame: int, events: List[Dict[str, int]], *, observed: int, tail: bool = False) -> Dict[str, Any]:
    return {
        "phase": "pressure",
        "frame": frame,
        "tailSample": tail,
        "surfaceCacheEvents": copy.deepcopy(events),
        "stats": {
            "schedulerFrameIndex": frame,
            "surfaceCacheFrameIndex": frame - 1,
            "requestObservedFrame": observed,
            "surfaceCacheEventCount": len(events),
            "surfaceCacheEventDropped": 0,
        },
    }


def _fixture_report(*, drain_samples: int = 3, unresolved: bool = False, invalid_capture: bool = False) -> Dict[str, Any]:
    e1 = _event(1, state=1, request=1)
    samples = [_fixture_sample(1, [], observed=0)]
    e1_requested = copy.deepcopy(e1)
    e1_capture = copy.deepcopy(e1)
    e1_capture.update({"state": 2, "captureFrame": 2, "pageID": 1, "generation": 1})
    if invalid_capture:
        e1_capture["captureFrame"] = 1
    e1_ready = copy.deepcopy(e1_capture)
    e1_ready.update({"state": 3, "readyFrame": 3})
    e1_hit = copy.deepcopy(e1_ready)
    e1_hit.update({"state": 4, "firstHitFrame": 4, "lookupHits": 1})
    e2_terminal = _event(2, state=5, request=4, card=2, page=0, generation=0, reason=STALE_OWNER_REASON_BIT)
    samples.extend([
        _fixture_sample(2, [e1_requested], observed=1),
        _fixture_sample(3, [e1_capture], observed=2),
        _fixture_sample(4, [e1_capture if unresolved else e1_ready], observed=3),
        _fixture_sample(5, [e1_capture if unresolved else e1_hit], observed=4),
    ])
    # Introduce a terminal stale/rejected event at the final active frame so
    # the self-test can distinguish a real N+2 drain from earlier samples.
    samples[-1]["surfaceCacheEvents"].append(copy.deepcopy(e2_terminal))
    samples[-1]["stats"]["surfaceCacheEventCount"] = len(samples[-1]["surfaceCacheEvents"])
    final_observed = 5
    for index in range(max(0, drain_samples - 1)):
        tail_events = [e1_capture if unresolved else e1_hit, e2_terminal]
        samples.append(_fixture_sample(6 + index, tail_events, observed=final_observed, tail=index == drain_samples - 2))
    return {
        "cases": {
            "pressure": {
                "properties": {"useSurfaceCache": True, "useCacheLighting": True},
                "samples": samples,
            }
        }
    }


def _self_test() -> int:
    passed = evaluate_report(_fixture_report())
    assert passed["status"] == "PASS", passed
    assert passed["cases"]["pressure"]["phases"]["pressure"]["drainWindow"]["observedSchedulerSampleCount"] >= 2

    # requestObservedFrame is allowed to advance in a quiet tail while the
    # per-event sequence stays constant.  It must remain diagnostic rather
    # than moving the strict drain anchor forward.
    quiet_tail = _fixture_report()
    for sample in quiet_tail["cases"]["pressure"]["samples"][-2:]:
        sample["stats"]["requestObservedFrame"] += 100
    quiet_result = evaluate_report(quiet_tail)
    assert quiet_result["status"] == "PASS", quiet_result

    blocked_window = evaluate_report(_fixture_report(drain_samples=1))
    assert blocked_window["status"] == "BLOCKED", blocked_window
    blocked_tail = evaluate_report(_fixture_report(unresolved=True))
    assert blocked_tail["status"] == "BLOCKED", blocked_tail

    failed = evaluate_report(_fixture_report(invalid_capture=True))
    assert failed["status"] == "FAIL", failed

    terminal = _event(1, state=5, request=1, card=1, page=1, generation=1, reason=0)
    terminal_report = _fixture_report()
    terminal_samples = terminal_report["cases"]["pressure"]["samples"]
    terminal_samples[-1]["surfaceCacheEvents"] = [terminal]
    terminal_samples[-1]["stats"]["surfaceCacheEventCount"] = 1
    # The sequence changed from 1 to 2 at the final snapshot, so this must be
    # an explicit FAIL rather than a falsely completed aggregate count.
    assert evaluate_report(terminal_report)["status"] == "FAIL"

    print("C6_PRESSURE_DRAIN_FIXTURE PASS pass=PASS blocked=BLOCKED fail=FAIL")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="surfacecache-effect.json report")
    parser.add_argument("--nextframe", "--nextframe-ledger", dest="nextframe", help="optional nextframe-gate.json ledger")
    parser.add_argument("--output", help="write drain gate JSON")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    if not args.input:
        parser.error("--input is required unless --self-test is used")
    try:
        with open(os.path.abspath(args.input), "r", encoding="utf-8") as stream:
            report = json.load(stream)
    except (OSError, ValueError) as exc:
        print("C6_PRESSURE_DRAIN BLOCKED:", exc)
        return 1
    nextframe = None
    if args.nextframe:
        try:
            with open(os.path.abspath(args.nextframe), "r", encoding="utf-8") as stream:
                nextframe = json.load(stream)
        except (OSError, ValueError) as exc:
            print("C6_PRESSURE_DRAIN BLOCKED:", exc)
            return 1
    result = evaluate_report(report, nextframe)
    output_path = os.path.abspath(args.output) if args.output else os.path.splitext(os.path.abspath(args.input))[0] + "-drain-gate.json"
    output_dir = os.path.dirname(output_path) or "."
    os.makedirs(output_dir, exist_ok=True)
    with open(output_path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    print("C6_PRESSURE_DRAIN_GATE", result["status"], output_path)
    return {"PASS": 0, "BLOCKED": 1, "FAIL": 2}.get(result["status"], 2)


if __name__ == "__main__":
    sys.exit(main())
