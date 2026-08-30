"""Strict C9 mark-on/mark-off deterministic replay gate.

The producer script ``run_resolved_showcase.py`` can emit two executions from
one Mogwai process when ``LUMEN_C9_DETERMINISTIC_REPLAY_OUT`` is set.  Each
phase reloads the scene, recreates the RenderGraph, and executes the identical
1..N frame table.  This gate rejects cross-process, mismatched-config, missing
snapshot, or mislabeled pairs before applying the frozen pixel thresholds.

It never relaxes a threshold and never converts missing evidence into PASS.
On success the output is also a valid ``LumenGI.C9.FinalColorRuntime.v1``
artifact consumable by ``run_finalcolor_contract.py``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import tempfile
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
THRESHOLDS = {
    "meanAbsError": 2e-5,
    "p99AbsError": 5e-4,
    "maxAbsError": 5e-3,
    "relativeMaxError": 1e-4,
}
REQUIRED_INPUTS = ["DirectResolve.output", "LumenGI.resolvedDiffuseGI"]


class EvidenceMissing(RuntimeError):
    """Required runtime evidence was not supplied."""


class EvidenceInvalid(RuntimeError):
    """Supplied evidence contradicts the strict replay contract."""


def _read_json(path: Path) -> dict:
    if not path.exists():
        raise EvidenceMissing("artifact not found: " + str(path))
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise EvidenceInvalid("invalid JSON %s: %s" % (path, exc)) from exc
    if not isinstance(value, dict):
        raise EvidenceInvalid("JSON root must be an object: " + str(path))
    return value


def _resolve_path(value: object, owner: Path, manifest: Path) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise EvidenceMissing("artifact path is absent")
    path = Path(value)
    candidates = [path] if path.is_absolute() else [ROOT / path, owner.parent / path, manifest.parent / path]
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    raise EvidenceMissing("artifact path does not resolve: " + value)


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise EvidenceInvalid(reason)


def _snapshot(runtime: dict, runtime_path: Path, manifest_path: Path) -> tuple[np.ndarray, Path]:
    final = runtime.get("finalColor")
    if not isinstance(final, dict):
        raise EvidenceMissing("finalColor metadata is absent: " + str(runtime_path))
    snapshot_path = _resolve_path(final.get("snapshot"), runtime_path, manifest_path)
    try:
        pixels = np.load(snapshot_path, allow_pickle=False)
    except Exception as exc:
        raise EvidenceInvalid("invalid numpy snapshot %s: %s" % (snapshot_path, exc)) from exc
    _require(isinstance(pixels, np.ndarray), "snapshot is not an ndarray")
    _require(pixels.ndim == 3 and pixels.shape[2] >= 3, "snapshot must be HxWxC with C>=3")
    pixels = np.ascontiguousarray(pixels[..., :3])
    digest = hashlib.sha256(pixels.tobytes()).hexdigest()
    _require(final.get("sha256") == digest, "snapshot SHA-256 does not match finalColor metadata")
    _require(bool(np.isfinite(pixels).all()), "snapshot contains non-finite values")
    _require(float(np.min(pixels)) >= 0.0, "snapshot contains negative values")
    return pixels, snapshot_path


def _phase_contract(runtime: dict, phase: str, manifest: dict) -> dict:
    replay = runtime.get("deterministicReplay")
    final = runtime.get("finalColor")
    producer = runtime.get("producerEvidence")
    if not isinstance(replay, dict):
        raise EvidenceMissing("deterministicReplay metadata is absent for " + phase)
    if not isinstance(final, dict) or not isinstance(producer, dict):
        raise EvidenceMissing("finalColor/producerEvidence metadata is absent for " + phase)
    expected_marked = phase == "mark-on"
    _require(replay.get("phase") == phase, "phase label mismatch for " + phase)
    _require(replay.get("pairId") == manifest.get("pairId"), "pairId mismatch for " + phase)
    _require(replay.get("processId") == manifest.get("processId"), "processId mismatch for " + phase)
    _require(replay.get("configFingerprint") == manifest.get("configFingerprint"), "config fingerprint mismatch for " + phase)
    _require(replay.get("sceneReloaded") is True, "scene reload was not recorded for " + phase)
    _require(replay.get("graphRecreated") is True, "graph recreation was not recorded for " + phase)
    _require(replay.get("captureExecutions") == 1, "capture execution count must be exactly one for " + phase)
    frames = replay.get("clockFrames")
    _require(isinstance(frames, list) and len(frames) == 2 and frames[0] == 1 and isinstance(frames[1], int) and frames[1] >= 1,
             "clockFrames must describe the bounded 1..N schedule for " + phase)
    _require(final.get("endpoint") == "ResolvedCompositePreview.out", "wrong final endpoint for " + phase)
    _require(final.get("marked") is True, "production composite must remain marked for " + phase)
    _require(final.get("lumenOutputsMarked") is expected_marked, "Lumen diagnostic mark policy mismatch for " + phase)
    _require(final.get("finite") is True and final.get("nonnegative") is True, "invalid finalColor range metadata for " + phase)
    _require(final.get("directEnabled") is True and final.get("indirectEnabled") is True, "full direct+indirect producer is required")
    _require(final.get("compositeInputs") == REQUIRED_INPUTS, "composite inputs do not match the C9 contract")
    _require(producer.get("directEnabled") is True and producer.get("indirectEnabled") is True, "producer evidence is incomplete")
    _require(producer.get("compositeInputs") == REQUIRED_INPUTS, "producer inputs do not match the C9 contract")
    return {"replay": replay, "final": final, "producer": producer}


def evaluate(manifest_path: Path) -> dict:
    manifest_path = manifest_path.resolve()
    manifest = _read_json(manifest_path)
    _require(manifest.get("schema") == "LumenGI.C9.DeterministicReplayManifest.v1", "unsupported replay manifest schema")
    _require(isinstance(manifest.get("pairId"), str) and bool(manifest["pairId"]), "manifest pairId is absent")
    _require(isinstance(manifest.get("processId"), int) and manifest["processId"] > 0, "manifest processId is invalid")
    _require(isinstance(manifest.get("configFingerprint"), str) and len(manifest["configFingerprint"]) == 64,
             "manifest config fingerprint is invalid")
    on_path = _resolve_path(manifest.get("markOn"), manifest_path, manifest_path)
    off_path = _resolve_path(manifest.get("markOff"), manifest_path, manifest_path)
    _require(on_path != off_path, "mark-on and mark-off runtime artifacts must be distinct")
    on = _read_json(on_path)
    off = _read_json(off_path)
    on_meta = _phase_contract(on, "mark-on", manifest)
    off_meta = _phase_contract(off, "mark-off", manifest)
    _require(on_meta["replay"].get("config") == off_meta["replay"].get("config"), "replay config payloads differ")
    _require(on_meta["replay"].get("clockFrames") == off_meta["replay"].get("clockFrames"), "frame schedules differ")
    for name in ("scene", "view", "frame", "endpoint", "compositeInputs"):
        _require(on_meta["final"].get(name) == off_meta["final"].get(name), "finalColor %s differs between phases" % name)

    on_pixels, on_snapshot = _snapshot(on, on_path, manifest_path)
    off_pixels, off_snapshot = _snapshot(off, off_path, manifest_path)
    _require(on_pixels.shape == off_pixels.shape, "snapshot shapes differ")
    error = np.abs(on_pixels.astype(np.float64) - off_pixels.astype(np.float64))
    metrics = {
        "meanAbsError": float(np.mean(error)),
        "p99AbsError": float(np.percentile(error, 99.0)),
        "maxAbsError": float(np.max(error)),
        "relativeMaxError": float(np.max(error) / max(1.0, abs(float(np.max(on_pixels))))),
        "exact": bool(np.array_equal(on_pixels, off_pixels)),
        "tolerance": dict(THRESHOLDS),
    }
    finite_metrics = all(math.isfinite(float(metrics[name])) for name in THRESHOLDS)
    within = finite_metrics and all(float(metrics[name]) <= limit for name, limit in THRESHOLDS.items())
    status = "PASS" if within else "FAIL"
    reason = None if within else "deterministic replay pixel error exceeds the frozen C9 tolerance"

    # Emit a strict runtime artifact understood by run_finalcolor_contract.py.
    # The same-process transition sidecar stays BLOCKED because this test uses
    # two fresh graphs; full PASS comes from explicit on/off exports + metrics.
    return {
        "schema": "LumenGI.C9.FinalColorRuntime.v1",
        "status": status,
        "producerEvidence": off_meta["producer"],
        "finalColor": off_meta["final"],
        "deterministicReplay": {
            "status": status,
            "pairId": manifest["pairId"],
            "processId": manifest["processId"],
            "configFingerprint": manifest["configFingerprint"],
            "markOnRuntime": str(on_path),
            "markOffRuntime": str(off_path),
        },
        "markOn": {"status": "PASS", "lumenOutputsMarked": True, "phase": "mark-on"},
        "markOff": {"status": "PASS", "lumenOutputsMarked": False, "phase": "mark-off"},
        "exportOn": {"status": "PASS", "snapshot": str(on_snapshot), "sameProcess": True},
        "exportOff": {"status": "PASS", "snapshot": str(off_snapshot), "sameProcess": True},
        "exportEquivalence": {
            "status": status,
            "comparisonMode": "same_process_deterministic_replay",
            "reason": reason,
            "metrics": metrics,
        },
        "sameProcessMarkTransition": {
            "status": "BLOCKED",
            "comparisonMode": "not_applicable_fresh_graph_replay",
            "reason": "strict evidence comes from two fresh same-process graph executions, not an in-place transition",
        },
    }


def evaluate_same_frame_runtime(runtime_path: Path) -> dict:
    """Validate the zero-execute retained-resource proof as bounded evidence.

    This intentionally cannot return full PASS.  It proves that changing the
    diagnostic output metadata did not mutate the already executed composite;
    it does not claim that a newly compiled mark-off graph was executed.
    """
    runtime_path = runtime_path.resolve()
    runtime = _read_json(runtime_path)
    evidence = runtime.get("sameProcessMarkTransition")
    if not isinstance(evidence, dict):
        raise EvidenceMissing("sameProcessMarkTransition evidence is absent")
    state = str(evidence.get("status", "")).upper()
    if state == "BLOCKED":
        return {
            "schema": "LumenGI.C9.SameFrameResourceInvariance.v1",
            "status": "BLOCKED",
            "reason": evidence.get("reason") or "same-frame evidence is blocked",
            "runtime": str(runtime_path),
        }
    _require(state in ("PASS", "PASS_BOUNDED"), "same-frame evidence status is not pass-like")
    required = {
        "comparisonMode": "same_process_graph_unmark",
        "renderedFrames": 0,
        "producerExecutions": 0,
        "graphRecompiled": False,
        "graphFetchBlockedUntilRecompile": True,
        "resourceIdentityRetained": True,
        "retainedResourceReadableAfterUnmark": True,
        "exact": True,
    }
    checks = {name: evidence.get(name) == expected for name, expected in required.items()}
    _require(all(checks.values()), "same-frame evidence violates the zero-execute retained-resource contract")
    _require(
        evidence.get("proofScope") == "metadata mutation against the retained compiled composite resource",
        "same-frame proof scope is missing or overstated",
    )
    mark_on = evidence.get("markOn")
    mark_off = evidence.get("markOff")
    _require(isinstance(mark_on, dict) and mark_on.get("status") == "PASS" and mark_on.get("lumenOutputsMarked") is True,
             "mark-on metadata is not explicit")
    _require(isinstance(mark_off, dict) and str(mark_off.get("status", "")).upper() in ("PASS", "PASS_BOUNDED")
             and mark_off.get("lumenOutputsMarked") is False, "mark-off metadata is not explicit")
    on_hash = evidence.get("markOnSha256")
    off_hash = evidence.get("markOffSha256")
    _require(isinstance(on_hash, str) and len(on_hash) == 64 and on_hash == off_hash, "same-frame resource hashes differ")
    _require(float(evidence.get("meanAbsErrorMarkOnOff", float("nan"))) == 0.0, "same-frame mean error must be zero")
    _require(float(evidence.get("maxAbsErrorMarkOnOff", float("nan"))) == 0.0, "same-frame max error must be zero")
    return {
        "schema": "LumenGI.C9.SameFrameResourceInvariance.v1",
        "status": "PASS_BOUNDED",
        "runtime": str(runtime_path),
        "checks": checks,
        "sha256": on_hash,
        "proofScope": evidence["proofScope"],
        "limitation": "no mark-off graph compile or producer execution occurred; full export equivalence remains open",
    }


def _write_fixture(root: Path, *, delta: float = 0.0, pid_off: int = 901) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    pixels = np.full((3, 4, 3), 0.25, dtype=np.float32)
    on_pixels = pixels.copy()
    off_pixels = pixels.copy()
    off_pixels[0, 0, 0] += delta
    fingerprint = "a" * 64
    pair_id = "fixture-pair"
    config = {"scene": "fixture", "settleFrames": 4}
    paths = {}
    for phase, marked, value, pid in (
        ("mark-on", True, on_pixels, 901),
        ("mark-off", False, off_pixels, pid_off),
    ):
        snapshot = root / (phase + ".npy")
        np.save(snapshot, value, allow_pickle=False)
        runtime = {
            "schema": "LumenGI.C9.FinalColorRuntime.v1",
            "deterministicReplay": {
                "pairId": pair_id,
                "phase": phase,
                "processId": pid,
                "configFingerprint": fingerprint,
                "config": config,
                "sceneReloaded": True,
                "graphRecreated": True,
                "clockFrames": [1, 4],
                "captureExecutions": 1,
            },
            "producerEvidence": {"directEnabled": True, "indirectEnabled": True, "compositeInputs": REQUIRED_INPUTS},
            "finalColor": {
                "endpoint": "ResolvedCompositePreview.out",
                "marked": True,
                "lumenOutputsMarked": marked,
                "finite": True,
                "nonnegative": True,
                "directEnabled": True,
                "indirectEnabled": True,
                "compositeInputs": REQUIRED_INPUTS,
                "frame": 4,
                "scene": "fixture",
                "view": "front",
                "sha256": hashlib.sha256(np.ascontiguousarray(value).tobytes()).hexdigest(),
                "snapshot": str(snapshot),
            },
        }
        path = root / (phase + ".json")
        path.write_text(json.dumps(runtime), encoding="utf-8")
        paths[phase] = path
    manifest = root / "replay-manifest.json"
    manifest.write_text(json.dumps({
        "schema": "LumenGI.C9.DeterministicReplayManifest.v1",
        "pairId": pair_id,
        "processId": 901,
        "configFingerprint": fingerprint,
        "markOn": str(paths["mark-on"]),
        "markOff": str(paths["mark-off"]),
    }), encoding="utf-8")
    return manifest


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="lumen-c9-repro-") as directory:
        root = Path(directory)
        passed = evaluate(_write_fixture(root / "pass"))
        failed = evaluate(_write_fixture(root / "fail", delta=0.01))
        merged_path = root / "merged-runtime.json"
        merged_path.write_text(json.dumps(passed), encoding="utf-8")
        import run_finalcolor_contract

        downstream = run_finalcolor_contract._runtime_contract(merged_path).get("status")
        try:
            evaluate(_write_fixture(root / "cross-process", pid_off=902))
            cross_process = "UNEXPECTED_PASS"
        except EvidenceInvalid:
            cross_process = "FAIL_AS_EXPECTED"
        digest = "b" * 64
        same_frame_path = root / "same-frame.json"
        same_frame_fixture = {
            "sameProcessMarkTransition": {
                "status": "PASS_BOUNDED",
                "comparisonMode": "same_process_graph_unmark",
                "renderedFrames": 0,
                "producerExecutions": 0,
                "graphRecompiled": False,
                "graphFetchBlockedUntilRecompile": True,
                "resourceIdentityRetained": True,
                "retainedResourceReadableAfterUnmark": True,
                "proofScope": "metadata mutation against the retained compiled composite resource",
                "exact": True,
                "meanAbsErrorMarkOnOff": 0.0,
                "maxAbsErrorMarkOnOff": 0.0,
                "markOnSha256": digest,
                "markOffSha256": digest,
                "markOn": {"status": "PASS", "lumenOutputsMarked": True},
                "markOff": {"status": "PASS_BOUNDED", "lumenOutputsMarked": False},
            }
        }
        same_frame_path.write_text(json.dumps(same_frame_fixture), encoding="utf-8")
        same_frame = evaluate_same_frame_runtime(same_frame_path).get("status")
        dishonest_fixture = json.loads(json.dumps(same_frame_fixture))
        dishonest_fixture["sameProcessMarkTransition"]["graphRecompiled"] = True
        dishonest_path = root / "same-frame-dishonest.json"
        dishonest_path.write_text(json.dumps(dishonest_fixture), encoding="utf-8")
        try:
            evaluate_same_frame_runtime(dishonest_path)
            dishonest = "UNEXPECTED_PASS"
        except EvidenceInvalid:
            dishonest = "FAIL_AS_EXPECTED"
        if (
            passed.get("status") != "PASS"
            or failed.get("status") != "FAIL"
            or cross_process != "FAIL_AS_EXPECTED"
            or downstream != "PASS"
            or same_frame != "PASS_BOUNDED"
            or dishonest != "FAIL_AS_EXPECTED"
        ):
            print(
                "C9_EXPORT_REPRO_SELF_TEST_FAIL",
                passed.get("status"),
                failed.get("status"),
                cross_process,
                downstream,
                same_frame,
                dishonest,
            )
            return 1
    print(
        "C9_EXPORT_REPRO_SELF_TEST_PASS thresholds",
        THRESHOLDS,
        "finalcolor-contract",
        downstream,
        "same-frame",
        same_frame,
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--manifest", type=Path)
    source.add_argument("--same-frame-runtime", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.manifest is None and args.same_frame_runtime is None:
        parser.error("--manifest or --same-frame-runtime is required unless --self-test is used")
    if args.same_frame_runtime is not None:
        output = args.output or args.same_frame_runtime.with_name("c9-same-frame-resource-invariance.json")
        try:
            report = evaluate_same_frame_runtime(args.same_frame_runtime)
        except EvidenceMissing as exc:
            report = {"schema": "LumenGI.C9.SameFrameResourceInvariance.v1", "status": "BLOCKED", "reason": str(exc)}
        except EvidenceInvalid as exc:
            report = {"schema": "LumenGI.C9.SameFrameResourceInvariance.v1", "status": "FAIL", "reason": str(exc)}
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print("C9_SAME_FRAME_RESOURCE_INVARIANCE", report["status"], output)
        return 0 if report["status"] == "PASS_BOUNDED" else 1
    output = args.output or args.manifest.with_name("c9-export-repro.json")
    try:
        report = evaluate(args.manifest)
    except EvidenceMissing as exc:
        report = {"schema": "LumenGI.C9.FinalColorRuntime.v1", "status": "BLOCKED", "reason": str(exc)}
    except EvidenceInvalid as exc:
        report = {"schema": "LumenGI.C9.FinalColorRuntime.v1", "status": "FAIL", "reason": str(exc)}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("C9_EXPORT_REPRO", report["status"], output)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
