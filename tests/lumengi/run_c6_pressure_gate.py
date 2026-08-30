"""Strict per-card C6 pressure/lifecycle gate.

This checker is deliberately narrower than the normal C6 next-frame checker.
It is the release gate for a *tiny* Surface Cache budget.  Aggregate counters
(``evictions``, ``staleOwnerRejects`` and friends) are used only to establish
that the run really exercised pressure.  They never stand in for a
card/page event.  Every accepted request must be represented by a
``surfaceCacheEvents`` record and is checked independently through

    request -> capture -> ready -> firstHit

or an explicit terminal stale-owner event (``state == 5``).  An unresolved
request is allowed only in the explicitly marked one-frame tail; it is
reported as ``PENDING_TAIL`` and is not silently counted as a pass.

The current host event ABI exposes request reason bits but does not yet emit
the terminal state-5 stale-owner outcome.  Consequently the current pressure
artifacts are expected to be ``BLOCKED`` until that per-card outcome and a
real page-generation reuse are observed.  ``--self-test`` contains a strict
PASS fixture as well as the missing-evidence fixtures.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple


SCHEMA_VERSION = "C6-pressure-nextframe-v1"
TAIL_WINDOW = 1
STALE_OWNER_REASON_BIT = 2

EVENT_FIELDS = (
    "sequence",
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

PRESSURE_STATS = (
    "totalPages",
    "evictions",
    "freePages",
    "schedAllocFailures",
    "generationMismatchRejects",
    "staleOwnerRejects",
    "surfaceCacheEventDropped",
)

_CUMULATIVE_STATS = {
    "evictions",
    "schedAllocFailures",
    "generationMismatchRejects",
    "staleOwnerRejects",
    "surfaceCacheEventDropped",
}


def _number(value: Any) -> Optional[float]:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(result) or result < 0.0:
        return None
    return result


def _integer(value: Any) -> Optional[int]:
    number = _number(value)
    if number is None or number != float(int(number)):
        return None
    return int(number)


def _frame(sample: Dict[str, Any]) -> Optional[int]:
    value = _integer(sample.get("frame"))
    return value if value is not None and value >= 0 else None


def _enabled(case: Dict[str, Any]) -> bool:
    properties = case.get("properties")
    if not isinstance(properties, dict):
        return True
    return bool(properties.get("useSurfaceCache", True) and properties.get("useCacheLighting", True))


def _stats_value(sample: Dict[str, Any], name: str) -> Optional[float]:
    stats = sample.get("stats")
    if isinstance(stats, dict) and name in stats:
        return _number(stats.get(name))
    # Newer normalized reports may retain the source value under fields.  It
    # is accepted only as an explicitly named host field, never from images.
    telemetry = sample.get("pageTelemetry")
    fields = telemetry.get("fields") if isinstance(telemetry, dict) else None
    entry = fields.get(name) if isinstance(fields, dict) else None
    if isinstance(entry, dict):
        return _number(entry.get("value"))
    return None


def _ordered_samples(samples: Iterable[Any]) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    ordered: List[Dict[str, Any]] = []
    errors: List[Dict[str, Any]] = []
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            errors.append({"index": index, "reason": "sample is not an object"})
            continue
        phase = str(sample.get("phase", "default"))
        grouped.setdefault(phase, []).append(sample)
    for phase, phase_samples in grouped.items():
        phase_samples.sort(key=lambda item: (_frame(item) is None, _frame(item) or 0))
        previous: Optional[int] = None
        for sample in phase_samples:
            current = _frame(sample)
            if current is None:
                errors.append({"phase": phase, "reason": "missing or invalid frame"})
                continue
            if previous is not None and current <= previous:
                errors.append(
                    {
                        "phase": phase,
                        "frame": current,
                        "previousFrame": previous,
                        "reason": "duplicate or descending frame",
                    }
                )
            previous = current
            copy = dict(sample)
            copy["_gatePhase"] = phase
            ordered.append(copy)
    ordered.sort(key=lambda item: (str(item.get("_gatePhase", "default")), _frame(item) or 0))
    return ordered, errors


def _normalize_event(record: Any) -> Tuple[Optional[Dict[str, int]], Optional[str]]:
    if not isinstance(record, dict):
        return None, "surfaceCacheEvents contains a non-object"
    normalized: Dict[str, int] = {}
    for name in EVENT_FIELDS:
        value = _integer(record.get(name))
        if value is None:
            return None, "event field is missing or invalid: %s" % name
        normalized[name] = value
    if normalized["sequence"] <= 0:
        return None, "event sequence must be positive"
    if normalized["sceneGeneration"] <= 0:
        return None, "event sceneGeneration must be positive"
    if normalized["requestFrame"] <= 0:
        return None, "event requestFrame must be positive"
    if normalized["requestCount"] <= 0:
        return None, "event requestCount must be positive"
    if normalized["state"] not in {1, 2, 3, 4, 5}:
        return None, "event state is outside the Surface Cache lifecycle"
    return normalized, None


def _event_snapshots(samples: List[Dict[str, Any]]) -> Tuple[Dict[int, Dict[str, int]], List[Dict[str, Any]], List[Dict[str, Any]], int]:
    latest: Dict[int, Dict[str, int]] = {}
    errors: List[Dict[str, Any]] = []
    history_errors: List[Dict[str, Any]] = []
    previous_sequences: Optional[Set[int]] = None
    previous_values: Dict[int, Dict[str, int]] = {}
    max_frame = max((_frame(sample) or 0 for sample in samples), default=0)

    for sample in samples:
        records = sample.get("surfaceCacheEvents")
        frame = _frame(sample)
        if records is None:
            errors.append({"frame": frame, "reason": "surfaceCacheEvents is missing"})
            continue
        if not isinstance(records, list):
            errors.append({"frame": frame, "reason": "surfaceCacheEvents is not a list"})
            continue
        current_sequences: Set[int] = set()
        for raw in records:
            event, error = _normalize_event(raw)
            if error is not None:
                errors.append({"frame": frame, "reason": error})
                continue
            assert event is not None
            sequence = event["sequence"]
            if sequence in current_sequences:
                errors.append({"frame": frame, "sequence": sequence, "reason": "duplicate event sequence in sample"})
                continue
            current_sequences.add(sequence)
            old = previous_values.get(sequence)
            if old is not None:
                # A request event is updated in place as the scheduler moves it
                # through the lifecycle. Identity cannot change and progress
                # cannot regress. A page assignment is the only 0 -> nonzero
                # transition permitted for pageID/generation.
                for name in ("sceneGeneration", "cardID", "requestFrame"):
                    if event[name] != old[name]:
                        history_errors.append({"frame": frame, "sequence": sequence, "field": name, "reason": "event identity changed"})
                for name in ("captureFrame", "readyFrame", "firstHitFrame", "lookupHits", "requestCount"):
                    if event[name] < old[name]:
                        history_errors.append({"frame": frame, "sequence": sequence, "field": name, "reason": "event progress regressed"})
                if old["pageID"] and event["pageID"] != old["pageID"]:
                    history_errors.append({"frame": frame, "sequence": sequence, "field": "pageID", "reason": "assigned page changed"})
                if old["generation"] and event["generation"] != old["generation"]:
                    history_errors.append({"frame": frame, "sequence": sequence, "field": "generation", "reason": "assigned generation changed"})
                if (event["reasonBits"] | old["reasonBits"]) != event["reasonBits"]:
                    history_errors.append({"frame": frame, "sequence": sequence, "field": "reasonBits", "reason": "reason bits regressed"})
                if old["state"] != 5 and event["state"] < old["state"]:
                    history_errors.append({"frame": frame, "sequence": sequence, "field": "state", "reason": "lifecycle state regressed"})
            previous_values[sequence] = event
            latest[sequence] = event
        if previous_sequences is not None and len(current_sequences) < len(previous_sequences):
            # A bounded ring may shrink only when the host also reports an
            # explicit dropped-event count. The aggregate is checked below.
            history_errors.append({"frame": frame, "reason": "event ring lost records between samples"})
        previous_sequences = current_sequences

    return latest, errors, history_errors, max_frame


def _event_lifecycle(
    events: Dict[int, Dict[str, int]],
    *,
    max_frame: int,
    tail_marked: bool,
) -> Dict[str, Any]:
    checked: List[Dict[str, Any]] = []
    violations: List[Dict[str, Any]] = []
    pending_tail: List[int] = []
    stale_outcomes: List[int] = []
    settled: List[Dict[str, int]] = []

    if events:
        sequences = sorted(events)
        expected = list(range(sequences[0], sequences[-1] + 1))
        if sequences != expected:
            violations.append({"reason": "surfaceCacheEvents sequence gap", "sequences": sequences})

    for sequence, event in sorted(events.items()):
        request = event["requestFrame"]
        capture = event["captureFrame"]
        ready = event["readyFrame"]
        first_hit = event["firstHitFrame"]
        page = event["pageID"]
        generation = event["generation"]
        state = event["state"]
        item: Dict[str, Any] = {"sequence": sequence, **event, "status": "BLOCKED"}

        if capture and capture <= request:
            violations.append({"sequence": sequence, "reason": "capture is not strictly after request", "requestFrame": request, "captureFrame": capture})
        if ready and capture and ready <= capture:
            violations.append({"sequence": sequence, "reason": "ready is not strictly after capture", "captureFrame": capture, "readyFrame": ready})
        if first_hit and ready and first_hit < ready:
            violations.append({"sequence": sequence, "reason": "firstHitFrame precedes readyFrame", "readyFrame": ready, "firstHitFrame": first_hit})
        if first_hit and event["lookupHits"] <= 0:
            violations.append({"sequence": sequence, "reason": "firstHitFrame has no lookupHits"})
        if event["lookupHits"] and not first_hit:
            violations.append({"sequence": sequence, "reason": "lookupHits has no firstHitFrame"})

        if state == 5:
            if not (event["reasonBits"] & STALE_OWNER_REASON_BIT):
                violations.append({"sequence": sequence, "reason": "state=5 stale outcome lacks stale-owner reason bit"})
            if capture or ready or first_hit:
                violations.append({"sequence": sequence, "reason": "stale-owner outcome has publication fields"})
            else:
                item["status"] = "STALE_REJECT"
                stale_outcomes.append(sequence)
            checked.append(item)
            continue

        if state == 1:
            if page or generation:
                violations.append({"sequence": sequence, "reason": "requested event has page/generation before capture"})
            if not capture and not ready and not first_hit:
                if request >= max_frame - TAIL_WINDOW:
                    if not tail_marked:
                        violations.append({"sequence": sequence, "reason": "tail pending lacks explicit tailSample marker"})
                    else:
                        item["status"] = "PENDING_TAIL"
                        pending_tail.append(sequence)
                else:
                    violations.append({"sequence": sequence, "reason": "non-tail request remains unresolved", "requestFrame": request, "lastSampleFrame": max_frame})
            else:
                violations.append({"sequence": sequence, "reason": "state=1 has publication fields"})
            checked.append(item)
            continue

        # States 2/3/4 are published page states and must carry a concrete
        # page/generation. This is intentionally card-specific; no aggregate
        # page count can satisfy it.
        if page <= 0 or generation <= 0:
            violations.append({"sequence": sequence, "reason": "published event lacks nonzero pageID/generation"})
        if not capture:
            violations.append({"sequence": sequence, "reason": "published event lacks captureFrame"})
        if state == 2 and (ready or first_hit):
            violations.append({"sequence": sequence, "reason": "state=2 has ready/firstHit fields"})
        if state == 2 and not ready:
            if capture >= max_frame - TAIL_WINDOW and tail_marked:
                item["status"] = "PENDING_TAIL"
                pending_tail.append(sequence)
            else:
                violations.append({"sequence": sequence, "reason": "captured event remains unready outside the explicit tail", "captureFrame": capture, "lastSampleFrame": max_frame})
        if state == 3 and (not ready or first_hit):
            violations.append({"sequence": sequence, "reason": "state=3 does not match ready/no-hit fields"})
        if state == 4 and (not ready or not first_hit or event["lookupHits"] <= 0):
            violations.append({"sequence": sequence, "reason": "state=4 does not match hit fields"})
        if capture and capture >= max_frame - TAIL_WINDOW and not ready:
            if tail_marked:
                item["status"] = "PENDING_TAIL"
                if sequence not in pending_tail:
                    pending_tail.append(sequence)
        if item["status"] == "BLOCKED" and capture and ready:
            item["status"] = "PASS"
            settled.append(event)
        checked.append(item)

    return {
        "events": checked,
        "settledEvents": settled,
        "tailPendingSequences": pending_tail,
        "staleOwnerOutcomeSequences": stale_outcomes,
        "violations": violations,
        "eventCount": len(events),
    }


def _pressure_evidence(case: Dict[str, Any], samples: List[Dict[str, Any]], events: Dict[int, Dict[str, int]]) -> Dict[str, Any]:
    properties = case.get("properties") if isinstance(case.get("properties"), dict) else {}
    missing: List[str] = []
    atlas = _number(properties.get("surfaceCacheAtlasSize"))
    budget = _number(properties.get("captureMaxPagesPerFrame"))
    if atlas is None or atlas <= 0:
        missing.append("properties.surfaceCacheAtlasSize")
    if budget is None or budget <= 0:
        missing.append("properties.captureMaxPagesPerFrame")

    values: Dict[str, List[float]] = {name: [] for name in PRESSURE_STATS}
    for sample in samples:
        for name in PRESSURE_STATS:
            value = _stats_value(sample, name)
            if value is None:
                missing.append("samples.stats.%s" % name)
            else:
                values[name].append(value)
    for name in PRESSURE_STATS:
        if name in _CUMULATIVE_STATS:
            previous: Optional[float] = None
            for value in values[name]:
                if previous is not None and value < previous:
                    missing.append("samples.stats.%s monotonic" % name)
                previous = value

    unique_cards = sorted({event["cardID"] for event in events.values()})
    total_pages = max(values["totalPages"], default=0.0)
    max_evictions = max(values["evictions"], default=0.0)
    min_free_pages = min(values["freePages"], default=float("inf"))
    max_alloc_failures = max(values["schedAllocFailures"], default=0.0)
    max_generation_rejects = max(values["generationMismatchRejects"], default=0.0)
    max_stale_rejects = max(values["staleOwnerRejects"], default=0.0)
    max_dropped = max(values["surfaceCacheEventDropped"], default=0.0)
    pressure_signals = {
        "evictions": max_evictions,
        "freePagesExhausted": min_free_pages == 0.0,
        "schedulerAllocationFailures": max_alloc_failures,
        "requestedCards": len(unique_cards),
        "totalPages": total_pages,
    }
    pressure_proven = bool(
        not missing
        and (max_evictions > 0.0 or min_free_pages == 0.0 or max_alloc_failures > 0.0)
        and (total_pages <= 0.0 or len(unique_cards) > total_pages or max_evictions > 0.0)
    )
    stale_reason_cards = sorted(
        {(event["sceneGeneration"], event["cardID"]) for event in events.values() if event["reasonBits"] & STALE_OWNER_REASON_BIT}
    )
    stale_source_present = bool(stale_reason_cards)
    return {
        "status": "PASS" if pressure_proven else "BLOCKED",
        "properties": {
            "surfaceCacheAtlasSize": atlas,
            "captureMaxPagesPerFrame": budget,
        },
        "signals": pressure_signals,
        "uniqueRequestCards": unique_cards,
        "generationMismatchRejects": max_generation_rejects,
        "staleOwnerRejects": max_stale_rejects,
        "eventDropped": max_dropped,
        "staleOwnerReasonCards": [list(item) for item in stale_reason_cards],
        "staleOwnerReasonSourcePresent": stale_source_present,
        "missingFields": sorted(set(missing)),
        "pressureProven": pressure_proven,
    }


def _generation_evidence(settled: List[Dict[str, int]]) -> Dict[str, Any]:
    by_page: Dict[Tuple[int, int], Set[int]] = {}
    for event in settled:
        by_page.setdefault((event["sceneGeneration"], event["pageID"]), set()).add(event["generation"])
    transitions = {
        "%d/%d" % key: sorted(generations)
        for key, generations in by_page.items()
        if len(generations) >= 2
    }
    return {
        "status": "PASS" if transitions else "BLOCKED",
        "pageGenerationSets": {"%d/%d" % key: sorted(value) for key, value in by_page.items()},
        "transitions": transitions,
        "requiredEvidence": "same sceneGeneration/pageID observed with at least two nonzero generations",
    }


def evaluate_case(case: Dict[str, Any]) -> Dict[str, Any]:
    samples = case.get("samples") if isinstance(case, dict) else None
    if not isinstance(samples, list) or not samples:
        return {"status": "BLOCKED", "reason": "missing frame samples", "hostTelemetryOnly": True, "imageInference": False}
    ordered, ordering_errors = _ordered_samples(samples)
    events, event_errors, history_errors, max_frame = _event_snapshots(ordered)
    last_frame = max((_frame(item) or 0 for item in ordered), default=0)
    tail_marked = any(_frame(sample) == last_frame and bool(sample.get("tailSample", False)) for sample in ordered)
    lifecycle = _event_lifecycle(events, max_frame=max_frame, tail_marked=tail_marked)
    pressure = _pressure_evidence(case, ordered, events)
    # A stale-owner terminal event is itself valid identity evidence for the old generation.
    # Restricting this check to successfully published events loses the victim side of a real
    # page reuse (the old owner is intentionally rejected before it can become ready).
    generation = _generation_evidence(lifecycle["events"])
    dropped = pressure["eventDropped"]
    stale_outcomes = lifecycle["staleOwnerOutcomeSequences"]
    missing_host: List[str] = []
    if not generation["transitions"]:
        missing_host.append("surfaceCacheEvents: reuse one pageID with multiple nonzero generation values under pressure")
    if not stale_outcomes:
        missing_host.append("surfaceCacheEvents: explicit state=5 stale-owner outcome (or per-event staleOwnerReject field)")
    if dropped > 0:
        missing_host.append("surfaceCacheEventDropped must remain zero for a complete per-card ring")
    if lifecycle["tailPendingSequences"]:
        missing_host.append("one additional post-tail sample per pending sequence, until capture/ready or terminal stale outcome")
    violations = ordering_errors + event_errors + history_errors + lifecycle["violations"]
    if dropped > 0:
        violations.append({"reason": "surfaceCacheEventDropped is nonzero", "value": dropped})
    if not pressure["pressureProven"]:
        violations.append({"reason": "budget pressure is not proven by host budget/eviction telemetry"})
    if not generation["transitions"]:
        violations.append({"reason": "no per-page generation transition observed"})
    if not stale_outcomes:
        violations.append({"reason": "no per-card stale-owner terminal outcome observed"})
    if lifecycle["tailPendingSequences"]:
        violations.append({"reason": "tail pending is not a completed lifecycle", "sequences": lifecycle["tailPendingSequences"]})
    if any(item.get("status") == "BLOCKED" for item in lifecycle["events"]):
        violations.append({"reason": "one or more per-card events did not reach PASS, STALE_REJECT, or explicit PENDING_TAIL"})
    if not events:
        violations.append({"reason": "no surfaceCacheEvents records"})
    status = "PASS" if events and not violations else "BLOCKED"
    return {
        "status": status,
        "reason": None if status == "PASS" else "strict per-card pressure lifecycle is incomplete or invalid",
        "lifecycle": lifecycle,
        "pressure": pressure,
        "generation": generation,
        "violations": violations,
        "sampleCount": len(ordered),
        "lastSampleFrame": max_frame,
        "tailSampleMarked": tail_marked,
        "requiredHostFieldsStillNeeded": sorted(set(missing_host)),
        "hostTelemetryOnly": True,
        "imageInference": False,
    }


def evaluate_report(report: Dict[str, Any]) -> Dict[str, Any]:
    cases = report.get("cases") if isinstance(report, dict) else None
    if not isinstance(cases, dict) or not cases:
        return {
            "schemaVersion": SCHEMA_VERSION,
            "status": "BLOCKED",
            "reason": "missing cases object",
            "cases": {},
            "hostTelemetryOnly": True,
            "imageInference": False,
        }
    case_reports: Dict[str, Any] = {}
    statuses: List[str] = []
    for name, case in cases.items():
        if not isinstance(case, dict) or not _enabled(case):
            case_reports[name] = {"status": "NOT_APPLICABLE", "reason": "cache disabled"}
            continue
        result = evaluate_case(case)
        case_reports[name] = result
        statuses.append(result["status"])
    status = "PASS" if statuses and all(item == "PASS" for item in statuses) else "BLOCKED"
    return {
        "schemaVersion": SCHEMA_VERSION,
        "status": status,
        "reason": None if status == "PASS" else "one or more cache-enabled cases lack strict pressure evidence",
        "cases": case_reports,
        "hostTelemetryOnly": True,
        "imageInference": False,
    }


def _fixture_event(sequence: int, *, card: int, page: int, generation: int, request: int, capture: int, ready: int, hit: int, state: int, reason: int = STALE_OWNER_REASON_BIT, lookup: int = 0) -> Dict[str, int]:
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


def _fixture_sample(frame: int, events: List[Dict[str, int]], *, tail: bool = False, evictions: int = 0, generation_rejects: int = 1, stale_rejects: int = 1, free_pages: int = 0) -> Dict[str, Any]:
    stats = {
        "totalPages": 1,
        "evictions": evictions,
        "freePages": free_pages,
        "schedAllocFailures": evictions,
        "generationMismatchRejects": generation_rejects,
        "staleOwnerRejects": stale_rejects,
        "surfaceCacheEventDropped": 0,
    }
    result: Dict[str, Any] = {"frame": frame, "phase": "pressure", "stats": stats, "surfaceCacheEvents": events}
    if tail:
        result["tailSample"] = True
    return result


def _self_test() -> int:
    # Two cards reuse one page at generation 1 -> 2. The third request settles
    # after the tail checkpoint. A state-5 record supplies per-card stale-owner
    # outcome evidence; a separate pending variant below checks tail blocking.
    e1 = _fixture_event(1, card=10, page=1, generation=1, request=1, capture=2, ready=3, hit=4, state=4, lookup=1)
    e2 = _fixture_event(2, card=11, page=1, generation=2, request=3, capture=4, ready=5, hit=0, state=3)
    e3_pending = _fixture_event(3, card=12, page=0, generation=0, request=7, capture=0, ready=0, hit=0, state=1)
    e3_done = _fixture_event(3, card=12, page=1, generation=3, request=7, capture=8, ready=9, hit=10, state=4, lookup=1)
    e4 = _fixture_event(4, card=13, page=0, generation=0, request=3, capture=0, ready=0, hit=0, state=5)
    fixture = {
        "cases": {
            "pressure": {
                "properties": {"useSurfaceCache": True, "useCacheLighting": True, "surfaceCacheAtlasSize": 8, "captureMaxPagesPerFrame": 1},
                "samples": [
                    _fixture_sample(1, []),
                    _fixture_sample(2, [e1], evictions=1),
                    _fixture_sample(3, [e1], evictions=1),
                    _fixture_sample(4, [e1, e2], evictions=2),
                    _fixture_sample(5, [e1, e2], evictions=2),
                    _fixture_sample(6, [e1, e2, e4], evictions=2),
                    _fixture_sample(7, [e1, e2, e4], evictions=2),
                    _fixture_sample(8, [e1, e2, e4], evictions=2),
                    _fixture_sample(9, [e1, e2, e3_done, e4], evictions=2),
                    _fixture_sample(10, [e1, e2, e3_done, e4], tail=True, evictions=2),
                ],
            }
        }
    }
    passed = evaluate_report(fixture)
    assert passed["status"] == "PASS", passed

    tail_pending = json.loads(json.dumps(fixture))
    tail_pending["cases"]["pressure"]["samples"][-1]["surfaceCacheEvents"] = [e1, e2, e3_pending, e4]
    assert evaluate_report(tail_pending)["status"] == "BLOCKED"

    non_tail = json.loads(json.dumps(fixture))
    non_tail["cases"]["pressure"]["samples"][-1]["surfaceCacheEvents"] = [e1, e2, e3_pending, e4]
    non_tail["cases"]["pressure"]["samples"][-1]["tailSample"] = False
    assert evaluate_report(non_tail)["status"] == "BLOCKED"

    missing_transition = json.loads(json.dumps(fixture))
    for sample in missing_transition["cases"]["pressure"]["samples"]:
        for event in sample["surfaceCacheEvents"]:
            if event["sequence"] in (2, 3):
                event["generation"] = 1
    assert evaluate_report(missing_transition)["status"] == "BLOCKED"

    missing_stale_outcome = json.loads(json.dumps(fixture))
    for sample in missing_stale_outcome["cases"]["pressure"]["samples"]:
        sample["surfaceCacheEvents"] = [event for event in sample["surfaceCacheEvents"] if event["sequence"] != 4]
    assert evaluate_report(missing_stale_outcome)["status"] == "BLOCKED"

    print("C6_PRESSURE_FIXTURE PASS pass=generation-reuse stale-owner-terminal tail-pending; blocked=tail-marker generation stale-owner")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="surfacecache-effect.json report")
    parser.add_argument("--output", help="write strict pressure gate JSON")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    if not args.input:
        parser.error("--input is required unless --self-test is used")
    input_path = os.path.abspath(args.input)
    try:
        with open(input_path, "r", encoding="utf-8") as stream:
            report = json.load(stream)
    except (OSError, ValueError) as exc:
        print("C6_PRESSURE BLOCKED:", exc)
        return 2
    result = evaluate_report(report)
    output_path = os.path.abspath(args.output) if args.output else os.path.splitext(input_path)[0] + "-c6-pressure-gate.json"
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    print("C6_PRESSURE_GATE", result["status"], output_path)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
