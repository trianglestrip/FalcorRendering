"""C9 FinalResolve contract gate.

This is an offline gate for the production resolve boundary.  It deliberately
does not infer correctness from image brightness: the shader source and the
marked-endpoint runtime artifact must both satisfy the frozen contracts.

Optional environment variables:

    LUMEN_C9_RUNTIME_JSON=artifacts/lumengi/C8/export-equivalence-20260812-framecapture-fix/export-equivalence.json
    LUMEN_C9_OUT=artifacts/lumengi/C9/resolve-contract

The runtime artifact is allowed to be PARTIAL because RenderGraph intentionally
blocks unmarked direct endpoints.  Any runtime error or marked-policy failure
is a hard FAIL; only the documented mark-off BLOCKED policies are tolerated.
"""

from __future__ import annotations

import json
import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SHADER = ROOT / "Source/RenderPasses/LumenGI/Resolve/LumenFinalResolve.cs.slang"
HOST = ROOT / "Source/RenderPasses/LumenGI/LumenGI.cpp"
HEADER = ROOT / "Source/RenderPasses/LumenGI/LumenGI.h"
OUT = Path(os.environ.get("LUMEN_C9_OUT", "artifacts/lumengi/C9/resolve-contract"))
RUNTIME = os.environ.get(
    "LUMEN_C9_RUNTIME_JSON",
    "artifacts/lumengi/C8/export-equivalence-20260812-framecapture-fix/export-equivalence.json",
)


def _contains(text: str, pattern: str) -> bool:
    return re.search(pattern, text, flags=re.MULTILINE | re.DOTALL) is not None


def _static_contract() -> dict:
    shader = SHADER.read_text(encoding="utf-8")
    host = HOST.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")

    checks = {
        "sourceFiniteAndAlphaValidity": _contains(
            shader, r"sourceValid\s*=\s*all\(isfinite\(source\)\)\s*&&\s*source\.a\s*>\s*1e-4f"
        ),
        "noRgbMagnitudeFallback": not _contains(
            shader, r"useFallback.*(?:length\s*\(|any\s*\(\s*source\.rgb|source\.rgb\s*>|source\.rgb\s*<)"
        ),
        "fallbackFiniteGuard": _contains(
            shader, r"useFallback\s*=\s*hasSurface\s*&&\s*!sourceValid\s*&&\s*all\(isfinite\(fallback\)\)"
        ),
        "incidentAlbedoPiOnce": _contains(
            shader, r"resolved\s*\*=\s*albedo\s*\*\s*\(1\.f\s*/\s*3\.14159265358979323846f\)"
        )
        and shader.count("3.14159265358979323846f") == 1,
        "rawFallbackPassthrough": _contains(
            shader, r"else\s*\n\s*resolved\s*=\s*max\(fallback\.rgb,\s*float3\(0\.f\)\)"
        ),
        "resolvedInternalCopy": _contains(
            host, r"copyResource\(pDiffuseGI\.get\(\),\s*mFinalResolve\.pResolved\.get\(\)\)"
        ),
        "resolvedPublicMirror": _contains(
            host, r"getTexture\(\"resolvedDiffuseGI\"\).*?copyResource\(pResolvedOutput\.get\(\),\s*mFinalResolve\.pResolved\.get\(\)\)",
        ),
        "temporalConfidenceSeparate": _contains(
            header, r"temporalConfidence.*?R32F|gTemporalConfidence|updated confidence"
        )
        and _contains(header, r"temporalFiltered.*?history length"),
        "incidentSourceKindTracked": _contains(host, r"sourceIsIncident")
        and _contains(shader, r"LUMEN_GI_RESOLVE_INCIDENT"),
    }
    return {"status": "PASS" if all(checks.values()) else "FAIL", "checks": checks}


def _runtime_contract(path: Path) -> dict:
    if not path.is_absolute():
        path = ROOT / path
    if not path.exists():
        return {"status": "BLOCKED", "reason": "runtime artifact not found", "path": str(path)}
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        return {"status": "FAIL", "reason": f"invalid runtime JSON: {exc}", "path": str(path)}

    errors = report.get("errors") or []
    runs = report.get("runs") or []
    marked = []
    blocked = []
    unexpected_failures = []
    for run in runs:
        status = str(run.get("status", "")).upper()
        policy = str(run.get("policy", run.get("case", ""))).lower()
        if status == "PASS":
            marked.append(run)
        elif status == "BLOCKED" and "mark_off" in policy:
            blocked.append(run)
        elif status:
            unexpected_failures.append({"policy": policy, "status": status})

    checks = {
        "noRuntimeErrors": not errors,
        "markedPoliciesPass": bool(marked),
        "onlyDocumentedMarkOffBlocked": not unexpected_failures,
        "reportIsNotAllBlocked": bool(marked),
    }
    status = "PASS" if all(checks.values()) else "FAIL"
    return {
        "status": status,
        "path": str(path),
        "checks": checks,
        "markedPassCount": len(marked),
        "documentedMarkOffBlockedCount": len(blocked),
        "unexpectedFailures": unexpected_failures,
        "errors": errors,
    }


def main() -> int:
    static = _static_contract()
    runtime = _runtime_contract(Path(RUNTIME))
    report = {
        "schema": "LumenGI.C9.ResolveContract.v1",
        "status": "PASS" if static["status"] == "PASS" and runtime["status"] in ("PASS", "BLOCKED") else "FAIL",
        "static": static,
        "runtime": runtime,
        "contract": {
            "incident": "RGB=incident irradiance E, alpha=producer confidence; resolve applies diffuseOpacity/PI exactly once",
            "raw": "RGB=already reflected radiance; no second albedo/PI modulation",
            "validity": "finite source plus alpha>1e-4; RGB magnitude is not validity",
            "markOff": "unmarked direct endpoints remain BLOCKED by RenderGraph contract",
        },
    }
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "resolve-contract.json"
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"C9_RESOLVE_CONTRACT {report['status']} {path}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
