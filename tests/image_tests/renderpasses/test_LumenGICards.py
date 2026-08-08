IMAGE_TEST = {
    "device_types": ["d3d12"],
}

"""LumenGI card capture image test skeleton (Cornell Box).

Purpose
-------
Define the capture list for the S2 card debug views - placement, coverage,
residency and eviction - and keep it runnable before the host integrates the
LumenCardCapture pass (task.md section 7, S2-C1). The graph is the
placeholder graphs/LumenGICards.py (GBufferRT -> LumenGI, identical to
graphs/LumenGI.py) until root wires the capture pass, so every capture below
also exercises graph loading, shader compilation and scene update plumbing.

Usage
-----
Standard harness invocation (discovery: files named test_*.py under
tests/image_tests/renderpasses are collected by tests/run_image_tests.bat):

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGICards" --parallel 1

Run-only mode (no golden comparison) is the default for CI until goldens are
frozen:

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGICards" --run-only --parallel 1

Determinism contract
--------------------
* Fixed resolution 640x360 and fixed frame rate 60 fps (helpers default).
* Fixed camera: cornell_box.pyscene defines the camera and exposure.
* Fixed seed: LumenGI has no host-side seed property; its PRNG is seeded per
  pixel and frame index (SampleGenerator(pixel, frameIndex) in
  LumenHardwareTrace.rt.slang), so outputs are deterministic for a fixed
  frame count. Seed handling is tracked by Agent B.
* No ToneMapper in this graph: the marked outputs are linear HDR (EXR).

Inputs / outputs
----------------
* Scene: media/test_scenes/cornell_box.pyscene
* Capture list (base names) for the S2 card debug views:
    card-placement-1.exr, card-placement-16.exr   (card AABB placement)
    card-coverage-1.exr, card-coverage-16.exr     (page/texel coverage + fallback)
    card-residency-1.exr, card-residency-16.exr   (allocated/resident pages)
    card-eviction-1.exr, card-eviction-16.exr     (LRU/atlas-pressure churn)
  When the host lands the capture pass, each capture will map to the
  corresponding LumenGI debugMode value (root defines the enum entries, e.g.
  DebugMode::CardPlacement/Coverage/Residency/Eviction in LumenGI.h) via
  g["LumenGI"].set_properties({"debugMode": "CardPlacement"}). Until then the
  captures are produced by the placeholder graph with debugMode None.
* Reference goldens: harness reference directory (frozen by root with
  --gen-refs; never overwritten by this script).
* Threshold placeholder: MSE tolerance 1e-5 suggested for freeze (S2 Gate
  decision, see Agent C report). Not yet enforced.

Status
------
RUN-ONLY: this script renders and captures only. In run-only mode the helper
skips captures entirely (harness semantics); golden comparison runs in the
harness (non --run-only) against the frozen reference directory.
"""

import sys

sys.path.append("..")
from helpers import render_frames
from graphs.LumenGICards import LumenGICards as g
from falcor import *


m.addGraph(g)
m.loadScene("test_scenes/cornell_box.pyscene")

# Capture list for the S2 card debug views. Each pair captures the
# first-frame initialization and a short steady-state history; run-only mode
# skips the captures themselves and only exercises the render loop.
render_frames(m, "card-placement", frames=[1, 16], resolution=[640, 360])
render_frames(m, "card-coverage", frames=[1, 16], resolution=[640, 360])
render_frames(m, "card-residency", frames=[1, 16], resolution=[640, 360])
render_frames(m, "card-eviction", frames=[1, 16], resolution=[640, 360])

exit()
