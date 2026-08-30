"""Launch the two S2 release churn phases with an auditable run manifest.

This launcher is intentionally separate from both ``run_churn_short.py`` and
``run_release_soak_gate.py``.  It owns process orchestration and provenance
collection only; it never turns a proxy into a release PASS and it does not
replace the offline gate's no-growth checks.

The runtime contract is deliberately strict:

* one explicitly selected GPU (``--gpu-index``), queried through nvidia-smi;
* a dynamic phase of at least 30 minutes and a second dynamic soak of at least
  two hours, both at the requested 60 Hz frame count;
* unique output directory, separate phase logs and child JSON artifacts;
* exact Mogwai ``--script`` invocation, process exit/timeout metadata and
  launcher-side authoritative VRAM samples;
* missing authoritative renderer/device provenance, GPU-wide VRAM, logs or
  duration evidence is ``BLOCKED``.

No GPU work is performed by ``--self-test``.  A normal invocation is expected
to be run from the repository root, for example::

    python -B tests/lumengi/run_release_soak_launcher.py \
      --gpu-index 0 \
      --dynamic-minutes 30 \
      --soak-hours 2 \
      --output-root artifacts/lumengi/release/soak-launch-20260820-rtx0 \
      --mogwai build/windows-vs2022/bin/Release/Mogwai.exe \
      --script tests/lumengi/run_churn_short.py

The resulting ``launcher-manifest.json`` is input/provenance evidence for
``run_release_soak_gate.py``; ``READY_FOR_OFFLINE_GATE`` is not release PASS.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import time
from collections.abc import Mapping
from pathlib import Path


SCHEMA_VERSION = "S2-release-soak-launcher-v2"
FRAME_RATE = 60.0
MIN_DYNAMIC_MINUTES = 30.0
MIN_SOAK_HOURS = 2.0
DEFAULT_MOGWAI = Path("build/windows-vs2022/bin/Release/Mogwai.exe")
DEFAULT_SCRIPT = Path("tests/lumengi/run_churn_short.py")
DEFAULT_VRAM_SAMPLE_SECONDS = 5.0
DEFAULT_TIMEOUT_SLACK_SECONDS = 300.0
VRAM_QUERY = (
    "nvidia-smi --query-gpu=index,name,driver_version,memory.total,memory.used,memory.free "
    "--format=csv,noheader,nounits"
)


def _finite(value):
    if value is None or isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _positive(value):
    number = _finite(value)
    return number if number is not None and number > 0 else None


def _memory_bytes(value):
    number = _finite(value)
    if number is None or number < 0:
        return None
    # nvidia-smi emits MiB when nounits is used.
    return int(number * 1024.0 * 1024.0)


def _write_json(path, payload):
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(str(path) + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True, allow_nan=False)
        handle.write("\n")
    os.replace(temporary, path)


def _read_json(path):
    with Path(path).open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    return value if isinstance(value, Mapping) else None


def query_vram(gpu_index):
    """Return an authoritative nvidia-smi sample, or a blocked record."""
    result = {
        "source": "nvidia-smi",
        "query": VRAM_QUERY,
        "available": False,
        "authoritative": False,
        "status": "BLOCKED",
        "gpu_index": str(gpu_index),
        "gpu_name": None,
        "driver_version": None,
        "total_bytes": None,
        "used_bytes": None,
        "free_bytes": None,
        "reason": None,
    }
    try:
        completed = subprocess.run(
            [
                "nvidia-smi",
                "--query-gpu=index,name,driver_version,memory.total,memory.used,memory.free",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError((completed.stderr or "nvidia-smi failed").strip())
        selected = None
        for line in completed.stdout.splitlines():
            fields = [field.strip() for field in line.split(",")]
            if len(fields) >= 6 and fields[0] == str(gpu_index):
                selected = fields
                break
        if selected is None:
            raise RuntimeError("selected GPU index %s was not returned" % gpu_index)
        total = _memory_bytes(selected[3])
        used = _memory_bytes(selected[4])
        free = _memory_bytes(selected[5])
        if not selected[1] or not selected[2] or total is None or used is None or free is None:
            raise RuntimeError("nvidia-smi returned incomplete selected-GPU fields")
        result.update(
            {
                "available": True,
                "authoritative": True,
                "status": "PASS",
                "gpu_index": selected[0],
                "gpu_name": selected[1],
                "driver_version": selected[2],
                "total_bytes": total,
                "used_bytes": used,
                "free_bytes": free,
            }
        )
    except Exception as exc:
        result["reason"] = "authoritative GPU-wide VRAM unavailable: %s" % exc
    return result


def build_command(mogwai, script, log_path):
    """Build the only supported renderer invocation shape."""
    return [
        str(Path(mogwai).resolve()),
        "--device-type",
        "d3d12",
        "--headless",
        "--precise",
        "--script",
        str(Path(script).resolve()),
        "--logfile",
        str(Path(log_path).resolve()),
    ]


def validate_config(dynamic_minutes, soak_hours, gpu_index, sample_seconds):
    errors = []
    dynamic = _finite(dynamic_minutes)
    soak = _finite(soak_hours)
    if dynamic is None or dynamic < MIN_DYNAMIC_MINUTES:
        errors.append("dynamicMinutes must be >= %.1f" % MIN_DYNAMIC_MINUTES)
    if soak is None or soak < MIN_SOAK_HOURS:
        errors.append("soakHours must be >= %.1f" % MIN_SOAK_HOURS)
    if gpu_index is None or not str(gpu_index).strip():
        errors.append("one explicit gpu-index is required")
    if _positive(sample_seconds) is None:
        errors.append("VRAM sample interval must be positive")
    return errors


def _renderer_from_artifact(artifact):
    renderer = artifact.get("renderer")
    if isinstance(renderer, Mapping):
        return renderer
    telemetry = artifact.get("telemetry_provenance")
    if isinstance(telemetry, Mapping) and isinstance(telemetry.get("renderer"), Mapping):
        return telemetry["renderer"]
    return None


def _vram_from_artifact(artifact):
    vram = artifact.get("vram")
    if isinstance(vram, Mapping):
        return vram
    telemetry = artifact.get("telemetry_provenance")
    if isinstance(telemetry, Mapping) and isinstance(telemetry.get("vram"), Mapping):
        return telemetry["vram"]
    return None


def validate_child_artifact(artifact, expected_seconds, launcher_samples, selected_gpu=None):
    """Validate only prerequisites needed before the offline release gate."""
    blockers = []
    if not isinstance(artifact, Mapping):
        return "BLOCKED", ["child artifact is absent or not a JSON object"]
    seconds = _finite(artifact.get("seconds"))
    frames = _finite(artifact.get("frame_count"))
    frame_rate = _finite(artifact.get("frame_rate")) or FRAME_RATE
    if seconds is None or seconds < expected_seconds:
        blockers.append("child duration is below requested phase duration")
    if frames is None or frames < math.ceil(expected_seconds * frame_rate):
        blockers.append("child frame_count is below requested phase frame count")
    if str(artifact.get("mode") or "").lower() == "60s-proxy":
        blockers.append("child artifact is marked 60s-proxy")
    # The child runner deliberately reports OPEN for a full-duration run until
    # this external gate has consumed it.  It must never, however, smuggle a
    # known blocked/failed child through the launcher as READY.
    child_status = str(artifact.get("status") or "").upper()
    if child_status in ("BLOCKED", "FAIL"):
        blockers.append("child artifact status is %s" % child_status)
    if artifact.get("stats_complete") is not True:
        blockers.append("child stats_complete=true is missing")
    if artifact.get("material_toggle_available") is not True:
        blockers.append("material_toggle_available=true is missing")
    for key in ("dirty_injections", "reloads", "resizes"):
        value = _finite(artifact.get(key))
        if value is None or value <= 0:
            blockers.append("child dynamic field %s>0 is missing" % key)
    renderer = _renderer_from_artifact(artifact)
    if not isinstance(renderer, Mapping):
        blockers.append("authoritative renderer provenance is missing")
    else:
        for key in ("adapter_name", "api_name", "device_type"):
            if not str(renderer.get(key) or "").strip():
                blockers.append("renderer.%s is missing" % key)
        if renderer.get("available") is not True or renderer.get("authoritative") is not True:
            blockers.append("renderer.available/authoritative is not true")
        if str(renderer.get("source") or "").lower().startswith("environment"):
            blockers.append("renderer source is non-authoritative")
        if isinstance(selected_gpu, Mapping):
            selected_name = str(selected_gpu.get("gpu_name") or "").strip().casefold()
            renderer_name = str(renderer.get("adapter_name") or "").strip().casefold()
            if not selected_name or renderer_name != selected_name:
                blockers.append("renderer adapter does not match selected GPU identity")
    vram = _vram_from_artifact(artifact)
    if not isinstance(vram, Mapping):
        blockers.append("child GPU-wide VRAM record is missing")
    else:
        for label in ("start", "end"):
            record = vram.get(label)
            if not isinstance(record, Mapping) or record.get("authoritative") is not True:
                blockers.append("child VRAM %s snapshot is not authoritative" % label)
        samples = vram.get("samples") or artifact.get("vram_samples")
        if not isinstance(samples, list) or len(samples) < 3:
            blockers.append("child authoritative VRAM sample sequence is missing/short")
    if not isinstance(launcher_samples, list) or not launcher_samples:
        blockers.append("launcher GPU-wide VRAM samples are missing")
    elif any(
        not isinstance(sample, Mapping) or sample.get("authoritative") is not True
        for sample in launcher_samples
    ):
        blockers.append("launcher VRAM sample is non-authoritative")
    elif isinstance(selected_gpu, Mapping) and any(
        sample.get("gpu_index") != selected_gpu.get("gpu_index")
        or sample.get("gpu_name") != selected_gpu.get("gpu_name")
        for sample in launcher_samples
    ):
        blockers.append("launcher VRAM sample GPU identity changed")
    return ("BLOCKED" if blockers else "READY"), blockers


def _unique_output_root(value):
    if not value:
        raise ValueError("--output-root is required; use a new unique directory")
    path = Path(value).resolve()
    if path.exists():
        raise ValueError("output directory already exists; refusing to overwrite: %s" % path)
    path.mkdir(parents=True, exist_ok=False)
    return path


def _phase_result(role, expected_seconds, phase_dir, command, timeout_seconds=None):
    return {
        "role": role,
        "expected_seconds": expected_seconds,
        "output_json": str((phase_dir / "churn.json").resolve()),
        "logfile": str((phase_dir / "mogwai.log").resolve()),
        "stdout_logfile": str((phase_dir / "launcher-stdout.log").resolve()),
        "command": command,
        "timeout_seconds": timeout_seconds,
        "process": {
            "pid": None,
            "exit_code": None,
            "timed_out": False,
            "intentional_termination": False,
            "termination_reason": None,
            "started_at_unix": None,
            "finished_at_unix": None,
            "elapsed_seconds": None,
        },
        "log": {"exists": False, "bytes": 0},
        "launcher_vram_samples": [],
        "child_contract_status": "NOT_RUN",
        "blocking_reasons": [],
    }


def run_phase(
    role,
    expected_seconds,
    root,
    mogwai,
    script,
    gpu_index,
    selected_gpu,
    sample_seconds,
    timeout_seconds,
):
    phase_dir = root / role
    phase_dir.mkdir(parents=False, exist_ok=False)
    log_path = phase_dir / "mogwai.log"
    stdout_path = phase_dir / "launcher-stdout.log"
    output_path = phase_dir / "churn.json"
    command = build_command(mogwai, script, log_path)
    result = _phase_result(role, expected_seconds, phase_dir, command, timeout_seconds)
    env = os.environ.copy()
    env.update(
        {
            "LUMEN_CHURN_SECONDS": str(expected_seconds),
            "LUMEN_CHURN_OUT": str(output_path.resolve()),
            "LUMEN_CHURN_GPU_INDEX": str(gpu_index),
            "LUMEN_CHURN_VRAM_SAMPLE_INTERVAL_FRAMES": str(max(1, int(sample_seconds * FRAME_RATE))),
            # Long phases use a bounded mutation cadence.  This still exercises
            # material/reload/resize churn while avoiding an unbounded retained
            # allocation pattern from changing a material every rendered frame.
            "LUMEN_CHURN_MATERIAL_INTERVAL_FRAMES": os.environ.get("LUMEN_CHURN_MATERIAL_INTERVAL_FRAMES", "60"),
            "LUMEN_CHURN_RELOAD_INTERVAL_FRAMES": os.environ.get("LUMEN_CHURN_RELOAD_INTERVAL_FRAMES", "3600"),
            "LUMEN_CHURN_RESIZE_INTERVAL_FRAMES": os.environ.get("LUMEN_CHURN_RESIZE_INTERVAL_FRAMES", "5400"),
            "LUMEN_CHURN_PHASE_ID": role,
        }
    )
    samples = []
    started = time.time()
    monotonic_started = time.monotonic()
    result["process"]["started_at_unix"] = started
    try:
        # Mogwai owns ``--logfile``.  Keep launcher stdout separate so two
        # writers never race on the same evidence file.
        with stdout_path.open("w", encoding="utf-8", newline="\n") as process_log:
            process = subprocess.Popen(
                command,
                cwd=str(Path.cwd()),
                env=env,
                stdout=process_log,
                stderr=subprocess.STDOUT,
            )
            result["process"]["pid"] = process.pid
            while process.poll() is None:
                # Falcor scripts commonly write their JSON artifact and call
                # ``exit()`` without terminating the Mogwai host process.
                # Once the child has proved the complete requested duration,
                # terminate that idle host deliberately instead of waiting for
                # the launcher timeout.  This is not a release-pass shortcut:
                # child renderer provenance and all offline gates remain
                # mandatory, and the record makes the intentional termination
                # explicit for the gate.
                if output_path.exists():
                    try:
                        child_probe = _read_json(output_path)
                    except Exception:
                        child_probe = None
                    if isinstance(child_probe, Mapping):
                        child_seconds = _finite(child_probe.get("seconds"))
                        child_frames = _finite(child_probe.get("frame_count"))
                        if (
                            child_seconds is not None
                            and child_frames is not None
                            and child_seconds + 1e-6 >= float(expected_seconds)
                            and child_frames + 0.5 >= float(expected_seconds) * FRAME_RATE
                        ):
                            result["process"]["intentional_termination"] = True
                            result["process"]["termination_reason"] = "child_artifact_complete"
                            process.terminate()
                            try:
                                process.wait(timeout=30)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait(timeout=30)
                            break
                snapshot = query_vram(gpu_index)
                snapshot["timestamp_unix"] = time.time()
                samples.append(snapshot)
                elapsed = time.monotonic() - monotonic_started
                if elapsed >= timeout_seconds:
                    result["process"]["timed_out"] = True
                    process.terminate()
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=30)
                    break
                time.sleep(min(sample_seconds, max(0.1, timeout_seconds - elapsed)))
            if result["process"]["timed_out"]:
                result["process"]["exit_code"] = process.returncode
            else:
                result["process"]["exit_code"] = process.wait(timeout=30)
    except Exception as exc:
        result["blocking_reasons"].append("process launch/wait failed: %s" % exc)
    finished = time.time()
    result["process"]["finished_at_unix"] = finished
    result["process"]["elapsed_seconds"] = finished - started
    final_snapshot = query_vram(gpu_index)
    final_snapshot["timestamp_unix"] = time.time()
    samples.append(final_snapshot)
    result["launcher_vram_samples"] = samples
    result["selected_gpu"] = {
        "gpu_index": selected_gpu.get("gpu_index"),
        "gpu_name": selected_gpu.get("gpu_name"),
        "driver_version": selected_gpu.get("driver_version"),
    }
    result["log"] = {
        "exists": log_path.exists(),
        "bytes": log_path.stat().st_size if log_path.exists() else 0,
        "stdout_exists": stdout_path.exists(),
        "stdout_bytes": stdout_path.stat().st_size if stdout_path.exists() else 0,
    }
    if result["process"]["timed_out"]:
        result["blocking_reasons"].append("Mogwai phase timed out")
    if result["process"]["exit_code"] != 0 and not result["process"].get("intentional_termination"):
        result["blocking_reasons"].append("Mogwai exit code is not zero")
    if not result["log"]["exists"] or result["log"]["bytes"] <= 0:
        result["blocking_reasons"].append("Mogwai log is missing or empty")
    artifact = _read_json(output_path) if output_path.exists() else None
    child_status, child_reasons = validate_child_artifact(
        artifact,
        expected_seconds,
        samples,
        selected_gpu,
    )
    result["child_contract_status"] = child_status
    result["blocking_reasons"].extend(child_reasons)
    result["artifact_status"] = artifact.get("status") if isinstance(artifact, Mapping) else None
    result["status"] = "BLOCKED" if result["blocking_reasons"] else "READY_FOR_OFFLINE_GATE"
    return result


def build_manifest(config, root, phases, status):
    return {
        "schema_version": SCHEMA_VERSION,
        "status": status,
        "release_gate_status": "NOT_RUN",
        "output_root": str(root.resolve()),
        "requested": {
            "dynamic_minutes": config["dynamic_minutes"],
            "soak_hours": config["soak_hours"],
            "dynamic_seconds": config["dynamic_seconds"],
            "soak_seconds": config["soak_seconds"],
            "frame_rate_hz": FRAME_RATE,
            "gpu_index": str(config["gpu_index"]),
            "vram_sample_seconds": config["vram_sample_seconds"],
        },
        "contract": {
            "single_gpu": True,
            "require_authoritative_renderer_provenance": True,
            "require_authoritative_gpu_wide_vram": True,
            "require_dynamic_minutes_at_least": MIN_DYNAMIC_MINUTES,
            "require_soak_hours_at_least": MIN_SOAK_HOURS,
            "proxy_is_not_soak": True,
            "offline_release_gate": "tests/lumengi/run_release_soak_gate.py",
        },
        "selected_gpu": None,
        # Keep simple canonical artifact paths alongside the rich per-phase
        # launch record.  The offline gate accepts this mapping for generic
        # manifests and inspects ``phases`` when it recognizes this schema.
        "artifacts": {
            role: phase.get("output_json")
            for role, phase in phases.items()
            if isinstance(phase, Mapping) and phase.get("output_json")
        },
        "phases": phases,
    }


def _self_test():
    errors = validate_config(30.0, 2.0, "0", 5.0)
    assert not errors, errors
    assert validate_config(29.9, 2.0, "0", 5.0)
    assert validate_config(30.0, 1.99, "0", 5.0)
    assert validate_config(30.0, 2.0, None, 5.0)
    command = build_command("Mogwai.exe", "run_churn_short.py", "out/mogwai.log")
    assert "--script" in command and "--logfile" in command
    assert command[1:5] == ["--device-type", "d3d12", "--headless", "--precise"]
    phase = _phase_result("dynamic", 1800.0, Path("out/dynamic"), command, 2100.0)
    assert phase["role"] == "dynamic"
    manifest = build_manifest(
        {
            "dynamic_minutes": 30.0,
            "soak_hours": 2.0,
            "dynamic_seconds": 1800.0,
            "soak_seconds": 7200.0,
            "gpu_index": "0",
            "vram_sample_seconds": 5.0,
        },
        Path("out"),
        {"dynamic": phase, "soak": dict(phase, role="soak", expected_seconds=7200.0)},
        "NOT_RUN",
    )
    assert manifest["schema_version"] == SCHEMA_VERSION
    assert manifest["release_gate_status"] == "NOT_RUN"
    assert manifest["contract"]["single_gpu"] is True
    assert manifest["artifacts"]["dynamic"].endswith("out\\dynamic\\churn.json") or manifest["artifacts"]["dynamic"].endswith("out/dynamic/churn.json")
    assert phase["timeout_seconds"] == 2100.0
    print(
        "S2_RELEASE_SOAK_LAUNCHER_SELF_TEST PASS command-schema=PASS "
        "manifest-schema=PASS gpu-run=NOT_RUN"
    )
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-index", help="single GPU index selected for the entire launch")
    parser.add_argument("--dynamic-minutes", type=float, default=None)
    parser.add_argument("--soak-hours", type=float, default=None)
    parser.add_argument("--output-root", help="new, non-existing unique output directory")
    parser.add_argument("--mogwai", default=str(DEFAULT_MOGWAI))
    parser.add_argument("--script", default=str(DEFAULT_SCRIPT))
    parser.add_argument("--vram-sample-seconds", type=float, default=DEFAULT_VRAM_SAMPLE_SECONDS)
    parser.add_argument(
        "--timeout-slack-seconds",
        type=float,
        default=DEFAULT_TIMEOUT_SLACK_SECONDS,
        help="extra timeout after each requested phase duration",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    errors = validate_config(args.dynamic_minutes, args.soak_hours, args.gpu_index, args.vram_sample_seconds)
    if _finite(args.timeout_slack_seconds) is None or args.timeout_slack_seconds < 0:
        errors.append("timeout slack must be non-negative")
    if errors:
        parser.error("; ".join(errors))
    mogwai = Path(args.mogwai).resolve()
    script = Path(args.script).resolve()
    if not mogwai.is_file():
        parser.error("Mogwai executable does not exist: %s" % mogwai)
    if not script.is_file():
        parser.error("runner script does not exist: %s" % script)
    root = _unique_output_root(args.output_root)
    initial_vram = query_vram(args.gpu_index)
    if initial_vram.get("authoritative") is not True:
        manifest = build_manifest(
            {
                "dynamic_minutes": args.dynamic_minutes,
                "soak_hours": args.soak_hours,
                "dynamic_seconds": args.dynamic_minutes * 60.0,
                "soak_seconds": args.soak_hours * 3600.0,
                "gpu_index": args.gpu_index,
                "vram_sample_seconds": args.vram_sample_seconds,
            },
            root,
            {},
            "BLOCKED",
        )
        manifest["blocking_reasons"] = [initial_vram.get("reason") or "authoritative VRAM unavailable"]
        _write_json(root / "launcher-manifest.json", manifest)
        print("S2_RELEASE_SOAK_LAUNCHER BLOCKED", root)
        return 2
    config = {
        "dynamic_minutes": args.dynamic_minutes,
        "soak_hours": args.soak_hours,
        "dynamic_seconds": args.dynamic_minutes * 60.0,
        "soak_seconds": args.soak_hours * 3600.0,
        "gpu_index": args.gpu_index,
        "vram_sample_seconds": args.vram_sample_seconds,
    }
    phases = {}
    for role, seconds in (("dynamic", config["dynamic_seconds"]), ("soak", config["soak_seconds"])):
        timeout = seconds + max(0.0, args.timeout_slack_seconds)
        phases[role] = run_phase(
            role,
            seconds,
            root,
            mogwai,
            script,
            args.gpu_index,
            initial_vram,
            args.vram_sample_seconds,
            timeout,
        )
        # Keep the two phases strictly serial on the single selected GPU.
        if phases[role]["status"] == "BLOCKED":
            break
    status = "READY_FOR_OFFLINE_GATE" if len(phases) == 2 and all(
        phase.get("status") == "READY_FOR_OFFLINE_GATE" for phase in phases.values()
    ) else "BLOCKED"
    manifest = build_manifest(config, root, phases, status)
    manifest["launcher_initial_vram"] = initial_vram
    manifest["selected_gpu"] = {
        "gpu_index": initial_vram.get("gpu_index"),
        "gpu_name": initial_vram.get("gpu_name"),
        "driver_version": initial_vram.get("driver_version"),
    }
    _write_json(root / "launcher-manifest.json", manifest)
    print("S2_RELEASE_SOAK_LAUNCHER", status, root)
    return 0 if status == "READY_FOR_OFFLINE_GATE" else 2


if __name__ == "__main__":
    sys.exit(main())
