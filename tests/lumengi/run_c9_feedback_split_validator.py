#!/usr/bin/env python3
"""Validate C9 feedback/request split diagnostics and telemetry freshness.

This is an offline evidence check. It does not change C9 pixel thresholds and
does not promote either split diagnostic to a production pass. The validator
proves that the requested isolation mode was actually recorded, cache lookup
activity remained present, counters in the disabled domain were zero, and the
one-frame/two-frame telemetry lag stayed within the observed contract.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, Tuple


FEEDBACK_COUNTER_FIELDS = (
    "surfaceCacheFeedbackHits",
    "surfaceCacheFeedbackPages",
    "surfaceCacheFeedbackDedup",
    "surfaceCacheFeedbackStaleRejects",
)

REQUEST_COUNTER_FIELDS = (
    "surfaceCacheRequestRaw",
    "surfaceCacheRequestCards",
    "surfaceCacheRequestDedup",
    "surfaceCacheRequestStaleRejects",
    "surfaceCacheRequestCaptureCompleted",
    "requestRawThisFrame",
    "requestCardsThisFrame",
    "requestCaptureCompletedThisFrame",
    "surfaceCacheRequestReasonUnmapped",
    "surfaceCacheRequestReasonStaleOwner",
    "surfaceCacheRequestReasonMetadataInvalid",
    "surfaceCacheRequestReasonVisibilityInvalid",
)

FEEDBACK_FIELDS = FEEDBACK_COUNTER_FIELDS + REQUEST_COUNTER_FIELDS

FEEDBACK_FRAME_FIELDS = (
    "requestObservedFrame",
    "requestCaptureFrame",
    "cacheFeedbackStatsFrame",
)

TARGET_ATOMIC_FLAG_FIELDS = (
    "disableSurfaceCacheFeedbackPageAtomics",
    "disableSurfaceCacheRequestAtomics",
)


def _number(mapping: Dict[str, Any], key: str) -> Tuple[bool, float]:
    value = mapping.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False, 0.0
    return True, float(value)


def _bool_config(config: Dict[str, Any], key: str) -> Tuple[bool, bool]:
    value = config.get(key)
    if not isinstance(value, bool):
        return False, False
    return True, value


def _expected_mode(mode: str) -> Tuple[Dict[str, bool], Tuple[str, ...]]:
    if mode == "atomics-off":
        return ({
            "disableSurfaceCacheFeedback": False,
            "disableSurfaceCacheFeedbackAtomics": True,
            "disableSurfaceCacheFeedbackReadback": False,
            "disableSurfaceCacheFeedbackPageAtomics": True,
            "disableSurfaceCacheRequestAtomics": True,
        }, ("feedback", "request"))
    if mode == "readback-off":
        return ({
            "disableSurfaceCacheFeedback": False,
            "disableSurfaceCacheFeedbackAtomics": False,
            "disableSurfaceCacheFeedbackReadback": True,
            "disableSurfaceCacheFeedbackPageAtomics": False,
            "disableSurfaceCacheRequestAtomics": False,
        }, ("feedback", "request"))
    if mode == "feedback-atomics-off":
        return ({
            "disableSurfaceCacheFeedback": False,
            "disableSurfaceCacheFeedbackAtomics": False,
            "disableSurfaceCacheFeedbackReadback": False,
            "disableSurfaceCacheFeedbackPageAtomics": True,
            "disableSurfaceCacheRequestAtomics": False,
        }, ("feedback",))
    if mode == "request-atomics-off":
        return ({
            "disableSurfaceCacheFeedback": False,
            "disableSurfaceCacheFeedbackAtomics": False,
            "disableSurfaceCacheFeedbackReadback": False,
            "disableSurfaceCacheFeedbackPageAtomics": False,
            "disableSurfaceCacheRequestAtomics": True,
        }, ("request",))
    raise ValueError(f"unsupported mode: {mode}")


def validate(payload: Dict[str, Any], mode: str) -> Dict[str, Any]:
    expected_flags, zero_groups = _expected_mode(mode)
    legacy_mode = mode in ("atomics-off", "readback-off")
    deterministic = payload.get("deterministicReplay")
    if not isinstance(deterministic, dict):
        return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                "reason": "missing deterministicReplay metadata"}
    config = deterministic.get("config")
    screen = payload.get("screenProbeStats")
    cache = payload.get("surfaceCacheStats")
    final_color = payload.get("finalColor")
    if not all(isinstance(value, dict) for value in (config, screen, cache, final_color)):
        return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                "reason": "missing config or telemetry objects"}

    # Artifacts produced before the page/request split may omit both target
    # flags. Preserve their compatibility for the two original modes, but a
    # partially present pair is ambiguous and therefore BLOCKED.
    for mapping, label in ((config, "config"), (cache, "surfaceCacheStats")):
        present = [key in mapping for key in TARGET_ATOMIC_FLAG_FIELDS]
        if legacy_mode and any(present) and not all(present):
            return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                    "reason": f"partial target atomic flags in {label}"}

    checks: Dict[str, Any] = {}
    for key, expected in expected_flags.items():
        ok, actual = _bool_config(config, key)
        if not ok:
            if legacy_mode and key in TARGET_ATOMIC_FLAG_FIELDS and not any(
                target_key in config for target_key in TARGET_ATOMIC_FLAG_FIELDS
            ):
                checks[key] = {"compatibility": "missing in legacy artifact", "pass": True}
                continue
            return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                    "reason": f"missing or non-boolean config flag: {key}"}
        checks[key] = {"expected": expected, "actual": actual, "pass": actual == expected}

    for key, expected in expected_flags.items():
        ok, actual = _number(cache, key)
        if not ok:
            if legacy_mode and key in TARGET_ATOMIC_FLAG_FIELDS and not any(
                target_key in cache for target_key in TARGET_ATOMIC_FLAG_FIELDS
            ):
                checks[f"telemetry.{key}"] = {"compatibility": "missing in legacy artifact", "pass": True}
                continue
            return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                    "reason": f"missing or non-numeric surface-cache flag: {key}"}
        checks[f"telemetry.{key}"] = {"expected": float(expected), "actual": actual, "pass": actual == expected}

    ok_attempts, attempts = _number(screen, "cacheLookupAttempts")
    ok_hits, hits = _number(screen, "cacheLookupHits")
    if not ok_attempts or not ok_hits:
        return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                "reason": "missing cache lookup counters"}
    checks["cacheLookupActivity"] = {"attempts": attempts, "hits": hits, "pass": attempts > 0 and hits > 0}

    missing = []
    values = {}
    for key in FEEDBACK_FIELDS:
        ok, value = _number(cache, key)
        if not ok:
            missing.append(key)
        else:
            values[key] = value
    if missing:
        return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                "reason": "missing feedback/request telemetry", "missing": missing}
    group_fields = {"feedback": FEEDBACK_COUNTER_FIELDS, "request": REQUEST_COUNTER_FIELDS}
    disabled_nonzero = {
        key: values[key]
        for group in zero_groups
        for key in group_fields[group]
        if values[key] != 0.0
    }
    checks["disabledCountersZero"] = {"pass": not disabled_nonzero, "nonzero": disabled_nonzero}

    active_group = {
        "feedback-atomics-off": "request",
        "request-atomics-off": "feedback",
    }.get(mode)
    if active_group:
        active_fields = {"feedback": FEEDBACK_COUNTER_FIELDS, "request": REQUEST_COUNTER_FIELDS}[active_group]
        active_activity = {key: values[key] for key in active_fields if values[key] > 0.0}
        if not active_activity:
            return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                    "reason": f"enabled {active_group} atomics have no observed activity"}
        checks["enabledCounterActivity"] = {"group": active_group, "positive": active_activity, "pass": True}

    frame_values = {}
    for key in FEEDBACK_FRAME_FIELDS:
        ok, value = _number(cache, key)
        if not ok:
            return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                    "reason": f"missing feedback/request frame stamp: {key}"}
        frame_values[key] = value
    # Atomics-off + readback-on legitimately stamps the submitted zeroed copy
    # (for example cacheFeedbackStatsFrame=95). Readback-off must leave every
    # feedback/request stamp untouched at zero because no copy is submitted.
    frame_pass = mode != "readback-off" or all(value == 0.0 for value in frame_values.values())
    checks["feedbackRequestFrameStamps"] = {"pass": frame_pass, "values": frame_values}

    ok_lookup_screen, lookup_screen = _number(screen, "cacheLookupStatsFrame")
    ok_lookup_cache, lookup_cache = _number(cache, "cacheLookupStatsFrame")
    ok_final_frame, final_frame = _number(final_color, "frame")
    ok_surface_frame, surface_frame = _number(cache, "surfaceCacheFrameIndex")
    ok_scheduler_frame, scheduler_frame = _number(cache, "schedulerFrameIndex")
    if not all((ok_lookup_screen, ok_lookup_cache, ok_final_frame, ok_surface_frame, ok_scheduler_frame)):
        return {"schema": "LumenGI.C9.FeedbackSplit.v1", "status": "BLOCKED", "mode": mode,
                "reason": "missing telemetry provenance frame fields"}
    provenance = {
        "captureFrame": final_frame,
        "lookupStatsFrame": lookup_screen,
        "lookupFramesEqual": lookup_screen == lookup_cache,
        "lookupLagFrames": final_frame - lookup_screen,
        "surfaceCacheFrame": surface_frame,
        "schedulerFrame": scheduler_frame,
        "schedulerSurfaceLagFrames": scheduler_frame - surface_frame,
    }
    provenance_pass = (
        provenance["lookupFramesEqual"]
        and 0.0 <= provenance["lookupLagFrames"] <= 2.0
        and 0.0 <= provenance["schedulerSurfaceLagFrames"] <= 1.0
    )
    checks["telemetryProvenance"] = {**provenance, "pass": provenance_pass}

    failed = [name for name, value in checks.items() if isinstance(value, dict) and value.get("pass") is False]
    status = "FAIL" if failed else "PASS"
    reason = "; ".join(failed) if failed else "split mode and telemetry provenance are valid"
    return {
        "schema": "LumenGI.C9.FeedbackSplit.v1",
        "status": status,
        "mode": mode,
        "reason": reason,
        "checks": checks,
    }


def _fixture(mode: str) -> Dict[str, Any]:
    flags, _ = _expected_mode(mode)
    cache = {
        key: float(value) for key, value in flags.items()
    }
    cache.update({
        "cacheLookupStatsFrame": 94.0,
        "surfaceCacheFrameIndex": 95.0,
        "schedulerFrameIndex": 96.0,
    })
    for key in FEEDBACK_FIELDS:
        cache[key] = 0.0
    for key in FEEDBACK_FRAME_FIELDS:
        cache[key] = 0.0
    if mode == "feedback-atomics-off":
        cache["surfaceCacheRequestRaw"] = 3.0
    elif mode == "request-atomics-off":
        cache["surfaceCacheFeedbackHits"] = 3.0
    return {
        "deterministicReplay": {"config": {
            **flags,
        }},
        "screenProbeStats": {"cacheLookupAttempts": 100.0, "cacheLookupHits": 5.0, "cacheLookupStatsFrame": 94.0},
        "surfaceCacheStats": cache,
        "finalColor": {"frame": 96.0},
    }


def self_test() -> None:
    for mode in ("atomics-off", "readback-off", "feedback-atomics-off", "request-atomics-off"):
        result = validate(_fixture(mode), mode)
        assert result["status"] == "PASS", (mode, result)
    feedback_fixture = _fixture("feedback-atomics-off")
    feedback_fixture["surfaceCacheStats"]["surfaceCacheRequestRaw"] = 3.0
    assert validate(feedback_fixture, "feedback-atomics-off")["status"] == "PASS"
    request_fixture = _fixture("request-atomics-off")
    request_fixture["surfaceCacheStats"]["surfaceCacheFeedbackHits"] = 3.0
    assert validate(request_fixture, "request-atomics-off")["status"] == "PASS"
    legacy = _fixture("atomics-off")
    for key in TARGET_ATOMIC_FLAG_FIELDS:
        legacy["deterministicReplay"]["config"].pop(key)
        legacy["surfaceCacheStats"].pop(key)
    assert validate(legacy, "atomics-off")["status"] == "PASS"
    partial = _fixture("atomics-off")
    partial["deterministicReplay"]["config"].pop(TARGET_ATOMIC_FLAG_FIELDS[0])
    assert validate(partial, "atomics-off")["status"] == "BLOCKED"
    bad = _fixture("atomics-off")
    bad["surfaceCacheStats"]["schedulerFrameIndex"] = 94.0
    assert validate(bad, "atomics-off")["status"] == "FAIL"
    blocked = _fixture("atomics-off")
    del blocked["surfaceCacheStats"]["cacheLookupStatsFrame"]
    assert validate(blocked, "atomics-off")["status"] == "BLOCKED"
    print("C9_FEEDBACK_SPLIT_SELF_TEST PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path)
    parser.add_argument(
        "--mode",
        choices=("atomics-off", "readback-off", "feedback-atomics-off", "request-atomics-off"),
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.input or not args.mode:
        parser.error("--input and --mode are required unless --self-test is used")
    payload = json.loads(args.input.read_text(encoding="utf-8"))
    result = validate(payload, args.mode)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("C9_FEEDBACK_SPLIT", result["status"], result.get("reason", ""))
    return 0 if result["status"] == "PASS" else 1 if result["status"] == "FAIL" else 2


if __name__ == "__main__":
    raise SystemExit(main())
