"""Headless, deterministic benchmark harness for the LumenGI render graph.

Run with Mogwai. Configuration is provided through LUMENGI_BENCHMARK_* environment
variables so the same script works in local runs and CI without argument parsing.
"""

from pathlib import Path
import os
import sys
import traceback

from falcor import *


REPO_ROOT = Path(__file__).resolve().parents[1]
HELPER_DIR = REPO_ROOT / "tests" / "lumengi"
if str(HELPER_DIR) not in sys.path:
    sys.path.insert(0, str(HELPER_DIR))

from benchmark_manifest import build_manifest, json_safe, write_json


def read_positive_int(name, default):
    value = int(os.getenv(name, str(default)))
    if value <= 0:
        raise ValueError(f"{name} must be greater than zero")
    return value


OUTPUT_DIR = Path(
    os.getenv(
        "LUMENGI_BENCHMARK_OUTPUT_DIR",
        str(REPO_ROOT / "artifacts" / "lumengi" / "benchmark"),
    )
).resolve()

CONFIGURATION = {
    "width": read_positive_int("LUMENGI_BENCHMARK_WIDTH", 1280),
    "height": read_positive_int("LUMENGI_BENCHMARK_HEIGHT", 720),
    "warmup_frames": read_positive_int("LUMENGI_BENCHMARK_WARMUP_FRAMES", 120),
    "capture_frames": read_positive_int("LUMENGI_BENCHMARK_CAPTURE_FRAMES", 600),
    "fixed_framerate": read_positive_int("LUMENGI_BENCHMARK_FRAMERATE", 60),
    "scene": os.getenv("LUMENGI_BENCHMARK_SCENE"),
    "quality_preset": os.getenv("LUMENGI_BENCHMARK_QUALITY", "High"),
    "random_seed": int(os.getenv("LUMENGI_BENCHMARK_SEED", "1")),
}


def collect_pass_data(graph):
    lumen_pass = graph.getPass("LumenGI")
    properties = json_safe(lumen_pass.getDictionary())
    runtime_stats = json_safe(getattr(lumen_pass, "stats", {}))
    return properties, runtime_stats


def run_benchmark():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    write_json(
        OUTPUT_DIR / "manifest.json",
        build_manifest(REPO_ROOT, CONFIGURATION, status="running"),
    )

    graph_script = REPO_ROOT / "scripts" / "LumenGI.py"
    with graph_script.open("r", encoding="utf-8") as source:
        exec(compile(source.read(), str(graph_script), "exec"), globals())

    graph = globals().get("LumenGI")
    if graph is None:
        raise RuntimeError("scripts/LumenGI.py did not create the LumenGI render graph")

    graph.updatePass("LumenGI", {"qualityPreset": CONFIGURATION["quality_preset"]})
    if CONFIGURATION["scene"]:
        m.loadScene(CONFIGURATION["scene"])

    m.resizeFrameBuffer(CONFIGURATION["width"], CONFIGURATION["height"])
    m.ui = False
    m.clock.framerate = CONFIGURATION["fixed_framerate"]
    m.clock.time = 0
    m.clock.pause()

    frame = 0
    for _ in range(CONFIGURATION["warmup_frames"]):
        frame += 1
        m.clock.frame = frame
        m.renderFrame()

    profiler = m.profiler
    profiler.reset_stats()
    profiler.start_capture(CONFIGURATION["capture_frames"])

    # The Profiler initializes its event lanes on the first captured frame, so render
    # one additional frame to obtain exactly capture_frames timing records.
    for _ in range(CONFIGURATION["capture_frames"] + 1):
        frame += 1
        m.clock.frame = frame
        m.renderFrame()

    capture = profiler.end_capture()
    capture = json_safe(capture or {})
    write_json(OUTPUT_DIR / "profiler_capture.json", capture)

    pass_properties, resource_stats = collect_pass_data(graph)
    manifest = build_manifest(
        REPO_ROOT,
        CONFIGURATION,
        status="completed",
        profiler_capture=capture,
        pass_properties=pass_properties,
        resource_stats=resource_stats,
    )
    write_json(OUTPUT_DIR / "manifest.json", manifest)

    if os.getenv("LUMENGI_BENCHMARK_CAPTURE_IMAGE", "0") == "1":
        capture_dir = OUTPUT_DIR / "captures"
        capture_dir.mkdir(parents=True, exist_ok=True)
        m.frameCapture.outputDir = str(capture_dir)
        m.frameCapture.baseFilename = "LumenGI_Benchmark"
        m.frameCapture.capture()


try:
    run_benchmark()
except Exception as error:
    failure = build_manifest(
        REPO_ROOT,
        CONFIGURATION,
        status="failed",
        error="".join(traceback.format_exception_only(type(error), error)).strip(),
    )
    write_json(OUTPUT_DIR / "manifest.json", failure)
    raise

exit()
