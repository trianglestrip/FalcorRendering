"""Offline contract gate for the E1 roughSpecularIndirect diagnostic pass.

The gate is intentionally independent of the diffuseGI and RTXDI paths.  It
audits the *actual* Host-dispatched trace shader, its optional-output contract,
and its isolation from the C9 final resolve.  Its small deterministic model is
only a sanity check for the legacy directional-payload validity encoding; it is
not a numerical reference for the scene-ray-traced implementation.  No GPU
process is started.  A Slang compiler can be requested with ``--compile`` (or
``LUMEN_ROUGHSPEC_SLANGC``); unavailable compilers are reported as SKIP rather
than silently treated as a production integration.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Any, Dict, Iterable, Tuple


ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "Source/RenderPasses/LumenGI/RoughSpecular/LumenRoughSpecularData.slang"
SHADER = ROOT / "Source/RenderPasses/LumenGI/RoughSpecular/LumenRoughSpecularTrace.cs.slang"
HOST = ROOT / "Source/RenderPasses/LumenGI/LumenGI.cpp"
FINAL_RESOLVE = ROOT / "Source/RenderPasses/LumenGI/Resolve/LumenFinalResolve.cs.slang"
OUT = Path(os.environ.get("LUMEN_ROUGHSPEC_OUT", "artifacts/lumengi/E1/rough-specular-gate"))
SCHEMA = "LumenGI.E1.RoughSpecularIndirectGate.v2"

DISABLED = 1 << 0
DIRECTION = 1 << 1
RADIANCE = 1 << 2
OUTPUT = 1 << 3
SURFACE = 1 << 4
EPSILON = 1e-6


def _finite(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def _vec3(value: Iterable[float]) -> Tuple[float, float, float]:
    values = tuple(float(component) for component in value)
    if len(values) != 3:
        raise ValueError("expected a three-component vector")
    return values


def _length(value: Tuple[float, float, float]) -> float:
    return math.sqrt(sum(component * component for component in value))


def _normalize(value: Tuple[float, float, float]) -> Tuple[float, float, float]:
    length = _length(value)
    if length <= EPSILON:
        return (0.0, 0.0, 0.0)
    return tuple(component / length for component in value)


def _dot(a: Tuple[float, float, float], b: Tuple[float, float, float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def _saturate(value: float) -> float:
    return max(0.0, min(1.0, value))


def _evaluate(
    enabled: bool,
    directional: Tuple[Tuple[float, float, float], float] | None,
    reflection: Tuple[Tuple[float, float, float], float] | None,
    surface: Tuple[Tuple[float, float, float], float] | None,
    view: Tuple[Tuple[float, float, float], float] | None,
) -> Tuple[Tuple[float, float, float, float], int]:
    """Small scalar reference model matching the shader's validity contract."""

    if not enabled:
        return (0.0, 0.0, 0.0, 0.0), DISABLED
    if directional is None or reflection is None or surface is None or view is None:
        return (0.0, 0.0, 0.0, 0.0), DISABLED

    d, da = _vec3(directional[0]), float(directional[1])
    r, ra = _vec3(reflection[0]), float(reflection[1])
    n, roughness = _vec3(surface[0]), float(surface[1])
    v, _ = _vec3(view[0]), float(view[1])
    finite = all(_finite(x) for x in (*d, da, *r, ra, *n, roughness, *v))
    validity = 0
    if finite and _length(r) > EPSILON and ra > EPSILON:
        validity |= DIRECTION
    if finite and all(x >= 0.0 for x in d) and da > EPSILON:
        validity |= RADIANCE
    if finite and _length(n) > EPSILON and 0.0 <= roughness <= 1.0 and _length(v) > EPSILON:
        validity |= SURFACE
    required = DIRECTION | RADIANCE | SURFACE
    if (validity & required) != required:
        return (0.0, 0.0, 0.0, 0.0), validity | DISABLED

    normal = _normalize(n)
    view_dir = _normalize(v)
    reflection_dir = _normalize(r)
    # reflect(-view, normal) = -view - 2*dot(-view,n)*n.
    incident = tuple(-component for component in view_dir)
    ideal = tuple(incident[i] - 2.0 * _dot(incident, normal) * normal[i] for i in range(3))
    ideal = _normalize(ideal)
    alignment = _saturate(_dot(ideal, reflection_dir))
    exponent = 128.0 + (2.0 - 128.0) * (roughness * roughness)
    lobe = max(alignment, EPSILON) ** exponent
    no_v = _saturate(_dot(normal, view_dir))
    fresnel = 0.04 + (1.0 - 0.04) * ((1.0 - no_v) ** 5.0)
    confidence = _saturate(da * ra * lobe)
    normal_weight = _saturate(_dot(normal, reflection_dir))
    scale = lobe * fresnel * normal_weight
    output = tuple(min(max(component, 0.0) * scale, 10.0) for component in d)
    if not all(_finite(component) for component in (*output, confidence)):
        return (0.0, 0.0, 0.0, 0.0), validity | DISABLED
    return (*output, confidence), validity | OUTPUT


