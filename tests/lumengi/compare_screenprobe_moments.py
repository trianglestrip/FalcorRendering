"""Compare identical ScreenProbe runs with source moments disabled/enabled.

The script deliberately treats runtime component PASS and image-quality improvement as
separate verdicts.  It reads the four serial checkpoints produced by
run_screenprobe_convergence.py and reports local luminance variance, temporal tail
framediff, p99/max, and same-frame off/on deltas.  No threshold is inferred from PNG
exposure; all quality metrics use the linear EXR RGB channels.
"""

from __future__ import annotations

import json
import math
import os
from pathlib import Path

import numpy as np
import OpenEXR


FRAMES = (1, 8, 32, 96)
CHANNELS = ("probeInterpolated", "temporalFiltered", "spatialFiltered", "resolvedDiffuseGI")
PREFIX = "screenprobe-cornell-front.LumenGI."


def _read_exr(path: Path) -> np.ndarray:
    import Imath

    handle = OpenEXR.InputFile(str(path))
    header = handle.header()
    window = header["dataWindow"]
    width = window.max.x - window.min.x + 1
    height = window.max.y - window.min.y + 1
    channels = []
    for name in ("R", "G", "B"):
        raw = handle.channel(name, Imath.PixelType(Imath.PixelType.FLOAT))
        channels.append(np.frombuffer(raw, dtype=np.float32).reshape(height, width))
    return np.stack(channels, axis=-1)


def _read_exr_without_imf(path: Path) -> np.ndarray:
    # OpenEXR's Python binding exposes Imf.PixelType in some builds and accepts a
    # string in others.  Keep the fallback local so the metric script remains usable
    # on both Falcor developer environments.
    handle = OpenEXR.InputFile(str(path))
    header = handle.header()
    window = header["dataWindow"]
    width = window.max.x - window.min.x + 1
    height = window.max.y - window.min.y + 1
    try:
        import Imath

        pixel_type = Imath.PixelType(Imath.PixelType.FLOAT)
        channels = [
            np.frombuffer(handle.channel(name, pixel_type), dtype=np.float32).reshape(height, width)
            for name in ("R", "G", "B")
        ]
    except Exception:
        channels = [
            np.frombuffer(handle.channel(name), dtype=np.float16).astype(np.float32).reshape(height, width)
            for name in ("R", "G", "B")
        ]
    return np.stack(channels, axis=-1)


def _load(path: Path) -> np.ndarray:
    try:
        import Imath

        return _read_exr(path)
    except Exception:
        return _read_exr_without_imf(path)


def _luma(image: np.ndarray) -> np.ndarray:
    return np.maximum(np.sum(image * np.array([0.2126, 0.7152, 0.0722], dtype=np.float32), axis=-1), 0.0)


def _finite_stats(image: np.ndarray) -> dict:
    lum = _luma(image)
    dx = lum[:, 1:] - lum[:, :-1]
    dy = lum[1:, :] - lum[:-1, :]
    local = 0.5 * (np.mean(dx * dx) + np.mean(dy * dy))
    return {
        "finite": bool(np.isfinite(image).all()),
        "nonnegative": bool((image >= -1e-6).all()),
        "mean": float(np.mean(lum)),
        "std": float(np.std(lum)),
        "p99": float(np.percentile(lum, 99.0)),
        "max": float(np.max(lum)),
        "localVariance": float(local),
    }


def _path(root: Path, channel: str, frame: int) -> Path:
    return root / f"{PREFIX}{channel}.{frame}.exr"


