"""LumenGI S9 full regression runner (SKELETON, Agent Z18).

Role / purpose
--------------
Host-side orchestrator for the S9 regression (task.md 15/17, gate in 18).
Runs the existing Mogwai GPU scripts back-to-back in dependency order and
aggregates every script's JSON report and logfile into
``artifacts/lumengi/S9/regression-manifest.json``.

THIS IS A SKELETON. It is written to be run by root on a GPU box (single
GPU, exclusive, Release build) and is NOT executed from this agent round.
Use ``--list`` to print the exact command checklist for manual / CI runs.

Scope (per task round)
----------------------
* reference   -> run_reference.py   (S1: fixed-frame Cornell EXR + stats)
* analytic    -> run_analytic.py    (S1: analytic light on/off black-scene)
* dynamic     -> run_dynamic.py     (S1: camera/light dynamic regression)
* stability   -> run_stability.py   (S3: black-room / white-furnace / emissive)
* lightstep   -> run_lightstep.py   (S3: light intensity step response)
* s2verify    -> run_s2verify.py    (S2: surface-cache on/off smoke)
* cards_coverage -> run_cards_coverage.py (S2/S3: card coverage series)
* screentrace -> run_screentrace.py (S4: screen-trace vs HWRT gate)
* probe       -> run_probe.py       (S4: probe distribution / hybrid)
* temporal    -> run_temporal.py    (S5: temporal trajectory / ghost proxy)
* spatial     -> run_spatial_gate.py(S5: spatial filter variance gate)

Command shape (each step, from the repo root)
---------------------------------------------
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\<script>.py ^
      --logfile artifacts\\lumengi\\S9\\<step>.log

JSON out override: scripts that emit a JSON report honour a LUMEN_*_OUT env
variable; the runner redirects each into artifacts/lumengi/S9/ so the S9 gate
is self-contained. Scripts without a JSON report (reference / analytic /
dynamic / s2verify) contribute their logfile + exit code + parsed VERDICT
lines to the manifest instead.

Manifest schema (task.md 17.2)
------------------------------
``regression-manifest.json`` records, per step: script, command, logfile,
json report path, exit code, duration, verdict lines, and an aggregated
``summary`` (pass/fail/skip) plus git/GPU/runtime context.
"""

from __future__ import print_function

import argparse
import json
import os
import platform
import subprocess
import sys
import time
from collections import OrderedDict

try:
    from pathlib import Path
except ImportError:  # Python 3.3-
    from pathlib2 import Path

try:
    import git  # optional, GitPython; degrades to a shell fallback
    HAS_GITPYTHON = True
except ImportError:
    HAS_GITPYTHON = False

SCHEMA_VERSION = 1
PHASE = "S9"

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
ARTIFACT_DIR = Path(os.environ.get("LUMEN_S9_ARTIFACTS", str(REPO_ROOT / "artifacts" / "lumengi" / "S9")))

MOGWAI = Path(os.environ.get(
    "LUMEN_MOGWAI",
    str(REPO_ROOT / "build" / "windows-vs2022" / "bin" / "Release" / "Mogwai.exe"),
))