def _strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL)


def _method_body(source: str, method: str, next_method: str) -> str:
    """Return a deliberately narrow C++ method region for isolation checks."""
    start = source.find(method)
    end = source.find(next_method, start + 1)
    return source[start:end] if start >= 0 and end > start else ""


def _static_contract() -> Dict[str, Any]:
    data = DATA.read_text(encoding="utf-8")
    shader = SHADER.read_text(encoding="utf-8")
    host = HOST.read_text(encoding="utf-8")
    final_resolve = FINAL_RESOLVE.read_text(encoding="utf-8")
    code = _strip_comments(data + "\n" + shader)
    host_method = _method_body(host, "void LumenGIPass::runRoughSpecularDiagnostic", "void LumenGIPass::runTransmissionDiagnostic")
    final_resolve_method = _method_body(host, "void LumenGIPass::runFinalResolve", "void LumenGIPass::createRadianceCachePrograms")
    checks = {
        "filesPresent": DATA.exists() and SHADER.exists() and HOST.exists() and FINAL_RESOLVE.exists(),
        "hostDispatchesAuditedTrace": "RenderPasses/LumenGI/RoughSpecular/LumenRoughSpecularTrace.cs.slang" in host_method,
        "outputOptIn": "if ((!pOutput && !pValidity)" in host_method,
        "hostBindsGBufferAndRayTracingScene": all(
            token in host_method
            for token in ("gLinearZ", "gViewW", "gNormalRoughnessMaterialID", "bindShaderDataForRaytracing")
        ),
        "traceUsesBoundedSceneQueries": "SceneRayQuery<0>" in shader and "const uint sampleCount = clamp(gSampleCount, 1u, 8u)" in shader,
        "traceWritesSeparateOutputAndValidity": "gRoughSpecularIndirect" in code and "gRoughSpecularValidity" in code,
        "traceFiniteNonNegativeAndCap": all(
            token in shader for token in ("isfinite", "min(max(sum / validSamples", "gMaxRadiance")
        ),
        "traceHandlesUnavailableGBuffer": "kRoughValidityDisabled" in shader and "#if !(is_valid_gLinearZ" in shader,
        "legacyPayloadHasIndependentDirectionalInputs": all(
            token in code
            for token in (
                "gDirectionalRadiance",
                "gReflectionDirection",
                "gSurfaceNormalRoughness",
                "gViewDirection",
            )
        ),
        "traceDoesNotAliasDiffuseOrRtxdi": not any(
            token in _strip_comments(shader) for token in ("gDiffuseGI", "gDiffuseRadiance", "resolvedDiffuseGI", "RTXDI")
        ),
        "finalResolveDoesNotConsumeRoughSpecular": "roughSpecular" not in final_resolve_method
        and "roughSpecular" not in final_resolve,
    }
    return {"status": "PASS" if all(checks.values()) else "FAIL", "checks": checks}


