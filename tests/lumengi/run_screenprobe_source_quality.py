"""A2/C5 ScreenProbe source-quality contract and A/B runner.

This file deliberately stays on the test side of the frozen producer ABI.  It
does not modify ``LumenGI`` or any shader.  The source channel is
``diffuseRadianceHitDist`` (RGBA16F): RGB is unmodulated radiance and **alpha
is the hit distance**.  ``65504`` remains the miss sentinel; alpha is never
treated as confidence, age, or history length.

The script has three modes:

``python ... --self-test``
    Dependency-light ABI/metric checks.  This is the only mode intended to run
    outside Mogwai without a Falcor Python module.

Mogwai (no argument)
    Runs one deterministic Cornell sequence.  Set
    ``LUMEN_SOURCE_QUALITY_VARIANT=off|on`` to disable/enable source moments,
    and give the two runs different ``LUMEN_SOURCE_QUALITY_OUT`` directories.
    The runner marks the raw source plus production ``resolvedDiffuseGI`` and
    ``fullColor``-compatible downstream channels, and writes ``.npy`` snapshots
    so the compare step does not depend on an EXR Python binding.

``python ... --compare``
    Compares two completed manifests selected by
    ``LUMEN_SOURCE_QUALITY_OFF`` and ``LUMEN_SOURCE_QUALITY_ON``.  Runtime
    validity, alpha-semantic invariance, and image-quality improvement are
    separate verdicts.  A missing source or manifest is BLOCKED; a valid A/B
    pair with no measurable improvement is NO_IMPROVEMENT, never PASS.

The runtime manifest requires both A2 history-reject counters and C5
Surface-Cache hit/coverage/page/metadata/visibility counters at every
checkpoint.  This keeps a smooth image from hiding a cache-coverage failure.

No GPU/build is started by the Python self-test or compare mode.
"""

from __future__ import annotations

import json
import math
import os
import sys
import traceback
import uuid
from pathlib import Path

# Keep this bootstrap independent of numpy and the Falcor Python extension.
# Mogwai may spend a long time importing/compiling those dependencies before
# the normal runner reaches ``_initial_manifest``.  Writing here makes a
# plugin-only or externally terminated process distinguishable from a script
# that was never loaded.
_BOOTSTRAP_SCHEMA = "LumenGI.ScreenProbeSourceQuality.v1"
_BOOTSTRAP_DEFAULT_SCENE = "media/test_scenes/cornell_box.pyscene"
_BOOTSTRAP_DEFAULT_RESOLUTION = "640x360"
_BOOTSTRAP_DEFAULT_CHECKPOINTS = "1,8,32,96"
_BOOTSTRAP_RECOMMENDED_INVOCATION = (
    "Mogwai.exe -s tests/lumengi/run_screenprobe_source_quality.py "
    "--headless --silent"
)
_BOOTSTRAP_OUT_DIR = Path(os.path.abspath(os.environ.get(
    "LUMEN_SOURCE_QUALITY_OUT", "artifacts/lumengi/A2/source-quality/run-on"
)))
_BOOTSTRAP_MANIFEST_PATH = _BOOTSTRAP_OUT_DIR / "source-quality-manifest.json"


