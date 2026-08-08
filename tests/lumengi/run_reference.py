"""LumenGI S1 Gate validation asset: fixed-frame Cornell reference run.

Role / purpose
--------------
Agent F (Test-Tooling) verification asset, run by root on GPU (Mogwai headless).
Renders the LumenGI graph on Cornell Box with a FIXED camera, FIXED frame
sequence and FIXED exposure, writes the diffuseGI EXR, and prints numeric
statistics of the linear HDR output (min/max/mean, non-negative, finite).

Status: RUN-ONLY. Nothing is compared against a golden image here; this
script produces the LumenGI side of the S1 reference comparison. The
PathTracer reference comparison step (256/1024 spp) is reserved below in the
comments and is NOT executed yet (samplesPerPixel is a static PathTracer
property, so those runs need a dedicated graph variant; the existing skeleton
is tests/image_tests/renderpasses/graphs/LumenGIReference.py).

Usage (run by root, from the repo root)
---------------------------------------
    build\windows-vs2022\bin\Release\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\lumengi\run_reference.py ^
      --logfile artifacts\lumengi\S1\reference.log
(create artifacts\lumengi\S1 first; EXR captures land in Mogwai's CWD.)

Determinism contract
---------------------
* Fixed seed: LumenGI has NO host seed property (its PRNG is seeded per pixel
  and per frame index internally -- see Source/RenderPasses/LumenGI). The
  FIXED seed 1337 belongs to the PathTracer reference step (property
  "fixedSeed"; setting it also forces useFixedSeed=true, PathTracer.cpp:224).
* Fixed camera: explicitly overwritten below from the cornell_box defaults
  (position (0,0.28,1.2), target (0,0.28,0), up (0,1,0), focalLength 35).
* Fixed exposure: the reserved PathTracer reference comparison must NOT rely
  on auto exposure. Method used by all existing PathTracer image tests
  (tests/image_tests/renderpasses/graphs/PathTracer.py and the
  LumenGIReference.py skeleton): PathTracer.color is linear HDR, so fixed
  exposure is achieved by chaining a ToneMapper pass with
  autoExposure=False and exposureCompensation=0.0 (plus optional explicit
  exposureValue/fNumber/shutter/filmSpeed via ToneMapper setters). Use the
  IDENTICAL ToneMapper settings on the LumenGI side for display-space
  comparison; the linear-HDR numeric comparison below needs no exposure at all.
* Fixed resolution 640x360, frames 1 and 8, fixed frame rate 60.

Reserved PathTracer reference step (S1 Gate, NOT executed by this script)
-------------------------------------------------------------------------
Reference runs (dedicated graph variant with the PathTracer pass,
samplesPerPixel 256 or 1024, maxSurfaceBounces 4, useRussianRoulette False,
fixedSeed 1337):

    build\windows-vs2022\bin\Release\Mogwai.exe --device-type d3d12 --headless --precise ^
      --script <variant with spp 256> --logfile artifacts\lumengi\S1\pt-256.log
    (same for spp 1024; output dir convention:
     artifacts/lumengi/reference/cornell/spp256/, .../spp1024/)

Comparison metric (placeholder, formula frozen at S1 Gate, task.md 15.4):
    on masked valid pixels of linear HDR pairs
      a = LumenGI.diffuseGI   (linear, indirect diffuse)
      b = PathTracer.color    (linear, full image; use indirect-only variant
                               if/when available)
    relRMSE = ||a - b||_2 / ||b||_2          (normalized by reference L2 norm)
    Also report FLIP mean/P95 on the identically tone-mapped pair and an
    energy ratio mean(a)/mean(b). Thresholds are NOT enforced here.

Inputs / outputs
----------------
* Scene: media/test_scenes/cornell_box.pyscene (read-only, packman link)
* Captures: lumengi-ref-cornell-frame1.exr, lumengi-ref-cornell-frame8.exr
  (LumenGI.diffuseGI via frame capture)
* Console: REF stats lines (min/max/mean/finite/non-negative) via
  m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
"""

from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)
CAPTURE_FRAMES = (1, 8)

# cornell_box.pyscene default camera, re-applied explicitly so the fixed
# camera is part of the script contract (survives scene file edits).
FIXED_CAMERA_POSITION = float3(0, 0.28, 1.2)
FIXED_CAMERA_TARGET = float3(0, 0.28, 0)
FIXED_CAMERA_UP = float3(0, 1, 0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0


def create_lumen_graph():
    graph = RenderGraph("LumenGIReference")
    graph.addPass(
        createPass(
            "GBufferRT",
            {
                "samplePattern": "Center",
                "sampleCount": 1,
                "useAlphaTest": True,
            },
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI"), "LumenGI")

    graph.addEdge("GBufferRT.vbuffer", "LumenGI.vbuffer")
    graph.addEdge("GBufferRT.linearZ", "LumenGI.linearZ")
    graph.addEdge("GBufferRT.mvec", "LumenGI.mvec")
    graph.addEdge("GBufferRT.mvecW", "LumenGI.mvecW")
    graph.addEdge("GBufferRT.normWRoughnessMaterialID", "LumenGI.normWRoughnessMaterialID")
    graph.addEdge("GBufferRT.viewW", "LumenGI.viewW")
    graph.addEdge("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity")
    graph.addEdge("GBufferRT.emissive", "LumenGI.emissive")

    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.confidence")
    graph.markOutput("LumenGI.debugOutput")
    return graph


def diffuse_gi_stats(label):
    import math

    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
    # RGB only: the alpha channel is hardcoded to 1.0 in the trace shader
    # (LumenHardwareTrace.rt.slang:318 gDiffuseGI = float4(diffuseGI, 1.f)).
    gi = gi[..., :3]
    vmin = float(gi.min())
    vmax = float(gi.max())
    vmean = float(gi.mean())
    finite = math.isfinite(vmin) and math.isfinite(vmax)
    nonneg = vmin >= 0.0
    print(
        "REF",
        label,
        "shape", gi.shape,
        "min", vmin,
        "max", vmax,
        "mean", vmean,
        "finite", finite,
        "nonnegative", nonneg,
    )
    return finite, nonneg


m.addGraph(create_lumen_graph())
m.loadScene("test_scenes/cornell_box.pyscene")
m.resizeFrameBuffer(*RESOLUTION)
m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.time = 0
m.clock.pause()
m.frameCapture.baseFilename = "lumengi-ref-cornell"

camera = m.scene.camera
camera.position = FIXED_CAMERA_POSITION
camera.target = FIXED_CAMERA_TARGET
camera.up = FIXED_CAMERA_UP
camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH

frame = 0
for capture_frame in CAPTURE_FRAMES:
    while frame < capture_frame:
        frame += 1
        m.clock.frame = frame
        m.renderFrame()
    diffuse_gi_stats("frame%d" % capture_frame)
    m.frameCapture.capture()

exit()
