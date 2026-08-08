IMAGE_TEST = {
    "device_types": ["d3d12"],
}

"""LumenGI vs PathTracer reference-comparison skeleton.

Purpose
-------
Render the LumenGI graph side by side with a fixed-seed PathTracer
(see graphs/LumenGIReference.py) on Cornell Box and Arcade. The reference
skeleton captures LumenGI's linear HDR outputs, PathTracer's color output,
and identically tone-mapped pairs (fixed exposure) for display-space metrics.

Usage
-----
Standard harness invocation:

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGIReference" --parallel 1

Run-only mode (default until references are frozen):

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGIReference" --run-only --parallel 1

Determinism contract
--------------------
* Fixed seed: PathTracer is configured with fixedSeed=1337 in the graph and
  re-locked at runtime below via the pass binding (fixedSeed property).
* Fixed exposure: both ToneMappers use autoExposure=False and
  exposureCompensation=0.0.
* Fixed resolution 512x512 (Cornell) and 640x360 (Arcade); fixed frame 1.
* Note: samplesPerPixel is a static PathTracer property and cannot be changed
  at runtime. The graph freezes 64 spp. The S1 Gate convention calls for
  256/1024 spp references (task.md section 15.1); that path requires a
  dedicated reference graph variant (root-owned graphs/LumenGIReference.py is
  not modified by Agent C) and is tracked below.

Inputs / outputs
----------------
* Scenes: media/test_scenes/cornell_box.pyscene, media/Arcade/Arcade.pyscene
* Captures: cornell-*.exr (LumenGI.diffuseGI, PathTracer.color, tone-mapped
  pairs), arcade-*.exr under the harness result directory.
* Reference goldens: harness reference directory (root freezes with
  --gen-refs; never overwritten by this script).
* Metric placeholder: relative RMSE between LumenGI.diffuseGI and
  PathTracer.color, and FLIP mean/P95 if the FLIP pass or a FLIP tool is
  available. Nothing is asserted yet; see compute_reference_metrics() below.

Status
------
RUN-ONLY: renders and captures only. The metric pipeline is a skeleton with
clear placeholders; thresholds are NOT enforced until references are frozen
at the S1 Gate (Agent C report).
"""

import sys

sys.path.append("..")
from helpers import render_frames
from graphs.LumenGIReference import LumenGIReference as g
from falcor import *


# ---------------------------------------------------------------------------
# Reference configuration (fixed seed and exposure contract).
# ---------------------------------------------------------------------------

# Fixed seed for the PathTracer reference. The graph already creates the pass
# with fixedSeed=1337; re-locking here guards against graph drift.
REFERENCE_FIXED_SEED = 1337

# 256/1024 spp reference paths (task.md section 15.1). samplesPerPixel is a
# static PathTracer property, so these runs need a dedicated graph variant:
# graphs/LumenGIReferencePathTracer.py with samplesPerPixel 256 or 1024.
# Output directory convention for those runs:
#   artifacts/lumengi/reference/cornell/spp256/
#   artifacts/lumengi/reference/cornell/spp1024/
#   artifacts/lumengi/reference/arcade/spp256/
#   artifacts/lumengi/reference/arcade/spp1024/
REFERENCE_SPP_PATHS = {256: None, 1024: None}  # placeholders for graph names


def lock_reference_seed():
    """Re-assert the fixed seed and fixed-seed mode on the PathTracer pass."""
    path_tracer = g.get_pass("PathTracer")
    path_tracer.useFixedSeed = True
    path_tracer.fixedSeed = REFERENCE_FIXED_SEED


def compute_reference_metrics():
    """Metric computation placeholder (S1 Gate).

    Planned inputs (EXR captures from the harness result directory):
      LumenGI.diffuseGI          -> linear HDR indirect diffuse
      PathTracer.color           -> linear HDR full image (fixed seed)
      ToneMapperLumen.dst        -> display-space LumenGI
      ToneMapperReference.dst    -> display-space PathTracer

    Planned metrics (nothing is computed or asserted yet):
      - relative RMSE on linear HDR pairs (masked to valid pixels)
      - FLIP mean / FLIP P95 on tone-mapped pairs, if a FLIP tool or the
        FLIPPass graph (tests/image_tests/renderpasses/graphs/FLIPPass.py)
        is available
      - energy error on Cornell (indirect-energy ratio)

    This function must stay a no-op placeholder until the S1 Gate freezes
    thresholds (task.md section 15.4: thresholds frozen in S0, never widened
    after a failure). It intentionally does NOT read or compare images.
    """
    return None


m.addGraph(g)
lock_reference_seed()

# Fixed camera, seed, exposure, and resolution provide reproducible outputs for
# external FLIP/MSE tooling. The path tracer evaluates 64 spp in one frame.
m.loadScene("test_scenes/cornell_box.pyscene")
render_frames(m, "cornell", frames=[1], resolution=[512, 512])

m.loadScene("Arcade/Arcade.pyscene")
render_frames(m, "arcade", frames=[1], resolution=[640, 360])

compute_reference_metrics()

exit()
