"""Offline gate for the E1 transmission/glass reference-only contract.

This gate does not start Mogwai and does not claim production transmission GI.
It verifies that the independent IOR/Fresnel/thickness/refraction/validity
inputs exist, that the actual Host-dispatched shader defaults to disabled and
reference-only, and that C9 final resolve does not consume the channel.  A
tiny reference model rejects missing directionality and total-internal-
reflection cases.
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
DATA = ROOT / "Source/RenderPasses/LumenGI/Transmission/LumenTransmissionData.slang"
SHADER = ROOT / "Source/RenderPasses/LumenGI/Transmission/LumenTransmissionDiagnostic.cs.slang"
HOST = ROOT / "Source/RenderPasses/LumenGI/LumenGI.cpp"
FINAL_RESOLVE = ROOT / "Source/RenderPasses/LumenGI/Resolve/LumenFinalResolve.cs.slang"
OUT = Path(os.environ.get("LUMEN_TRANSMISSION_OUT", "artifacts/lumengi/E1/transmission-gate"))
SCHEMA = "LumenGI.E1.TransmissionReferenceGate.v2"

DISABLED = 1 << 0
REFERENCE_ONLY = 1 << 1
RADIANCE = 1 << 2
IOR = 1 << 3
FRESNEL = 1 << 4
REFRACTION = 1 << 5
THICKNESS = 1 << 6
OUTPUT = 1 << 7
TIR = 1 << 8
SURFACE = 1 << 9
EPSILON = 1e-6


def _finite(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def _vec3(value: Iterable[float]) -> Tuple[float, float, float]:
    result = tuple(float(component) for component in value)
    if len(result) != 3:
        raise ValueError("expected vec3")
    return result


def _length(value: Tuple[float, float, float]) -> float:
    return math.sqrt(sum(component * component for component in value))


def _normalize(value: Tuple[float, float, float]) -> Tuple[float, float, float]:
    length = _length(value)
    return tuple(component / length for component in value) if length > EPSILON else (0.0, 0.0, 0.0)


def _dot(a: Tuple[float, float, float], b: Tuple[float, float, float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def _evaluate(enabled: bool, source: Tuple[Tuple[float, float, float], float] | None,
              surface: Tuple[Tuple[float, float, float], float] | None,
              ior: Tuple[float, float, float], ior_confidence: float,
              fresnel: Tuple[float, float], fresnel_confidence: float,
              refraction: Tuple[Tuple[float, float, float], float] | None,
              validity_inputs: int) -> Tuple[Tuple[float, float, float, float], int]:
    if not enabled:
        return (0.0, 0.0, 0.0, 0.0), DISABLED
    if source is None or surface is None or refraction is None:
        return (0.0, 0.0, 0.0, 0.0), DISABLED | REFERENCE_ONLY
    rgb, source_conf = _vec3(source[0]), float(source[1])
    normal, roughness = _vec3(surface[0]), float(surface[1])
    refracted, refracted_conf = _vec3(refraction[0]), float(refraction[1])
    eta_ratio, thickness, _unused = (float(ior[0]), float(ior[1]), float(ior[2]))
    fresnel_value, _unused2 = (float(fresnel[0]), float(fresnel[1]))
    values = (*rgb, source_conf, *normal, roughness, *refracted, refracted_conf,
              eta_ratio, thickness, ior_confidence, fresnel_value, fresnel_confidence)
    if not all(_finite(value) for value in values):
        return (0.0, 0.0, 0.0, 0.0), DISABLED | REFERENCE_ONLY
    mask = REFERENCE_ONLY
    source_ok = all(value >= 0.0 for value in rgb) and source_conf > EPSILON and (validity_inputs & 1)
    surface_ok = _length(normal) > EPSILON and 0.0 <= roughness <= 1.0 and (validity_inputs & 2)
    ior_ok = 0.1 < eta_ratio < 4.0 and ior_confidence > EPSILON and (validity_inputs & 4)
    fresnel_ok = 0.0 <= fresnel_value <= 1.0 and fresnel_confidence > EPSILON and (validity_inputs & 8)
    refract_ok = _length(refracted) > EPSILON and refracted_conf > EPSILON and (validity_inputs & 16)
    if source_ok: mask |= RADIANCE
    if surface_ok: mask |= SURFACE
    if ior_ok: mask |= IOR
    if fresnel_ok: mask |= FRESNEL
    if refract_ok: mask |= REFRACTION
    if ior_ok and thickness >= 0.0: mask |= THICKNESS
    if not (source_ok and surface_ok and ior_ok and fresnel_ok and refract_ok and thickness >= 0.0):
        return (0.0, 0.0, 0.0, 0.0), mask | DISABLED
    n = _normalize(normal)
    direction = _normalize(refracted)
    cosine = _dot(n, direction)
    if cosine <= EPSILON:
        return (0.0, 0.0, 0.0, 0.0), mask | TIR | DISABLED
    attenuation = math.exp(-max(thickness, 0.0) * 0.1)
    scale = (1.0 - fresnel_value) * attenuation * max(0.0, min(1.0, cosine))
    output = tuple(min(max(value, 0.0) * scale, 10.0) for value in rgb)
    confidence = max(0.0, min(1.0, source_conf * refracted_conf * ior_confidence * fresnel_confidence))
    return (*output, confidence), mask | OUTPUT


def _method_body(source: str, method: str, next_method: str) -> str:
    start = source.find(method)
    end = source.find(next_method, start + 1)
    return source[start:end] if start >= 0 and end > start else ""


def _static_contract() -> Dict[str, Any]:
    data = DATA.read_text(encoding="utf-8")
    shader = SHADER.read_text(encoding="utf-8")
    host = HOST.read_text(encoding="utf-8")
    final_resolve = FINAL_RESOLVE.read_text(encoding="utf-8")
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", data + "\n" + shader, flags=re.DOTALL)
    host_method = _method_body(host, "void LumenGIPass::runTransmissionDiagnostic", "// =====================================================================================\n// S6:")
    final_resolve_method = _method_body(host, "void LumenGIPass::runFinalResolve", "void LumenGIPass::createRadianceCachePrograms")
    checks = {
        "filesPresent": DATA.exists() and SHADER.exists() and HOST.exists() and FINAL_RESOLVE.exists(),
        "hostDispatchesAuditedShader": "RenderPasses/LumenGI/Transmission/LumenTransmissionDiagnostic.cs.slang" in host_method,
        "outputOptIn": "if ((!pOutput && !pValidity)" in host_method,
        "hostForcesDisabledReferenceOnly": all(
            token in host_method for token in (
                '"LUMEN_GI_TRANSMISSION_ENABLED", "0"',
                '"LUMEN_GI_TRANSMISSION_REFERENCE_ONLY", "1"',
            )
        ),
        "defaultDisabled": bool(re.search(r"#define\s+LUMEN_GI_TRANSMISSION_ENABLED\s+0", data)),
        "referenceOnlyDefault": bool(re.search(r"#define\s+LUMEN_GI_TRANSMISSION_REFERENCE_ONLY\s+1", data)),
        "independentInputs": all(token in text for token in (
            "gTransmissionIOR", "gTransmissionFresnel", "gTransmissionRefraction",
            "gTransmissionSurface", "gTransmissionRadiance", "gTransmissionInputValidity")),
        "separateOutputs": "gTransmissionIndirect" in text and "gTransmissionValidityOutput" in text,
        "disabledPath": "kLumenTransmissionValidityDisabled" in text and "lumenTransmissionWriteZero" in text,
        "tirGuard": "kLumenTransmissionValidityTotalInternal" in text and "normalCosine <=" in text,
        "finiteGuard": "lumenTransmissionFinite3(outputRadiance)" in text,
        "noFinalColorOrDiffuseAlias": not any(token in text for token in (
            "finalColor", "gDiffuseGI", "resolvedDiffuseGI", "RTXDI")),
        "finalResolveDoesNotConsumeTransmission": "transmission" not in final_resolve_method.lower()
        and "transmission" not in final_resolve.lower(),
        "productionUnsupportedMarker": "reference-only" in (data + shader).lower(),
    }
    return {"status": "PASS" if all(checks.values()) else "FAIL", "checks": checks}


def _offline_cases() -> Dict[str, Any]:
    src = ((1.0, 0.8, 0.6), 0.9)
    surf = ((0.0, 0.0, 1.0), 0.15)
    ior = (1.5, 0.5, 0.0)
    fres = (0.04, 0.0)
    refr = ((0.0, 0.0, 1.0), 0.9)
    cases = []
    payload, mask = _evaluate(False, src, surf, ior, 0.9, fres, 0.9, refr, 31)
    cases.append({"name": "default-disabled", "pass": payload == (0.0, 0.0, 0.0, 0.0) and mask == DISABLED})
    payload, mask = _evaluate(True, None, surf, ior, 0.9, fres, 0.9, refr, 31)
    cases.append({"name": "directionality-unavailable", "pass": payload == (0.0, 0.0, 0.0, 0.0) and (mask & DISABLED) != 0})
    payload, mask = _evaluate(True, src, surf, ior, 0.9, fres, 0.9, refr, 31)
    cases.append({"name": "valid-reference-transmission", "pass": (mask & OUTPUT) != 0 and payload[0] > 0.0 and payload[3] > 0.0})
    tir_refr = ((0.0, 0.0, -1.0), 0.9)
    payload, mask = _evaluate(True, src, surf, ior, 0.9, fres, 0.9, tir_refr, 31)
    cases.append({"name": "total-internal-reflection-rejected", "pass": payload == (0.0, 0.0, 0.0, 0.0) and (mask & TIR) != 0 and (mask & DISABLED) != 0})
    negative_thickness = (1.5, -0.1, 0.0)
    payload, mask = _evaluate(True, src, surf, negative_thickness, 0.9, fres, 0.9, refr, 31)
    cases.append({"name": "negative-thickness-rejected", "pass": payload == (0.0, 0.0, 0.0, 0.0) and (mask & DISABLED) != 0})
    return {"status": "PASS" if all(case["pass"] for case in cases) else "FAIL", "cases": cases}


def _optional_compile(requested: bool) -> Dict[str, Any]:
    if not requested:
        return {"status": "SKIP", "reason": "compile not requested"}
    compiler = os.environ.get("LUMEN_TRANSMISSION_SLANGC") or shutil.which("slangc")
    if not compiler:
        return {"status": "SKIP", "reason": "slangc not found"}
    OUT.mkdir(parents=True, exist_ok=True)
    output = OUT / "transmission.dxil"
    command = [compiler, "-target", "dxil", "-stage", "compute", "-entry", "main", "-I", str(SHADER.parent), "-o", str(output), str(SHADER)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
    return {"status": "PASS" if result.returncode == 0 else "FAIL", "command": command, "returnCode": result.returncode,
            "stdout": result.stdout[-4000:], "stderr": result.stderr[-4000:], "artifact": str(output) if result.returncode == 0 else None}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile", action="store_true")
    args = parser.parse_args()
    static = _static_contract()
    offline = _offline_cases()
    compile_result = _optional_compile(args.compile or bool(os.environ.get("LUMEN_TRANSMISSION_SLANGC")))
    report = {"schema": SCHEMA,
              "status": "PASS" if static["status"] == "PASS" and offline["status"] == "PASS" and compile_result["status"] != "FAIL" else "FAIL",
              "static": static, "offline": offline, "compile": compile_result,
              "productionStatus": "UNSUPPORTED_REFERENCE_ONLY",
              "contract": {"default": "disabled; zero output when disabled or inputs unavailable",
                           "medium": "IOR ratio + thickness + refraction direction + input validity are required",
                           "fresnel": "precomputed Fresnel [0,1] is consumed without inventing a missing medium transition",
                           "tir": "total-internal-reflection/back-facing refraction is rejected with explicit validity bit",
                           "integration": "Host-dispatched opt-in diagnostic only; no finalColor, diffuseGI or RTXDI integration"}}
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "transmission-gate.json"
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"E1_TRANSMISSION_GATE {report['status']} {path}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
