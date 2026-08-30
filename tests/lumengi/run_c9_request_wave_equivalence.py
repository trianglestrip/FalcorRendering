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


def _canonical_events(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    # Sequence is included: it proves the same accepted-card order reached the
    # scheduler.  Keep the complete event fields so a reason/count mismatch
    # cannot hide behind equal aggregate totals.
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


def evaluate(baseline_path: Path, candidate_path: Path) -> dict[str, Any]:
    baseline_path = baseline_path.resolve()
    candidate_path = candidate_path.resolve()
    baseline = _read(baseline_path)
    candidate = _read(candidate_path)
    baseline_config = _config(baseline, baseline_path)
    candidate_config = _config(candidate, candidate_path)
    if baseline_config.get("enableSurfaceCacheRequestWaveAggregation") is not False:
        raise EvidenceBlocked("baseline must have wave aggregation disabled")
    if candidate_config.get("enableSurfaceCacheRequestWaveAggregation") is not True:
        raise EvidenceBlocked("candidate must have wave aggregation enabled")

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
    baseline_events = _canonical_events(_events(baseline, baseline_path))
    candidate_events = _canonical_events(_events(candidate, candidate_path))
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
            "request aggregates or per-card scheduler ledger differ: "
            + json.dumps({"stats": stat_differences, "events": event_detail}, sort_keys=True)
        )

    return {
        "schema": "LumenGI.C9.RequestWaveEquivalence.v1",
        "status": "PASS",
        "baseline": str(baseline_path),
        "candidate": str(candidate_path),
        "requestStats": {name: baseline_stats.get(name) for name in REQUEST_STATS},
        "eventCount": len(baseline_events),
        "comparison": "exact_request_aggregates_and_scheduler_event_ledger",
    }


def _fixture(wave: bool) -> dict[str, Any]:
    config = {
        "scene": "fixture",
        "view": "front",
        "enableSurfaceCacheRequestWaveAggregation": wave,
    }
    stats = {name: 7 for name in REQUEST_STATS}
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
    return {"deterministicReplay": {"config": config}, "surfaceCacheStats": stats, "surfaceCacheEvents": events}


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
    print("C9_REQUEST_WAVE_EQUIVALENCE_SELF_TEST PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline")
    parser.add_argument("--candidate")
    parser.add_argument("--output")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.baseline or not args.candidate:
        parser.error("--baseline and --candidate are required")
    try:
        result = evaluate(Path(args.baseline), Path(args.candidate))
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