def _offline_cases() -> Dict[str, Any]:
    aligned = ((1.5, 0.7, 0.2), 0.9)
    direction = ((0.0, 0.0, 1.0), 0.8)
    surface = ((0.0, 0.0, 1.0), 0.15)
    view = ((0.0, 0.0, 1.0), 1.0)
    cases = []
    payload, mask = _evaluate(False, aligned, direction, surface, view)
    cases.append({"name": "default-disabled", "pass": payload == (0.0, 0.0, 0.0, 0.0) and mask == DISABLED})
    payload, mask = _evaluate(True, None, None, None, None)
    cases.append({"name": "directionality-unavailable", "pass": payload == (0.0, 0.0, 0.0, 0.0) and (mask & DISABLED) != 0})
    payload, mask = _evaluate(True, aligned, direction, surface, view)
    cases.append({"name": "valid-directional-sample", "pass": (mask & (DIRECTION | RADIANCE | SURFACE | OUTPUT)) == (DIRECTION | RADIANCE | SURFACE | OUTPUT) and payload[0] > 0.0 and payload[3] > 0.0})
    invalid = ((1.5, 0.7, 0.2), 0.0)
    payload, mask = _evaluate(True, invalid, direction, surface, view)
    cases.append({"name": "zero-confidence-source", "pass": payload == (0.0, 0.0, 0.0, 0.0) and (mask & DISABLED) != 0})
    rough_surface = ((0.0, 0.0, 1.0), 1.0)
    payload, mask = _evaluate(True, aligned, direction, rough_surface, view)
    cases.append({"name": "roughness-one-finite", "pass": all(_finite(x) and x >= 0.0 for x in payload) and (mask & OUTPUT) != 0})
    return {"status": "PASS" if all(case["pass"] for case in cases) else "FAIL", "cases": cases}


def _compile_requested(path: Path, requested: bool) -> Dict[str, Any]:
    if not requested:
        return {"status": "SKIP", "reason": "compile not requested"}
    compiler = os.environ.get("LUMEN_ROUGHSPEC_SLANGC") or shutil.which("slangc")
    if not compiler:
        return {"status": "SKIP", "reason": "slangc not found"}
    OUT.mkdir(parents=True, exist_ok=True)
    output = OUT / "rough-specular.dxil"
    command = [
        compiler,
        "-target",
        "dxil",
        "-stage",
        "compute",
        "-entry",
        "main",
        "-I",
        str(SHADER.parent),
        "-o",
        str(output),
        str(SHADER),
    ]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
    return {
        "status": "PASS" if result.returncode == 0 else "FAIL",
        "command": command,
        "returnCode": result.returncode,
        "stdout": result.stdout[-4000:],
        "stderr": result.stderr[-4000:],
        "artifact": str(output) if result.returncode == 0 else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile", action="store_true", help="attempt an optional slangc compile")
    args = parser.parse_args()
    static = _static_contract()
    offline = _offline_cases()
    compile_result = _compile_requested(SHADER, args.compile or bool(os.environ.get("LUMEN_ROUGHSPEC_SLANGC")))
    report = {
        "schema": SCHEMA,
        "status": "PASS"
        if static["status"] == "PASS" and offline["status"] == "PASS" and compile_result["status"] != "FAIL"
        else "FAIL",
        "static": static,
        "offline": offline,
        "compile": compile_result,
        "contract": {
            "dispatch": "Host opt-in output allocates and dispatches LumenRoughSpecularTrace.cs.slang with GBuffer and ray-tracing scene bindings",
            "payload": "RGB bounded scene-ray-traced reflected indirect radiance, A sample confidence [0,1]",
            "validity": "explicit surface/hit/sky/finite/disabled/no-hit bits; black radiance remains legal",
            "integration": "diagnostic-only; no C9 final-resolve, diffuseGI or RTXDI direct-light aliasing",
            "productionStatus": "NOT_PRODUCTION: no rough-specular temporal history, denoiser, composition, or runtime quality/performance evidence",
        },
    }
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "rough-specular-gate.json"
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"E1_ROUGH_SPECULAR_GATE {report['status']} {path}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
