"""Aggregate offline quality evidence from A2 dynamic ScreenProbe runs.

The Mogwai producer in :mod:`run_screenprobe_dynamic` deliberately owns one
transition case per manifest.  This script is the read-only aggregator for a
set of those manifests.  It validates the runtime contract first (real PNG
and EXR provenance, finite/non-negative linear output, and explicit history
generation/reset telemetry), then computes quality metrics from the captured
linear buffers.  It never treats a display PNG as a quality measurement.

Typical invocation::

    python -B tests/lumengi/run_screenprobe_dynamic_quality.py \
        --manifest artifacts/lumengi/A2/dynamic-static-20260817/screenprobe-dynamic.json \
        --manifest artifacts/lumengi/A2/dynamic-camera-cut-20260817/screenprobe-dynamic.json \
        --output artifacts/lumengi/A2/dynamic-quality-20260817/dynamic-quality-gate.json

The frozen bounded rule compares the final raw source against the final
resolved output: resolved local luminance variance must be below 99% of raw,
and the final checkpoint-to-checkpoint tail RMSE must not exceed 101% of raw.
Failure of that quality rule is reported as ``NO_IMPROVEMENT``/``OPEN``;
missing or unverifiable evidence remains ``BLOCKED``.  No threshold is
changed and no result is inferred from a visually smooth PNG.

``--self-test`` creates only temporary NumPy fixtures and exercises PASS,
OPEN, and BLOCKED outcomes without Falcor, Mogwai, a build, or a GPU.
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


SCHEMA = "LumenGI.ScreenProbeDynamicQuality.v1"
INPUT_SCHEMA = "LumenGI.ScreenProbeDynamicHistory.v1"
SUPPORTED_CASES = {
    "static",
    "camera_cut",
    "scene_reload",
    "lighting_generation",
    "material_or_geometry",
}
REQUIRED_CASE_MATRIX = (
    "static",
    "camera_cut",
    "scene_reload",
    "lighting_generation",
    "material_or_geometry",
)
REQUIRED_CHECKPOINTS = (1, 8, 16, 32, 64)
REQUIRED_STATS = (
    "historyAccepted",
    "historyReset",
    "historyGeneration",
    "lightingGeneration",
)
TAIL_FROM = 32
TAIL_TO = 64
VARIANCE_FACTOR = 0.99
TAIL_RMSE_FACTOR = 1.01
LUMA_WEIGHTS = (0.2126, 0.7152, 0.0722)


def _numpy():
    """Import NumPy lazily so a missing dependency becomes an honest BLOCKED."""

    try:
        import numpy as np  # type: ignore

        return np, None
    except Exception as error:  # pragma: no cover - environment dependent.
        return None, "NumPy is required for linear EXR metrics: %s" % error


def _number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


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
        return None, "manifest is not a JSON object: %s" % path
    return value, None


def _as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return list(value)
    return [value]


def _resolve_path(value: Any, manifest_path: Path, output_dir: Path | None = None) -> Path | None:
    if not isinstance(value, str) or not value.strip():
        return None
    candidate = Path(value)
    bases = [Path.cwd(), manifest_path.parent]
    if output_dir is not None:
        bases.insert(0, output_dir)
    candidates = [candidate] if candidate.is_absolute() else [base / candidate for base in bases]
    for item in candidates:
        try:
            if item.is_file():
                return item.resolve()
        except OSError:
            continue
    # Preserve a normalized path for useful error reporting even when absent.
    return candidates[0].absolute()


def _resolve_paths(values: Any, manifest_path: Path, output_dir: Path | None = None) -> list[Path]:
    result: list[Path] = []
    for value in _as_list(values):
        resolved = _resolve_path(value, manifest_path, output_dir)
        if resolved is not None:
            result.append(resolved)
    return result


def _output_dir(frame_capture: Mapping[str, Any], manifest_path: Path) -> Path | None:
    raw = frame_capture.get("outputDir")
    resolved = _resolve_path(raw, manifest_path)
    if resolved is not None:
        return resolved if resolved.is_dir() else resolved.parent
    return None


def _provenance(record: Mapping[str, Any], manifest_path: Path) -> dict[str, Any]:
    """Resolve the producer's declared PNG/EXR paths without inventing files."""

    frame_capture = record.get("frameCapture")
    if not isinstance(frame_capture, Mapping):
        return {
            "status": "BLOCKED",
            "png": [],
            "exr": [],
            "raw": None,
            "resolved": None,
            "errors": ["capture is missing frameCapture provenance"],
        }

    output_dir = _output_dir(frame_capture, manifest_path)
    png = _resolve_paths(frame_capture.get("png"), manifest_path, output_dir)
    exr = _resolve_paths(frame_capture.get("exr"), manifest_path, output_dir)
    base = frame_capture.get("baseFilename")
    if output_dir is not None and isinstance(base, str) and base:
        if not png:
            png = [Path(item).resolve() for item in glob.glob(str(output_dir / (base + ".*.png")))]
        if not exr:
            exr = [Path(item).resolve() for item in glob.glob(str(output_dir / (base + ".*.exr")))]

    def _declared_channel(names: Sequence[str]) -> Path | None:
        for name in names:
            value = frame_capture.get(name)
            if isinstance(value, Mapping):
                value = value.get("path") or value.get("file")
            path = _resolve_path(value, manifest_path, output_dir)
            if path is not None:
                return path
        return None

    raw = _declared_channel(("raw", "rawPath", "rawExr", "diffuseRadianceHitDist"))
    resolved = _declared_channel(("resolved", "resolvedPath", "resolvedExr", "resolvedDiffuseGI"))

    # Real run_screenprobe_dynamic.py lists every EXR but does not duplicate
    # channel paths in frameCapture.  Select the two linear outputs by their
    # stable Falcor output names.  The explicit sidecar fields above are used
    # by the dependency-free synthetic self-test and future producers.
    for path in exr:
        name = path.name
        if raw is None and (".LumenGI.diffuseRadianceHitDist." in name or ".raw." in name):
            raw = path
        if resolved is None and ".LumenGI.resolvedDiffuseGI." in name:
            resolved = path
    if output_dir is not None and isinstance(base, str) and base:
        if raw is None:
            matches = glob.glob(str(output_dir / (base + ".LumenGI.diffuseRadianceHitDist.*")))
            if matches:
                raw = Path(matches[0]).resolve()
        if resolved is None:
            matches = glob.glob(str(output_dir / (base + ".LumenGI.resolvedDiffuseGI.*")))
            if matches:
                resolved = Path(matches[0]).resolve()

    errors: list[str] = []
    if str(frame_capture.get("status", "")).upper() != "PASS":
        errors.append("frameCapture status is not PASS")
    if not png:
        errors.append("missing PNG provenance")
    if not exr:
        errors.append("missing EXR provenance")
    if png and any(not path.is_file() for path in png):
        errors.append("declared PNG provenance contains missing files")
    if exr and any(not path.is_file() for path in exr):
        errors.append("declared EXR provenance contains missing files")
    if raw is None:
        errors.append("raw diffuseRadianceHitDist capture is missing")
    elif not raw.is_file():
        errors.append("raw capture is missing: %s" % raw)
    if resolved is None:
        errors.append("resolvedDiffuseGI capture is missing")
    elif not resolved.is_file():
        errors.append("resolved capture is missing: %s" % resolved)
    return {
        "status": "PASS" if not errors else "BLOCKED",
        "outputDir": str(output_dir) if output_dir is not None else None,
        "baseFilename": base,
        "png": [str(path) for path in png],
        "exr": [str(path) for path in exr],
        "raw": str(raw) if raw is not None else None,
        "resolved": str(resolved) if resolved is not None else None,
        "errors": errors,
    }


