"""Compare test-only C9 producer-trace sentinel snapshots.

The producer trace is deliberately separate from strict C9 export equivalence.
``run_resolved_showcase.py`` adds marked BlitPass sentinels only when
``LUMEN_C9_PRODUCER_TRACE_OUT`` is enabled, which changes graph topology.  This
tool measures the resulting DirectResolve, LumenGI, and Composite deltas without
promoting any result to a C9 gate.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


KEYS = ("direct", "lumen", "composite")


def _resolve(value: str, base: Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else (base / path).resolve()


def _load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def _trace_path(runtime_path: Path) -> Path:
    runtime = _load_json(runtime_path)
    trace = runtime.get("producerTrace")
    if not isinstance(trace, dict):
        raise ValueError(f"producerTrace is missing: {runtime_path}")
    if trace.get("diagnosticOnly") is not True or trace.get("topologyChanged") is not True:
        raise ValueError(f"producerTrace is not marked diagnostic-only: {runtime_path}")
    metadata_path = trace.get("metadataPath")
    if metadata_path:
        candidate = _resolve(str(metadata_path), runtime_path.parent)
        if candidate.is_file():
            trace = _load_json(candidate)
    return trace


def _load_sidecar(runtime_path: Path) -> tuple[dict, dict]:
    trace = _trace_path(runtime_path)
    outputs = trace.get("outputs")
    if not isinstance(outputs, dict):
        raise ValueError(f"producerTrace.outputs is missing: {runtime_path}")
    arrays = {}
    for key in KEYS:
        record = outputs.get(key)
        if not isinstance(record, dict) or record.get("status") != "PASS" or not record.get("path"):
            raise ValueError(f"producer trace output {key!r} is unavailable: {runtime_path}")
        array_path = _resolve(str(record["path"]), runtime_path.parent)
        arrays[key] = np.asarray(np.load(array_path, allow_pickle=False))
    return trace, arrays


def _compare(left: np.ndarray, right: np.ndarray) -> dict:
    if left.shape != right.shape:
        return {"status": "FAIL", "reason": "shape mismatch", "leftShape": list(left.shape), "rightShape": list(right.shape)}
    if not np.isfinite(left).all() or not np.isfinite(right).all():
        return {"status": "FAIL", "reason": "non-finite producer snapshot"}
    error = np.abs(left.astype(np.float64) - right.astype(np.float64))
    scale = max(float(np.max(np.abs(left))), float(np.max(np.abs(right))), 1.0)
    return {
        "status": "MEASURED",
        "shape": list(left.shape),
        "dtypeLeft": str(left.dtype),
        "dtypeRight": str(right.dtype),
        "meanAbs": float(np.mean(error)) if error.size else 0.0,
        "p99Abs": float(np.percentile(error, 99.0)) if error.size else 0.0,
        "maxAbs": float(np.max(error)) if error.size else 0.0,
        "relativeMax": float(np.max(error) / scale) if error.size else 0.0,
        "nonzeroFraction": float(np.mean(error != 0.0)) if error.size else 0.0,
    }


def compare(mark_on_path: Path, mark_off_path: Path) -> dict:
    on_trace, on_arrays = _load_sidecar(mark_on_path)
    off_trace, off_arrays = _load_sidecar(mark_off_path)
    comparisons = {key: _compare(on_arrays[key], off_arrays[key]) for key in KEYS}
    complete = all(item.get("status") == "MEASURED" for item in comparisons.values())
    return {
        "schema": "LumenGI.C9.ProducerTraceDiff.v1",
        "diagnosticOnly": True,
        "status": "PASS" if complete else "FAIL",
        "markOnRuntime": str(mark_on_path),
        "markOffRuntime": str(mark_off_path),
        "markOnPhase": on_trace.get("phase"),
        "markOffPhase": off_trace.get("phase"),
        "comparisons": comparisons,
        "limitation": "sentinel copy passes change RenderGraph topology; measurements cannot promote strict C9 equivalence",
    }


def _self_test() -> None:
    left = np.zeros((2, 2, 4), dtype=np.float32)
    right = left.copy()
    right[0, 0, 0] = 0.25
    result = _compare(left, right)
    assert result["status"] == "MEASURED"
    assert result["maxAbs"] == 0.25
    assert result["nonzeroFraction"] == 0.0625
    bad = _compare(left, np.zeros((1, 2, 4), dtype=np.float32))
    assert bad["status"] == "FAIL"
    print("C9_PRODUCER_SNAPSHOT_DIFF_SELF_TEST_PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, help="C9 replay manifest containing markOn/markOff runtime paths")
    parser.add_argument("--mark-on", type=Path, help="mark-on runtime JSON")
    parser.add_argument("--mark-off", type=Path, help="mark-off runtime JSON")
    parser.add_argument("--output", type=Path, help="JSON output path")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        _self_test()
        return 0

    if args.manifest:
        manifest_path = args.manifest.resolve()
        manifest = _load_json(manifest_path)
        if not args.mark_on:
            args.mark_on = _resolve(str(manifest["markOn"]), manifest_path.parent)
        if not args.mark_off:
            args.mark_off = _resolve(str(manifest["markOff"]), manifest_path.parent)
    if not args.mark_on or not args.mark_off:
        parser.error("provide --manifest or both --mark-on and --mark-off")

    report = compare(args.mark_on.resolve(), args.mark_off.resolve())
    output = args.output.resolve() if args.output else Path("c9-producer-snapshot-diff.json").resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("C9_PRODUCER_SNAPSHOT_DIFF", report["status"], output)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
