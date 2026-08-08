IMAGE_TEST = {
    "device_types": ["d3d12"],
}

"""LumenGI basic image test (Cornell Box).

Purpose
-------
Render the LumenGI HWRT graph (GBufferRT -> LumenGI, see graphs/LumenGI.py)
on the Cornell Box and capture the first frame and a short steady-state
history. This covers RenderGraph load, shader compilation, first-frame
initialization, and frame 16 state.

Usage
-----
Standard harness invocation (discovery: files named test_*.py under
tests/image_tests/renderpasses are collected by tests/run_image_tests.bat):

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGI" --parallel 1

Run-only mode (no golden comparison) is the default for CI until goldens are
frozen:

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGI" --run-only --parallel 1

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
* Output captures (base name "cornell"): cornell-1.exr, cornell-16.exr
* Reference goldens: harness reference directory (frozen by root with
  --gen-refs; never overwritten by this script).
* Threshold placeholder: MSE tolerance 1e-5 suggested for freeze (S1 Gate
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
from graphs.LumenGI import LumenGI as g
from falcor import *


m.addGraph(g)
m.loadScene("test_scenes/cornell_box.pyscene")

# Capture both the first-frame initialization and a short steady-state history.
render_frames(m, "cornell", frames=[1, 16], resolution=[640, 360])

exit()