def _read_openexr(path: Path):
    """Read RGB planes through an installed OpenEXR Python binding."""

    np, error = _numpy()
    if np is None:
        raise RuntimeError(error)
    import OpenEXR  # type: ignore

    try:
        import Imath  # type: ignore

        float_type = Imath.PixelType(Imath.PixelType.FLOAT)
    except Exception:
        float_type = None
    handle = OpenEXR.InputFile(str(path))
    try:
        header = handle.header()
        window = header["dataWindow"]
        width = int(window.max.x - window.min.x + 1)
        height = int(window.max.y - window.min.y + 1)
        channels = header.get("channels", {})
        names = [name for name in ("R", "G", "B") if name in channels]
        if len(names) < 3:
            names = list(channels.keys())[:3]
        if len(names) < 3:
            raise RuntimeError("EXR has fewer than three readable channels")
        planes = []
        for name in names[:3]:
            raw = handle.channel(name, float_type) if float_type is not None else handle.channel(name)
            plane = np.frombuffer(raw, dtype=np.float32)
            if plane.size != width * height:
                # Half-float output is accepted by some bindings when no
                # Imath PixelType is available.
                half = np.frombuffer(raw, dtype=np.float16)
                if half.size != width * height:
                    raise RuntimeError("EXR channel size does not match dataWindow")
                plane = half.astype(np.float32)
            planes.append(plane.reshape(height, width))
        return np.stack(planes, axis=-1)
    finally:
        try:
            handle.close()
        except Exception:
            pass


def _ffprobe_size(path: Path) -> tuple[int, int]:
    if shutil.which("ffprobe") is None:
        raise RuntimeError("OpenEXR binding unavailable and ffprobe is not installed")
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height",
            "-of",
            "csv=p=0:s=x",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=60,
    )
    width_text, height_text = result.stdout.strip().split("x", 1)
    width, height = int(width_text), int(height_text)
    if width <= 0 or height <= 0:
        raise RuntimeError("ffprobe returned invalid EXR dimensions")
    return width, height


