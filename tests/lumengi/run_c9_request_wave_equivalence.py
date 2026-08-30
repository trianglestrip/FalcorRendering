"""Compare scheduler-visible request telemetry for a request-wave A/B.

This is a diagnostic validator, not a C9 pixel gate.  It requires one replay
artifact with ``enableSurfaceCacheRequestWaveAggregation=false`` and another
with it enabled, then compares the GPU request aggregates and the accepted
per-card event ledger.  Missing or incompatible telemetry is BLOCKED; any
count/reason/card identity difference is FAIL.  No threshold is changed.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
REQUEST_STATS = (
    "surfaceCacheRequestRaw",
    "surfaceCacheRequestCards",
    "surfaceCacheRequestDedup",
    "surfaceCacheRequestStaleRejects",
    "surfaceCacheRequestReasonUnmapped",
    "surfaceCacheRequestReasonStaleOwner",
    "surfaceCacheRequestReasonMetadataInvalid",
    "surfaceCacheRequestReasonVisibilityInvalid",
)


class EvidenceBlocked(RuntimeError):
    pass


class EvidenceFailed(RuntimeError):
    pass


def _read(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise EvidenceBlocked(f"artifact not found: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise EvidenceBlocked(f"invalid JSON: {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceBlocked(f"JSON root must be an object: {path}")
    return value


def _config(runtime: dict[str, Any], path: Path) -> dict[str, Any]:
    replay = runtime.get("deterministicReplay")
    config = replay.get("config") if isinstance(replay, dict) else None
    if not isinstance(config, dict):
        raise EvidenceBlocked(f"deterministicReplay.config missing: {path}")
    if config.get("enableSurfaceCacheRequestWaveAggregation") not in (True, False):
        raise EvidenceBlocked(f"wave aggregation flag missing/invalid: {path}")
    return config


def _events(runtime: dict[str, Any], path: Path) -> list[dict[str, Any]]:
    events = runtime.get("surfaceCacheEvents")
    if not isinstance(events, list):
        raise EvidenceBlocked(f"surfaceCacheEvents missing: {path}")
    if not all(isinstance(event, dict) for event in events):
        raise EvidenceBlocked(f"surfaceCacheEvents contains a non-object: {path}")
    return events


def _process_id(runtime: dict[str, Any], path: Path) -> int | str:
    """Return a usable artifact process id, or block same-process validation."""
    value: Any
    if "processId" in runtime:
        name = "processId"
        value = runtime.get("processId")
    else:
        replay = runtime.get("deterministicReplay")
        if not isinstance(replay, dict) or "processId" not in replay:
            raise EvidenceBlocked(f"processId missing/invalid: {path}")
        name = "deterministicReplay.processId"
        value = replay.get("processId")
    if isinstance(value, bool):
        raise EvidenceBlocked(f"{name} missing/invalid: {path}")
    if isinstance(value, int) and value > 0:
        return value
    if isinstance(value, str) and value.strip():
        return value.strip()
    raise EvidenceBlocked(f"{name} missing/invalid: {path}")


def _replay_metadata(runtime: dict[str, Any], path: Path) -> dict[str, Any]:
    replay = runtime.get("deterministicReplay")
    if not isinstance(replay, dict):
        raise EvidenceBlocked(f"deterministicReplay missing/invalid: {path}")
    for field in ("sameProcess", "graphRecreated", "sceneReloaded"):
        if replay.get(field) is not True:
            raise EvidenceBlocked(f"deterministicReplay.{field} must be true: {path}")
    return replay


def _non_empty_text(value: Any, field: str, path: Path) -> str:
    if not isinstance(value, str) or not value.strip():
        raise EvidenceBlocked(f"deterministicReplay.{field} missing/invalid: {path}")
    return value.strip()


def _finite_number(stats: dict[str, Any], field: str, path: Path) -> float:
    value = stats.get(field)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        raise EvidenceBlocked(f"surfaceCacheStats.{field} missing/invalid: {path}")
    return float(value)


def _validate_same_process_evidence(
    baseline: dict[str, Any],
    candidate: dict[str, Any],
    baseline_path: Path,
    candidate_path: Path,
    baseline_config: dict[str, Any],
    candidate_config: dict[str, Any],
) -> int | str:
    baseline_replay = _replay_metadata(baseline, baseline_path)
    candidate_replay = _replay_metadata(candidate, candidate_path)
    baseline_process_id = _process_id(baseline, baseline_path)
    candidate_process_id = _process_id(candidate, candidate_path)
    if baseline_process_id != candidate_process_id:
        raise EvidenceBlocked(
            "same-process requirement failed: processId differs: "
            + repr((baseline_process_id, candidate_process_id))
        )

    baseline_pair_id = _non_empty_text(baseline_replay.get("pairId"), "pairId", baseline_path)
    candidate_pair_id = _non_empty_text(candidate_replay.get("pairId"), "pairId", candidate_path)
    if baseline_pair_id != candidate_pair_id:
        raise EvidenceBlocked("same-process pairId differs")
    baseline_fingerprint = _non_empty_text(
        baseline_replay.get("baseConfigFingerprint"), "baseConfigFingerprint", baseline_path
    )
    candidate_fingerprint = _non_empty_text(
        candidate_replay.get("baseConfigFingerprint"), "baseConfigFingerprint", candidate_path
    )
    if baseline_fingerprint != candidate_fingerprint:
        raise EvidenceBlocked("same-process baseConfigFingerprint differs")

    for runtime, config, expected, path in (
        (baseline, baseline_config, False, baseline_path),
        (candidate, candidate_config, True, candidate_path),
    ):
        stats = runtime.get("surfaceCacheStats")
        if not isinstance(stats, dict):
            raise EvidenceBlocked(f"surfaceCacheStats missing/invalid: {path}")
        if config.get("enableSurfaceCacheRequestWaveAggregation") is not expected:
            raise EvidenceBlocked(f"wave aggregation config disagrees with expected phase: {path}")
        observed = _finite_number(stats, "enableSurfaceCacheRequestWaveAggregation", path)
        if observed not in (0.0, 1.0) or bool(observed) != expected:
            raise EvidenceBlocked(
                "surfaceCacheStats.enableSurfaceCacheRequestWaveAggregation disagrees with config: "
                + str(path)
            )
        raw = _finite_number(stats, "surfaceCacheRequestRaw", path)
        cards = _finite_number(stats, "surfaceCacheRequestCards", path)
        if raw <= 0 or cards <= 0:
            raise EvidenceBlocked(f"positive request activity missing: {path}")
        events = _events(runtime, path)
        if not events:
            raise EvidenceBlocked(f"positive event ledger activity missing: {path}")
        dropped_fields = [field for field in ("surfaceCacheEventDropped", "eventDropped") if field in stats]
        if not dropped_fields:
            raise EvidenceBlocked(f"eventDropped missing: {path}")
        for field in dropped_fields:
            if _finite_number(stats, field, path) != 0.0:
                raise EvidenceBlocked(f"eventDropped must be zero: {path}")
    return baseline_process_id


def _canonical_events(events: list[dict[str, Any]], scope: str = "exact") -> list[dict[str, Any]]:
    # Sequence is included in both scopes: it proves the same accepted-card
    # order reached the scheduler.  ``request`` intentionally excludes fields
    # populated after the request sink (capture/ready/first-hit/state and
    # lookup-hit counts), allowing a semantic A/B to report lifecycle jitter
    # separately without weakening the default exact gate.
    if scope == "request":
        fields = (
            "sequence",
            "sceneGeneration",
            "cardID",
            "pageID",
            "generation",
            "requestFrame",
            "reasonBits",
            "requestCount",
        )
    else:
        fields = (
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
    return [{field: event.get(field) for field in fields} for event in events]


def evaluate(
    baseline_path: Path,
    candidate_path: Path,
    scope: str = "exact",
    require_same_process: bool = False,
) -> dict[str, Any]:
    if scope not in ("exact", "request"):
        raise EvidenceBlocked(f"unsupported comparison scope: {scope}")
    baseline_path = baseline_path.resolve()
    candidate_path = candidate_path.resolve()
    baseline = _read(baseline_path)
    candidate = _read(candidate_path)
    same_process_id: int | str | None = None
    baseline_config = _config(baseline, baseline_path)
    candidate_config = _config(candidate, candidate_path)
    if baseline_config.get("enableSurfaceCacheRequestWaveAggregation") is not False:
        raise EvidenceBlocked("baseline must have wave aggregation disabled")
    if candidate_config.get("enableSurfaceCacheRequestWaveAggregation") is not True:
        raise EvidenceBlocked("candidate must have wave aggregation enabled")
    if require_same_process:
        same_process_id = _validate_same_process_evidence(
            baseline,
            candidate,
            baseline_path,
            candidate_path,
            baseline_config,
            candidate_config,
        )

    # The experiment must vary only the aggregation switch.  The replay runner
    # already fingerprints this payload; comparing the actual config here gives
    # a direct diagnostic error instead of silently mixing scenes or schedules.
    differing = {
        key
        for key in set(baseline_config) | set(candidate_config)
        if baseline_config.get(key) != candidate_config.get(key)
    }
    if differing != {"enableSurfaceCacheRequestWaveAggregation"}:
        raise EvidenceBlocked("A/B config differs outside the wave aggregation flag: " + repr(sorted(differing)))

    baseline_stats = baseline.get("surfaceCacheStats")
    candidate_stats = candidate.get("surfaceCacheStats")
    if not isinstance(baseline_stats, dict) or not isinstance(candidate_stats, dict):
        raise EvidenceBlocked("surfaceCacheStats missing from one or both artifacts")
    stat_differences = {
        name: (baseline_stats.get(name), candidate_stats.get(name))
        for name in REQUEST_STATS
        if baseline_stats.get(name) != candidate_stats.get(name)
    }
    baseline_events = _canonical_events(_events(baseline, baseline_path), scope)
    candidate_events = _canonical_events(_events(candidate, candidate_path), scope)
    if stat_differences or baseline_events != candidate_events:
        event_detail: dict[str, Any] = {
            "baselineEventCount": len(baseline_events),
            "candidateEventCount": len(candidate_events),
        }
        for index, (left, right) in enumerate(zip(baseline_events, candidate_events)):
            if left != right:
                event_detail["firstEventDifference"] = {"index": index, "baseline": left, "candidate": right}
                break
        raise EvidenceFailed(
            "%s aggregates or per-card ledger differ: " % scope
            + json.dumps({"stats": stat_differences, "events": event_detail}, sort_keys=True)
        )

    result = {
        "schema": "LumenGI.C9.RequestWaveEquivalence.v1",
        "status": "PASS",
        "baseline": str(baseline_path),
        "candidate": str(candidate_path),
        "requestStats": {name: baseline_stats.get(name) for name in REQUEST_STATS},
        "eventCount": len(baseline_events),
        "comparison": (
            "exact_request_aggregates_and_scheduler_event_ledger"
            if scope == "exact"
            else "exact_request_aggregates_and_request_sink_event_ledger"
        ),
        "scope": scope,
    }
    if require_same_process:
        result["sameProcess"] = {"required": True, "processId": same_process_id}
    return result


def _fixture(wave: bool) -> dict[str, Any]:
    config = {
        "scene": "fixture",
        "view": "front",
        "enableSurfaceCacheRequestWaveAggregation": wave,
    }
    stats = {name: 7 for name in REQUEST_STATS}
    stats["enableSurfaceCacheRequestWaveAggregation"] = 1.0 if wave else 0.0
    stats["surfaceCacheEventDropped"] = 0.0
    events = [{
        "sequence": 1,
        "sceneGeneration": 1,
        "cardID": 3,
        "pageID": 4,
        "generation": 2,
        "requestFrame": 8,
        "captureFrame": 9,
        "readyFrame": 10,
        "firstHitFrame": 11,
        "reasonBits": 6,
        "requestCount": 7,
        "lookupHits": 2,
        "state": 4,
    }]
    return {
        "deterministicReplay": {
            "config": config,
            "processId": 4242,
            "pairId": "fixture-pair",
            "baseConfigFingerprint": "fixture-base",
            "sameProcess": True,
            "graphRecreated": True,
            "sceneReloaded": True,
        },
        "surfaceCacheStats": stats,
        "surfaceCacheEvents": events,
    }


def self_test() -> None:
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        baseline_path = root / "baseline.json"
        candidate_path = root / "candidate.json"
        baseline_path.write_text(json.dumps(_fixture(False)), encoding="utf-8")
        candidate_path.write_text(json.dumps(_fixture(True)), encoding="utf-8")
        result = evaluate(baseline_path, candidate_path)
        assert result["status"] == "PASS"
        same_process_result = evaluate(
            baseline_path,
            candidate_path,
            require_same_process=True,
        )
        assert same_process_result["status"] == "PASS"
        assert same_process_result["sameProcess"] == {"required": True, "processId": 4242}

        different_process = _fixture(True)
        different_process["deterministicReplay"]["processId"] = 4243
        candidate_path.write_text(json.dumps(different_process), encoding="utf-8")
        try:
            evaluate(baseline_path, candidate_path, require_same_process=True)
        except EvidenceBlocked:
            pass
        else:
            raise AssertionError("different process ids must block")

        missing_process = _fixture(True)
        del missing_process["deterministicReplay"]["processId"]
        candidate_path.write_text(json.dumps(missing_process), encoding="utf-8")
        try:
            evaluate(baseline_path, candidate_path, require_same_process=True)
        except EvidenceBlocked:
            pass
        else:
            raise AssertionError("missing process id must block")

        root_process_baseline = _fixture(False)
        root_process_candidate = _fixture(True)
        for fixture in (root_process_baseline, root_process_candidate):
            fixture["processId"] = 4343
            del fixture["deterministicReplay"]["processId"]
        baseline_path.write_text(json.dumps(root_process_baseline), encoding="utf-8")
        candidate_path.write_text(json.dumps(root_process_candidate), encoding="utf-8")
        root_process_result = evaluate(
            baseline_path,
            candidate_path,
            require_same_process=True,
        )
        assert root_process_result["sameProcess"]["processId"] == 4343

        missing_metadata = _fixture(True)
        missing_metadata["processId"] = 4343
        del missing_metadata["deterministicReplay"]["processId"]
        del missing_metadata["deterministicReplay"]["graphRecreated"]
        candidate_path.write_text(json.dumps(missing_metadata), encoding="utf-8")
        try:
            evaluate(baseline_path, candidate_path, require_same_process=True)
        except EvidenceBlocked:
            pass
        else:
            raise AssertionError("missing same-process metadata must block")

        changed = _fixture(True)
        changed["surfaceCacheStats"]["surfaceCacheRequestRaw"] = 8
        candidate_path.write_text(json.dumps(changed), encoding="utf-8")
        try:
            evaluate(baseline_path, candidate_path)
        except EvidenceFailed:
            pass
        else:
            raise AssertionError("counter mismatch must fail")

        bad = _fixture(True)
        bad["deterministicReplay"]["config"]["scene"] = "different"
        candidate_path.write_text(json.dumps(bad), encoding="utf-8")
        try:
            evaluate(baseline_path, candidate_path)
        except EvidenceBlocked:
            pass
        else:
            raise AssertionError("config mismatch must block")

        lifecycle_only = _fixture(True)
        lifecycle_only["surfaceCacheEvents"][0]["readyFrame"] = 99
        lifecycle_only["surfaceCacheEvents"][0]["lookupHits"] = 42
        candidate_path.write_text(json.dumps(lifecycle_only), encoding="utf-8")
        request_result = evaluate(baseline_path, candidate_path, scope="request")
        assert request_result["status"] == "PASS"
        try:
            evaluate(baseline_path, candidate_path, scope="exact")
        except EvidenceFailed:
            pass
        else:
            raise AssertionError("lifecycle mismatch must fail exact scope")
    print("C9_REQUEST_WAVE_EQUIVALENCE_SELF_TEST PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline")
    parser.add_argument("--candidate")
    parser.add_argument("--output")
    parser.add_argument(
        "--scope",
        choices=("exact", "request"),
        default="exact",
        help="compare the full scheduler ledger (default) or request-sink fields only",
    )
    parser.add_argument(
        "--require-same-process",
        action="store_true",
        help="require both artifacts to carry the same non-empty processId",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.baseline or not args.candidate:
        parser.error("--baseline and --candidate are required")
    try:
        result = evaluate(
            Path(args.baseline),
            Path(args.candidate),
            scope=args.scope,
            require_same_process=args.require_same_process,
        )
        status = "PASS"
        code = 0
    except EvidenceFailed as exc:
        result = {"schema": "LumenGI.C9.RequestWaveEquivalence.v1", "status": "FAIL", "reason": str(exc)}
        status = "FAIL"
        code = 1
    except EvidenceBlocked as exc:
        result = {"schema": "LumenGI.C9.RequestWaveEquivalence.v1", "status": "BLOCKED", "reason": str(exc)}
        status = "BLOCKED"
        code = 2
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("C9_REQUEST_WAVE_EQUIVALENCE", status)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
