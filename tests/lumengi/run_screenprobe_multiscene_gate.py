"""Strict A2 multi-scene and no-noise evidence gate.

``run_screenprobe_dynamic_quality.py`` intentionally reports a bounded
per-case quality result.  A single scene with all five transition cases may
therefore be ``PASS`` while the production scope is still incomplete.  This
read-only gate is the stricter, release-facing aggregation layer:

* every supplied manifest is re-evaluated by the dynamic linear-buffer gate;
* every scene must have the complete case matrix and exact checkpoints
  ``[1, 8, 16, 32, 64]``;
* at least two distinct scene identifiers are required; and
* every scene needs an explicit no-noise sidecar whose basis is linear
  raw/resolved evidence.  A PNG can be recorded as provenance, but it can
  never satisfy this gate or be used as a quality measurement.

Missing scope evidence is ``OPEN``.  Malformed manifests, duplicate
scene/case identities, invalid sidecars, and blocked linear evidence are
``BLOCKED``.  No threshold is relaxed and no image is rendered or inferred
from display-space PNGs.

Typical invocation (after five cases have been captured for each scene)::

    python -B tests/lumengi/run_screenprobe_multiscene_gate.py \
        --manifest artifacts/lumengi/A2/scene-a/static.json \
        --manifest artifacts/lumengi/A2/scene-a/camera-cut.json \
        --manifest artifacts/lumengi/A2/scene-a/scene-reload.json \
        --manifest artifacts/lumengi/A2/scene-a/lighting.json \
        --manifest artifacts/lumengi/A2/scene-a/material.json \
        --manifest artifacts/lumengi/A2/scene-b/static.json \
        --manifest artifacts/lumengi/A2/scene-b/camera-cut.json \
        --manifest artifacts/lumengi/A2/scene-b/scene-reload.json \
        --manifest artifacts/lumengi/A2/scene-b/lighting.json \
        --manifest artifacts/lumengi/A2/scene-b/material.json \
        --no-noise-evidence artifacts/lumengi/A2/scene-a/no-noise.json \
        --no-noise-evidence artifacts/lumengi/A2/scene-b/no-noise.json \
        --output artifacts/lumengi/A2/multiscene-gate.json

The sidecar contract is deliberately explicit and does not claim to create
visual evidence by itself::

    {
      "schema": "LumenGI.ScreenProbeNoNoiseEvidence.v1",
      "status": "PASS",
      "scene": "test_scenes/cornell_box.pyscene",
      "checkpoints": [1, 8, 16, 32, 64],
      "basis": "raw/resolved linear",
      "displayPngUsed": false,
      "method": "independent linear source-quality review",
      "raw": {"paths": ["...diffuseRadianceHitDist...exr", "..."]},
      "resolved": {"paths": ["...resolvedDiffuseGI...exr", "..."]}
    }

The sidecar's ``status=PASS`` must come from an independent source-quality
assessment.  This gate validates its provenance contract and path existence;
it does not turn a declaration into a new image-quality threshold.
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


SCHEMA = "LumenGI.ScreenProbeMultiSceneGate.v1"
NOISE_SCHEMA = "LumenGI.ScreenProbeNoNoiseEvidence.v1"
INPUT_SCHEMA = "LumenGI.ScreenProbeDynamicHistory.v1"
QUALITY_SCHEMA = "LumenGI.ScreenProbeDynamicQuality.v1"
REQUIRED_CASE_MATRIX = (
    "static",
    "camera_cut",
    "scene_reload",
    "lighting_generation",
    "material_or_geometry",
)
REQUIRED_CHECKPOINTS = (1, 8, 16, 32, 64)


def _load_quality_module():
    """Load the sibling quality gate without importing Falcor or running GPU."""

    module_path = Path(__file__).with_name("run_screenprobe_dynamic_quality.py")
    spec = importlib.util.spec_from_file_location("_a2_dynamic_quality", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load dynamic quality gate: %s" % module_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def _read_json(path: Path) -> tuple[Mapping[str, Any] | None, str | None]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream)
    except Exception as error:
        return None, "%s: %s" % (path, error)
    if not isinstance(value, Mapping):
        return None, "%s is not a JSON object" % path
    return value, None


def _write_json(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def _expand_inputs(values: Iterable[str]) -> list[Path]:
    result: list[Path] = []
    for raw in values:
        path = Path(raw)
        if path.is_dir():
            path = path / "screenprobe-dynamic.json"
        result.append(path)
    unique: list[Path] = []
    seen: set[str] = set()
    for path in result:
        key = str(path.absolute()).lower()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def _resolve_sidecar_path(value: Any, sidecar_path: Path) -> Path | None:
    if not isinstance(value, str) or not value.strip():
        return None
    path = Path(value)
    if not path.is_absolute():
        path = sidecar_path.parent / path
    return path.absolute()


def _validate_no_noise_sidecar(path: Path) -> dict[str, Any]:
    """Validate explicit provenance, not a PNG-derived quality claim."""

    payload, error = _read_json(path)
    result: dict[str, Any] = {
        "path": str(path.absolute()),
        "status": "BLOCKED",
        "scene": None,
        "checkpoints": None,
        "basis": None,
        "displayPngUsed": None,
        "raw": None,
        "resolved": None,
        "errors": [],
    }
    if error or payload is None:
        result["errors"] = [error or "sidecar unavailable"]
        return result
    result["scene"] = payload.get("scene")
    result["checkpoints"] = payload.get("checkpoints")
    result["basis"] = payload.get("basis")
    result["displayPngUsed"] = payload.get("displayPngUsed")
    result["raw"] = payload.get("raw")
    result["resolved"] = payload.get("resolved")
    errors: list[str] = []
    if payload.get("schema") != NOISE_SCHEMA:
        errors.append("unexpected no-noise schema: %s" % payload.get("schema"))
    declared_status = str(payload.get("status", "")).upper()
    if declared_status not in {"PASS", "OPEN", "BLOCKED"}:
        errors.append("no-noise status must be PASS, OPEN, or BLOCKED")
    if not isinstance(payload.get("scene"), str) or not str(payload.get("scene")).strip():
        errors.append("no-noise scene is required")
    checkpoints = payload.get("checkpoints")
    parsed_checkpoints: list[int] = []
    if isinstance(checkpoints, list):
        try:
            parsed_checkpoints = [int(value) for value in checkpoints]
        except (TypeError, ValueError):
            parsed_checkpoints = []
    if parsed_checkpoints != list(REQUIRED_CHECKPOINTS):
        errors.append(
            "no-noise checkpoints must exactly match %s" % list(REQUIRED_CHECKPOINTS)
        )
    if payload.get("basis") != "raw/resolved linear":
        errors.append("basis must be exactly raw/resolved linear")
    if payload.get("displayPngUsed") is not False:
        errors.append("displayPngUsed must be false; PNG is provenance only")
    method = payload.get("method")
    if not isinstance(method, str) or not method.strip():
        errors.append("independent no-noise method is required")
    for key in ("raw", "resolved"):
        record = payload.get(key)
        paths_value = record.get("paths") if isinstance(record, Mapping) else None
        if not isinstance(paths_value, list) or len(paths_value) != len(REQUIRED_CHECKPOINTS):
            errors.append(
                "%s linear evidence must provide one path per checkpoint (%d)"
                % (key, len(REQUIRED_CHECKPOINTS))
            )
            continue
        for index, path_value in enumerate(paths_value):
            resolved = _resolve_sidecar_path(path_value, path)
            if resolved is None:
                errors.append("%s checkpoint %d linear evidence path is required" % (key, REQUIRED_CHECKPOINTS[index]))
            elif not resolved.is_file():
                errors.append("%s checkpoint %d linear evidence path does not exist: %s" % (key, REQUIRED_CHECKPOINTS[index], resolved))
            elif resolved.suffix.lower() in {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"}:
                errors.append("%s checkpoint %d evidence must be linear raw/resolved data, not display image: %s" % (key, REQUIRED_CHECKPOINTS[index], resolved))
    declared_metrics = payload.get("metrics")
    if declared_metrics is not None and not isinstance(declared_metrics, Mapping):
        errors.append("metrics must be an object when present")
    # A PASS sidecar is an externally assessed statement, but it must still
    # carry the contract above.  Missing/OPEN is intentionally not promoted.
    if errors:
        result["errors"] = errors
        result["status"] = "BLOCKED"
    elif declared_status == "PASS":
        result["status"] = "PASS"
    elif declared_status == "OPEN":
        result["status"] = "OPEN"
        result["errors"] = ["no-noise evidence is explicitly OPEN"]
    else:
        result["status"] = "BLOCKED"
        result["errors"] = ["no-noise evidence is explicitly BLOCKED"]
    return result


def _case_entry(path: Path, quality_module: Any) -> dict[str, Any]:
    """Run the existing exact checkpoint/raw-resolved gate for one manifest."""

    evaluated = quality_module._evaluate_manifest(path)
    scene = evaluated.get("scene")
    case = evaluated.get("case")
    return {
        "manifest": evaluated.get("manifest", str(path.absolute())),
        "scene": scene,
        "case": case,
        "status": evaluated.get("status", "BLOCKED"),
        "checkpointContract": evaluated.get("checkpointContract", {}),
        "validity": evaluated.get("validity", {}),
        "telemetry": evaluated.get("telemetry", {}),
        "quality": evaluated.get("quality", {}),
        "transitionDelta": evaluated.get("transitionDelta", {}),
    }


def evaluate(
    manifest_paths: Sequence[Path],
    sidecar_paths: Sequence[Path],
) -> dict[str, Any]:
    """Aggregate scene/case manifests under a strict production scope."""

    report: dict[str, Any] = {
        "schema": SCHEMA,
        "status": "BLOCKED",
        "productionPass": False,
        "inputCount": len(manifest_paths),
        "noNoiseInputCount": len(sidecar_paths),
        "cases": [],
        "scenes": [],
        "noNoiseEvidence": [],
        "scope": {
            "requiredCaseMatrix": list(REQUIRED_CASE_MATRIX),
            "requiredCheckpoints": list(REQUIRED_CHECKPOINTS),
            "multiSceneEvidenceStatus": "BLOCKED",
            "noNoiseEvidenceStatus": "BLOCKED",
            "displayPngQualityInference": "PROHIBITED",
        },
        "qualityRule": {
            "checkpoints": list(REQUIRED_CHECKPOINTS),
            "linearChannels": {
                "raw": "LumenGI.diffuseRadianceHitDist RGB",
                "resolved": "LumenGI.resolvedDiffuseGI RGB",
            },
            "varianceFactor": 0.99,
            "tailRMSEFactor": 1.01,
            "pngIsProvenanceOnly": True,
            "noThresholdRelaxation": True,
        },
        "errors": [],
    }
    errors: list[str] = []
    try:
        quality_module = _load_quality_module()
    except Exception as error:
        report["errors"] = ["dynamic quality gate unavailable: %s" % error]
        return report

    entries: list[dict[str, Any]] = []
    for path in manifest_paths:
        try:
            entries.append(_case_entry(path, quality_module))
        except Exception as error:
            entries.append(
                {
                    "manifest": str(path.absolute()),
                    "scene": None,
                    "case": None,
                    "status": "BLOCKED",
                    "errors": ["manifest evaluation failed: %s" % error],
                }
            )
    report["cases"] = entries

    identities: dict[tuple[str, str], dict[str, Any]] = {}
    scene_cases: dict[str, set[str]] = {}
    malformed_identity = False
    duplicate_identity = False
    for entry in entries:
        scene_value = entry.get("scene")
        case_value = entry.get("case")
        scene = str(scene_value).strip() if isinstance(scene_value, str) else ""
        case = str(case_value).strip().lower() if isinstance(case_value, str) else ""
        if not scene:
            errors.append("manifest has no scene identifier: %s" % entry.get("manifest"))
            malformed_identity = True
            continue
        if case not in REQUIRED_CASE_MATRIX:
            errors.append("unsupported or missing case for scene %s: %s" % (scene, case))
            malformed_identity = True
            continue
        identity = (scene, case)
        if identity in identities:
            errors.append("duplicate scene/case manifest: %s/%s" % identity)
            duplicate_identity = True
            continue
        identities[identity] = entry
        scene_cases.setdefault(scene, set()).add(case)

    scene_names = sorted(scene_cases)
    report["scenes"] = [
        {
            "scene": scene,
            "providedCases": sorted(scene_cases[scene]),
            "missingCases": [case for case in REQUIRED_CASE_MATRIX if case not in scene_cases[scene]],
            "caseMatrixComplete": all(case in scene_cases[scene] for case in REQUIRED_CASE_MATRIX),
        }
        for scene in scene_names
    ]
    report["scope"]["sceneCount"] = len(scene_names)
    report["scope"]["scenes"] = scene_names
    if len(scene_names) >= 2:
        report["scope"]["multiSceneEvidenceStatus"] = "PASS"
        report["scope"]["multiSceneEvidenceReason"] = "at least two distinct scene identifiers are present"
    else:
        report["scope"]["multiSceneEvidenceStatus"] = "OPEN"
        report["scope"]["multiSceneEvidenceReason"] = (
            "strict gate requires at least two distinct scene identifiers"
        )
        errors.append("multi-scene evidence requires at least two distinct scenes")
    incomplete = [
        scene
        for scene in scene_names
        if not all(case in scene_cases[scene] for case in REQUIRED_CASE_MATRIX)
    ]
    if incomplete:
        errors.extend("scene %s is missing required cases" % scene for scene in incomplete)

    noise_entries = [_validate_no_noise_sidecar(path) for path in sidecar_paths]
    report["noNoiseEvidence"] = noise_entries
    noise_by_scene: dict[str, list[dict[str, Any]]] = {}
    for entry in noise_entries:
        scene = entry.get("scene")
        if isinstance(scene, str) and scene.strip():
            noise_by_scene.setdefault(scene.strip(), []).append(entry)
    missing_noise = [scene for scene in scene_names if scene not in noise_by_scene]
    duplicate_noise = [scene for scene, values in noise_by_scene.items() if len(values) > 1]
    unknown_noise = [scene for scene in noise_by_scene if scene not in scene_names]
    invalid_noise = [
        scene
        for scene, values in noise_by_scene.items()
        if any(value.get("status") == "BLOCKED" for value in values)
    ]
    open_noise = [
        scene
        for scene, values in noise_by_scene.items()
        if any(value.get("status") == "OPEN" for value in values)
    ]
    report["scope"]["noNoiseEvidenceByScene"] = {
        scene: [value.get("status") for value in values]
        for scene, values in sorted(noise_by_scene.items())
    }
    malformed_noise = any(value.get("status") == "BLOCKED" for value in noise_entries)
    if malformed_noise:
        report["scope"]["noNoiseEvidenceStatus"] = "BLOCKED"
        errors.append("one or more no-noise sidecars are malformed or blocked")
    elif duplicate_noise:
        report["scope"]["noNoiseEvidenceStatus"] = "BLOCKED"
        errors.extend("duplicate no-noise evidence for scene %s" % scene for scene in duplicate_noise)
    elif unknown_noise:
        report["scope"]["noNoiseEvidenceStatus"] = "BLOCKED"
        errors.extend("no-noise evidence is not bound to an input scene: %s" % scene for scene in unknown_noise)
    elif invalid_noise:
        report["scope"]["noNoiseEvidenceStatus"] = "BLOCKED"
        errors.extend("invalid no-noise evidence for scene %s" % scene for scene in invalid_noise)
    elif missing_noise:
        report["scope"]["noNoiseEvidenceStatus"] = "OPEN"
        errors.extend("missing explicit no-noise evidence for scene %s" % scene for scene in missing_noise)
    elif open_noise:
        report["scope"]["noNoiseEvidenceStatus"] = "OPEN"
        errors.extend("no-noise evidence remains OPEN for scene %s" % scene for scene in open_noise)
    elif scene_names and all(scene in noise_by_scene for scene in scene_names):
        report["scope"]["noNoiseEvidenceStatus"] = "PASS"
    else:
        report["scope"]["noNoiseEvidenceStatus"] = "OPEN"

    blocked_cases = [entry for entry in entries if entry.get("status") == "BLOCKED"]
    if blocked_cases:
        errors.append("one or more dynamic manifests have BLOCKED linear/provenance evidence")
    if not entries:
        errors.append("no dynamic manifests were provided")
    report["errors"] = errors

    # BLOCKED is reserved for evidence that is malformed/unreadable or a
    # duplicate identity.  Scope absence and a valid NO_IMPROVEMENT result are
    # OPEN, so a future run can supply the missing evidence without changing
    # the frozen quality rule.
    hard_block = bool(
        blocked_cases
        or not entries
        or malformed_identity
        or duplicate_identity
        or malformed_noise
        or duplicate_noise
        or unknown_noise
        or invalid_noise
    )
    quality_open = any(entry.get("status") == "NO_IMPROVEMENT" for entry in entries)
    complete_scope = (
        len(scene_names) >= 2
        and not incomplete
        and report["scope"]["noNoiseEvidenceStatus"] == "PASS"
    )
    if hard_block:
        report["status"] = "BLOCKED"
    elif complete_scope and not errors and not quality_open:
        report["status"] = "PASS"
        report["productionPass"] = True
    else:
        report["status"] = "OPEN"
    return report


def _make_no_noise_sidecar(path: Path, scene: str, status: str = "PASS") -> Path:
    raw_paths: list[str] = []
    resolved_paths: list[str] = []
    for checkpoint in REQUIRED_CHECKPOINTS:
        raw = path.parent / (path.stem + "-raw-%04d.exr" % checkpoint)
        resolved = path.parent / (path.stem + "-resolved-%04d.exr" % checkpoint)
        raw.write_bytes(b"linear raw fixture")
        resolved.write_bytes(b"linear resolved fixture")
        raw_paths.append(str(raw))
        resolved_paths.append(str(resolved))
    payload = {
        "schema": NOISE_SCHEMA,
        "status": status,
        "scene": scene,
        "checkpoints": list(REQUIRED_CHECKPOINTS),
        "basis": "raw/resolved linear",
        "displayPngUsed": False,
        "method": "synthetic independent linear source-quality review",
        "raw": {"paths": raw_paths},
        "resolved": {"paths": resolved_paths},
    }
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def _make_scene_fixture(root: Path, scene: str) -> list[Path]:
    """Build five dynamic-quality fixtures without Falcor or a GPU."""

    quality_module = _load_quality_module()
    source = quality_module._make_fixture(root / "source", quality_improved=True)
    payload = json.loads(source.read_text(encoding="utf-8"))
    paths: list[Path] = []
    for case in REQUIRED_CASE_MATRIX:
        case_root = root / case
        case_root.mkdir(parents=True, exist_ok=True)
        case_payload = json.loads(json.dumps(payload))
        case_payload["case"] = case
        case_payload["scene"] = scene
        if case == "static":
            case_payload["baseline"] = {}
        else:
            # The fixture's baseline/first-post counters satisfy all dynamic
            # mutation contracts; static is the only case with no baseline.
            case_payload["baseline"] = payload["baseline"]
        # ``_make_fixture`` is a camera-cut fixture.  Adjust the first-post
        # telemetry and corroborating producer evidence when exercising the
        # other dynamic cases so the strict gate tests the real case matrix
        # rather than a mislabeled transition.
        if case == "lighting_generation":
            case_payload["captures"][0]["screenProbeStats"] = quality_module._fixture_stats(
                1.0, 2.0, 0.0
            )
            case_payload["transitionEvidence"] = {
                "telemetryReadable": True,
                "lightingGenerationChanged": True,
                "expectedMutationSatisfied": True,
            }
        elif case == "material_or_geometry":
            case_payload["transitionEvidence"] = {
                "telemetryReadable": True,
                "historyGenerationChanged": True,
                "lightingGenerationChanged": True,
                "expectedMutationSatisfied": True,
            }
        path = case_root / "screenprobe-dynamic.json"
        path.write_text(json.dumps(case_payload), encoding="utf-8")
        paths.append(path)
    return paths


def _run_self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="screenprobe-multiscene-gate-") as directory:
        root = Path(directory)
        scene_a = "synthetic/scene_a.pyscene"
        scene_b = "synthetic/scene_b.pyscene"
        manifests_a = _make_scene_fixture(root / "scene-a", scene_a)
        manifests_b = _make_scene_fixture(root / "scene-b", scene_b)
        all_manifests = manifests_a + manifests_b
        missing_scope = evaluate(manifests_a, [])
        assert missing_scope["status"] == "OPEN", missing_scope
        assert missing_scope["scope"]["multiSceneEvidenceStatus"] == "OPEN"
        assert missing_scope["scope"]["noNoiseEvidenceStatus"] == "OPEN"
        noise_a = _make_no_noise_sidecar(root / "scene-a" / "no-noise.json", scene_a)
        noise_b = _make_no_noise_sidecar(root / "scene-b" / "no-noise.json", scene_b)
        passed = evaluate(all_manifests, [noise_a, noise_b])
        assert passed["status"] == "PASS", passed
        assert passed["productionPass"] is True
        assert passed["scope"]["multiSceneEvidenceStatus"] == "PASS"
        assert passed["scope"]["noNoiseEvidenceStatus"] == "PASS"
        opened_noise = _make_no_noise_sidecar(root / "scene-b" / "no-noise-open.json", scene_b, "OPEN")
        opened = evaluate(all_manifests, [noise_a, opened_noise])
        assert opened["status"] == "OPEN", opened
        assert opened["scope"]["noNoiseEvidenceStatus"] == "OPEN"
        checkpoint_payload = json.loads(noise_a.read_text(encoding="utf-8"))
        checkpoint_payload.pop("checkpoints")
        checkpoint_sidecar = root / "scene-a" / "no-noise-missing-checkpoints.json"
        checkpoint_sidecar.write_text(json.dumps(checkpoint_payload), encoding="utf-8")
        checkpoint_blocked = evaluate(all_manifests, [checkpoint_sidecar, noise_b])
        assert checkpoint_blocked["status"] == "BLOCKED", checkpoint_blocked
        assert checkpoint_blocked["scope"]["noNoiseEvidenceStatus"] == "BLOCKED"
        blocked_payload = json.loads(noise_b.read_text(encoding="utf-8"))
        blocked_payload["basis"] = "display PNG"
        noise_b.write_text(json.dumps(blocked_payload), encoding="utf-8")
        blocked = evaluate(all_manifests, [noise_a, noise_b])
        assert blocked["status"] == "BLOCKED", blocked
        assert blocked["scope"]["noNoiseEvidenceStatus"] == "BLOCKED"
        duplicate = evaluate(all_manifests + [manifests_a[0]], [noise_a, noise_b])
        assert duplicate["status"] == "BLOCKED", duplicate
        assert any("duplicate scene/case" in error for error in duplicate["errors"])
    print("SCREENPROBE_MULTISCENE_GATE_SELF_TEST PASS")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", help="dynamic manifest paths or directories")
    parser.add_argument(
        "--manifest",
        "--input",
        dest="manifest_options",
        action="append",
        default=[],
        help="dynamic manifest path (repeat for every scene/case)",
    )
    parser.add_argument(
        "--no-noise-evidence",
        dest="no_noise_options",
        action="append",
        default=[],
        help="explicit linear raw/resolved no-noise sidecar (repeat per scene)",
    )
    parser.add_argument(
        "--output",
        default="artifacts/lumengi/A2/multiscene-gate/multiscene-gate.json",
        help="strict aggregation report JSON path",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return _run_self_test()
    manifests = _expand_inputs(list(args.manifest_options) + list(args.paths))
    sidecars = _expand_inputs(args.no_noise_options)
    report = evaluate(manifests, sidecars)
    output = Path(args.output).absolute()
    _write_json(output, report)
    print("SCREENPROBE_MULTISCENE_GATE", report["status"], output)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