def _read_ffmpeg_exr(path: Path):
    """Fallback EXR reader using ffmpeg's float planar decoder."""

    np, error = _numpy()
    if np is None:
        raise RuntimeError(error)
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("OpenEXR binding unavailable and ffmpeg is not installed")
    width, height = _ffprobe_size(path)
    result = subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(path),
            "-frames:v",
            "1",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "gbrpf32le",
            "pipe:1",
        ],
        check=True,
        capture_output=True,
        timeout=120,
    )
    values = np.frombuffer(result.stdout, dtype=np.float32)
    expected = 3 * width * height
    if values.size != expected:
        raise RuntimeError("ffmpeg EXR decode returned %d floats, expected %d" % (values.size, expected))
    # gbrpf32le is planar.  Channel order is irrelevant for luma only if the
    # planes are interpreted as RGB; ffmpeg documents this pixel format as
    # GBR planes, so reorder them to RGB before calculating metrics.
    planes = values.reshape(3, height, width)
    return np.stack((planes[2], planes[0], planes[1]), axis=-1)


def _read_image(path: Path):
    np, error = _numpy()
    if np is None:
        raise RuntimeError(error)
    suffix = path.suffix.lower()
    if suffix == ".npy":
        return np.asarray(np.load(path, allow_pickle=False), dtype=np.float64)
    if suffix == ".npz":
        loaded = np.load(path, allow_pickle=False)
        try:
            if not loaded.files:
                raise RuntimeError("NPZ has no arrays")
            return np.asarray(loaded[loaded.files[0]], dtype=np.float64)
        finally:
            loaded.close()
    if suffix == ".exr":
        try:
            return np.asarray(_read_openexr(path), dtype=np.float64)
        except Exception as openexr_error:
            try:
                return np.asarray(_read_ffmpeg_exr(path), dtype=np.float64)
            except Exception as ffmpeg_error:
                raise RuntimeError(
                    "cannot decode EXR %s (OpenEXR: %s; ffmpeg: %s)"
                    % (path, openexr_error, ffmpeg_error)
                ) from ffmpeg_error
    raise RuntimeError("unsupported linear capture format: %s" % path)


def _luma(values):
    np, error = _numpy()
    if np is None:
        raise RuntimeError(error)
    array = np.asarray(values, dtype=np.float64)
    if array.ndim != 3 or array.shape[-1] < 3 or array.size == 0:
        raise RuntimeError("expected non-empty HxWxC linear image with RGB")
    return np.maximum(np.sum(array[..., :3] * np.asarray(LUMA_WEIGHTS), axis=-1), 0.0)


def _image_metrics(values) -> dict[str, Any]:
    np, error = _numpy()
    if np is None:
        return {"status": "BLOCKED", "error": error}
    array = np.asarray(values, dtype=np.float64)
    if array.ndim != 3 or array.shape[-1] < 3 or array.size == 0:
        return {"status": "BLOCKED", "error": "expected non-empty HxWxC image with RGB"}
    finite = bool(np.isfinite(array).all())
    nonnegative = bool(float(np.nanmin(array)) >= -1e-6) if finite else False
    if finite:
        luma = _luma(array)
        dx = luma[:, 1:] - luma[:, :-1] if luma.shape[1] > 1 else np.zeros((luma.shape[0], 0))
        dy = luma[1:, :] - luma[:-1, :] if luma.shape[0] > 1 else np.zeros((0, luma.shape[1]))
        local = float(0.5 * (np.mean(dx * dx) + np.mean(dy * dy)))
        mean = float(np.mean(luma))
        p99 = float(np.percentile(luma, 99.0))
        max_luma = float(np.max(luma))
    else:
        local = mean = p99 = max_luma = None
    return {
        "status": "PASS" if finite and nonnegative else "FAIL",
        "shape": list(array.shape),
        "finite": finite,
        "nonnegative": nonnegative,
        "meanLuma": mean,
        "p99Luma": p99,
        "maxLuma": max_luma,
        "localVariance": local,
    }


def _rmse(first, second) -> float:
    np, error = _numpy()
    if np is None:
        raise RuntimeError(error)
    a = _luma(first)
    b = _luma(second)
    if a.shape != b.shape:
        raise RuntimeError("tail images have different shapes")
    return float(np.sqrt(np.mean((b - a) ** 2)))


def _capture_number(record: Mapping[str, Any], default: int = 0) -> int:
    value = record.get("frame", default)
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _record_stats(record: Mapping[str, Any]) -> Mapping[str, Any] | None:
    value = record.get("screenProbeStats")
    return value if isinstance(value, Mapping) else None