def _write_python_bootstrap_manifest(phase: str = "python-bootstrap") -> None:
    scene = os.environ.get("LUMEN_SOURCE_QUALITY_SCENE", _BOOTSTRAP_DEFAULT_SCENE)
    resolution = os.environ.get("LUMEN_SOURCE_QUALITY_RESOLUTION", _BOOTSTRAP_DEFAULT_RESOLUTION)
    checkpoints = os.environ.get("LUMEN_SOURCE_QUALITY_CHECKPOINTS", _BOOTSTRAP_DEFAULT_CHECKPOINTS)
    try:
        parsed_checkpoints = [
            int(item.strip()) for item in str(checkpoints).split(",") if item.strip()
        ]
    except Exception:
        # Preserve the raw environment value when it is malformed; the normal
        # parser below will retain its existing fallback semantics.
        parsed_checkpoints = str(checkpoints)
    payload = {
        "schema": _BOOTSTRAP_SCHEMA,
        "status": "BLOCKED",
        "phase": phase,
        "runner": {
            "script": "tests/lumengi/run_screenprobe_source_quality.py",
            "recommendedMogwaiInvocation": _BOOTSTRAP_RECOMMENDED_INVOCATION,
        },
        "env": {
            "scene": scene,
            "resolution": resolution,
            "checkpoints": checkpoints,
        },
        "scene": scene,
        "resolution": resolution,
        "checkpointFrames": parsed_checkpoints,
        "captures": [],
        "errors": [],
    }
    try:
        _BOOTSTRAP_OUT_DIR.mkdir(parents=True, exist_ok=True)
        temporary = _BOOTSTRAP_MANIFEST_PATH.with_suffix(
            _BOOTSTRAP_MANIFEST_PATH.suffix + ".bootstrap.tmp"
        )
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(payload, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
        temporary.replace(_BOOTSTRAP_MANIFEST_PATH)
    except Exception:
        # A malformed/unwritable user-selected output path must not prevent
        # numpy/Falcor import or alter the existing BLOCKED behavior.
        pass


_write_python_bootstrap_manifest()

_write_python_bootstrap_manifest("falcor-import")
try:
    from falcor import *  # type: ignore  # noqa: F401,F403

    FALCOR_AVAILABLE = True
    FALCOR_IMPORT_ERROR = None
except Exception as _falcor_error:  # pragma: no cover - exercised outside Mogwai.
    FALCOR_AVAILABLE = False
    FALCOR_IMPORT_ERROR = repr(_falcor_error)
_write_python_bootstrap_manifest("falcor-import-complete")

_write_python_bootstrap_manifest("numpy-import")
try:
    import numpy as np
except Exception as _numpy_error:  # pragma: no cover - environment dependent.
    np = None
    NUMPY_IMPORT_ERROR = repr(_numpy_error)
else:
    NUMPY_IMPORT_ERROR = None
_write_python_bootstrap_manifest("numpy-import-complete")


SCHEMA = _BOOTSTRAP_SCHEMA
COMPARE_SCHEMA = "LumenGI.ScreenProbeSourceQualityComparison.v1"
FRAME_RATE = 60
# Mogwai's short option is the least ambiguous spelling across the bundled
# command-line parser versions.  The long form is only safe with an equals
# value (``--script=path``); keeping this in the artifact makes an idle
# plugin-only process distinguishable from a script-side initialization stall.
RECOMMENDED_MOGWAI_INVOCATION = _BOOTSTRAP_RECOMMENDED_INVOCATION
def _parse_checkpoints(value: str) -> tuple[int, ...]:
    parsed = tuple(sorted({int(item.strip()) for item in str(value).split(",") if item.strip()}))
    if not parsed or parsed[0] < 1:
        raise ValueError("checkpoints must be positive")
    return parsed


try:
    CHECKPOINTS = _parse_checkpoints(os.environ.get("LUMEN_SOURCE_QUALITY_CHECKPOINTS", "1,8,32,96"))
except Exception:
    CHECKPOINTS = (1, 8, 32, 96)
RAW_CHANNEL = "diffuseRadianceHitDist"
REQUIRED_REJECT_TELEMETRY = (
    "historyAccepted",
    "historyRejectDepth",
    "historyRejectGuide",
    "historyRejectMotion",
    "historyRejectLighting",
    "historyRejectCurrentInvalid",
    "historyRejectPreviousInvalid",
)
REQUIRED_CACHE_TELEMETRY = (
    "cacheLookupAttempts",
    "cacheLookupHits",
    "cachePageRejects",
    "cacheCoverageRejects",
    "cacheMetadataRejects",
    "cacheVisibilityRejects",
)
# Keep the alpha contract explicit: half-float max is the frozen miss value.
HIT_DISTANCE_MISS = 65504.0
HIT_DISTANCE_EPSILON = 0.5
RGB_CHANNELS = 3
LUMA_WEIGHTS = np.asarray([0.2126, 0.7152, 0.0722], dtype=np.float64) if np is not None else None

# Use the media-root spelling used by the existing Mogwai convergence runners.
# The shorter ``test_scenes/...`` alias is accepted by some script entry points,
# but is not resolved consistently when Mogwai is started with a long option or
# from a different working directory.  A failed/ambiguous scene load leaves the
# process in its idle headless loop and, historically, made this runner appear
# to have timed out before writing a manifest.
DEFAULT_SCENE = _BOOTSTRAP_DEFAULT_SCENE
SCENE_PATH = os.environ.get("LUMEN_SOURCE_QUALITY_SCENE", DEFAULT_SCENE)
RESOLUTION_TEXT = os.environ.get("LUMEN_SOURCE_QUALITY_RESOLUTION", "640x360")
PROBE_DIRECTIONS = max(1, int(os.environ.get("LUMEN_SOURCE_QUALITY_DIRECTIONS", "8")))
PROBE_MAX_PER_FRAME = max(0, int(os.environ.get("LUMEN_SOURCE_QUALITY_MAX_PROBES", "256")))


def _parse_resolution(value: str) -> tuple[int, int]:
    normalized = str(value).lower().replace(" ", "")
    for separator in ("x", ","):
        if separator in normalized:
            width, height = normalized.split(separator, 1)
            parsed = (int(width), int(height))
            if min(parsed) < 1:
                raise ValueError("resolution must be positive: %s" % value)
            return parsed
    raise ValueError("resolution must be WxH or W,H: %s" % value)


try:
    RESOLUTION = _parse_resolution(RESOLUTION_TEXT)
except Exception:
    RESOLUTION = (640, 360)

VARIANT = os.environ.get("LUMEN_SOURCE_QUALITY_VARIANT", "on").strip().lower()
if VARIANT not in ("off", "on"):
    VARIANT = "on"
USE_SOURCE_MOMENTS = VARIANT == "on"

OUT_DIR = Path(os.path.abspath(os.environ.get(
    "LUMEN_SOURCE_QUALITY_OUT", "artifacts/lumengi/A2/source-quality/run-on"
)))
MANIFEST_PATH = OUT_DIR / "source-quality-manifest.json"
COMPARE_OFF = Path(os.path.abspath(os.environ.get(
    "LUMEN_SOURCE_QUALITY_OFF", "artifacts/lumengi/A2/source-quality/run-off"
)))
COMPARE_ON = Path(os.path.abspath(os.environ.get(
    "LUMEN_SOURCE_QUALITY_ON", "artifacts/lumengi/A2/source-quality/run-on"
)))
COMPARE_OUT = Path(os.path.abspath(os.environ.get(
    "LUMEN_SOURCE_QUALITY_COMPARE_OUT", "artifacts/lumengi/A2/source-quality/compare"
)))


def _json_safe(value):
    """Convert numpy/Falcor values to strict JSON without hiding non-finite data."""

    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if np is not None and isinstance(value, np.generic):
        return _json_safe(value.item())
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    return str(value)


def _write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    safe_payload = _json_safe(payload)
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(safe_payload, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    try:
        temporary.replace(path)
    except OSError as error:
        # Some embedded Mogwai launchers deny the atomic rename even though
        # the artifact directory itself is writable (WinError 5 / EACCES).
        # Preserve the atomic path as the first choice, but make the manifest
        # observable through a closed direct write in that narrow case.  Do
        # not catch/convert any other JSON or filesystem error.
        if not isinstance(error, PermissionError) and getattr(error, "winerror", None) != 5:
            raise
        with path.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(safe_payload, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")


def _write_progress(manifest: dict, phase: str) -> None:
    """Persist a phase marker before every potentially blocking Mogwai call.

    A Mogwai process can be externally terminated while compiling a first-use
    graph.  The old runner only wrote its manifest in ``finally``, so that
    perfectly legitimate timeout path looked identical to a script that was
    never loaded.  Keeping the same manifest path and updating a small marker
    makes the artifact useful without weakening any quality verdict.
    """

    manifest["phase"] = str(phase)
    manifest["progressFrame"] = int(manifest.get("progressFrame", 0))
    _write_json(MANIFEST_PATH, manifest)


def _self_test_scratch(label: str) -> Path:
    """Create a unique scratch directory beside the configured artifact root.

    ``tempfile.TemporaryDirectory`` resolves to the user Temp directory on
    Windows.  In the managed desktop runtime that location can be ACL-
    restricted (including its cleanup pass), while the repository artifact
    root is explicitly writable.  Keep the test bounded to a fresh directory
    under that root and never remove a caller-selected path.
    """

    root = OUT_DIR.parent / (".source-quality-%s-%s" % (label, uuid.uuid4().hex))
    root.mkdir(parents=True, exist_ok=False)
    return root


def _cleanup_self_test_scratch(root: Path) -> None:
    """Remove only files created by ``_self_test_scratch``; tolerate ACL races."""

    try:
        for child in root.iterdir():
            if child.is_file() or child.is_symlink():
                try:
                    child.unlink()
                except OSError:
                    pass
        try:
            root.rmdir()
        except OSError:
            pass
    except OSError:
        pass


def _luma(values: np.ndarray) -> np.ndarray:
    rgb = np.maximum(np.asarray(values, dtype=np.float64)[..., :RGB_CHANNELS], 0.0)
    return np.sum(rgb * LUMA_WEIGHTS, axis=-1)


def _image_metrics(values: np.ndarray) -> dict:
    """Return finite linear-light mottle metrics; no exposure/PNG inference."""

    array = np.asarray(values, dtype=np.float64)
    if array.ndim != 3 or array.shape[-1] < RGB_CHANNELS:
        return {"status": "BLOCKED", "reason": "expected HxWxC with at least RGB"}
    finite = bool(np.isfinite(array).all())
    rgb = array[..., :RGB_CHANNELS]
    nonnegative = bool(float(np.nanmin(rgb)) >= -1e-6) if rgb.size and finite else False
    lum = _luma(array)
    if lum.shape[0] > 1:
        dy = lum[1:, :] - lum[:-1, :]
    else:
        dy = np.zeros((0, lum.shape[1]), dtype=np.float64)
    if lum.shape[1] > 1:
        dx = lum[:, 1:] - lum[:, :-1]
    else:
        dx = np.zeros((lum.shape[0], 0), dtype=np.float64)
    gradients = np.concatenate((np.abs(dx).reshape(-1), np.abs(dy).reshape(-1)))
    local_variance = float(0.5 * (np.mean(dx * dx) + np.mean(dy * dy))) if gradients.size else 0.0
    return {
        "status": "PASS" if finite and nonnegative else "FAIL",
        "shape": list(array.shape),
        "finite": finite,
        "nonnegativeRGB": nonnegative,
        "meanLuma": float(np.mean(lum)) if lum.size else None,
        "stdLuma": float(np.std(lum)) if lum.size else None,
        "p99Luma": float(np.percentile(lum, 99.0)) if lum.size else None,
        "maxLuma": float(np.max(lum)) if lum.size else None,
        "localVariance": local_variance,
        "gradientP95": float(np.percentile(gradients, 95.0)) if gradients.size else 0.0,
        "gradientP99": float(np.percentile(gradients, 99.0)) if gradients.size else 0.0,
    }


def _raw_source_contract(values: np.ndarray) -> dict:
    """Validate RGB radiance and frozen RGBA16F hit-distance alpha separately."""

    array = np.asarray(values, dtype=np.float64)
    result = {
        "schema": "LumenGI.DiffuseRadianceHitDist.v1",
        "status": "BLOCKED",
        "channels": int(array.shape[-1]) if array.ndim == 3 else None,
        "alphaSemantic": "hitDistanceMeters; 65504=miss; never age/confidence",
        "missSentinel": HIT_DISTANCE_MISS,
    }
    if array.ndim != 3 or array.shape[-1] < 4 or array.size == 0:
        result["reason"] = "raw source is not a non-empty HxWx4 texture"
        return result
    rgb = array[..., :3]
    alpha = array[..., 3]
    rgb_finite = bool(np.isfinite(rgb).all())
    alpha_finite = bool(np.isfinite(alpha).all())
    rgb_nonnegative = bool(float(np.nanmin(rgb)) >= -1e-6) if rgb.size and rgb_finite else False
    alpha_nonnegative = bool(float(np.nanmin(alpha)) >= 0.0) if alpha.size and alpha_finite else False
    alpha_in_range = bool(float(np.nanmax(alpha)) <= HIT_DISTANCE_MISS + HIT_DISTANCE_EPSILON) if alpha.size and alpha_finite else False
    miss = np.isclose(alpha, HIT_DISTANCE_MISS, atol=HIT_DISTANCE_EPSILON, rtol=0.0)
    finite_hit = np.isfinite(alpha) & (alpha > 0.0) & (alpha < HIT_DISTANCE_MISS - HIT_DISTANCE_EPSILON)
    zero = np.isclose(alpha, 0.0, atol=HIT_DISTANCE_EPSILON, rtol=0.0)
    result.update({
        "status": "PASS" if rgb_finite and alpha_finite and rgb_nonnegative and alpha_nonnegative and alpha_in_range else "FAIL",
        "rgbFinite": rgb_finite,
        "rgbNonnegative": rgb_nonnegative,
        "alphaFinite": alpha_finite,
        "alphaNonnegative": alpha_nonnegative,
        "alphaInRange": alpha_in_range,
        "pixelCount": int(alpha.size),
        "missCount": int(np.count_nonzero(miss)),
        "hitDistanceCount": int(np.count_nonzero(finite_hit)),
        "zeroAlphaCount": int(np.count_nonzero(zero)),
        "missFraction": float(np.mean(miss)),
        "hitDistanceMin": float(np.min(alpha[finite_hit])) if np.any(finite_hit) else None,
        "hitDistanceMax": float(np.max(alpha[finite_hit])) if np.any(finite_hit) else None,
        "alphaMin": float(np.min(alpha)) if alpha.size else None,
        "alphaMax": float(np.max(alpha)) if alpha.size else None,
    })
    return result


def _integer_output_contract(values: np.ndarray, name: str) -> dict:
    """Validate R32Uint diagnostics without conflating them with RGBA alpha."""

    array = np.asarray(values, dtype=np.float64)
    finite = bool(np.isfinite(array).all()) if array.size else False
    nonnegative = bool(float(np.nanmin(array)) >= 0.0) if array.size and finite else False
    integral = bool(np.all(np.abs(array - np.rint(array)) <= 1e-6)) if array.size and finite else False
    return {
        "status": "PASS" if finite and nonnegative and integral else "FAIL",
        "name": name,
        "shape": list(array.shape),
        "finite": finite,
        "nonnegative": nonnegative,
        "integral": integral,
        "min": float(np.min(array)) if array.size and finite else None,
        "max": float(np.max(array)) if array.size and finite else None,
    }


def _alpha_delta(off: np.ndarray, on: np.ndarray) -> dict:
    """Compare only alpha semantics; RGB may legitimately change downstream."""

    a = np.asarray(off, dtype=np.float64)
    b = np.asarray(on, dtype=np.float64)
    if a.ndim != 3 or b.ndim != 3 or a.shape != b.shape or a.shape[-1] < 4:
        return {"status": "BLOCKED", "reason": "A/B raw source shapes differ or alpha is missing"}
    alpha_a = a[..., 3]
    alpha_b = b[..., 3]
    delta = np.abs(alpha_a - alpha_b)
    # Source moments are downstream of S1; changing them must not repurpose the
    # source alpha. Half-float readback can differ by at most one ULP.
    max_delta = float(np.max(delta)) if delta.size else 0.0
    same = bool(np.all(delta <= HIT_DISTANCE_EPSILON))
    sentinels_same = bool(np.array_equal(
        np.isclose(alpha_a, HIT_DISTANCE_MISS, atol=HIT_DISTANCE_EPSILON, rtol=0.0),
        np.isclose(alpha_b, HIT_DISTANCE_MISS, atol=HIT_DISTANCE_EPSILON, rtol=0.0),
    ))
    return {
        "status": "PASS" if same and sentinels_same else "FAIL",
        "maxAbsDelta": max_delta,
        "sameWithinHalfFloatTolerance": same,
        "missMaskInvariant": sentinels_same,
        "pixelCount": int(alpha_a.size),
    }


def _tail_rmse(images: dict[int, np.ndarray], start: int, end: int) -> float | None:
    if start not in images or end not in images or images[start] is None or images[end] is None:
        return None
    a = _luma(images[start])
    b = _luma(images[end])
    return float(np.sqrt(np.mean((b - a) ** 2)))


def _load_manifest(root: Path) -> tuple[dict | None, str | None]:
    path = root / "source-quality-manifest.json"
    if not path.is_file():
        return None, "missing manifest: %s" % path
    try:
        return json.loads(path.read_text(encoding="utf-8")), None
    except Exception as error:
        return None, "invalid manifest %s: %s" % (path, error)


def _load_snapshot(root: Path, channel: str, frame: int) -> np.ndarray | None:
    path = root / ("%s-%s.npy" % (channel, frame))
    if not path.is_file() or np is None:
        return None
    try:
        return np.asarray(np.load(path, allow_pickle=False), dtype=np.float64)
    except Exception:
        return None


def _compare_runs(off_root: Path, on_root: Path, out_root: Path) -> int:
    if np is None:
        report = {"schema": COMPARE_SCHEMA, "status": "BLOCKED", "reason": NUMPY_IMPORT_ERROR}
        _write_json(out_root / "source-quality-comparison.json", report)
        return 2
    off, off_error = _load_manifest(off_root)
    on, on_error = _load_manifest(on_root)
    errors = [error for error in (off_error, on_error) if error]
    if errors:
        report = {"schema": COMPARE_SCHEMA, "status": "BLOCKED", "errors": errors}
        _write_json(out_root / "source-quality-comparison.json", report)
        print("SCREENPROBE_SOURCE_QUALITY_COMPARE BLOCKED", out_root / "source-quality-comparison.json")
        return 2

    validity = {
        "off": off.get("status") == "PASS",
        "on": on.get("status") == "PASS",
        "offRejectTelemetry": _manifest_telemetry_ok(off),
        "onRejectTelemetry": _manifest_telemetry_ok(on),
        "rawSource": True,
        "snapshots": True,
    }
    frame_records = {}
    alpha_records = {}
    raw_off = {}
    raw_on = {}
    output_off = {"spatialFiltered": {}, "resolvedDiffuseGI": {}}
    output_on = {"spatialFiltered": {}, "resolvedDiffuseGI": {}}
    for frame in CHECKPOINTS:
        raw_off[frame] = _load_snapshot(off_root, RAW_CHANNEL, frame)
        raw_on[frame] = _load_snapshot(on_root, RAW_CHANNEL, frame)
        if raw_off[frame] is None or raw_on[frame] is None:
            validity["snapshots"] = False
            continue
        alpha_records[str(frame)] = _alpha_delta(raw_off[frame], raw_on[frame])
        frame_records[str(frame)] = {
            "offRaw": _raw_source_contract(raw_off[frame]),
            "onRaw": _raw_source_contract(raw_on[frame]),
        }
        for channel, table, root in (
            ("spatialFiltered", output_off, off_root),
            ("resolvedDiffuseGI", output_off, off_root),
            ("spatialFiltered", output_on, on_root),
            ("resolvedDiffuseGI", output_on, on_root),
        ):
            sample = _load_snapshot(root, channel, frame)
            if sample is None:
                validity["snapshots"] = False
                table[channel][str(frame)] = {"status": "BLOCKED", "reason": "missing snapshot"}
            else:
                table[channel][str(frame)] = _image_metrics(sample)

    alpha_ok = bool(alpha_records) and all(item.get("status") == "PASS" for item in alpha_records.values())
    raw_ok = bool(frame_records) and all(
        frame.get(side, {}).get("status") == "PASS"
        for frame in frame_records.values()
        for side in ("offRaw", "onRaw")
    )

    # The quality gate is intentionally a bounded A/B signal, not a visual claim
    # that all source mottle has disappeared.  It mirrors the existing moments
    # comparison rule while preserving a distinct raw-source contract.
    target = "spatialFiltered"
    off_96 = output_off[target].get("96", {})
    on_96 = output_on[target].get("96", {})
    off_var = off_96.get("localVariance", math.inf)
    on_var = on_96.get("localVariance", math.inf)
    off_tail = _tail_rmse({f: _load_snapshot(off_root, target, f) for f in CHECKPOINTS}, 32, 96)
    on_tail = _tail_rmse({f: _load_snapshot(on_root, target, f) for f in CHECKPOINTS}, 32, 96)
    quality_improved = bool(
        math.isfinite(off_var)
        and math.isfinite(on_var)
        and on_var < off_var * 0.99
        and off_tail is not None
        and on_tail is not None
        and on_tail <= off_tail * 1.01
    )
    runtime_status = (
        "PASS"
        if validity["off"]
        and validity["on"]
        and validity["offRejectTelemetry"]
        and validity["onRejectTelemetry"]
        and validity["snapshots"]
        and raw_ok
        and alpha_ok
        else "BLOCKED"
    )
    report = {
        "schema": COMPARE_SCHEMA,
        "status": runtime_status if runtime_status != "PASS" else ("PASS" if quality_improved else "NO_IMPROVEMENT"),
        "off": {"root": str(off_root), "manifestStatus": off.get("status"), "variant": off.get("variant")},
        "on": {"root": str(on_root), "manifestStatus": on.get("status"), "variant": on.get("variant")},
        "validityGate": {
            "status": runtime_status,
            "bothManifestsPass": validity["off"] and validity["on"],
            "offRejectTelemetry": validity["offRejectTelemetry"],
            "onRejectTelemetry": validity["onRejectTelemetry"],
            "allRequiredSnapshots": validity["snapshots"],
            "rawSourceContractPass": raw_ok,
            "alphaSemanticInvariantPass": alpha_ok,
            "noFabricatedPass": True,
        },
        "alphaSemanticComparison": alpha_records,
        "rawSourceFrames": frame_records,
        "qualityGate": {
            "target": target,
            "offLocalVarianceFrame96": off_var,
            "onLocalVarianceFrame96": on_var,
            "offTailRMSE32to96": off_tail,
            "onTailRMSE32to96": on_tail,
            "qualityImproved": quality_improved,
            "rule": "on local variance < 0.99*off and 32->96 tail RMSE <= 1.01*off",
            "sourceMottleIsDiagnosticOnly": True,
        },
        "outputs": {"off": output_off, "on": output_on},
    }
    output_path = out_root / "source-quality-comparison.json"
    _write_json(output_path, report)
    print("SCREENPROBE_SOURCE_QUALITY_COMPARE", report["status"], output_path)
    return 0 if report["status"] == "PASS" else 2


def _screen_probe_stats(graph) -> tuple[dict, str | None]:
    try:
        raw = dict(graph.getPass("LumenGI").screenProbeStats)
    except Exception as error:
        return {}, "screenProbeStats unavailable: %s" % error
    stats = {str(key): _json_safe(value) for key, value in raw.items()}
    missing = [key for key in REQUIRED_REJECT_TELEMETRY + REQUIRED_CACHE_TELEMETRY if key not in stats]
    if not missing:
        try:
            attempts = float(stats["cacheLookupAttempts"])
            hits = float(stats["cacheLookupHits"])
            coverage_rejects = float(stats["cacheCoverageRejects"])
            if math.isfinite(attempts) and attempts > 0.0:
                stats["cacheHitRate"] = hits / attempts
                stats["cacheCoverageRejectRate"] = coverage_rejects / attempts
            else:
                stats["cacheHitRate"] = 0.0
                stats["cacheCoverageRejectRate"] = 0.0
        except (TypeError, ValueError, ZeroDivisionError):
            return stats, "cache telemetry is non-numeric"
    return stats, ("missing telemetry: %s" % ", ".join(missing) if missing else None)


def _manifest_telemetry_ok(manifest: dict) -> bool:
    captures = manifest.get("captures")
    if not isinstance(captures, list) or len(captures) != len(CHECKPOINTS):
        return False
    for capture in captures:
        stats = capture.get("screenProbeStats")
        if not isinstance(stats, dict):
            return False
        if any(key not in stats for key in REQUIRED_REJECT_TELEMETRY + REQUIRED_CACHE_TELEMETRY):
            return False
    return True


def _graph():
    graph = RenderGraph("LumenGI.ScreenProbeSourceQuality")
    graph.addPass(createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}), "GBufferRT")
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "enabled": True,
                "useSurfaceCache": True,
                "useCacheLighting": True,
                "useScreenTrace": True,
                "useScreenProbes": True,
                # UE's default quality reference is eight directions.  The old
                # hard-coded 32 made this diagnostic graph allocate/compile an
                # unnecessarily large hit-record workload and could stall the
                # first Mogwai dispatch before any manifest was written.
                "probeDirectionsPerProbe": PROBE_DIRECTIONS,
                "probeMaxProbesPerFrame": PROBE_MAX_PER_FRAME,
                "useTemporalFilter": True,
                "useScreenRadianceMoments": USE_SOURCE_MOMENTS,
                "useSpatialFilter": True,
                "spatialRadiusMin": 1.0,
                "spatialRadiusMax": 4.0,
                "spatialVarianceThresholdLow": 0.0,
                "spatialVarianceThresholdHigh": 0.20,
                "debugMode": "None",
            },
        ),
        "LumenGI",
    )
    for channel in ("vbuffer", "linearZ", "mvec", "mvecW", "normWRoughnessMaterialID", "viewW", "diffuseOpacity", "emissive"):
        graph.addEdge("GBufferRT." + channel, "LumenGI." + channel)
    graph.addPass(createPass("ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0}), "ToneMapperDisplay")
    for channel in (RAW_CHANNEL, "probeInterpolated", "temporalFiltered", "temporalConfidence", "spatialFiltered", "resolvedDiffuseGI", "screenRadianceHistoryAge", "screenRadianceHistoryValidity", "screenRadianceLightingGeneration"):
        graph.markOutput("LumenGI." + channel)
    graph.addEdge("LumenGI.resolvedDiffuseGI", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph


def _output_array(graph, channel: str) -> np.ndarray:
    resource = graph.get_output("LumenGI." + channel)
    if resource is None:
        raise RuntimeError("LumenGI.%s returned no resource" % channel)
    values = np.asarray(resource.to_numpy(), dtype=np.float64)
    if values.size == 0:
        raise RuntimeError("LumenGI.%s is empty" % channel)
    return values


def _setup_scene() -> None:
    m.loadScene(SCENE_PATH)
    m.resizeFrameBuffer(*RESOLUTION)
    m.scene.camera.position = float3(0.0, 0.28, 1.2)
    m.scene.camera.target = float3(0.0, 0.28, 0.0)
    m.scene.camera.up = float3(0.0, 1.0, 0.0)
    m.scene.camera.focalLength = 35.0
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0


def _initial_manifest() -> dict:
    return {
        "schema": SCHEMA,
        "runner": {
            "script": "tests/lumengi/run_screenprobe_source_quality.py",
            "recommendedMogwaiInvocation": RECOMMENDED_MOGWAI_INVOCATION,
            "phaseManifest": "written before scene/graph setup and updated at each blocking boundary",
        },
        "status": "BLOCKED",
        "variant": VARIANT,
        "sourceMomentsEnabled": USE_SOURCE_MOMENTS,
        "scene": SCENE_PATH,
        "resolution": list(RESOLUTION),
        "checkpointFrames": list(CHECKPOINTS),
        "rawSource": {
            "channel": RAW_CHANNEL,
            "format": "RGBA16F",
            "alphaSemantic": "hitDistanceMeters",
            "missSentinel": HIT_DISTANCE_MISS,
            "confidenceOrAgeInAlpha": False,
        },
        "rawHistoryValidity": {
            "channel": "screenRadianceHistoryValidity",
            "format": "R32Uint",
            "semantic": "1=valid raw history sample, 0=reset/miss",
            "separateFromHitDistanceAndAge": True,
        },
        # This wave never relabels a probe texture as final color.  The
        # production endpoints remain the resolved diffuse channel and the
        # separately-owned full-scene composite.
        "productionOutputs": {
            "resolvedDiffuseGI": "LumenGI.resolvedDiffuseGI",
            "fullColor": "external full-scene composite; not synthesized here",
        },
        "captures": [],
        "errors": [],
    }


def _run_mogwai() -> int:
    manifest = _initial_manifest()
    graph = None
    try:
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        # Write before scene/graph setup so an externally bounded run always
        # leaves an honest BLOCKED artifact instead of a missing manifest.
        _write_progress(manifest, "starting")
        print("SCREENPROBE_SOURCE_QUALITY_PHASE starting", flush=True)
        _write_progress(manifest, "loadScene")
        _setup_scene()
        _write_progress(manifest, "buildGraph")
        graph = _graph()
        _write_progress(manifest, "addGraph")
        m.addGraph(graph)
        _write_progress(manifest, "setActiveGraph")
        m.setActiveGraph(graph)
        previous = 0
        for frame in CHECKPOINTS:
            manifest["progressFrame"] = int(frame)
            _write_progress(manifest, "renderFrame:%d" % frame)
            for render_frame in range(previous + 1, frame + 1):
                m.clock.frame = render_frame
                m.renderFrame()
            _write_progress(manifest, "readback:%d" % frame)
            capture = {"frame": frame, "status": "PASS", "outputs": {}, "metrics": {}, "screenProbeStats": {}}
            for channel in (RAW_CHANNEL, "probeInterpolated", "temporalFiltered", "spatialFiltered", "resolvedDiffuseGI"):
                values = _output_array(graph, channel)
                np.save(OUT_DIR / ("%s-%s.npy" % (channel, frame)), values, allow_pickle=False)
                capture["outputs"][channel] = {
                    "shape": list(values.shape),
                    "finite": bool(np.isfinite(values).all()),
                    "nonnegative": bool(float(np.nanmin(values)) >= -1e-6),
                }
                capture["metrics"][channel] = _image_metrics(values)
                if channel == RAW_CHANNEL:
                    capture["rawContract"] = _raw_source_contract(values)
                    if capture["rawContract"]["status"] != "PASS":
                        capture["status"] = "BLOCKED"
            for channel in ("temporalConfidence", "screenRadianceHistoryAge", "screenRadianceHistoryValidity", "screenRadianceLightingGeneration"):
                values = _output_array(graph, channel)
                np.save(OUT_DIR / ("%s-%s.npy" % (channel, frame)), values, allow_pickle=False)
                if channel in ("screenRadianceHistoryAge", "screenRadianceHistoryValidity", "screenRadianceLightingGeneration"):
                    sidecar = _integer_output_contract(values, channel)
                    capture["outputs"][channel] = sidecar
                    if sidecar["status"] != "PASS":
                        capture["status"] = "BLOCKED"
                else:
                    capture["outputs"][channel] = {
                        "shape": list(values.shape),
                        "finite": bool(np.isfinite(values).all()),
                        "nonnegative": bool(float(np.nanmin(values)) >= 0.0),
                    }
            stats, stats_error = _screen_probe_stats(graph)
            capture["screenProbeStats"] = stats
            if stats_error:
                capture["status"] = "BLOCKED"
                capture["errors"] = [stats_error]
            manifest["captures"].append(capture)
            _write_progress(manifest, "captured:%d" % frame)
            previous = frame
    except Exception as error:
        manifest["errors"].append("runtime: %s" % error)
        manifest["traceback"] = traceback.format_exc()
        _write_progress(manifest, "error")
    finally:
        if graph is not None:
            _write_progress(manifest, "removeGraph")
            try:
                m.removeGraph(graph)
            except Exception as error:
                manifest["errors"].append("removeGraph: %s" % error)
    manifest["status"] = (
        "PASS"
        if not manifest["errors"]
        and len(manifest["captures"]) == len(CHECKPOINTS)
        and all(item.get("status") == "PASS" for item in manifest["captures"])
        else "BLOCKED"
    )
    manifest["phase"] = "complete"
    _write_json(MANIFEST_PATH, manifest)
    print("SCREENPROBE_SOURCE_QUALITY", manifest["status"], MANIFEST_PATH)
    return 0 if manifest["status"] == "PASS" else 2


def _write_external_blocked() -> int:
    payload = _initial_manifest()
    payload["errors"] = ["Falcor Python module unavailable: %s" % FALCOR_IMPORT_ERROR]
    _write_json(MANIFEST_PATH, payload)
    print("SCREENPROBE_SOURCE_QUALITY BLOCKED", MANIFEST_PATH)
    return 2


def _self_test() -> int:
    if np is None:
        print("SCREENPROBE_SOURCE_QUALITY_SELF_TEST BLOCKED", NUMPY_IMPORT_ERROR)
        return 2
    raw = np.zeros((2, 3, 4), dtype=np.float64)
    raw[..., :3] = 0.25
    raw[..., 3] = HIT_DISTANCE_MISS
    raw[0, 0, 3] = 1.25
    raw[1, 2, 3] = 0.0
    contract = _raw_source_contract(raw)
    assert contract["status"] == "PASS", contract
    bad = raw.copy()
    bad[0, 0, 3] = HIT_DISTANCE_MISS + 2.0
    assert _raw_source_contract(bad)["status"] == "FAIL"
    changed_rgb = raw.copy()
    changed_rgb[..., :3] *= 2.0
    assert _alpha_delta(raw, changed_rgb)["status"] == "PASS"
    changed_alpha = raw.copy()
    changed_alpha[0, 0, 3] = 2.0
    assert _alpha_delta(raw, changed_alpha)["status"] == "FAIL"
    metrics = _image_metrics(raw)
    assert metrics["status"] == "PASS" and math.isfinite(metrics["localVariance"])
    assert _integer_output_contract(np.asarray([[0.0, 2.0]]), "age")["status"] == "PASS"
    assert _integer_output_contract(np.asarray([[0.25]]), "age")["status"] == "FAIL"
    # The initialization handoff must be observable even if Mogwai is killed
    # while compiling the first graph.  Exercise the same atomic writer used
    # by _run_mogwai without touching the real artifact directory.
    global MANIFEST_PATH, _BOOTSTRAP_OUT_DIR, _BOOTSTRAP_MANIFEST_PATH
    saved_manifest_path = MANIFEST_PATH
    progress_scratch = _self_test_scratch("progress")
    try:
        MANIFEST_PATH = progress_scratch / "source-quality-manifest.json"
        progress = _initial_manifest()
        _write_progress(progress, "self-test")
        assert MANIFEST_PATH.is_file(), MANIFEST_PATH
        persisted = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        assert persisted["status"] == "BLOCKED"
        assert persisted["phase"] == "self-test"
        assert persisted["runner"]["recommendedMogwaiInvocation"].startswith("Mogwai.exe -s ")
    finally:
        MANIFEST_PATH = saved_manifest_path
        _cleanup_self_test_scratch(progress_scratch)
    saved_bootstrap_dir = _BOOTSTRAP_OUT_DIR
    saved_bootstrap_manifest = _BOOTSTRAP_MANIFEST_PATH
    bootstrap_scratch = _self_test_scratch("bootstrap")
    try:
        _BOOTSTRAP_OUT_DIR = bootstrap_scratch
        _BOOTSTRAP_MANIFEST_PATH = _BOOTSTRAP_OUT_DIR / "source-quality-manifest.json"
        _write_python_bootstrap_manifest()
        assert _BOOTSTRAP_MANIFEST_PATH.is_file(), _BOOTSTRAP_MANIFEST_PATH
        bootstrap = json.loads(_BOOTSTRAP_MANIFEST_PATH.read_text(encoding="utf-8"))
        assert bootstrap["status"] == "BLOCKED"
        assert bootstrap["phase"] == "python-bootstrap"
        assert bootstrap["env"]["scene"] == os.environ.get(
            "LUMEN_SOURCE_QUALITY_SCENE", DEFAULT_SCENE
        )
        assert bootstrap["env"]["resolution"] == os.environ.get(
            "LUMEN_SOURCE_QUALITY_RESOLUTION", RESOLUTION_TEXT
        )
        assert bootstrap["env"]["checkpoints"] == os.environ.get(
            "LUMEN_SOURCE_QUALITY_CHECKPOINTS", "1,8,32,96"
        )
    finally:
        _BOOTSTRAP_OUT_DIR = saved_bootstrap_dir
        _BOOTSTRAP_MANIFEST_PATH = saved_bootstrap_manifest
        _cleanup_self_test_scratch(bootstrap_scratch)
    if SCENE_PATH == DEFAULT_SCENE:
        assert Path(DEFAULT_SCENE).is_file(), DEFAULT_SCENE
    print("SCREENPROBE_SOURCE_QUALITY_SELF_TEST PASS")
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return _self_test()
    if "--compare" in sys.argv[1:]:
        return _compare_runs(COMPARE_OFF, COMPARE_ON, COMPARE_OUT)
    if not FALCOR_AVAILABLE or np is None:
        return _write_external_blocked()
    return _run_mogwai()


_write_python_bootstrap_manifest("runner-defined")

if __name__ == "__main__":
    raise SystemExit(main())
elif FALCOR_AVAILABLE and np is not None:
    # Mogwai may embed this script under a launcher-specific module name
    # instead of ``__main__``.  Keep the normal Python entrypoint above for
    # self-test/compare/external-blocked behavior, while allowing the runtime
    # graph to execute in that embedded case as the other Mogwai runners do.
    main()
    exit()
