from falcor import *

"""Phase-0 performance baseline for LumenGI (S0 evidence, not a Gate).

Runs the main LumenGI graph (scripts/LumenGI.py) headless and records
per-frame times for:

  * Cornell Box @ 1280x720
  * Arcade     @ 1280x720

Warm-up is 120 frames, sample window is 600 frames per scene (task.md 15.5).
Frame times are written to the file passed via argv-style env var
LUMEN_PERF_OUT (one time per line, milliseconds). The caller computes
mean/P50/P95/P99/max and samples nvidia-smi for peak VRAM.

The recorded time is the CPU-side full-frame time (FrameRate::getLastFrameTime),
i.e. an upper bound of the GPU time; it is the baseline metric available
through the timingCapture extension.
"""

import os

LUMEN_PERF_OUT = os.environ.get("LUMEN_PERF_OUT", "artifacts/lumengi/S0/perf/frame-times.txt")
RESOLUTION = (1280, 720)
WARMUP_FRAMES = 120
SAMPLE_FRAMES = 600
FRAME_RATE = 60


def run_scene(scene_path, frame_count):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0
    for _ in range(frame_count):
        m.clock.frame = m.clock.frame + 1
        m.renderFrame()


m.script("scripts/LumenGI.py")
g = m.getGraph("LumenGI")
if g is None:
    raise RuntimeError("LumenGI graph not found after scripts/LumenGI.py")
m.setActiveGraph(g)

out_dir = os.path.dirname(LUMEN_PERF_OUT)
if out_dir:
    os.makedirs(out_dir, exist_ok=True)
m.timingCapture.captureFrameTime(LUMEN_PERF_OUT)

run_scene("test_scenes/cornell_box.pyscene", WARMUP_FRAMES)
run_scene("Arcade/Arcade.pyscene", WARMUP_FRAMES)
run_scene("test_scenes/cornell_box.pyscene", SAMPLE_FRAMES)
run_scene("Arcade/Arcade.pyscene", SAMPLE_FRAMES)

exit()
