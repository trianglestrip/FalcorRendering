"""Strict C6 request -> next-frame publication gate.

The Surface Cache request and publication counters are cumulative host
telemetry.  This checker deliberately operates only on those counters; it
never samples GI images or derives page state from pixels.  For every observed
request transition at frame ``N`` it requires an explicitly sampled frame
``N+1``.  A capture completion or ready-page increase on ``N`` is rejected, and
the first cache hit must not occur until the request has been published.

Typical use after ``run_surfacecache_effect.py``::

    python tests/lumengi/run_c6_nextframe_gate.py \
      --input artifacts/lumengi/C6/surfacecache-effect.json \
      --output artifacts/lumengi/C6/nextframe-gate.json

Missing fields, sparse frame samples, counter resets, and no observed request
activity are ``BLOCKED``.  They are not converted into a visual pass/fail.
``--self-test`` runs dependency-free PASS/FAIL/BLOCKED fixtures.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from typing import Any, Dict, Iterable, List, Optional, Tuple


SCHEMA_VERSION = "C6-next-frame-validity-v1"

STRICT_HOST_REQUIREMENTS = (
    "stats.requestRawThisFrame",
    "stats.requestCardsThisFrame",
    "stats.requestCaptureCompletedThisFrame",
    "stats.pageMetadataPendingThisFrame",
    "stats.pageMetadataReadyThisFrame",
    "stats.requestObservedFrame",
    "stats.requestCaptureFrame",
    "stats.surfaceCacheFrameIndex",
    "stats.schedulerFrameIndex",
    "stats.surfaceCacheSceneGeneration",
    "surfaceCacheFrameIndex is monotonic within each phase and schedulerFrameIndex == surfaceCacheFrameIndex + 1",
    "stats.cacheLookupHitsThisFrame (or cacheLookupHitsFrame/request association)",
    "sample.surfaceCacheEvents[{sequence,sceneGeneration,cardID,pageID,generation,requestFrame,captureFrame,readyFrame,firstHitFrame,reasonBits,requestCount,lookupHits,state}]",
)

# The per-card event ledger is the authoritative N -> N+1 contract when the
# host exports it.  ``state=5`` is an explicit terminal stale/rejected
# outcome, not an incomplete capture; it must carry the stale-owner reason bit
# and must not carry publication or lookup fields.  States 3/4 are ready/no-hit
# and ready/hit respectively.  States 1/2 remain unresolved until a later
# sample and therefore cannot be promoted to PASS by aggregate counters.
_LIFECYCLE_EVENT_FIELDS = (
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
_LIFECYCLE_IDENTITY_FIELDS = ("sceneGeneration", "cardID", "requestFrame")
_LIFECYCLE_PROGRESS_FIELDS = ("captureFrame", "readyFrame", "firstHitFrame", "lookupHits", "requestCount")
_STALE_OWNER_REASON_BIT = 2

# The Surface Cache lifecycle fields are authored by the host scheduler.  The
# GI history counter (stats.frameIndex / shader gFrameIndex) is deliberately
# not used as their clock because history resets may rewind it.  A report that
# omits this clock provenance must remain BLOCKED even when the event records
# themselves look internally consistent.
_SURFACE_CLOCK_FIELDS = (
    "surfaceCacheFrameIndex",
    "schedulerFrameIndex",
    "surfaceCacheSceneGeneration",
)

# These are the frozen host fields for this gate.  ``cacheLookupHits`` is read
# from stats exactly; accepting feedback/page counters as a substitute would
# allow a report to claim a lookup hit without the lookup counter being bound.
REQUIRED_FIELDS = (
    "pageMetadataPending",
    "pageMetadataReady",
    "requestRaw",
    "requestCards",
    "requestCaptureCompleted",
    "cacheLookupHits",
    "generationMismatchRejects",
    "stateMismatchRejects",
    "staleOwnerRejects",
)

# Pending metadata is a live queue depth and may legitimately decrease after a
# publish.  All other counters used for deltas are cumulative and a decrease
# indicates a reset/reload that must be represented as a separate phase.
_MONOTONIC_FIELDS = tuple(
    name for name in REQUIRED_FIELDS if name not in {"pageMetadataPending", "cacheLookupHits"}
)

_TELEMETRY_FIELDS = {
    "pageMetadataPending",
    "pageMetadataReady",
    "generationMismatchRejects",
    "stateMismatchRejects",
    "staleOwnerRejects",
}


def _finite_nonnegative(value: Any) -> Optional[float]:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number) or number < 0.0:
        return None
    return number


def _field_value(sample: Dict[str, Any], name: str) -> Tuple[Optional[float], Optional[str]]:
    """Read one frozen field and return ``(value, source)``.

    Normalized page telemetry is preferred because it records its source key;
    stats is accepted as the same host contract for artifacts produced before
    normalization.  No image/output key is consulted.
    """
    telemetry = sample.get("pageTelemetry")
    # The surface-cache runner normalizes every frozen field under
    # pageTelemetry.fields, including request/capture counters.  Accepting
    # that explicit host source keeps the gate schema-compatible while still
    # rejecting image-derived or ambiguous aliases.
    if isinstance(telemetry, dict):
        fields = telemetry.get("fields")
        if isinstance(fields, dict):
            entry = fields.get(name)
            if isinstance(entry, dict) and entry.get("value") is not None:
                value = _finite_nonnegative(entry.get("value"))
                return value, "pageTelemetry.fields.%s" % name
    stats = sample.get("stats")
    if isinstance(stats, dict) and name in stats and stats.get(name) is not None:
        value = _finite_nonnegative(stats.get(name))
        return value, "stats.%s" % name
    # Some host snapshots expose page fields in a flat pageMetadata object.
    metadata = sample.get("pageMetadata")
    if name in _TELEMETRY_FIELDS and isinstance(metadata, dict) and name in metadata:
        value = _finite_nonnegative(metadata.get(name))
        return value, "pageMetadata.%s" % name
    return None, None


def _sample_frame(sample: Dict[str, Any]) -> Optional[int]:
    try:
        frame = int(sample.get("frame"))
    except (TypeError, ValueError):
        return None
    return frame if frame >= 0 else None


def _surface_clock_values(sample: Dict[str, Any]) -> Tuple[Optional[Dict[str, int]], List[str]]:
    """Read and validate the host-owned Surface Cache clock for one sample.

    ``stats.frameIndex`` is the GI/history clock and can reset on invalidation;
    it is not evidence for a page publication fence.  The scheduler exports
    the clock used by ``requestFrame``, ``captureFrame``, and ``readyFrame`` as
    ``surfaceCacheFrameIndex``.  ``schedulerFrameIndex`` is sampled after the
    scheduler's end-of-frame tick and therefore must be exactly one greater.
    ``surfaceCacheSceneGeneration`` binds event records to the active host
    epoch, preventing a stale ledger record from being attributed to a new
    scene/reset phase.
    """
    stats = sample.get("stats") if isinstance(sample, dict) else None
    if not isinstance(stats, dict):
        return None, list(_SURFACE_CLOCK_FIELDS)
    values: Dict[str, int] = {}
    missing: List[str] = []
    for name in _SURFACE_CLOCK_FIELDS:
        value = _finite_nonnegative(stats.get(name))
        if value is None or value != float(int(value)):
            missing.append(name)
        else:
            values[name] = int(value)
    if missing:
        return None, missing
    if values["schedulerFrameIndex"] != values["surfaceCacheFrameIndex"] + 1:
        missing.append("schedulerFrameIndex must equal surfaceCacheFrameIndex + 1")
    if values["surfaceCacheSceneGeneration"] <= 0:
        missing.append("surfaceCacheSceneGeneration must be positive")
    return (values if not missing else None), missing


def _surface_clock_provenance(samples: List[Dict[str, Any]]) -> Tuple[Dict[int, Dict[str, int]], List[Dict[str, Any]]]:
    """Validate monotonic host clock provenance without using GI history frames.

    Samples are grouped by the existing phase marker. A phase is the unit in
    which a scheduler reset is legal; a clock decrease, duplicate sample frame,
    or scene-generation change inside one phase is otherwise a hard blocker.
    The returned mapping is keyed by object identity so the lifecycle checker
    can bind each event's frame fields to the clock from the same host sample.
    """
    by_id: Dict[int, Dict[str, int]] = {}
    violations: List[Dict[str, Any]] = []
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            violations.append({"index": index, "reason": "sample is not an object"})
            continue
        phase = str(sample.get("phase", "default"))
        grouped.setdefault(phase, []).append(sample)

    for phase, phase_samples in grouped.items():
        previous_frame: Optional[int] = None
        previous_clock: Optional[Dict[str, int]] = None
        for index, sample in enumerate(phase_samples):
            sample_frame = _sample_frame(sample)
            clock, missing = _surface_clock_values(sample)
            if clock is None:
                violations.append({
                    "phase": phase,
                    "frame": sample_frame,
                    "reason": "missing or invalid Surface Cache clock provenance",
                    "fields": missing,
                })
                continue
            by_id[id(sample)] = clock
            if sample_frame is None:
                violations.append({"phase": phase, "reason": "clock sample lacks a valid host frame"})
            elif previous_frame is not None and sample_frame <= previous_frame:
                violations.append({
                    "phase": phase,
                    "frame": sample_frame,
                    "previousFrame": previous_frame,
                    "reason": "duplicate or descending host sample frame",
                })
            if previous_clock is not None:
                if clock["surfaceCacheFrameIndex"] < previous_clock["surfaceCacheFrameIndex"]:
                    violations.append({
                        "phase": phase,
                        "frame": sample_frame,
                        "reason": "surfaceCacheFrameIndex regressed",
                        "previous": previous_clock["surfaceCacheFrameIndex"],
                        "current": clock["surfaceCacheFrameIndex"],
                    })
                if clock["schedulerFrameIndex"] < previous_clock["schedulerFrameIndex"]:
                    violations.append({
                        "phase": phase,
                        "frame": sample_frame,
                        "reason": "schedulerFrameIndex regressed",
                        "previous": previous_clock["schedulerFrameIndex"],
                        "current": clock["schedulerFrameIndex"],
                    })
                if clock["surfaceCacheSceneGeneration"] != previous_clock["surfaceCacheSceneGeneration"]:
                    violations.append({
                        "phase": phase,
                        "frame": sample_frame,
                        "reason": "surfaceCacheSceneGeneration changed within a phase",
                        "previous": previous_clock["surfaceCacheSceneGeneration"],
                        "current": clock["surfaceCacheSceneGeneration"],
                    })
            previous_frame = sample_frame if sample_frame is not None else previous_frame
            previous_clock = clock
    return by_id, violations


def _sample_values(sample: Dict[str, Any]) -> Tuple[Optional[Dict[str, float]], List[Dict[str, str]]]:
    values: Dict[str, float] = {}
    missing: List[Dict[str, str]] = []
    for name in REQUIRED_FIELDS:
        value, source = _field_value(sample, name)
        if value is None:
            missing.append({"field": name, "source": source or "missing"})
        else:
            values[name] = value
    return (values if not missing else None), missing


def _event_values(sample: Dict[str, Any]) -> Optional[Dict[str, float]]:
    """Read explicit per-host-frame event counters when available.

    The legacy fields are cumulative (except cacheLookupHits, which is a
    dispatch-local counter). Newer Host telemetry exports ``*ThisFrame``
    counters so asynchronous readback cannot be mistaken for a same-frame
    publication. Returning None keeps old artifacts conservative/blocked.
    """
    stats = sample.get("stats")
    if not isinstance(stats, dict):
        return None
    aliases = {
        "requestRaw": "requestRawThisFrame",
        "requestCards": "requestCardsThisFrame",
        "requestCaptureCompleted": "requestCaptureCompletedThisFrame",
        "pageMetadataPending": "pageMetadataPendingThisFrame",
        "pageMetadataReady": "pageMetadataReadyThisFrame",
    }
    result: Dict[str, float] = {}
    for name, alias in aliases.items():
        value = _finite_nonnegative(stats.get(alias))
        if value is None:
            return None
        result[name] = value
    hits = _finite_nonnegative(stats.get("cacheLookupHitsThisFrame"))
    # The current host exports cacheLookupHits as the latest GPU counter
    # readback, but not its originating frame.  Keep the value as an
    # observation for diagnostics, while marking it unattributable below;
    # falling back here would allow a same-frame hit to be mistaken for proof.
    legacy_hits = _finite_nonnegative(stats.get("cacheLookupHits"))
    if hits is None and legacy_hits is None:
        return None
    result["cacheLookupHits"] = hits if hits is not None else legacy_hits
    result["cacheLookupHitsPerFrame"] = hits is not None
    lookup_frame = _finite_nonnegative(stats.get("cacheLookupStatsFrame"))
    # A per-frame counter is only attributable when the producer also exports
    # the host frame that completed the readback.  Without this stamp the value
    # remains diagnostic and must not be used to reject/pass an N->N+1 event.
    if lookup_frame is not None:
        result["cacheLookupStatsFrame"] = lookup_frame
    observed = _finite_nonnegative(stats.get("requestObservedFrame"))
    captured = _finite_nonnegative(stats.get("requestCaptureFrame"))
    if observed is None or captured is None:
        return None
    result["requestObservedFrame"] = observed
    result["requestCaptureFrame"] = captured
    return result


def _ordered_phase_samples(samples: Iterable[Dict[str, Any]]) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    """Return sorted samples and structural errors.

    A reload may reuse frame indices, so samples are partitioned by ``phase``
    before ordering.  Duplicate or descending frame indices in one phase are
    blocked because they cannot prove an N -> N+1 transition.
    """
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    errors: List[Dict[str, Any]] = []
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            errors.append({"reason": "sample is not an object", "index": index})
            continue
        phase = str(sample.get("phase", "default"))
        grouped.setdefault(phase, []).append(sample)
    ordered: List[Dict[str, Any]] = []
    for phase, phase_samples in grouped.items():
        phase_samples = sorted(phase_samples, key=lambda item: (_sample_frame(item) is None, _sample_frame(item) or 0))
        previous: Optional[int] = None
        for sample in phase_samples:
            frame = _sample_frame(sample)
            if frame is None:
                errors.append({"phase": phase, "reason": "missing or invalid frame"})
                continue
            if previous is not None and frame <= previous:
                errors.append({"phase": phase, "frame": frame, "previousFrame": previous, "reason": "duplicate or descending frame"})
            previous = frame
            sample_copy = dict(sample)
            sample_copy["_gatePhase"] = phase
            ordered.append(sample_copy)
    # Preserve phase-local frame ordering while making the event report stable.
    ordered.sort(key=lambda item: (str(item.get("_gatePhase", "default")), _sample_frame(item) or 0))
    return ordered, errors


def _lifecycle_event_gate(
    samples: List[Dict[str, Any]],
    clock_by_sample: Optional[Dict[int, Dict[str, int]]] = None,
    clock_violations: Optional[List[Dict[str, Any]]] = None,
) -> Optional[Dict[str, Any]]:
    """Validate explicit card/page events when the host exports them.

    Events are snapshots of a bounded host ring, so the latest record for each
    sequence is authoritative after identity/progress history has been checked.
    This path deliberately does not fall back to aggregate counters: a
    card-specific request is only complete when its own page/generation reaches
    capture and ready, or when the host emits an explicit terminal state-5
    stale/rejected outcome.  A lookup is accepted only when its own event binds
    ``lookupHits`` to ``firstHitFrame`` at/after ready.
    """
    latest: Dict[int, Dict[str, Any]] = {}
    previous: Dict[int, Dict[str, int]] = {}
    latest_sample_frame: Dict[int, int] = {}
    latest_sample_clock: Dict[int, Dict[str, int]] = {}
    history_violations: List[Dict[str, Any]] = list(clock_violations or [])
    seen = False
    sample_frames: List[int] = []
    previous_sequences: Optional[set[int]] = None
    dropped_values: List[float] = []
    for sample in samples:
        records = sample.get("surfaceCacheEvents") if isinstance(sample, dict) else None
        if records is None:
            continue
        sample_frame = _sample_frame(sample)
        if sample_frame is None:
            history_violations.append({"reason": "event sample lacks a valid frame"})
            continue
        sample_frames.append(sample_frame)
        if not isinstance(records, list):
            return {"status": "BLOCKED", "reason": "surfaceCacheEvents is not a list", "events": [], "hostTelemetryOnly": True}
        seen = True
        current_sequences: set[int] = set()
        sample_clock = (clock_by_sample or {}).get(id(sample))
        stats = sample.get("stats") if isinstance(sample.get("stats"), dict) else {}
        dropped = _finite_nonnegative(stats.get("surfaceCacheEventDropped"))
        if dropped is not None:
            dropped_values.append(dropped)
        event_count = _finite_nonnegative(stats.get("surfaceCacheEventCount"))
        if event_count is not None and event_count != float(len(records)):
            history_violations.append({
                "sampleFrame": sample_frame,
                "reason": "surfaceCacheEventCount does not match exported ledger length",
                "reported": event_count,
                "exported": len(records),
            })
        for record in records:
            if not isinstance(record, dict):
                return {"status": "BLOCKED", "reason": "surfaceCacheEvents contains a non-object", "events": [], "hostTelemetryOnly": True}
            try:
                sequence = int(record.get("sequence"))
            except (TypeError, ValueError):
                return {"status": "BLOCKED", "reason": "surfaceCacheEvents record lacks sequence", "events": [], "hostTelemetryOnly": True}
            if sequence <= 0:
                return {"status": "BLOCKED", "reason": "surfaceCacheEvents sequence is invalid", "events": [], "hostTelemetryOnly": True}
            if sequence in current_sequences:
                history_violations.append({"sampleFrame": sample_frame, "sequence": sequence, "reason": "duplicate event sequence in sample"})
            current_sequences.add(sequence)
            normalized: Dict[str, int] = {"sequence": sequence}
            invalid_fields: List[str] = []
            for name in _LIFECYCLE_EVENT_FIELDS:
                value = _finite_nonnegative(record.get(name))
                if value is None or value != float(int(value)):
                    invalid_fields.append(name)
                else:
                    normalized[name] = int(value)
            if invalid_fields:
                history_violations.append({
                    "sampleFrame": sample_frame,
                    "sequence": sequence,
                    "reason": "missing or invalid lifecycle field",
                    "fields": invalid_fields,
                })
                continue
            if normalized["sceneGeneration"] <= 0:
                history_violations.append({"sampleFrame": sample_frame, "sequence": sequence, "reason": "sceneGeneration is not positive"})
            if normalized["requestFrame"] <= 0:
                history_violations.append({"sampleFrame": sample_frame, "sequence": sequence, "reason": "requestFrame is not positive"})
            if normalized["requestCount"] <= 0:
                history_violations.append({"sampleFrame": sample_frame, "sequence": sequence, "reason": "requestCount is not positive"})
            if normalized["state"] not in {1, 2, 3, 4, 5}:
                history_violations.append({"sampleFrame": sample_frame, "sequence": sequence, "reason": "state is outside the Surface Cache lifecycle", "state": normalized["state"]})
            if normalized["requestFrame"] > sample_frame:
                history_violations.append({
                    "sampleFrame": sample_frame,
                    "sequence": sequence,
                    "reason": "requestFrame is in the future of the sampled host frame",
                    "requestFrame": normalized["requestFrame"],
                })
            if sample_clock is not None:
                if normalized["sceneGeneration"] != sample_clock["surfaceCacheSceneGeneration"]:
                    history_violations.append({
                        "sampleFrame": sample_frame,
                        "sequence": sequence,
                        "reason": "event sceneGeneration does not match host Surface Cache scene generation",
                        "eventSceneGeneration": normalized["sceneGeneration"],
                        "hostSceneGeneration": sample_clock["surfaceCacheSceneGeneration"],
                    })
                if normalized["requestFrame"] > sample_clock["surfaceCacheFrameIndex"]:
                    history_violations.append({
                        "sampleFrame": sample_frame,
                        "sequence": sequence,
                        "reason": "event requestFrame is ahead of host surfaceCacheFrameIndex",
                        "requestFrame": normalized["requestFrame"],
                        "surfaceCacheFrameIndex": sample_clock["surfaceCacheFrameIndex"],
                    })
            old = previous.get(sequence)
            if old is not None:
                for name in _LIFECYCLE_IDENTITY_FIELDS:
                    if normalized[name] != old[name]:
                        history_violations.append({
                            "sampleFrame": sample_frame,
                            "sequence": sequence,
                            "field": name,
                            "reason": "event identity changed",
                            "old": old[name],
                            "new": normalized[name],
                        })
                for name in _LIFECYCLE_PROGRESS_FIELDS:
                    if normalized[name] < old[name]:
                        history_violations.append({
                            "sampleFrame": sample_frame,
                            "sequence": sequence,
                            "field": name,
                            "reason": "event progress regressed",
                            "old": old[name],
                            "new": normalized[name],
                        })
                for name in ("pageID", "generation"):
                    if old[name] != 0 and normalized[name] != old[name]:
                        history_violations.append({
                            "sampleFrame": sample_frame,
                            "sequence": sequence,
                            "field": name,
                            "reason": "assigned page identity changed",
                            "old": old[name],
                            "new": normalized[name],
                        })
                if (normalized["reasonBits"] | old["reasonBits"]) != normalized["reasonBits"]:
                    history_violations.append({
                        "sampleFrame": sample_frame,
                        "sequence": sequence,
                        "field": "reasonBits",
                        "reason": "reason bits regressed",
                    })
                # State 5 is terminal.  A later non-terminal state would be a
                # reuse of the same sequence and cannot be associated safely.
                if old["state"] == 5 and normalized["state"] != 5:
                    history_violations.append({
                        "sampleFrame": sample_frame,
                        "sequence": sequence,
                        "field": "state",
                        "reason": "terminal stale/rejected event reopened",
                    })
                elif old["state"] != 5 and normalized["state"] < old["state"]:
                    history_violations.append({
                        "sampleFrame": sample_frame,
                        "sequence": sequence,
                        "field": "state",
                        "reason": "lifecycle state regressed",
                        "old": old["state"],
                        "new": normalized["state"],
                    })
            previous[sequence] = normalized
            latest[sequence] = normalized
            latest_sample_frame[sequence] = sample_frame
            if sample_clock is not None:
                latest_sample_clock[sequence] = sample_clock
        if previous_sequences is not None and not previous_sequences.issubset(current_sequences):
            # A bounded ring may legitimately lose old entries only when the
            # host reports a nonzero drop counter.  Without that explicit
            # accounting, a later PASS could silently omit requests.
            if not dropped_values or max(dropped_values) <= 0.0:
                history_violations.append({"sampleFrame": sample_frame, "reason": "event ring lost records without drop telemetry"})
        previous_sequences = current_sequences
    if not seen:
        return None
    required = _LIFECYCLE_EVENT_FIELDS
    checked: List[Dict[str, Any]] = []
    violations: List[Dict[str, Any]] = list(history_violations)
    max_sample_frame = max(sample_frames, default=0)
    if max(latest, default=0) > 0:
        sequences = sorted(latest)
        expected = list(range(sequences[0], sequences[-1] + 1))
        if sequences[0] != 1:
            history_violations.append({"reason": "surfaceCacheEvents ledger does not start at sequence 1", "firstSequence": sequences[0]})
        if sequences != expected and (not dropped_values or max(dropped_values) <= 0.0):
            history_violations.append({"reason": "surfaceCacheEvents sequence gap without drop telemetry", "sequences": sequences})
    if dropped_values and max(dropped_values) > 0.0:
        history_violations.append({"reason": "surfaceCacheEventDropped is nonzero", "value": max(dropped_values)})
    # Sequence/ring checks above append after the initial list copy. Keep the
    # final violation list synchronized before evaluating each terminal event.
    violations = list(history_violations)
    tail_pending: List[int] = []
    terminal_count = 0
    published_count = 0
    lookup_count = 0
    for sequence, record in sorted(latest.items()):
        values = {name: int(record[name]) for name in required}
        request_frame = values["requestFrame"]
        capture_frame = values["captureFrame"]
        ready_frame = values["readyFrame"]
        hit_frame = values["firstHitFrame"]
        state = values["state"]
        event = {"sequence": sequence, **values, "status": "BLOCKED"}
        sampled_frame = latest_sample_frame[sequence]
        sample_clock = latest_sample_clock.get(sequence)
        for name, value in (("requestFrame", request_frame), ("captureFrame", capture_frame), ("readyFrame", ready_frame), ("firstHitFrame", hit_frame)):
            if value > sampled_frame:
                violations.append({
                    "sequence": sequence,
                    "field": name,
                    "reason": "lifecycle frame is in the future of the event sample",
                    "value": value,
                    "sampleFrame": sampled_frame,
                })
            if sample_clock is not None and value > sample_clock["surfaceCacheFrameIndex"]:
                violations.append({
                    "sequence": sequence,
                    "field": name,
                    "reason": "lifecycle frame is ahead of host surfaceCacheFrameIndex",
                    "value": value,
                    "surfaceCacheFrameIndex": sample_clock["surfaceCacheFrameIndex"],
                    "sampleFrame": sampled_frame,
                })
        if state == 5:
            # Host code uses state 5 for both scheduler rejection and stale
            # owner terminal records.  The shared reason bit is the frozen
            # provenance marker; no publication or lookup may be attached.
            if not (values["reasonBits"] & _STALE_OWNER_REASON_BIT):
                violations.append({"sequence": sequence, "reason": "state=5 terminal outcome lacks stale/rejected reason bit"})
            if capture_frame or ready_frame or hit_frame or values["lookupHits"]:
                violations.append({"sequence": sequence, "reason": "state=5 terminal outcome has publication or lookup fields"})
            else:
                event["status"] = "PASS_TERMINAL"
                terminal_count += 1
        elif state in {3, 4}:
            published_count += 1
            if values["pageID"] <= 0 or values["generation"] <= 0:
                violations.append({"sequence": sequence, "reason": "published event lacks pageID/generation"})
            if capture_frame <= request_frame:
                violations.append({"sequence": sequence, "reason": "capture is not strictly after request", "requestFrame": request_frame, "captureFrame": capture_frame})
            elif capture_frame != request_frame + 1:
                violations.append({"sequence": sequence, "reason": "capture is not on explicit frame N+1", "requestFrame": request_frame, "captureFrame": capture_frame})
            if ready_frame <= capture_frame:
                violations.append({"sequence": sequence, "reason": "ready is not strictly after capture", "captureFrame": capture_frame, "readyFrame": ready_frame})
            if state == 3 and (hit_frame or values["lookupHits"]):
                violations.append({"sequence": sequence, "reason": "state=3 ready event has lookup fields"})
            if state == 4:
                lookup_count += 1
                if not hit_frame or hit_frame < ready_frame or values["lookupHits"] <= 0:
                    violations.append({"sequence": sequence, "reason": "state=4 hit event is not bound to ready/lookup fields"})
            if not any(item.get("sequence") == sequence for item in violations):
                event["status"] = "PASS"
        elif state in {1, 2}:
            # A tail marker is evidence that the last request is still inside
            # the intentional deferred handoff, never a successful lifecycle.
            if request_frame >= max_sample_frame - 1:
                tail_pending.append(sequence)
                event["status"] = "PENDING_TAIL"
            violations.append({"sequence": sequence, "reason": "request lifecycle remains unresolved", "state": state, "requestFrame": request_frame, "lastSampleFrame": max_sample_frame})
        else:
            violations.append({"sequence": sequence, "reason": "unsupported lifecycle state", "state": state})
        checked.append(event)
    status = "PASS" if checked and not violations and all(item.get("status") in {"PASS", "PASS_TERMINAL"} for item in checked) else "BLOCKED"
    return {
        "status": status,
        "reason": None if status == "PASS" else "card-specific request lifecycle is incomplete or invalid",
        "events": checked,
        "tailPendingSequences": tail_pending,
        "violations": violations,
        "sampleCount": len(samples),
        "hostRequirements": list(STRICT_HOST_REQUIREMENTS),
        "telemetryMode": "cardPageLifecycleEvents",
        "clockDomain": "surfaceCacheFrameIndex",
        "lifecycleFields": list(_LIFECYCLE_EVENT_FIELDS),
        "frameOriginCoverage": bool(checked) and not history_violations,
        "cardAssociationCoverage": bool(checked) and all(item["cardID"] >= 0 for item in checked),
        "terminalOutcomeCount": terminal_count,
        "publishedEventCount": published_count,
        "lookupEventCount": lookup_count,
        "hostTelemetryOnly": True,
        "imageInference": False,
    }


def evaluate_case(case: Dict[str, Any]) -> Dict[str, Any]:
    """Evaluate one cache-enabled C6 case without image inference."""
    samples = case.get("samples") if isinstance(case, dict) else None
    if not isinstance(samples, list) or not samples:
        return {
            "status": "BLOCKED",
            "reason": "missing frame samples",
            "requiredFields": list(REQUIRED_FIELDS),
            "events": [],
            "violations": [{"reason": "missing frame samples"}],
            "hostRequirements": list(STRICT_HOST_REQUIREMENTS),
            "hostTelemetryOnly": True,
        }

    clock_by_sample, clock_violations = _surface_clock_provenance(samples)
    explicit_lifecycle = _lifecycle_event_gate(samples, clock_by_sample, clock_violations)
    if explicit_lifecycle is not None:
        return explicit_lifecycle

    ordered, structural_errors = _ordered_phase_samples(samples)
    violations: List[Dict[str, Any]] = list(structural_errors) + list(clock_violations)
    snapshots: List[Dict[str, Any]] = []
    by_phase: Dict[str, List[Dict[str, Any]]] = {}
    for sample in ordered:
        phase = str(sample.get("_gatePhase", "default"))
        values, missing = _sample_values(sample)
        frame = _sample_frame(sample)
        snapshot = {
            "phase": phase,
            "frame": frame,
            "tailSample": bool(sample.get("tailSample", False)),
            "values": values,
            "eventValues": _event_values(sample),
            "missing": missing,
        }
        snapshots.append(snapshot)
        by_phase.setdefault(phase, []).append(snapshot)
        if missing:
            violations.append({"phase": phase, "frame": frame, "reason": "missing or invalid frozen telemetry", "fields": missing})

    events: List[Dict[str, Any]] = []
    for phase, phase_snapshots in by_phase.items():
        previous: Optional[Dict[str, Any]] = None
        for current in phase_snapshots:
            # The runner may append one explicit post-checkpoint tail frame so
            # the final configured request has a next-frame observation.  That
            # tail is evidence for the previous event, not a new event whose
            # own N+1 would require an unbounded run.
            if current.get("tailSample"):
                previous = current
                continue
            values = current.get("values")
            if values is None:
                previous = current
                continue
            frame = current["frame"]
            current_events = current.get("eventValues")
            previous_events = previous.get("eventValues") if previous is not None else None
            explicit_events = isinstance(current_events, dict) and isinstance(previous_events, dict)
            explicit_event_frames = bool(
                explicit_events
                and "requestObservedFrame" in current_events
                and "requestCaptureFrame" in current_events
                and "requestObservedFrame" in previous_events
                and "requestCaptureFrame" in previous_events
            )
            if explicit_events:
                deltas = {name: float(current_events.get(name, 0.0)) for name in values}
                # Raw miss samples are emitted every probe dispatch and may repeat
                # an already queued card.  A new request transition is the unique
                # card insertion count, not the raw atomic miss total.
                request_delta = deltas["requestCards"]
                previous_values = previous["values"] if previous and previous.get("values") else {name: 0.0 for name in values}
                negative_deltas: List[str] = []
                current_clock = clock_by_sample.get(id(current))
                if current_clock is not None:
                    for stamp_name in ("requestObservedFrame", "requestCaptureFrame", "cacheLookupStatsFrame"):
                        stamp = current_events.get(stamp_name)
                        if stamp is not None and float(stamp) > current_clock["surfaceCacheFrameIndex"]:
                            violations.append({
                                "phase": phase,
                                "frame": frame,
                                "reason": "%s is ahead of host surfaceCacheFrameIndex" % stamp_name,
                                "value": float(stamp),
                                "surfaceCacheFrameIndex": current_clock["surfaceCacheFrameIndex"],
                            })
            elif previous is None or previous.get("values") is None:
                # A non-zero first sample is conservatively treated as a
                # request transition; the preceding frame was not captured.
                deltas = {name: values[name] for name in ("requestRaw", "requestCards", "requestCaptureCompleted", "pageMetadataReady", "cacheLookupHits")}
                request_delta = max(deltas["requestRaw"], deltas["requestCards"])
                previous_values = {name: 0.0 for name in values}
            else:
                previous_values = previous["values"]
                deltas = {name: values[name] - previous_values[name] for name in values}
                request_delta = max(deltas["requestRaw"], deltas["requestCards"])
            negative_deltas = [name for name in _MONOTONIC_FIELDS if deltas.get(name, 0.0) < 0.0]
            if negative_deltas:
                violations.append({"phase": phase, "frame": frame, "reason": "counter reset or decrease", "fields": negative_deltas})
            if request_delta <= 0.0:
                previous = current
                continue

            event: Dict[str, Any] = {
                "phase": phase,
                "requestFrame": frame,
                "requestDelta": {
                    "requestRaw": deltas["requestRaw"],
                    "requestCards": deltas["requestCards"],
                },
                "sameFrame": {
                    "captureCompletedDelta": deltas["requestCaptureCompleted"],
                    "pageReadyDelta": deltas["pageMetadataReady"],
                    "cacheLookupHitsDelta": deltas["cacheLookupHits"],
                },
                "status": "BLOCKED",
            }
            if explicit_events:
                event["telemetryMode"] = "perHostFrame"
            request_anchor = (
                float(current_events["requestObservedFrame"])
                if explicit_event_frames
                else None
            )
            capture_anchor = (
                float(current_events["requestCaptureFrame"])
                if explicit_event_frames
                else None
            )
            # Capture/ready counters are aggregate frame events.  When the
            # host's last capture frame is not newer than this request's
            # observed frame, a same-sample completion belongs to an older
            # request (or is ambiguous) and must not be called same-frame
            # publication.  A newer capture frame is definitive evidence of a
            # same-frame violation only when the host can bind it to this
            # request; otherwise the next-frame relation below remains strict.
            same_frame_publish = bool(
                deltas["requestCaptureCompleted"] > 0.0
                and explicit_event_frames
                and capture_anchor is not None
                and request_anchor is not None
                and capture_anchor > request_anchor
            )
            lookup_frame = current_events.get("cacheLookupStatsFrame") if explicit_events else None
            same_frame_hit = bool(
                deltas["cacheLookupHits"] > 0.0
                and explicit_events
                and (
                    current_events.get("cacheLookupHitsPerFrame", False) is False
                    or lookup_frame is None
                    or lookup_frame == float(frame)
                )
            )
            if same_frame_publish:
                event["reason"] = "page ready/publish advanced on request frame; requires N+1"
                violations.append({"phase": phase, "frame": frame, "reason": event["reason"]})
            elif same_frame_hit:
                event["reason"] = (
                    "cache lookup hit advanced on request frame"
                    if current_events.get("cacheLookupHitsPerFrame", False)
                    else "cacheLookupHits has no originating frame; same-frame hit attribution is unavailable"
                )
                violations.append({"phase": phase, "frame": frame, "reason": event["reason"]})

            next_snapshot = None
            for candidate in phase_snapshots:
                if candidate["frame"] == frame + 1:
                    next_snapshot = candidate
                    break
            if next_snapshot is None or next_snapshot.get("values") is None:
                event["reason"] = "missing explicit frame N+1 sample"
                violations.append({"phase": phase, "frame": frame, "reason": event["reason"]})
            else:
                next_values = next_snapshot["values"]
                next_events = next_snapshot.get("eventValues")
                if explicit_events and isinstance(next_events, dict):
                    next_deltas = {name: float(next_events.get(name, 0.0)) for name in values}
                else:
                    next_deltas = {name: next_values[name] - values[name] for name in values}
                next_capture_frame = (
                    float(next_events["requestCaptureFrame"])
                    if isinstance(next_events, dict) and "requestCaptureFrame" in next_events
                    else None
                )
                next_publish = (
                    next_deltas["requestCaptureCompleted"] >= deltas["requestCards"]
                    and next_deltas["requestCaptureCompleted"] > 0.0
                    and (
                        request_anchor is None
                        or (next_capture_frame is not None and next_capture_frame > request_anchor)
                    )
                )
                next_ready = bool(
                    next_deltas["pageMetadataReady"] > 0.0
                    and (
                        request_anchor is None
                        or (next_capture_frame is not None and next_capture_frame > request_anchor)
                    )
                )
                next_hit = next_deltas["cacheLookupHits"] > 0.0
                event["nextFrame"] = {
                    "frame": frame + 1,
                    "captureCompletedDelta": next_deltas["requestCaptureCompleted"],
                    "pageReadyDelta": next_deltas["pageMetadataReady"],
                    "cacheLookupHitsDelta": next_deltas["cacheLookupHits"],
                    "published": next_publish,
                    "pageReady": next_ready,
                    "cacheHit": next_hit,
                    "requestCaptureFrame": next_capture_frame,
                }
                if not next_publish and not next_ready:
                    event["reason"] = "no page ready/publish evidence on frame N+1"
                    violations.append({"phase": phase, "frame": frame, "reason": event["reason"]})
                elif next_hit and not next_publish and not next_ready:
                    event["reason"] = "cache hit advanced before page publication"
                    violations.append({"phase": phase, "frame": frame, "reason": event["reason"]})
                elif not same_frame_publish and not same_frame_hit:
                    event["status"] = "PASS"
            events.append(event)
            previous = current

    if not events:
        violations.append({"reason": "no request transition observed"})
    legacy_hit_samples = [
        snapshot for snapshot in snapshots
        if isinstance(snapshot.get("eventValues"), dict)
        and snapshot["eventValues"].get("cacheLookupHits", 0.0) > 0.0
        and not snapshot["eventValues"].get("cacheLookupHitsPerFrame", False)
    ]
    host_requirements = list(STRICT_HOST_REQUIREMENTS)
    if not legacy_hit_samples:
        host_requirements = [
            item for item in host_requirements
            if "cacheLookupHitsThisFrame" not in item
        ]
    status = "PASS" if events and not violations and all(event.get("status") == "PASS" for event in events) else "BLOCKED"
    return {
        "status": status,
        "reason": None if status == "PASS" else "strict request-to-next-frame evidence is incomplete or invalid",
        "requiredFields": list(REQUIRED_FIELDS),
        "events": events,
        "violations": violations,
        "sampleCount": len(samples),
        "hostRequirements": host_requirements,
        "clockDomain": "surfaceCacheFrameIndex",
        "legacyHitAttributionSamples": len(legacy_hit_samples),
        "hostTelemetryOnly": True,
        "imageInference": False,
    }


def _case_enabled(case: Dict[str, Any]) -> bool:
    properties = case.get("properties") if isinstance(case, dict) else None
    if not isinstance(properties, dict):
        return True
    return bool(properties.get("useSurfaceCache", True) and properties.get("useCacheLighting", True))


def evaluate_report(report: Dict[str, Any]) -> Dict[str, Any]:
    cases = report.get("cases") if isinstance(report, dict) else None
    if not isinstance(cases, dict) or not cases:
        return {
            "schemaVersion": SCHEMA_VERSION,
            "status": "BLOCKED",
            "reason": "missing cases object",
            "cases": {},
            "hostRequirements": list(STRICT_HOST_REQUIREMENTS),
            "hostTelemetryOnly": True,
            "imageInference": False,
        }
    case_reports: Dict[str, Any] = {}
    statuses: List[str] = []
    for name, case in cases.items():
        if not _case_enabled(case):
            case_reports[name] = {"status": "NOT_APPLICABLE", "reason": "cache disabled"}
            continue
        result = evaluate_case(case)
        case_reports[name] = result
        statuses.append(result["status"])
    status = "PASS" if statuses and all(item == "PASS" for item in statuses) else "BLOCKED"
    return {
        "schemaVersion": SCHEMA_VERSION,
        "status": status,
        "reason": None if status == "PASS" else "one or more cache-enabled cases lack strict N+1 evidence",
        "cases": case_reports,
        "requiredFields": list(REQUIRED_FIELDS),
        "hostRequirements": list(STRICT_HOST_REQUIREMENTS),
        "hostTelemetryOnly": True,
        "imageInference": False,
    }


def _sample(frame: int, *, request_raw: float = 0.0, request_cards: float = 0.0, capture: float = 0.0, ready: float = 0.0, pending: float = 0.0, hits: float = 0.0, phase: str = "default") -> Dict[str, Any]:
    fields = {
        name: {"value": 0.0, "sourceKey": name}
        for name in _TELEMETRY_FIELDS
    }
    fields["pageMetadataPending"]["value"] = pending
    fields["pageMetadataReady"]["value"] = ready
    fields["generationMismatchRejects"]["value"] = 0.0
    fields["stateMismatchRejects"]["value"] = 0.0
    fields["staleOwnerRejects"]["value"] = 0.0
    return {
        "phase": phase,
        "frame": frame,
        "stats": {
            "requestRaw": request_raw,
            "requestCards": request_cards,
            "requestCaptureCompleted": capture,
            "cacheLookupHits": hits,
            "surfaceCacheFrameIndex": max(0, frame - 1),
            "schedulerFrameIndex": max(0, frame),
            "surfaceCacheSceneGeneration": 1,
            # Fixture host frame events.  The request is observed one host
            # frame before its capture completion, matching the deferred
            # Surface Cache scheduler contract.
            "requestRawThisFrame": request_raw,
            "requestCardsThisFrame": request_cards,
            "requestCaptureCompletedThisFrame": capture,
            "pageMetadataPendingThisFrame": pending,
            "pageMetadataReadyThisFrame": ready,
            "requestObservedFrame": max(0, frame - 2),
            "requestCaptureFrame": max(0, frame - 2) if capture > 0.0 else 0.0,
        },
        "pageTelemetry": {"fields": fields},
    }


def _self_test() -> int:
    base = {
        "properties": {"useSurfaceCache": True, "useCacheLighting": True},
        "samples": [
            _sample(1, ready=1.0),
            _sample(2, request_raw=1.0, request_cards=1.0, pending=1.0, ready=1.0),
            _sample(3, capture=1.0, ready=2.0),
            _sample(4, ready=2.0, hits=1.0),
        ],
    }
    passed = evaluate_case(base)
    assert passed["status"] == "PASS", passed
    clock_missing = json.loads(json.dumps(base))
    clock_missing["samples"][2]["stats"].pop("surfaceCacheFrameIndex")
    assert evaluate_case(clock_missing)["status"] == "BLOCKED"
    clock_reset = json.loads(json.dumps(base))
    clock_reset["samples"][2]["stats"]["surfaceCacheFrameIndex"] = 0
    assert evaluate_case(clock_reset)["status"] == "BLOCKED"

    same_frame = {
        "properties": base["properties"],
        "samples": [
            _sample(1),
            _sample(2, request_raw=1.0, request_cards=1.0, capture=1.0, ready=2.0, hits=1.0),
            _sample(3, capture=1.0, ready=2.0, hits=1.0),
        ],
    }
    assert evaluate_case(same_frame)["status"] == "BLOCKED"

    sparse = {
        "properties": base["properties"],
        "samples": [
            _sample(1),
            _sample(2, request_raw=1.0, request_cards=1.0, pending=1.0),
            _sample(4, capture=1.0, ready=1.0),
        ],
    }
    sparse_result = evaluate_case(sparse)
    assert sparse_result["status"] == "BLOCKED"
    assert any("N+1" in item.get("reason", "") for item in sparse_result["violations"])

    missing = {
        "properties": base["properties"],
        "samples": [{"frame": 1, "stats": {}, "pageTelemetry": {"fields": {}}}],
    }
    assert evaluate_case(missing)["status"] == "BLOCKED"
    no_request = {
        "properties": base["properties"],
        "samples": [_sample(1), _sample(2), _sample(3)],
    }
    assert evaluate_case(no_request)["status"] == "BLOCKED"
    lifecycle = {
        "properties": base["properties"],
        "samples": [
            {"frame": 1, "surfaceCacheEvents": []},
            {"frame": 2, "surfaceCacheEvents": [{
                "sequence": 1, "sceneGeneration": 1, "cardID": 7, "pageID": 3,
                "generation": 2, "requestFrame": 1, "captureFrame": 0,
                "readyFrame": 0, "firstHitFrame": 0, "reasonBits": 4,
                "requestCount": 1, "lookupHits": 0, "state": 1,
            }]},
            {"frame": 3, "surfaceCacheEvents": [{
                "sequence": 1, "sceneGeneration": 1, "cardID": 7, "pageID": 3,
                "generation": 2, "requestFrame": 1, "captureFrame": 2,
                "readyFrame": 0, "firstHitFrame": 0, "reasonBits": 4,
                "requestCount": 1, "lookupHits": 0, "state": 2,
            }]},
            {"frame": 4, "surfaceCacheEvents": [{
                "sequence": 1, "sceneGeneration": 1, "cardID": 7, "pageID": 3,
                "generation": 2, "requestFrame": 1, "captureFrame": 2,
                "readyFrame": 3, "firstHitFrame": 0, "reasonBits": 4,
                "requestCount": 1, "lookupHits": 0, "state": 3,
            }, {
                "sequence": 2, "sceneGeneration": 1, "cardID": 8, "pageID": 0,
                "generation": 0, "requestFrame": 2, "captureFrame": 0,
                "readyFrame": 0, "firstHitFrame": 0, "reasonBits": 2,
                "requestCount": 1, "lookupHits": 0, "state": 5,
            }]},
        ],
    }
    for lifecycle_sample in lifecycle["samples"]:
        lifecycle_frame = int(lifecycle_sample["frame"])
        lifecycle_sample["stats"] = {
            "surfaceCacheFrameIndex": max(0, lifecycle_frame - 1),
            "schedulerFrameIndex": max(0, lifecycle_frame),
            "surfaceCacheSceneGeneration": 1,
        }
    lifecycle_result = evaluate_case(lifecycle)
    assert lifecycle_result["status"] == "PASS", lifecycle_result
    assert lifecycle_result["frameOriginCoverage"] is True
    assert lifecycle_result["cardAssociationCoverage"] is True
    assert lifecycle_result["terminalOutcomeCount"] == 1
    assert lifecycle_result["clockDomain"] == "surfaceCacheFrameIndex"
    invalid_terminal = json.loads(json.dumps(lifecycle))
    invalid_terminal["samples"][-1]["surfaceCacheEvents"][-1]["reasonBits"] = 0
    assert evaluate_case(invalid_terminal)["status"] == "BLOCKED"
    unresolved = json.loads(json.dumps(lifecycle))
    unresolved["samples"][-1]["surfaceCacheEvents"] = [{
        "sequence": 3, "sceneGeneration": 1, "cardID": 9, "pageID": 0,
        "generation": 0, "requestFrame": 4, "captureFrame": 0,
        "readyFrame": 0, "firstHitFrame": 0, "reasonBits": 4,
        "requestCount": 1, "lookupHits": 0, "state": 1,
    }]
    assert evaluate_case(unresolved)["status"] == "BLOCKED"
    ring_gap = json.loads(json.dumps(lifecycle))
    ring_gap["samples"][2]["surfaceCacheEvents"].append({
        "sequence": 2, "sceneGeneration": 1, "cardID": 8, "pageID": 0,
        "generation": 0, "requestFrame": 2, "captureFrame": 0,
        "readyFrame": 0, "firstHitFrame": 0, "reasonBits": 2,
        "requestCount": 1, "lookupHits": 0, "state": 5,
    })
    ring_gap["samples"][-1]["surfaceCacheEvents"] = [ring_gap["samples"][-1]["surfaceCacheEvents"][0]]
    assert evaluate_case(ring_gap)["status"] == "BLOCKED"
    print("C6_NEXT_FRAME_FIXTURE PASS pass=same-frame-blocked sparse-blocked missing-blocked no-request-blocked")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="surfacecache-effect.json report")
    parser.add_argument("--output", help="write strict gate JSON")
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
        print("C6_NEXT_FRAME BLOCKED:", exc)
        return 2
    result = evaluate_report(report)
    output_path = os.path.abspath(args.output) if args.output else os.path.splitext(input_path)[0] + "-nextframe-gate.json"
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    print("C6_NEXT_FRAME_GATE", result["status"], output_path)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
