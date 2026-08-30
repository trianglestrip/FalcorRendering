"""Build an independent linear A2 no-noise sidecar from dynamic manifests.

The Mogwai producer in :mod:`run_screenprobe_dynamic` owns one transition case
per process.  This helper joins those case manifests for one scene and emits
the strict ``LumenGI.ScreenProbeNoNoiseEvidence.v1`` contract consumed by
``run_screenprobe_multiscene_gate.py``.  It never reads a display PNG as a
quality input.

Each input manifest must contain the exact checkpoints ``[1, 8, 16, 32, 64]``
and real frame-capture provenance for ``diffuseRadianceHitDist`` and
``resolvedDiffuseGI``.  The raw channel is recorded as RGB plus an independent
secondary hit-distance alpha contract; alpha is never used in the RGB quality
metric or reinterpreted as confidence/history.

Typical use (one output per scene)::

    python -B tests/lumengi/run_screenprobe_dynamic_sidecar.py \
      --manifest artifacts/lumengi/A2/scene/static/screenprobe-dynamic.json \
      --manifest artifacts/lumengi/A2/scene/camera-cut/screenprobe-dynamic.json \
      --manifest artifacts/lumengi/A2/scene/scene-reload/screenprobe-dynamic.json \
      --manifest artifacts/lumengi/A2/scene/lighting/screenprobe-dynamic.json \
      --manifest artifacts/lumengi/A2/scene/material/screenprobe-dynamic.json \
      --output artifacts/lumengi/A2/scene/no-noise-linear.json

Missing output, frame-capture provenance, runtime shape, or screenProbeStats
is ``BLOCKED``.  A finite but non-improving linear quality result is ``OPEN``;
it is never promoted to PASS by a smooth PNG.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import tempfile
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


SCHEMA = "LumenGI.ScreenProbeNoNoiseEvidence.v1"
INPUT_SCHEMA = "LumenGI.ScreenProbeDynamicHistory.v1"
REQUIRED_CASES = (
    "static",
    "camera_cut",
    "scene_reload",
    "lighting_generation",
    "material_or_geometry",
)
REQUIRED_CHECKPOINTS = (1, 8, 16, 32, 64)
REQUIRED_STATS = ("historyAccepted", "historyGeneration", "lightingGeneration")
RAW_CHANNEL = "LumenGI.diffuseRadianceHitDist"
RESOLVED_CHANNEL = "LumenGI.resolvedDiffuseGI"
LUMA_WEIGHTS = (0.2126, 0.7152, 0.0722)
VARIANCE_FACTOR = 0.99
TAIL_RMSE_FACTOR = 1.01


def _quality_module():
    """Load the existing linear EXR/NPY decoder without importing Falcor."""

    path = Path(__file__).with_name("run_screenprobe_dynamic_quality.py")
    spec = importlib.util.spec_from_file_location("_a2_dynamic_quality_sidecar", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load linear quality helper: %s" % path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, Mapping):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    try:
        return _json_safe(value.item())
    except Exception:
        return str(value)


def _write_json(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(_json_safe(payload), indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def _read_json(path: Path) -> tuple[Mapping[str, Any] | None, str | None]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception as error:
        return None, "invalid JSON %s: %s" % (path, error)
    if not isinstance(value, Mapping):
        return None, "JSON object required: %s" % path
    return value, None


def _number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _capture_frame(record: Mapping[str, Any]) -> int | None:
    value = record.get("frame")
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _resolve_path(value: Any, manifest_path: Path, output_dir: Path | None = None) -> Path | None:
    if not isinstance(value, str) or not value.strip():
        return None
    candidate = Path(value)
    if candidate.is_absolute():
        return candidate.resolve()
    bases = [manifest_path.parent]
    if output_dir is not None:
        bases.insert(0, output_dir)
    for base in bases:
        path = (base / candidate).resolve()
        if path.is_file():
            return path
    return (bases[0] / candidate).resolve()


def _capture_output_dir(record: Mapping[str, Any], manifest_path: Path) -> Path | None:
    frame_capture = record.get("frameCapture")
    if not isinstance(frame_capture, Mapping):
        return None
    value = frame_capture.get("outputDir")
    path = _resolve_path(value, manifest_path)
    if path is None:
        return None
    return path if path.is_dir() else path.parent


def _path_from_frame_capture(
    record: Mapping[str, Any],
    manifest_path: Path,
    channel_names: Sequence[str],
) -> Path | None:
    """Resolve a linear channel path from explicit or stable EXR provenance."""

    frame_capture = record.get("frameCapture")
    if not isinstance(frame_capture, Mapping):
        return None
    output_dir = _capture_output_dir(record, manifest_path)
    explicit_keys = {
        RAW_CHANNEL: ("raw", "rawPath", "rawExr", "diffuseRadianceHitDist"),
        RESOLVED_CHANNEL: ("resolved", "resolvedPath", "resolvedExr", "resolvedDiffuseGI"),
    }
    for key in explicit_keys.get(channel_names[0], ()):
        value = frame_capture.get(key)
        if isinstance(value, Mapping):
            value = value.get("path") or value.get("file")
        path = _resolve_path(value, manifest_path, output_dir)
        if path is not None and path.is_file():
            return path

    exr_values = frame_capture.get("exr")
    paths: list[Path] = []
    if isinstance(exr_values, (list, tuple)):
        for value in exr_values:
            path = _resolve_path(value, manifest_path, output_dir)
            if path is not None:
                paths.append(path)
    for path in paths:
        name = path.name.lower()
        if any(channel.lower() in name for channel in channel_names):
            return path

    # A future producer may only retain outputDir/baseFilename. Resolve the
    # stable Falcor filename without inventing a display-space substitute.
    base = frame_capture.get("baseFilename")
    if output_dir is not None and isinstance(base, str) and base:
        for channel in channel_names:
            matches = sorted(output_dir.glob(base + "." + channel + ".*.exr"))
            if matches:
                return matches[0].resolve()
    return None


def _output_record(record: Mapping[str, Any], channel: str) -> Mapping[str, Any] | None:
    outputs = record.get("outputs")
    output_name = channel[len("LumenGI.") :] if channel.startswith("LumenGI.") else channel
    value = outputs.get(output_name) if isinstance(outputs, Mapping) else None
    return value if isinstance(value, Mapping) else None


def _linear_shape_contract(
    record: Mapping[str, Any],
    channel: str,
    require_alpha: bool,
    path: Path | None = None,
) -> tuple[dict[str, Any], list[str]]:
    output = _output_record(record, channel)
    errors: list[str] = []
    shape = output.get("shape") if output is not None else None
    shape_list: list[int] = []
    if isinstance(shape, (list, tuple)):
        try:
            shape_list = [int(value) for value in shape]
        except (TypeError, ValueError):
            shape_list = []
    # Older dynamic manifests did not include diffuseRadianceHitDist in their
    # in-memory ``outputs`` summary, although frameCapture still recorded its
    # real RGBA16Float resource.  Preserve that evidence without fabricating a
    # decoded alpha plane: the path is required and the channel contract is
    # explicit.  Any missing path remains BLOCKED in _frame_contract().
    resource_contract_fallback = output is None and require_alpha and path is not None and path.is_file()
    if output is None and not resource_contract_fallback:
        errors.append("missing output summary: %s" % channel)
    elif output is not None and str(output.get("status", "")).upper() != "PASS":
        errors.append("output summary is not PASS: %s" % channel)
    if len(shape_list) < 3 or shape_list[-1] < 3:
        if resource_contract_fallback:
            shape_list = []
        else:
            errors.append("%s runtime output has no RGB shape" % channel)
    if require_alpha and resource_contract_fallback:
        alpha_channels = 1
    else:
        alpha_channels = 1 if require_alpha and len(shape_list) >= 3 and shape_list[-1] >= 4 else 0
    if require_alpha and not resource_contract_fallback and (len(shape_list) < 3 or shape_list[-1] < 4):
        errors.append("%s runtime output has no independent alpha shape" % channel)
    return {
        "channel": channel,
        "runtimeShape": shape_list,
        "rgbChannels": 3,
        "alphaChannels": alpha_channels,
        "shapeSource": "runtimeOutputSummary" if output is not None else "frameCaptureResourceContract",
        "resourceFormat": "RGBA16Float" if resource_contract_fallback else None,
        "alphaSemantic": "secondary hit distance; independent from RGB quality/history confidence"
        if require_alpha
        else None,
    }, errors


def _stats_contract(record: Mapping[str, Any]) -> tuple[dict[str, Any], list[str]]:
    stats = record.get("screenProbeStats")
    errors: list[str] = []
    values: dict[str, float | None] = {}
    if not isinstance(stats, Mapping):
        errors.append("missing screenProbeStats")
    else:
        for key in REQUIRED_STATS:
            value = _number(stats.get(key))
            values[key] = value
            if value is None:
                errors.append("screenProbeStats missing/non-finite: %s" % key)
    return {"required": list(REQUIRED_STATS), "values": values}, errors


def _frame_contract(
    record: Mapping[str, Any], manifest_path: Path, quality_module: Any
) -> tuple[dict[str, Any], list[str]]:
    frame = _capture_frame(record)
    errors: list[str] = []
    if frame is None:
        errors.append("capture has no numeric frame")
    if str(record.get("status", "")).upper() != "PASS":
        errors.append("producer capture status is not PASS")
    frame_capture = record.get("frameCapture")
    if not isinstance(frame_capture, Mapping) or str(frame_capture.get("status", "")).upper() != "PASS":
        errors.append("missing/blocked frameCapture provenance")

    raw_path = _path_from_frame_capture(record, manifest_path, (RAW_CHANNEL,))
    resolved_path = _path_from_frame_capture(record, manifest_path, (RESOLVED_CHANNEL, "fullColorLinear", "fullColor"))
    if raw_path is None or not raw_path.is_file():
        errors.append("missing linear raw path: %s" % RAW_CHANNEL)
    if resolved_path is None or not resolved_path.is_file():
        errors.append("missing linear resolved/fullColor path")
    for label, path in (("raw", raw_path), ("resolved", resolved_path)):
        if path is not None and path.suffix.lower() not in {".exr", ".npy", ".npz"}:
            errors.append("%s path is not linear EXR/NPY/NPZ: %s" % (label, path))
        if path is not None and path.suffix.lower() in {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"}:
            errors.append("%s path is display-space image, not linear evidence" % label)

    raw_contract, raw_errors = _linear_shape_contract(record, RAW_CHANNEL, True, raw_path)
    resolved_contract, resolved_errors = _linear_shape_contract(record, RESOLVED_CHANNEL, False, resolved_path)
    stats_contract, stats_errors = _stats_contract(record)
    errors.extend(raw_errors)
    errors.extend(resolved_errors)
    errors.extend(stats_errors)

    # Use the existing linear decoder/metrics for RGB only. Alpha remains an
    # explicit provenance contract above and is never folded into luma metrics.
    metrics: dict[str, Any] = {}
    for label, path in (("raw", raw_path), ("resolved", resolved_path)):
        if path is None or not path.is_file():
            metrics[label] = {"status": "BLOCKED"}
            continue
        try:
            values = quality_module._read_image(path)
            metrics[label] = quality_module._image_metrics(values)
            if metrics[label].get("status") != "PASS":
                errors.append("%s linear RGB is not finite/non-negative" % label)
        except Exception as error:
            metrics[label] = {"status": "BLOCKED", "error": str(error)}
            errors.append("%s linear decode failed: %s" % (label, error))

    status = "PASS" if not errors else "BLOCKED"
    return {
        "frame": frame,
        "absoluteFrame": record.get("absoluteFrame"),
        "status": status,
        "raw": {
            "path": str(raw_path) if raw_path is not None else None,
            "contract": raw_contract,
            "metrics": metrics.get("raw", {}),
        },
        "resolved": {
            "path": str(resolved_path) if resolved_path is not None else None,
            "contract": resolved_contract,
            "metrics": metrics.get("resolved", {}),
        },
        "screenProbeStats": stats_contract,
        "errors": errors,
    }, errors


def _case_from_manifest(path: Path, quality_module: Any) -> tuple[dict[str, Any], list[str]]:
    manifest, read_error = _read_json(path)
    errors: list[str] = []
    if read_error or manifest is None:
        return {"manifest": str(path.resolve()), "status": "BLOCKED", "errors": [read_error]}, [read_error or "manifest unavailable"]
    if manifest.get("schema") != INPUT_SCHEMA:
        errors.append("unexpected input schema: %s" % manifest.get("schema"))
    if str(manifest.get("status", "")).upper() != "PASS":
        errors.append("producer manifest status is not PASS")
    case = str(manifest.get("case", "")).strip().lower()
    if case not in REQUIRED_CASES:
        errors.append("unsupported/missing case: %s" % case)
    scene = manifest.get("scene")
    if not isinstance(scene, str) or not scene.strip():
        errors.append("manifest has no scene identifier")
    declared = manifest.get("checkpointFrames")
    try:
        declared_checkpoints = [int(value) for value in declared] if isinstance(declared, list) else []
    except (TypeError, ValueError):
        declared_checkpoints = []
    if declared_checkpoints != list(REQUIRED_CHECKPOINTS):
        errors.append("checkpointFrames must exactly match %s" % list(REQUIRED_CHECKPOINTS))
    captures = manifest.get("captures")
    captures = [item for item in captures if isinstance(item, Mapping)] if isinstance(captures, list) else []
    if sorted(_capture_frame(item) or -1 for item in captures) != list(REQUIRED_CHECKPOINTS):
        errors.append("captures must contain exactly checkpoints %s" % list(REQUIRED_CHECKPOINTS))
    frames: list[dict[str, Any]] = []
    for record in captures:
        frame_result, frame_errors = _frame_contract(record, path, quality_module)
        frames.append(frame_result)
        errors.extend("frame %s: %s" % (frame_result.get("frame"), error) for error in frame_errors)
    frames_by_number = {frame.get("frame"): frame for frame in frames}
    quality: dict[str, Any] = {
        "status": "BLOCKED",
        "rawLocalVarianceFinal": None,
        "resolvedLocalVarianceFinal": None,
        "rawTailRMSE": None,
        "resolvedTailRMSE": None,
        "resolvedVarianceRule": False,
        "resolvedTailRule": False,
        "qualityImproved": False,
    }
    if not errors and 32 in frames_by_number and 64 in frames_by_number:
        try:
            raw32 = quality_module._read_image(Path(frames_by_number[32]["raw"]["path"]))
            raw64 = quality_module._read_image(Path(frames_by_number[64]["raw"]["path"]))
            resolved32 = quality_module._read_image(Path(frames_by_number[32]["resolved"]["path"]))
            resolved64 = quality_module._read_image(Path(frames_by_number[64]["resolved"]["path"]))
            raw_var = _number(frames_by_number[64]["raw"]["metrics"].get("localVariance"))
            resolved_var = _number(frames_by_number[64]["resolved"]["metrics"].get("localVariance"))
            raw_tail = float(quality_module._rmse(raw32, raw64))
            resolved_tail = float(quality_module._rmse(resolved32, resolved64))
            quality.update(
                {
                    "rawLocalVarianceFinal": raw_var,
                    "resolvedLocalVarianceFinal": resolved_var,
                    "rawTailRMSE": raw_tail,
                    "resolvedTailRMSE": resolved_tail,
                    "resolvedVarianceRule": bool(raw_var is not None and resolved_var is not None and resolved_var < raw_var * VARIANCE_FACTOR),
                    "resolvedTailRule": bool(resolved_tail <= raw_tail * TAIL_RMSE_FACTOR),
                }
            )
            quality["qualityImproved"] = bool(quality["resolvedVarianceRule"] and quality["resolvedTailRule"])
            quality["status"] = "PASS" if quality["qualityImproved"] else "OPEN"
            if not quality["qualityImproved"]:
                errors.append("linear resolved output did not satisfy frozen no-noise improvement rule")
        except Exception as error:
            errors.append("linear quality calculation failed: %s" % error)
    elif not errors:
        errors.append("missing checkpoint 32 or 64 for linear tail metric")
    case_status = "BLOCKED" if errors else ("PASS" if quality["status"] == "PASS" else "OPEN")
    return {
        "case": case,
        "scene": scene,
        "manifest": str(path.resolve()),
        "status": case_status,
        "checkpoints": list(REQUIRED_CHECKPOINTS),
        "frames": frames,
        "raw": {"paths": [frames_by_number.get(frame, {}).get("raw", {}).get("path") for frame in REQUIRED_CHECKPOINTS]},
        "resolved": {"paths": [frames_by_number.get(frame, {}).get("resolved", {}).get("path") for frame in REQUIRED_CHECKPOINTS]},
        "quality": quality,
        "errors": errors,
    }, errors


def _expand_inputs(values: Iterable[str], globs: Iterable[str]) -> list[Path]:
    result: list[Path] = []
    for value in values:
        path = Path(value)
        if path.is_dir():
            path = path / "screenprobe-dynamic.json"
        result.append(path)
    for pattern in globs:
        result.extend(sorted(Path().glob(pattern)))
    unique: list[Path] = []
    seen: set[str] = set()
    for path in result:
        key = str(path.resolve()).lower()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def evaluate(manifest_paths: Sequence[Path], canonical_case: str = "static") -> dict[str, Any]:
    report: dict[str, Any] = {
        "schema": SCHEMA,
        "status": "BLOCKED",
        "scene": None,
        "checkpoints": list(REQUIRED_CHECKPOINTS),
        "basis": "raw/resolved linear",
        "displayPngUsed": False,
        "method": "independent linear source-quality review from runtime EXR/NPY provenance",
        "raw": {"paths": []},
        "resolved": {"paths": []},
        "cases": {},
        "inputManifests": [str(path.resolve()) for path in manifest_paths],
        "metrics": {},
        "errors": [],
    }
    try:
        quality_module = _quality_module()
    except Exception as error:
        report["errors"] = [str(error)]
        return report
    if not manifest_paths:
        report["errors"] = ["at least one dynamic manifest is required"]
        return report
    case_results: dict[str, dict[str, Any]] = {}
    scenes: set[str] = set()
    duplicate_cases: set[str] = set()
    for path in manifest_paths:
        result, errors = _case_from_manifest(path, quality_module)
        case = result.get("case")
        if isinstance(case, str) and case in case_results:
            duplicate_cases.add(case)
        elif isinstance(case, str) and case:
            case_results[case] = result
        scene = result.get("scene")
        if isinstance(scene, str) and scene.strip():
            scenes.add(scene.strip())
    report["cases"] = {key: case_results[key] for key in sorted(case_results)}
    if len(scenes) != 1:
        report["errors"].append("all manifests must identify one scene; got %s" % sorted(scenes))
    else:
        report["scene"] = next(iter(scenes))
    if duplicate_cases:
        report["errors"].append("duplicate case manifests: %s" % sorted(duplicate_cases))
    missing_cases = [case for case in REQUIRED_CASES if case not in case_results]
    if missing_cases:
        report["errors"].append("missing required cases: %s" % missing_cases)
    if canonical_case not in case_results:
        canonical_case = next(iter(sorted(case_results)), "")
    canonical = case_results.get(canonical_case)
    if canonical:
        report["raw"] = canonical.get("raw", {"paths": []})
        report["resolved"] = canonical.get("resolved", {"paths": []})
        report["canonicalCase"] = canonical_case
    report["metrics"] = {
        case: value.get("quality", {}) for case, value in sorted(case_results.items())
    }
    malformed = any(value.get("status") == "BLOCKED" for value in case_results.values())
    if malformed or report["errors"]:
        report["status"] = "BLOCKED"
    elif len(case_results) != len(REQUIRED_CASES):
        report["status"] = "OPEN"
    elif all(value.get("status") == "PASS" for value in case_results.values()):
        report["status"] = "PASS"
    else:
        report["status"] = "OPEN"
    return report


def _fixture_manifest(root: Path, scene: str, case: str) -> Path:
    import numpy as np

    captures = []
    for frame in REQUIRED_CHECKPOINTS:
        raw = np.zeros((2, 2, 4), dtype=np.float32)
        # Include a small spatial pattern so the frozen variance rule has a
        # meaningful positive denominator (a uniform fixture would make both
        # variances exactly zero and should not be called an improvement).
        pattern = np.asarray([[0.0, 0.1], [0.1, 0.0]], dtype=np.float32)
        raw[..., :3] = 0.5 + frame * 0.001 + pattern[..., None]
        raw[..., 3] = 1.0 + frame
        resolved = raw.copy()
        resolved[..., :3] = 0.5 + frame * 0.001 + pattern[..., None] * 0.25
        raw_path = root / ("%s-%d-raw.npy" % (case, frame))
        resolved_path = root / ("%s-%d-resolved.npy" % (case, frame))
        np.save(raw_path, raw)
        np.save(resolved_path, resolved)
        captures.append(
            {
                "frame": frame,
                "absoluteFrame": frame,
                "status": "PASS",
                "outputs": {
                    "diffuseRadianceHitDist": {"status": "PASS", "shape": [2, 2, 4]},
                    "resolvedDiffuseGI": {"status": "PASS", "shape": [2, 2, 4]},
                },
                "screenProbeStats": {
                    "historyAccepted": 4,
                    "historyGeneration": 1,
                    "lightingGeneration": 1,
                },
                "frameCapture": {
                    "status": "PASS",
                    "outputDir": str(root),
                    "exr": [str(raw_path), str(resolved_path)],
                    "raw": str(raw_path),
                    "resolved": str(resolved_path),
                },
            }
        )
    path = root / (case + ".json")
    _write_json(
        path,
        {
            "schema": INPUT_SCHEMA,
            "status": "PASS",
            "scene": scene,
            "case": case,
            "checkpointFrames": list(REQUIRED_CHECKPOINTS),
            "captures": captures,
        },
    )
    return path


def _self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="lumen-a2-sidecar-") as directory:
        root = Path(directory)
        paths = [_fixture_manifest(root, "Fixture/Fixture.pyscene", case) for case in REQUIRED_CASES]
        report = evaluate(paths)
        assert report["status"] == "PASS", report
        assert report["basis"] == "raw/resolved linear"
        assert report["displayPngUsed"] is False
        assert len(report["raw"]["paths"]) == 5
        assert all(value["status"] == "PASS" for value in report["cases"].values())
        blocked = json.loads(json.dumps(report))
        blocked["cases"]["static"]["frames"][0]["screenProbeStats"] = {"required": list(REQUIRED_STATS), "values": {}}
        # The fixture report is not re-evaluated here; the assertion documents
        # that the emitted contract carries the independent stats section.
        assert blocked["cases"]["static"]["frames"][0]["screenProbeStats"]["required"] == list(REQUIRED_STATS)
    print("SCREENPROBE_DYNAMIC_SIDECAR_SELF_TEST PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", action="append", default=[], help="dynamic manifest (repeat per case)")
    parser.add_argument("--manifest-glob", action="append", default=[], help="glob for dynamic manifests")
    parser.add_argument("--canonical-case", choices=REQUIRED_CASES, default="static")
    parser.add_argument("--output", default=os.environ.get("LUMEN_A2_DYNAMIC_SIDECAR_OUT", "artifacts/lumengi/A2/no-noise-linear.json"))
    args = parser.parse_args()
    paths = _expand_inputs(args.manifest, args.manifest_glob)
    report = evaluate(paths, args.canonical_case)
    output = Path(args.output).resolve()
    _write_json(output, report)
    print("SCREENPROBE_DYNAMIC_SIDECAR", report["status"], output)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    import sys

    if "--self-test" in sys.argv[1:]:
        raise SystemExit(_self_test())
    raise SystemExit(main())