# Ordered pipeline. Each entry:
#   id, script, phase, json_env (LUMEN_*_OUT override or None),
#   default_json (report path the script writes by default), log name.
# Order is intentional: S1 baselines first, then S2/S3 cache, then S4 trace/
# probe, then S5 temporal/spatial.
PIPELINE = [
    OrderedDict([
        ("id", "reference"),
        ("script", "run_reference.py"),
        ("phase", "S1"),
        ("json_env", None),
        ("json_default", None),
        ("note", "fixed-frame Cornell EXR + linear-HDR stats (no JSON report)"),
    ]),
    OrderedDict([
        ("id", "analytic"),
        ("script", "run_analytic.py"),
        ("phase", "S1"),
        ("json_env", None),
        ("json_default", None),
        ("note", "analytic light on/off -> black-scene invariant (no JSON report)"),
    ]),
    OrderedDict([
        ("id", "dynamic"),
        ("script", "run_dynamic.py"),
        ("phase", "S1"),
        ("json_env", None),
        ("json_default", None),
        ("note", "camera/light dynamic regression (no JSON report)"),
    ]),
    OrderedDict([
        ("id", "stability"),
        ("script", "run_stability.py"),
        ("phase", "S3"),
        ("json_env", "LUMEN_STABILITY_OUT"),
        ("json_default", "artifacts/lumengi/S3/stability.json"),
        ("note", "black-room / white-furnace / emissive hard invariants"),
    ]),
    OrderedDict([
        ("id", "lightstep"),
        ("script", "run_lightstep.py"),
        ("phase", "S3"),
        ("json_env", "LUMEN_LIGHTSTEP_OUT"),
        ("json_default", "artifacts/lumengi/S3/lightstep.json"),
        ("note", "light intensity step response plateau"),
    ]),
    OrderedDict([
        ("id", "s2verify"),
        ("script", "run_s2verify.py"),
        ("phase", "S2"),
        ("json_env", None),
        ("json_default", None),
        ("note", "surface-cache on/off + reload + resize smoke (no JSON report)"),
    ]),
    OrderedDict([
        ("id", "cards_coverage"),
        ("script", "run_cards_coverage.py"),
        ("phase", "S2"),
        ("json_env", "LUMEN_COVERAGE_OUT"),
        ("json_default", "artifacts/lumengi/S2/cards-coverage.json"),
        ("note", "card coverage-vs-frame series"),
    ]),
    OrderedDict([
        ("id", "screentrace"),
        ("script", "run_screentrace.py"),
        ("phase", "S4"),
        ("json_env", "LUMEN_SCREENTRACE_OUT"),
        ("json_default", "artifacts/lumengi/S4/screentrace.json"),
        ("note", "screen-trace hit distance vs HWRT + miss-reason histogram"),
    ]),
    OrderedDict([
        ("id", "probe"),
        ("script", "run_probe.py"),
        ("phase", "S4"),
        ("json_env", "LUMEN_PROBE_OUT"),
        ("json_default", "artifacts/lumengi/S4/probe/probe.json"),
        ("note", "probe distribution / hybrid ray reduction"),
    ]),
    OrderedDict([
        ("id", "temporal"),
        ("script", "run_temporal.py"),
        ("phase", "S5"),
        ("json_env", "LUMEN_TEMPORAL_OUT"),
        ("json_default", "artifacts/lumengi/S5/temporal.json"),
        ("note", "temporal trajectory / ghost proxy"),
    ]),
    OrderedDict([
        ("id", "spatial"),
        ("script", "run_spatial_gate.py"),
        ("phase", "S5"),
        ("json_env", "LUMEN_SPATIAL_GATE_OUT"),
        ("json_default", "artifacts/lumengi/S5/gate/spatial-gate.json"),
        ("note", "spatial filter variance-guided gate"),
    ]),
]


def _json_safe(value):
    """Best-effort JSON-safe coercion for pybind / numpy / arbitrary objects."""
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value
    if isinstance(value, dict):
        return {str(k): _json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(v) for v in value]
    return str(value)


