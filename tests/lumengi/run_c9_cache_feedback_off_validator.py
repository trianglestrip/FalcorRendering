"""Validate C9 cache-lookup activity with Surface Cache feedback disabled.

This is an offline diagnostic validator.  It deliberately does not change or
replace the C9 pixel gate and it never modifies any threshold used by that
gate.  A C9 replay runtime JSON is expected to contain the two host telemetry
objects emitted by ``run_resolved_showcase.py``::

    screenProbeStats
    surfaceCacheStats

When ``disableSurfaceCacheFeedback`` is explicitly true in the replay config,
both telemetry objects must show non-zero cache lookup attempts and hits.  The
feedback/request counters owned by the Surface Cache demand-feedback path must
be present and exactly zero.  Missing configuration, sections, or counters is
``BLOCKED`` (insufficient evidence); a present non-zero forbidden counter or
missing lookup activity is ``FAIL``.  A complete, consistent record is
``PASS``.

Typical use::

    python -B tests/lumengi/run_c9_cache_feedback_off_validator.py \
      --input artifacts/lumengi/C9/.../replay/cornell-front/mark-on.json \
      --output artifacts/lumengi/C9/.../cache-feedback-off-check.json

Exit codes are stable for automation: 0 = PASS, 1 = FAIL, 2 = BLOCKED.
``--self-test`` runs dependency-free PASS/FAIL/BLOCKED fixtures and does not
touch the workspace.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any, Mapping


SCHEMA = "LumenGI.C9.CacheFeedbackOffValidator.v1"

# ``cacheLookupAttempts``/``cacheLookupHits`` are cumulative values in the
# current LumenGI bindings.  The ThisFrame aliases are accepted as a bounded
# compatibility fallback when an older/newer artifact omits the cumulative
# spelling; the selected key is recorded in the report.
LOOKUP_KEYS = {
    "attempts": ("cacheLookupAttempts", "cacheLookupAttemptsThisFrame"),
    "hits": ("cacheLookupHits", "cacheLookupHitsThisFrame"),
}

# These are the demand-feedback/request counters exported by
# LumenGIPass::getSurfaceCacheStats().  Scheduler capture counters and frame
# stamps are intentionally excluded: disabling GPU feedback must not imply
# that initial card capture or a scheduler timestamp is zero.
ZERO_COUNTER_FIELDS = (
    "cacheLightingFeedbackEnabled",
    "surfaceCacheFeedbackHits",
    "surfaceCacheFeedbackPages",
    "surfaceCacheFeedbackDedup",
    "surfaceCacheFeedbackStaleRejects",
    "surfaceCacheRequestRaw",
    "surfaceCacheRequestCards",
    "surfaceCacheRequestDedup",
    "surfaceCacheRequestStaleRejects",
    "surfaceCacheRequestCaptureCompleted",
    "requestRawThisFrame",
    "requestCardsThisFrame",
    "requestCaptureCompletedThisFrame",
    "surfaceCacheRequestReasonUnmapped",
    "surfaceCacheRequestReasonStaleOwner",
    "surfaceCacheRequestReasonMetadataInvalid",
    "surfaceCacheRequestReasonVisibilityInvalid",
)


class EvidenceBlocked(RuntimeError):
    """The artifact is missing or cannot support this diagnostic claim."""


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def _as_mapping(value: Any, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise EvidenceBlocked(f"{label} is missing or is not an object")
    return value


def _find_config(root: Mapping[str, Any]) -> tuple[Mapping[str, Any], str]:
    # The current C9 runtime places config under deterministicReplay.  Keep a
    # direct config fallback for small offline fixtures without accepting an
    # arbitrary nested object as authoritative.
    replay = root.get("deterministicReplay")
    if isinstance(replay, Mapping) and isinstance(replay.get("config"), Mapping):
        return replay["config"], "deterministicReplay.config"
    config = root.get("config")
    if isinstance(config, Mapping):
        return config, "config"
    raise EvidenceBlocked("replay config is missing")


def _lookup(section: Mapping[str, Any], kind: str, section_name: str) -> dict[str, Any]:
    keys = LOOKUP_KEYS[kind]
    missing: list[str] = []
    for key in keys:
        if key not in section:
            missing.append(key)
            continue
        value = section[key]
        if not _is_number(value):
            raise EvidenceBlocked(f"{section_name}.{key} is not a finite number")
        return {"key": key, "value": float(value), "missingAlternatives": missing}
    raise EvidenceBlocked(f"{section_name} is missing {kind} lookup counter ({' or '.join(keys)})")


def _lookup_report(section: Mapping[str, Any], section_name: str) -> dict[str, Any]:
    attempts = _lookup(section, "attempts", section_name)
    hits = _lookup(section, "hits", section_name)
    attempts_ok = attempts["value"] > 0.0
    hits_ok = hits["value"] > 0.0
    return {
        "status": "PASS" if attempts_ok and hits_ok else "FAIL",
        "attempts": attempts,
        "hits": hits,
        "attemptsPositive": attempts_ok,
        "hitsPositive": hits_ok,
        "reason": None
        if attempts_ok and hits_ok
        else f"{section_name} must observe attempts > 0 and hits > 0",
    }


def _zero_counter_report(section: Mapping[str, Any], section_name: str) -> dict[str, Any]:
    # Feedback/request counters are owned by surfaceCacheStats in the current
    # schema.  If a future artifact mirrors one of these fields into
    # screenProbeStats, validating it there as well is safer than ignoring a
    # contradictory value; absent fields in the non-authoritative section do
    # not make a valid current artifact BLOCKED.
    missing: list[str] = []
    invalid: list[str] = []
    nonzero: dict[str, float] = {}
    checked: list[str] = []
    for field in ZERO_COUNTER_FIELDS:
        if field not in section:
            missing.append(field)
            continue
        checked.append(field)
        value = section[field]
        if not _is_number(value):
            invalid.append(field)
            continue
        numeric = float(value)
        if numeric != 0.0:
            nonzero[field] = numeric
    if invalid:
        status = "BLOCKED"
        reason = f"{section_name} has non-numeric feedback/request counters"
    elif nonzero:
        status = "FAIL"
        reason = f"{section_name} has non-zero feedback/request counters"
    elif not checked:
        status = "BLOCKED"
        reason = f"{section_name} has no recognizable feedback/request counters"
    else:
        # Missing fields are not immediately a failure because the current
        # ABI has grown these telemetry fields over time.  The caller decides
        # whether the authoritative surfaceCacheStats object is complete.
        status = "PASS"
        reason = None
    return {
        "status": status,
        "requiredFields": list(ZERO_COUNTER_FIELDS),
        "checkedFields": checked,
        "missingFields": missing,
        "invalidFields": invalid,
        "nonZero": nonzero,
        "reason": reason,
    }


def evaluate_payload(root: Mapping[str, Any], *, input_path: str | None = None) -> dict[str, Any]:
    """Evaluate one C9 replay JSON and return a serializable report."""

    try:
        config, config_path = _find_config(root)
        if "disableSurfaceCacheFeedback" not in config:
            raise EvidenceBlocked("disableSurfaceCacheFeedback is absent from replay config")
        disabled = config["disableSurfaceCacheFeedback"]
        if not isinstance(disabled, bool):
            raise EvidenceBlocked("disableSurfaceCacheFeedback is not a boolean")
        if not disabled:
            raise EvidenceBlocked("diagnostic applies only when disableSurfaceCacheFeedback=true")

        screen = _as_mapping(root.get("screenProbeStats"), "screenProbeStats")
        surface = _as_mapping(root.get("surfaceCacheStats"), "surfaceCacheStats")
        screen_lookup = _lookup_report(screen, "screenProbeStats")
        surface_lookup = _lookup_report(surface, "surfaceCacheStats")

        # The authoritative zero-counter contract is surfaceCacheStats.  Also
        # report any mirrored fields in screenProbeStats without requiring the
        # mirror to exist.
        surface_zero = _zero_counter_report(surface, "surfaceCacheStats")
        screen_mirror_fields = {name: screen[name] for name in ZERO_COUNTER_FIELDS if name in screen}
        screen_zero = _zero_counter_report(screen_mirror_fields, "screenProbeStats")

        lookup_failures = [
            item for item in (screen_lookup, surface_lookup) if item["status"] != "PASS"
        ]
        nonzero = dict(surface_zero["nonZero"])
        nonzero.update({f"screenProbeStats.{name}": value for name, value in screen_zero["nonZero"].items()})
        # A present contradiction is a FAIL even when another counter is
        # missing.  Otherwise the report would hide a concrete violation as
        # merely incomplete evidence.  Missing/invalid-only records remain
        # BLOCKED below.
        if lookup_failures or nonzero:
            reasons: list[str] = []
            if lookup_failures:
                reasons.extend(item["reason"] for item in lookup_failures if item.get("reason"))
            if nonzero:
                reasons.append("feedback/request counters must all be zero")
            status = "FAIL"
            reason = "; ".join(reasons)
        elif surface_zero["invalidFields"]:
            status = "BLOCKED"
            reason = "surfaceCacheStats has non-numeric feedback/request counters"
        elif surface_zero["missingFields"]:
            status = "BLOCKED"
            reason = (
                "surfaceCacheStats is missing feedback/request counters: "
                + ", ".join(surface_zero["missingFields"])
            )
        else:
            status = "PASS"
            reason = "cache lookup activity is present and feedback/request counters are zero"

        return {
            "schema": SCHEMA,
            "status": status,
            "input": input_path,
            "config": {
                "path": config_path,
                "disableSurfaceCacheFeedback": True,
            },
            "lookup": {
                "screenProbeStats": screen_lookup,
                "surfaceCacheStats": surface_lookup,
            },
            "feedbackRequestCounters": {
                "status": "FAIL" if nonzero else "PASS",
                "surfaceCacheStats": surface_zero,
                "screenProbeStatsMirror": screen_zero,
            },
            "reason": reason,
        }
    except EvidenceBlocked as exc:
        return {
            "schema": SCHEMA,
            "status": "BLOCKED",
            "input": input_path,
            "reason": str(exc),
        }


def evaluate_file(path: Path) -> dict[str, Any]:
    """Load and evaluate a JSON file; unreadable evidence is BLOCKED."""

    try:
        if not path.exists():
            raise EvidenceBlocked("input artifact does not exist: " + str(path))
        try:
            root = json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:  # malformed JSON is missing usable evidence
            raise EvidenceBlocked(f"input artifact is not valid JSON: {exc}") from exc
        if not isinstance(root, Mapping):
            raise EvidenceBlocked("input JSON root must be an object")
        return evaluate_payload(root, input_path=str(path.resolve()))
    except EvidenceBlocked as exc:
        return {
            "schema": SCHEMA,
            "status": "BLOCKED",
            "input": str(path.resolve()),
            "reason": str(exc),
        }


def _fixture(*, lookup_attempts: float = 5.0, lookup_hits: float = 2.0, nonzero_field: str | None = None) -> dict[str, Any]:
    surface = {field: 0.0 for field in ZERO_COUNTER_FIELDS}
    if nonzero_field:
        surface[nonzero_field] = 1.0
    return {
        "deterministicReplay": {"config": {"disableSurfaceCacheFeedback": True}},
        "screenProbeStats": {
            "cacheLookupAttempts": lookup_attempts,
            "cacheLookupHits": lookup_hits,
        },
        "surfaceCacheStats": {
            "cacheLookupAttempts": lookup_attempts,
            "cacheLookupHits": lookup_hits,
            **surface,
        },
    }


def _self_test() -> int:
    passed = evaluate_payload(_fixture())
    assert passed["status"] == "PASS", passed

    zero_lookup = evaluate_payload(_fixture(lookup_attempts=0.0, lookup_hits=0.0))
    assert zero_lookup["status"] == "FAIL", zero_lookup

    forbidden_feedback = evaluate_payload(_fixture(nonzero_field="surfaceCacheFeedbackHits"))
    assert forbidden_feedback["status"] == "FAIL", forbidden_feedback

    missing_counter = _fixture()
    del missing_counter["surfaceCacheStats"]["surfaceCacheRequestRaw"]
    blocked_missing = evaluate_payload(missing_counter)
    assert blocked_missing["status"] == "BLOCKED", blocked_missing

    disabled_not_requested = _fixture()
    disabled_not_requested["deterministicReplay"]["config"]["disableSurfaceCacheFeedback"] = False
    blocked_flag = evaluate_payload(disabled_not_requested)
    assert blocked_flag["status"] == "BLOCKED", blocked_flag

    missing_sections = _fixture()
    del missing_sections["screenProbeStats"]
    blocked_sections = evaluate_payload(missing_sections)
    assert blocked_sections["status"] == "BLOCKED", blocked_sections

    # Verify the compatibility fallback does not accidentally treat booleans
    # as numeric counters.
    fallback = _fixture()
    for section in (fallback["screenProbeStats"], fallback["surfaceCacheStats"]):
        section["cacheLookupAttemptsThisFrame"] = section.pop("cacheLookupAttempts")
        section["cacheLookupHitsThisFrame"] = section.pop("cacheLookupHits")
    fallback_result = evaluate_payload(fallback)
    assert fallback_result["status"] == "PASS", fallback_result

    invalid_number = _fixture()
    invalid_number["surfaceCacheStats"]["surfaceCacheFeedbackHits"] = True
    blocked_invalid = evaluate_payload(invalid_number)
    assert blocked_invalid["status"] == "BLOCKED", blocked_invalid

    print("C9_CACHE_FEEDBACK_OFF_SELF_TEST PASS")
    return 0


def _write_report(path: Path, report: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, help="C9 replay runtime JSON")
    parser.add_argument("--output", type=Path, help="optional validator JSON artifact")
    parser.add_argument("--self-test", action="store_true", help="run dependency-free validator fixtures")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    if args.input is None:
        parser.error("--input is required unless --self-test is used")

    report = evaluate_file(args.input)
    if args.output:
        _write_report(args.output, report)
    print("C9_CACHE_FEEDBACK_OFF", report["status"], report.get("reason", ""))
    return {"PASS": 0, "FAIL": 1, "BLOCKED": 2}[report["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
