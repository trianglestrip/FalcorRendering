IMAGE_TEST = {
    "device_types": ["d3d12"],
}

"""LumenGI dynamic-regression test (Arcade).

Purpose
-------
Exercise camera translation, resolution changes, and scene-update invalidation
on the LumenGI graph. This is the run-only dynamic regression entry: history
reset on camera/scene updates and screen-resource reallocation on resize are
validated by generating a fixed frame sequence without asserting on pixels.

Usage
-----
Standard harness invocation:

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGIDynamic" --parallel 1

Run-only mode (no golden comparison until goldens are frozen):

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGIDynamic" --run-only --parallel 1

Determinism contract
--------------------
* Fixed camera path: initial view, then a +0.15 world-space translation.
* Fixed resolutions: 640x360, 320x180, back to 640x360.
* Fixed frame counts: 16 warmup frames, then 8 frames per state.
* Fixed seed: LumenGI PRNG is seeded per pixel and frame index (no host seed
  property; Agent B tracks seed handling).
* Exposure: linear HDR outputs, no ToneMapper in this graph.

Inputs / outputs
----------------
* Scene: media/Arcade/Arcade.pyscene
* Captures: history-1.exr, history-16.exr, camera-translation-*.exr,
  resize-small-*.exr, resize-restored-*.exr
* Reference goldens: harness reference directory (root freezes with
  --gen-refs; never overwritten here).
* Threshold placeholder: MSE tolerance 1e-5 suggested for freeze (S1 Gate);
  the dynamic golden set is expected to be lenient on frame 1 captures.

Status
------
RUN-ONLY: renders and captures only; golden comparison is the harness's job.
Note: the "resize" sequence currently reuses the same capture base names as
other states would collide with --gen-refs runs; if freeze reveals collisions,
rename captures in a follow-up (do not rename the test file itself).
"""

import sys

sys.path.append("..")
from helpers import render_frames
from graphs.LumenGI import LumenGI as g
from falcor import *


m.addGraph(g)
m.loadScene("Arcade/Arcade.pyscene")

# Establish history at the default resolution.
render_frames(m, "history", frames=[1, 16], resolution=[640, 360])

# Translate the active camera to exercise scene-update and history invalidation.
camera = m.scene.camera
camera.position = camera.position + float3(0.15, 0.0, 0.0)
camera.target = camera.target + float3(0.15, 0.0, 0.0)
render_frames(m, "camera-translation", frames=[1, 8], resolution=[640, 360])

# Resize down and back up to exercise screen-resource reallocation. Surface-cache
# state, once implemented, should remain valid across these changes.
render_frames(m, "resize-small", frames=[1, 8], resolution=[320, 180])
render_frames(m, "resize-restored", frames=[1, 8], resolution=[640, 360])

exit()