def git_context():
    """Return commit / dirty / branch context (GitPython or shell fallback)."""
    ctx = {"branch": None, "commit": None, "dirty": None}
    try:
        if HAS_GITPYTHON:
            repo = git.Repo(str(REPO_ROOT))
            ctx["branch"] = repo.active_branch.name
            ctx["commit"] = repo.head.commit.hexsha
            ctx["dirty"] = repo.is_dirty()
            return ctx
    except Exception as exc:  # pragma: no cover - defensive
        ctx["git_error"] = str(exc)
    try:
        proc = subprocess.Popen(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        out, _ = proc.communicate()
        ctx["commit"] = out.decode("utf-8").strip() or None
        proc = subprocess.Popen(
            ["git", "-C", str(REPO_ROOT), "status", "--porcelain"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        out, _ = proc.communicate()
        ctx["dirty"] = bool(out.strip())
    except Exception as exc:  # pragma: no cover - defensive
        ctx["git_error"] = str(exc)
    return ctx


def host_context():
    """GPU-adjacent host context (driver version lives in the per-step logs)."""
    return {
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor() or None,
    }


def build_command(step, logfile):
    """Return the Mogwai command line for one pipeline step."""
    return [
        str(MOGWAI),
        "--device-type", "d3d12",
        "--headless",
        "--precise",
        "--script", str(REPO_ROOT / "tests" / "lumengi" / step["script"]),
        "--logfile", str(logfile),
    ]


def step_json_path(step):
    """S9-local path the step's JSON report is redirected to (or None)."""
    if not step["json_env"]:
        return None
    return ARTIFACT_DIR / ("step-%s.json" % step["id"])


def run_step(step):
    """Run one step, capture log + JSON, return the manifest entry."""
    logfile = ARTIFACT_DIR / ("%s.log" % step["id"])
    json_path = step_json_path(step)
    entry = OrderedDict([
        ("id", step["id"]),
        ("script", step["script"]),
        ("phase", step["phase"]),
        ("command", None),
        ("logfile", str(logfile)),
        ("json_report", str(json_path) if json_path else None),
        ("exit_code", None),
        ("duration_s", None),
        ("verdicts", []),
        ("fatal_error", None),
    ])
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    cmd = build_command(step, logfile)
    entry["command"] = [os.fspath(p) for p in cmd]
    env = dict(os.environ)
    if json_path:
        env[step["json_env"]] = str(json_path)
    started = time.time()
    try:
        proc = subprocess.Popen(
            cmd, cwd=str(REPO_ROOT), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        log_lines = []
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", errors="replace").rstrip("\n")
            log_lines.append(line)
        proc.stdout.close()
        proc.wait()
        entry["exit_code"] = proc.returncode
        for line in log_lines:
            if "VERDICT" in line or "ERROR" in line or "SKIP" in line:
                entry["verdicts"].append(line.strip())
    except Exception as exc:  # pragma: no cover - defensive
        entry["fatal_error"] = str(exc)
        entry["exit_code"] = -1
    entry["duration_s"] = round(time.time() - started, 3)
    if json_path and json_path.exists():
        try:
            entry["json_summary"] = _json_safe(json.loads(json_path.read_text(encoding="utf-8")))
        except Exception as exc:
            entry["json_parse_error"] = str(exc)
    return entry


def summarize(steps):
    """Roll up per-step results into a phase-level summary."""
    total = len(steps)
    failed = [s for s in steps if s["exit_code"] not in (None, 0)]
    skipped = [s for s in steps if "SKIP" in " ".join(s["verdicts"]) and s["exit_code"] == 0]
    return {
        "total": total,
        "passed": total - len(failed),
        "failed": len(failed),
        "failed_steps": [s["id"] for s in failed],
        "skipped_report": [s["id"] for s in skipped],
        "scheme": "exit code 0 == pass; a SKIP verdict with exit 0 is a report-level skip",
    }


def write_manifest(entries, summary):
    manifest = OrderedDict([
        ("schema_version", SCHEMA_VERSION),
        ("phase", PHASE),
        ("date", time.strftime("%Y-%m-%d")),
        ("generator", "tests/lumengi/run_full_regression.py"),
        ("git", git_context()),
        ("host", host_context()),
        ("mogwai", os.fspath(MOGWAI)),
        ("artifacts_dir", os.fspath(ARTIFACT_DIR)),
        ("steps", entries),
        ("summary", summary),
    ])
    out = ARTIFACT_DIR / "regression-manifest.json"
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=2, sort_keys=False), encoding="utf-8")
    return out


def list_commands():
    """Print the copy-pasteable command checklist (no execution)."""
    print("S9 regression command checklist (run from the repo root, single exclusive GPU, Release):\n")
    for step in PIPELINE:
        logfile = ARTIFACT_DIR / ("%s.log" % step["id"])
        cmd = build_command(step, logfile)
        print("## %-14s (%s) %s" % (step["id"], step["phase"], step["note"]))
        print("  " + " ^\n  ".join(cmd) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description="LumenGI S9 full regression runner (skeleton).")
    parser.add_argument("--list", action="store_true", help="print the command checklist and exit")
    parser.add_argument("--run", action="store_true", help="execute the pipeline (GPU required)")
    args = parser.parse_args(argv)

    if args.list or not args.run:
        list_commands()
        if not args.run:
            print("S9 skeleton: nothing executed. Re-run with --run on a GPU box to execute the pipeline.")
            return 0
    if args.run:
        entries = [run_step(step) for step in PIPELINE]
        summary = summarize(entries)
        out = write_manifest(entries, summary)
        print("S9 manifest written:", os.fspath(out))
        print("summary:", json.dumps(summary))
        if summary["failed"]:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