def _run_metrics(root: Path) -> tuple[dict, dict]:
    images: dict[tuple[str, int], np.ndarray] = {}
    metrics: dict = {}
    missing = []
    for channel in CHANNELS:
        metrics[channel] = {}
        for frame in FRAMES:
            path = _path(root, channel, frame)
            if not path.is_file():
                missing.append(str(path))
                continue
            image = _load(path)
            images[(channel, frame)] = image
            metrics[channel][str(frame)] = _finite_stats(image)
        tail = []
        for prev, frame in zip(FRAMES, FRAMES[1:]):
            if (channel, prev) in images and (channel, frame) in images:
                diff = _luma(images[(channel, frame)]) - _luma(images[(channel, prev)])
                tail.append({"from": prev, "to": frame, "rmse": float(np.sqrt(np.mean(diff * diff))), "meanAbs": float(np.mean(np.abs(diff)))})
        metrics[channel]["framediff"] = tail
    return metrics, images, missing


def main() -> int:
    off_root = Path(os.environ.get("LUMEN_SCREENPROBE_MOMENTS_OFF", "artifacts/lumengi/A2/screenradiance-moments-off-20260811-images"))
    on_root = Path(os.environ.get("LUMEN_SCREENPROBE_MOMENTS_ON", "artifacts/lumengi/A2/screenradiance-moments-on-20260811-images"))
    out_root = Path(os.environ.get("LUMEN_SCREENPROBE_MOMENTS_COMPARE_OUT", "artifacts/lumengi/A2/screenradiance-moments-compare-20260811"))
    out_root.mkdir(parents=True, exist_ok=True)

    off_metrics, off_images, off_missing = _run_metrics(off_root)
    on_metrics, on_images, on_missing = _run_metrics(on_root)
    deltas = {}
    for channel in CHANNELS:
        deltas[channel] = {}
        for frame in FRAMES:
            key = (channel, frame)
            if key not in off_images or key not in on_images:
                continue
            diff = on_images[key] - off_images[key]
            deltas[channel][str(frame)] = {
                "rmse": float(np.sqrt(np.mean(diff * diff))),
                "meanAbs": float(np.mean(np.abs(diff))),
                "p99Abs": float(np.percentile(np.abs(diff), 99.0)),
            }

    required_ok = not off_missing and not on_missing
    for run in (off_metrics, on_metrics):
        for channel in CHANNELS:
            for frame in FRAMES:
                item = run[channel].get(str(frame), {})
                required_ok = required_ok and item.get("finite", False) and item.get("nonnegative", False)

    # A quality improvement is intentionally stricter than runtime validity.  Either
    # spatial or resolved output must show a measurable local-variance reduction and
    # the 32->96 tail must not regress.  Otherwise the report stays NO_IMPROVEMENT.
    target = "spatialFiltered"
    off_var = off_metrics.get(target, {}).get("96", {}).get("localVariance", math.inf)
    on_var = on_metrics.get(target, {}).get("96", {}).get("localVariance", math.inf)
    off_tail = next((x["rmse"] for x in off_metrics.get(target, {}).get("framediff", []) if x["from"] == 32 and x["to"] == 96), math.inf)
    on_tail = next((x["rmse"] for x in on_metrics.get(target, {}).get("framediff", []) if x["from"] == 32 and x["to"] == 96), math.inf)
    quality_improved = bool(on_var < off_var * 0.99 and on_tail <= off_tail * 1.01)
    report = {
        "schema": "LumenGI.ScreenProbeMomentsComparison.v1",
        "status": "PASS" if required_ok and quality_improved else ("NO_IMPROVEMENT" if required_ok else "BLOCKED"),
        "off": {"root": str(off_root), "metrics": off_metrics, "missing": off_missing},
        "on": {"root": str(on_root), "metrics": on_metrics, "missing": on_missing},
        "sameFrameDeltas": deltas,
        "qualityGate": {
            "target": target,
            "offLocalVarianceFrame96": off_var,
            "onLocalVarianceFrame96": on_var,
            "offTailRMSE32to96": off_tail,
            "onTailRMSE32to96": on_tail,
            "qualityImproved": quality_improved,
            "rule": "on local variance < 0.99*off and tail RMSE <= 1.01*off",
        },
    }
    (out_root / "moments-comparison.json").write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    print("SCREENPROBE_MOMENTS_COMPARE", report["status"], out_root / "moments-comparison.json")
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