def _telemetry(manifest: Mapping[str, Any], baseline: Mapping[str, Any] | None, captures: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    records: list[tuple[str, Mapping[str, Any]]] = []
    if baseline is not None:
        records.append(("baseline", baseline))
    records.extend(("capture-%s" % _capture_number(record), record) for record in captures)
    missing: list[str] = []
    values: dict[str, list[float]] = {key: [] for key in REQUIRED_STATS}
    for label, record in records:
        stats = _record_stats(record)
        if stats is None:
            missing.append("%s:screenProbeStats" % label)
            continue
        for key in REQUIRED_STATS:
            number = _number(stats.get(key))
            if number is None:
                missing.append("%s:%s" % (label, key))
            else:
                values[key].append(number)
    readable = not missing and len(records) == len(captures) + (1 if baseline is not None else 0)
    generation_delta = None
    lighting_delta = None
    reset_delta = None
    reset_observed = False
    if baseline is not None and captures:
        before = _record_stats(baseline) or {}
        after = _record_stats(captures[0]) or {}
        before_history = _number(before.get("historyGeneration"))
        after_history = _number(after.get("historyGeneration"))
        before_lighting = _number(before.get("lightingGeneration"))
        after_lighting = _number(after.get("lightingGeneration"))
        before_reset = _number(before.get("historyReset"))
        after_reset = _number(after.get("historyReset"))
        if before_history is not None and after_history is not None:
            generation_delta = after_history - before_history
        if before_lighting is not None and after_lighting is not None:
            lighting_delta = after_lighting - before_lighting
        if before_reset is not None and after_reset is not None:
            reset_delta = after_reset - before_reset
        reset_observed = any(
            (_number((_record_stats(record) or {}).get("historyReset")) or 0.0) != 0.0
            for record in captures
        )
    evidence = manifest.get("transitionEvidence")
    evidence = evidence if isinstance(evidence, Mapping) else {}
    case = str(manifest.get("case", "")).strip().lower()
    if case == "static":
        expected = True
    elif case == "lighting_generation":
        expected = bool(lighting_delta is not None and lighting_delta != 0.0) or bool(
            evidence.get("lightingGenerationChanged")
        )
    elif case in ("camera_cut", "scene_reload"):
        expected = bool(generation_delta is not None and generation_delta != 0.0) or bool(
            reset_observed or evidence.get("historyGenerationChanged") or evidence.get("historyResetObserved")
        )
    else:
        expected = bool(
            (generation_delta is not None and generation_delta != 0.0)
            or (lighting_delta is not None and lighting_delta != 0.0)
            or reset_observed
            or evidence.get("historyGenerationChanged")
            or evidence.get("lightingGenerationChanged")
            or evidence.get("historyResetObserved")
        )
    # Do not let a producer's derived boolean hide missing raw fields.  It is
    # useful as corroboration, never as a substitute for the series above.
    status = "PASS" if readable and expected else "BLOCKED"
    return {
        "status": status,
        "readable": readable,
        "missing": missing,
        "historyGenerationValues": values["historyGeneration"],
        "lightingGenerationValues": values["lightingGeneration"],
        "historyResetValues": values["historyReset"],
        "historyGenerationDeltaBaselineToFirstPost": generation_delta,
        "lightingGenerationDeltaBaselineToFirstPost": lighting_delta,
        "historyResetDeltaBaselineToFirstPost": reset_delta,
        "historyResetObservedAfterBaseline": reset_observed,
        "expectedMutationSatisfied": expected,
        "producerEvidence": dict(evidence),
    }


def _load_capture_images(
    records: Sequence[Mapping[str, Any]],
    manifest_path: Path,
    cache: dict[str, Any],
) -> tuple[list[dict[str, Any]], list[str]]:
    frames: list[dict[str, Any]] = []
    errors: list[str] = []
    np, numpy_error = _numpy()
    if np is None:
        return [], [numpy_error or "NumPy unavailable"]
    for record in records:
        number = _capture_number(record)
        provenance = _provenance(record, manifest_path)
        record_status = str(record.get("status", "")).upper()
        item: dict[str, Any] = {
            "frame": number,
            "absoluteFrame": record.get("absoluteFrame"),
            "status": provenance["status"],
            "provenance": provenance,
            "raw": {"path": provenance.get("raw")},
            "resolved": {"path": provenance.get("resolved")},
        }
        if record_status != "PASS":
            errors.append("frame %s: producer capture status is not PASS" % number)
            item["status"] = "BLOCKED"
        errors.extend("frame %s: %s" % (number, error) for error in provenance.get("errors", []))
        for key in ("raw", "resolved"):
            path_text = provenance.get(key)
            if not path_text:
                item[key]["status"] = "BLOCKED"
                continue
            cache_key = str(Path(path_text).resolve())
            try:
                if cache_key not in cache:
                    cache[cache_key] = _read_image(Path(path_text))
                values = cache[cache_key]
                metrics = _image_metrics(values)
                item[key]["metrics"] = metrics
                item[key]["status"] = metrics.get("status", "BLOCKED")
                if metrics.get("status") != "PASS":
                    errors.append("frame %s %s output failed finite/nonnegative gate" % (number, key))
            except Exception as error:
                item[key]["status"] = "BLOCKED"
                item[key]["error"] = str(error)
                errors.append("frame %s %s output decode failed: %s" % (number, key, error))
        if (
            record_status == "PASS"
            and item["status"] == "PASS"
            and item["raw"].get("status") == "PASS"
            and item["resolved"].get("status") == "PASS"
        ):
            item["status"] = "PASS"
        else:
            item["status"] = "BLOCKED"
        frames.append(item)
    return frames, errors


def _quality(frames: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    by_frame = {_capture_number(item): item for item in frames}
    final = by_frame.get(TAIL_TO)
    earlier = by_frame.get(TAIL_FROM)
    missing: list[str] = []
    if final is None:
        missing.append("missing final frame %d" % TAIL_TO)
    if earlier is None:
        missing.append("missing tail start frame %d" % TAIL_FROM)
    result: dict[str, Any] = {
        "status": "BLOCKED",
        "targetFinalFrame": TAIL_TO,
        "tailFromFrame": TAIL_FROM,
        "tailToFrame": TAIL_TO,
        "rawLocalVarianceFinal": None,
        "resolvedLocalVarianceFinal": None,
        "rawTailRMSE": None,
        "resolvedTailRMSE": None,
        "resolvedVarianceRule": False,
        "resolvedTailRule": False,
        "qualityImproved": False,
        "rule": "resolved final localVariance < 0.99*raw and resolved 32->64 tail RMSE <= 1.01*raw",
        "interpretation": {
            "linearVarianceIsDiagnostic": True,
            "displayPngIsNotAQualityInput": True,
            "mottleOrNoNoiseProductionClaim": "OPEN",
        },
        "errors": missing,
    }
    if missing:
        return result
    try:
        raw_final = final["raw"]["metrics"]
        resolved_final = final["resolved"]["metrics"]
        raw_start = earlier["raw"]["path"]
        raw_end = final["raw"]["path"]
        resolved_start = earlier["resolved"]["path"]
        resolved_end = final["resolved"]["path"]
        # Frames were already decoded and metric-checked.  Re-reading here is
        # intentionally avoided by the caller's cache; paths remain in the
        # report as auditable provenance.
        raw_a = _read_image(Path(raw_start))
        raw_b = _read_image(Path(raw_end))
        resolved_a = _read_image(Path(resolved_start))
        resolved_b = _read_image(Path(resolved_end))
        raw_var = _number(raw_final.get("localVariance"))
        resolved_var = _number(resolved_final.get("localVariance"))
        raw_tail = _rmse(raw_a, raw_b)
        resolved_tail = _rmse(resolved_a, resolved_b)
        result.update(
            {
                "rawLocalVarianceFinal": raw_var,
                "resolvedLocalVarianceFinal": resolved_var,
                "rawTailRMSE": raw_tail,
                "resolvedTailRMSE": resolved_tail,
                "resolvedVarianceRule": bool(
                    raw_var is not None and resolved_var is not None and resolved_var < raw_var * VARIANCE_FACTOR
                ),
                "resolvedTailRule": bool(resolved_tail <= raw_tail * TAIL_RMSE_FACTOR),
            }
        )
        result["qualityImproved"] = bool(result["resolvedVarianceRule"] and result["resolvedTailRule"])
        result["status"] = "PASS" if result["qualityImproved"] else "NO_IMPROVEMENT"
    except Exception as error:
        result["errors"].append("quality metric computation failed: %s" % error)
    return result


def _transition_delta(
    baseline_frame: Mapping[str, Any] | None,
    first_post_frame: Mapping[str, Any] | None,
    telemetry: Mapping[str, Any],
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "status": "PASS" if baseline_frame is not None and first_post_frame is not None else "BLOCKED",
        "rawRMSEBaselineToFirstPost": None,
        "resolvedRMSEBaselineToFirstPost": None,
        "historyGenerationDeltaBaselineToFirstPost": telemetry.get("historyGenerationDeltaBaselineToFirstPost"),
        "lightingGenerationDeltaBaselineToFirstPost": telemetry.get("lightingGenerationDeltaBaselineToFirstPost"),
        "historyResetDeltaBaselineToFirstPost": telemetry.get("historyResetDeltaBaselineToFirstPost"),
        "errors": [],
    }
    if baseline_frame is None or first_post_frame is None:
        result["errors"].append("dynamic transition requires baseline and first post-transition capture")
        return result
    try:
        result["rawRMSEBaselineToFirstPost"] = _rmse(
            _read_image(Path(baseline_frame["raw"]["path"])),
            _read_image(Path(first_post_frame["raw"]["path"])),
        )
        result["resolvedRMSEBaselineToFirstPost"] = _rmse(
            _read_image(Path(baseline_frame["resolved"]["path"])),
            _read_image(Path(first_post_frame["resolved"]["path"])),
        )
    except Exception as error:
        result["status"] = "BLOCKED"
        result["errors"].append("transition delta computation failed: %s" % error)
    return result


def _evaluate_manifest(path: Path) -> dict[str, Any]:
    manifest, error = _read_json(path)
    if error or manifest is None:
        return {
            "status": "BLOCKED",
            "manifest": str(path),
            "case": None,
            "errors": [error or "manifest unavailable"],
        }
    case = str(manifest.get("case", "")).strip().lower()
    errors: list[str] = []
    schema = str(manifest.get("schema", ""))
    if schema != INPUT_SCHEMA:
        errors.append("unexpected input schema: %s" % schema)
    if case not in SUPPORTED_CASES:
        errors.append("unsupported or missing case: %s" % case)
    if str(manifest.get("status", "")).upper() != "PASS":
        errors.append("producer manifest status is not PASS")
    captures_value = manifest.get("captures")
    captures = [item for item in captures_value if isinstance(item, Mapping)] if isinstance(captures_value, list) else []
    if not captures:
        errors.append("manifest has no captures")
    declared_checkpoints = manifest.get("checkpointFrames")
    declared: list[int] = []
    if isinstance(declared_checkpoints, (list, tuple)):
        try:
            declared = [int(value) for value in declared_checkpoints]
        except (TypeError, ValueError):
            declared = []
    actual = [_capture_number(record, -1) for record in captures]
    if declared != list(REQUIRED_CHECKPOINTS):
        errors.append(
            "checkpointFrames must exactly match %s, got %s"
            % (list(REQUIRED_CHECKPOINTS), declared)
        )
    if len(actual) != len(set(actual)):
        errors.append("captures contain duplicate checkpoint frame numbers")
    if sorted(actual) != list(REQUIRED_CHECKPOINTS):
        errors.append(
            "capture checkpoint set must exactly match %s, got %s"
            % (list(REQUIRED_CHECKPOINTS), sorted(actual))
        )
    # The static producer intentionally serializes ``baseline`` as an empty
    # object.  Treat only a populated mapping as a real baseline capture; an
    # empty placeholder must not become a phantom frame-0 BLOCKED record.
    baseline_value = manifest.get("baseline")
    baseline = baseline_value if isinstance(baseline_value, Mapping) and baseline_value else None
    if case != "static" and baseline is None:
        errors.append("dynamic case is missing baseline capture")
    records: list[Mapping[str, Any]] = []
    if baseline is not None:
        records.append(baseline)
    records.extend(captures)
    cache: dict[str, Any] = {}
    frames, capture_errors = _load_capture_images(records, path, cache)
    errors.extend(capture_errors)
    baseline_frame = frames[0] if baseline is not None and frames else None
    post_frames = frames[1:] if baseline is not None else frames
    telemetry = _telemetry(manifest, baseline, captures)
    if telemetry.get("status") != "PASS":
        errors.extend("telemetry: %s" % item for item in telemetry.get("missing", []))
        if not telemetry.get("expectedMutationSatisfied"):
            errors.append("required history reset/generation transition was not observed")
    quality = _quality(post_frames)
    transition = _transition_delta(
        baseline_frame if case != "static" else None,
        post_frames[0] if post_frames else None,
        telemetry,
    )
    if case != "static" and transition.get("status") != "PASS":
        errors.extend("transition: %s" % item for item in transition.get("errors", []))
    validity = not errors and all(item.get("status") == "PASS" for item in frames)
    if not validity:
        status = "BLOCKED"
    elif quality.get("status") == "PASS":
        status = "PASS"
    else:
        status = "NO_IMPROVEMENT"
    return {
        "status": status,
        "manifest": str(path.resolve()),
        "case": case,
        "scene": manifest.get("scene"),
        "resolution": manifest.get("resolution"),
        "producerStatus": manifest.get("status"),
        "checkpointContract": {
            "status": "PASS" if not any("checkpoint" in error for error in errors) else "BLOCKED",
            "required": list(REQUIRED_CHECKPOINTS),
            "declared": declared,
            "actual": actual,
        },
        "validity": {
            "status": "PASS" if validity else "BLOCKED",
            "captureCount": len(captures),
            "pngExrProvenance": validity,
            "finiteNonnegativeLinearOutputs": validity,
            "errors": errors,
        },
        "telemetry": telemetry,
        "frames": frames,
        "quality": quality,
        "transitionDelta": transition,
    }


def _expand_inputs(values: Iterable[str]) -> list[Path]:
    result: list[Path] = []
    for raw in values:
        path = Path(raw)
        if path.is_dir():
            path = path / "screenprobe-dynamic.json"
        if any(char in raw for char in "*?["):
            result.extend(Path(item) for item in sorted(glob.glob(raw)))
            continue
        result.append(path)
    unique: list[Path] = []
    seen: set[str] = set()
    for path in result:
        key = str(path.absolute()).lower()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def evaluate(paths: Sequence[Path]) -> dict[str, Any]:
    if not paths:
        return {
            "schema": SCHEMA,
            "status": "BLOCKED",
            "cases": [],
            "errors": ["no dynamic manifests were provided"],
        }
    cases = [_evaluate_manifest(path) for path in paths]
    errors: list[str] = []
    seen_cases: set[str] = set()
    for item in cases:
        case = item.get("case")
        if case in seen_cases:
            errors.append("duplicate case manifest: %s" % case)
        elif case:
            seen_cases.add(str(case))
    if len(seen_cases) < len(cases):
        errors.append("every manifest must identify a unique dynamic case")
    if any(item.get("status") == "BLOCKED" for item in cases) or errors:
        status = "BLOCKED"
    elif any(item.get("status") == "NO_IMPROVEMENT" for item in cases):
        status = "OPEN"
    else:
        status = "PASS"
    provided_cases = sorted(str(item.get("case")) for item in cases if item.get("case"))
    missing_cases = [case for case in REQUIRED_CASE_MATRIX if case not in provided_cases]
    scenes = sorted(
        {
            str(item.get("scene"))
            for item in cases
            if item.get("scene") is not None and str(item.get("scene"))
        }
    )
    return {
        "schema": SCHEMA,
        "status": status,
        "inputCount": len(paths),
        "cases": cases,
        "scope": {
            "boundedQualityStatus": status,
            "requiredCaseMatrix": list(REQUIRED_CASE_MATRIX),
            "providedCases": provided_cases,
            "missingCases": missing_cases,
            "caseMatrixComplete": not missing_cases and not errors,
            "sceneCount": len(scenes),
            "scenes": scenes,
            "multiSceneEvidenceStatus": "PASS" if len(scenes) >= 2 else "OPEN",
            "multiSceneEvidenceReason": (
                "at least two distinct scene identifiers are present"
                if len(scenes) >= 2
                else "all supplied cases share one scene; quality is bounded per-case evidence"
            ),
            "productionNoNoiseStatus": "OPEN",
            "displayPngQualityInference": "PROHIBITED",
        },
        "qualityRule": {
            "varianceFactor": VARIANCE_FACTOR,
            "tailRMSEFactor": TAIL_RMSE_FACTOR,
            "tailFrames": [TAIL_FROM, TAIL_TO],
            "linearChannels": {
                "raw": "LumenGI.diffuseRadianceHitDist RGB",
                "resolved": "LumenGI.resolvedDiffuseGI RGB",
            },
            "pngIsProvenanceOnly": True,
            "noThresholdRelaxation": True,
        },
        "errors": errors,
    }


def _fixture_stats(history: float, lighting: float, reset: float) -> dict[str, float]:
    return {
        "historyAccepted": 1.0,
        "historyRejectDepth": 0.0,
        "historyRejectGuide": 0.0,
        "historyRejectMotion": 0.0,
        "historyRejectLighting": 0.0,
        "historyRejectCurrentInvalid": 0.0,
        "historyRejectPreviousInvalid": 0.0,
        "historyReset": reset,
        "historyGeneration": history,
        "lightingGeneration": lighting,
    }


def _make_fixture(root: Path, quality_improved: bool, blocked: bool = False) -> Path:
    np, error = _numpy()
    if np is None:
        raise RuntimeError(error)
    capture_dir = root / "captures"
    capture_dir.mkdir(parents=True, exist_ok=True)
    # A deterministic high-frequency source and a smooth resolved signal make
    # the frozen rule observable without using any display-space image.
    yy, xx = np.mgrid[0:8, 0:8]
    frames: list[dict[str, Any]] = []
    for frame in (1, 8, 16, 32, 64):
        phase = float(frame) / 64.0
        # Keep a checkerboard component for spatial variance and add a
        # frame-dependent offset so the raw 32->64 tail is non-zero.  Without
        # the offset, the even-frame checkerboard would make the synthetic raw
        # tail accidentally zero and exercise the wrong branch of the rule.
        raw = (0.2 + 0.1 * ((xx + yy + frame) % 2) + 0.2 * phase)[..., None]
        raw = np.repeat(raw, 3, axis=2).astype(np.float32)
        if quality_improved:
            resolved = np.repeat((0.2 + 0.01 * phase * (xx + yy))[..., None], 3, axis=2).astype(np.float32)
        else:
            resolved = raw.copy()
        raw_path = capture_dir / ("frame-%04d-raw.npy" % frame)
        resolved_path = capture_dir / ("frame-%04d-resolved.npy" % frame)
        np.save(raw_path, raw)
        np.save(resolved_path, resolved)
        png_path = capture_dir / ("frame-%04d.png" % frame)
        png_path.write_bytes(b"synthetic-png-provenance")
        record = {
            "frame": frame,
            "absoluteFrame": frame,
            "status": "PASS",
            "screenProbeStats": _fixture_stats(2.0, 2.0, 0.0),
            "frameCapture": {
                "status": "PASS",
                "png": [str(png_path)],
                "exr": [str(raw_path), str(resolved_path)],
                "rawPath": str(raw_path),
                "resolvedPath": str(resolved_path),
            },
        }
        frames.append(record)
    baseline_raw = capture_dir / "baseline-raw.npy"
    baseline_resolved = capture_dir / "baseline-resolved.npy"
    np.save(baseline_raw, np.zeros((8, 8, 3), dtype=np.float32))
    np.save(baseline_resolved, np.zeros((8, 8, 3), dtype=np.float32))
    baseline_png = capture_dir / "baseline.png"
    baseline_png.write_bytes(b"synthetic-png-provenance")
    baseline = {
        "frame": 0,
        "absoluteFrame": 0,
        "status": "PASS",
        "screenProbeStats": _fixture_stats(1.0, 1.0, 0.0),
        "frameCapture": {
            "status": "PASS",
            "png": [str(baseline_png)],
            "exr": [str(baseline_raw), str(baseline_resolved)],
            "rawPath": str(baseline_raw),
            "resolvedPath": str(baseline_resolved),
        },
    }
    if blocked:
        frames[-1]["frameCapture"]["exr"] = []
        frames[-1]["screenProbeStats"].pop("historyGeneration")
    manifest = {
        "schema": INPUT_SCHEMA,
        "status": "PASS",
        "case": "camera_cut",
        "scene": "synthetic",
        "resolution": [8, 8],
        "checkpointFrames": list(REQUIRED_CHECKPOINTS),
        "baseline": baseline,
        "captures": frames,
        "transitionEvidence": {
            "telemetryReadable": True,
            "historyGenerationChanged": True,
            "historyResetObserved": True,
            "expectedMutationSatisfied": True,
        },
    }
    # The explicit reset/generation transition is in the baseline/first post
    # records, not only in this corroborating producer field.
    manifest["captures"][0]["screenProbeStats"] = _fixture_stats(2.0, 1.0, 1.0)
    path = root / "screenprobe-dynamic.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def _run_self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="screenprobe-dynamic-quality-") as directory:
        root = Path(directory)
        pass_manifest = _make_fixture(root / "pass", quality_improved=True)
        open_manifest = _make_fixture(root / "open", quality_improved=False)
        blocked_manifest = _make_fixture(root / "blocked", quality_improved=True, blocked=True)
        incomplete_manifest = root / "incomplete" / "screenprobe-dynamic.json"
        incomplete_manifest.parent.mkdir(parents=True, exist_ok=True)
        incomplete_payload = json.loads(pass_manifest.read_text(encoding="utf-8"))
        incomplete_payload["captures"] = incomplete_payload["captures"][:-1]
        incomplete_manifest.write_text(json.dumps(incomplete_payload), encoding="utf-8")
        passed = evaluate([pass_manifest])
        opened = evaluate([open_manifest])
        blocked = evaluate([blocked_manifest])
        incomplete = evaluate([incomplete_manifest])
        assert passed["status"] == "PASS", passed
        assert opened["status"] == "OPEN", opened
        assert blocked["status"] == "BLOCKED", blocked
        assert incomplete["status"] == "BLOCKED", incomplete
        assert incomplete["cases"][0]["checkpointContract"]["status"] == "BLOCKED"
        assert passed["cases"][0]["quality"]["resolvedVarianceRule"]
        assert passed["cases"][0]["quality"]["resolvedTailRule"]
        assert passed["scope"]["multiSceneEvidenceStatus"] == "OPEN"
        assert passed["scope"]["productionNoNoiseStatus"] == "OPEN"
        assert opened["cases"][0]["status"] == "NO_IMPROVEMENT"
    print("SCREENPROBE_DYNAMIC_QUALITY_FIXTURE PASS")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="dynamic manifest JSON paths or directories containing screenprobe-dynamic.json",
    )
    parser.add_argument(
        "--manifest",
        "--input",
        dest="manifest_options",
        action="append",
        default=[],
        help="dynamic manifest JSON path (repeat for each case)",
    )
    parser.add_argument(
        "--output",
        default="artifacts/lumengi/A2/dynamic-quality/dynamic-quality-gate.json",
        help="aggregate report JSON path",
    )
    parser.add_argument("--self-test", action="store_true", help="run dependency-light synthetic PASS/OPEN/BLOCKED checks")
    args = parser.parse_args(argv)
    if args.self_test:
        return _run_self_test()
    inputs = _expand_inputs(list(args.manifest_options) + list(args.paths))
    report = evaluate(inputs)
    output = Path(args.output).absolute()
    _write_json(output, report)
    print("SCREENPROBE_DYNAMIC_QUALITY", report["status"], output)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
