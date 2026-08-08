"""LumenGI S1 wrap-up: PathTracer fixed-frame reference renders (GPU, Agent C2).

Role / purpose
--------------
Runs the PathTracer correctness reference for the S1 Gate ("Cornell Box 固定
曝光下，间接光颜色传播方向与 PathTracer 一致", task.md 6 / 15.1). One Mogwai
headless run renders, on test_scenes/cornell_box.pyscene at a FIXED camera and
FIXED resolution 640x360, frame 1:

  * PathTracer.full:  effective spp 256 and 1024, maxSurfaceBounces=4
                      -> 15.1 correctness reference
  * PathTracer.ind:   effective spp 256 and 1024, maxSurfaceBounces=1
  * PathTracer.direct:effective spp 256 and 1024, maxSurfaceBounces=0
    (indirect-only estimate = PT(1) - PT(0); per frame the two runs share the
     IDENTICAL seed so shared segments cancel exactly, see docstring below)

VERIFIED ENVIRONMENT FACT (affects the 256/1024 spp requirement):
  PathTracer's static 'samplesPerPixel' is hard-clamped to
  kMaxSamplesPerPixel = 16 (PathTracer/Params.slang:67,
  PathTracer.cpp:294-297). A single frame can therefore never carry 256/1024
  samples, so this script renders 16 spp per frame over 16 (256 spp) or 64
  (1024 spp) frames and averages the frames in numpy. Determinism is kept by
  a fixed per-frame seed schedule: frame f uses fixedSeed = 1337 + f (runtime
  property, PathTracer.cpp:167-169; mParams.seed = useFixedSeed ? fixedSeed :
  frameCount is re-evaluated every beginFrame, PathTracer.cpp:1249). The
  result is bit-reproducible on the same GPU/driver/seed-schedule and equals
  the AccumulatePass average (plain mean). Deviations from the literal
  "fixedSeed=1337, static 256/1024 spp" brief are documented in the report.

  Cancellation property: at a given spp, the b0/b1/b4 configurations share
  the same seed schedule, so primary-direct / primary-emission /
  vertex-1-emission contributions are IDENTICAL between b1 and b0 and cancel
  exactly in PT(1) - PT(0), leaving exactly the one-bounce indirect transport
  (vertex-1 NEE direct light + vertex-2 emissive scatter) - the same physical
  quantity LumenGI.diffuseGI estimates.

Numerics vs display:
  * Numeric comparison uses LINEAR HDR PathTracer.color and
    LumenGI.diffuseGI (run_reference.py contract: "linear-HDR numeric
    comparison needs no exposure at all").
  * A ToneMapper (autoExposure=False, exposureCompensation=0.0) is chained
    on the full reference for display-space verification/FLIP later; its dst
    is NOT invertible so it is not used numerically.

Evidence:
  artifacts/lumengi/S1/reference-compare/
    lumengi_diffuseGI_f1.npy, pt_<spp>_b<b>_f1.npy   (averaged arrays)
    pt-cornell-<spp>-b<b>.*.exr                       (frame capture, last
     frame; outputDir is set on frameCapture so EXRs land here)

Usage (root, from the repo root; artifacts dir must exist):
    build\windows-vs2022\bin\Release\Mogwai.exe --device-type d3d12 ^
      --headless --precise --script tests\lumengi\run_pathreference.py ^
      --logfile artifacts\lumengi\S1\reference-compare\pt-ref.log
"""

from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)
CAPTURE_FRAME = 1
BASE_SEED = 1337
SAMPLES_PER_FRAME = 16  # static spp cap, PathTracer/Params.slang:67

# (effective spp, maxSurfaceBounces) configurations rendered. b=4 is the 15.1
# full reference; b=1/b=0 are the indirect-variant pair (difference ==
# 1-bounce indirect, which is what LumenGI.diffuseGI computes).
CONFIGS = ((256, 4), (256, 1), (256, 0), (1024, 4), (1024, 1), (1024, 0))

# Same fixed camera as run_reference.py (survives scene file edits).
FIXED_CAMERA_POSITION = float3(0, 0.28, 1.2)
FIXED_CAMERA_TARGET = float3(0, 0.28, 0)
FIXED_CAMERA_UP = float3(0, 1, 0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0

OUT_DIR = "artifacts/lumengi/S1/reference-compare"

# Display-space chain on the full reference only (fixed exposure, per
# tests/image_tests/renderpasses/graphs/PathTracer.py convention).
FIXED_TONE_MAPPER = {"autoExposure": False, "exposureCompensation": 0.0}


def build_graph(max_bounces, tone_map):
    graph = RenderGraph("LumenGIPathRefB%d" % max_bounces)
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "PathTracer",
            {
                "samplesPerPixel": SAMPLES_PER_FRAME,
                "maxSurfaceBounces": max_bounces,
                "useRussianRoulette": False,
                "fixedSeed": BASE_SEED,
            },
        ),
        "PT",
    )
    graph.addPass(createPass("ToneMapper", dict(FIXED_TONE_MAPPER)), "ToneMapperDisplay")

    graph.addEdge("GBufferRT.vbuffer", "PT.vbuffer")
    graph.addEdge("GBufferRT.mvec", "PT.mvec")
    graph.addEdge("GBufferRT.viewW", "PT.viewW")
    graph.markOutput("PT.color")

    graph.addEdge("PT.color", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph


def linear_rgb(output_name):
    """Grab a marked linear-HDR output as float32 (H, W, 3) numpy array."""
    import math

    arr = m.activeGraph.get_output(output_name).to_numpy()
    arr = arr[..., :3].astype("float32")  # alpha ignored (hardcoded 1.0)
    finite = math.isfinite(float(arr.min())) and math.isfinite(float(arr.max()))
    nonneg = float(arr.min()) >= 0.0
    print(
        "PTCMP", output_name,
        "shape", arr.shape,
        "min", float(arr.min()),
        "max", float(arr.max()),
        "mean", float(arr.mean()),
        "finite", finite,
        "nonnegative", nonneg,
    )
    return arr


def main():
    import os
    import numpy as np

    os.makedirs(OUT_DIR, exist_ok=True)

    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.loadScene("test_scenes/cornell_box.pyscene")
    m.resizeFrameBuffer(*RESOLUTION)

    # EXR evidence lands in OUT_DIR (absolute; CaptureTrigger resolves
    # relative paths against the RUNTIME directory, not CWD).
    m.frameCapture.outputDir = os.path.abspath(OUT_DIR)

    camera = m.scene.camera
    camera.position = FIXED_CAMERA_POSITION
    camera.target = FIXED_CAMERA_TARGET
    camera.up = FIXED_CAMERA_UP
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH

    print("PTCMP baseSeed", BASE_SEED, "sppPerFrame", SAMPLES_PER_FRAME, "configs", CONFIGS)
    print("PTCMP resolution", RESOLUTION, "frame", CAPTURE_FRAME, "fps", FRAME_RATE)
    print("PTCMP toneMapper", FIXED_TONE_MAPPER, "(display only, not used numerically)")
    print("PTCMP useEnvLight", m.scene.renderSettings.useEnvLight,
          "useAnalyticLights", m.scene.renderSettings.useAnalyticLights,
          "useEmissiveLights", m.scene.renderSettings.useEmissiveLights)

    # LumenGI diffuseGI at frame 1, rendered once (its PRNG is per-pixel and
    # per-frame; only frame 1 is part of the comparison).
    lumen_graph = RenderGraph("LumenGIDiffuseGI")
    lumen_graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    lumen_graph.addPass(createPass("LumenGI"), "LumenGI")
    for edge in (
        ("vbuffer", "vbuffer"), ("linearZ", "linearZ"), ("mvec", "mvec"),
        ("mvecW", "mvecW"), ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"), ("diffuseOpacity", "diffuseOpacity"), ("emissive", "emissive"),
    ):
        lumen_graph.addEdge("GBufferRT." + edge[0], "LumenGI." + edge[1])
    lumen_graph.markOutput("LumenGI.diffuseGI")
    lumen_graph.markOutput("LumenGI.confidence")
    lumen_graph.markOutput("LumenGI.debugOutput")
    m.addGraph(lumen_graph)
    m.setActiveGraph(lumen_graph)
    m.clock.frame = CAPTURE_FRAME
    m.renderFrame()
    lumens = linear_rgb("LumenGI.diffuseGI")
    np.save(os.path.join(OUT_DIR, "lumengi_diffuseGI_f1.npy"), lumens)
    print("PTCMP saved lumengi_diffuseGI_f1.npy")

    # PathTracer configs: one graph each, 16 spp/frame, per-frame seed
    # schedule 1337+f, numpy-averaged over the frames.
    for spp_eff, max_bounces in CONFIGS:
        frames = spp_eff // SAMPLES_PER_FRAME
        graph = build_graph(max_bounces, max_bounces == 4)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        pt = graph.getPass("PT")
        acc = None
        for f in range(frames):
            pt.fixedSeed = BASE_SEED + f
            m.clock.frame = f + 1
            m.renderFrame()
            arr = linear_rgb("PT.color")
            acc = arr if acc is None else acc + arr
        acc = acc / float(frames)
        key = "pt_%d_b%d_f1" % (spp_eff, max_bounces)
        np.save(os.path.join(OUT_DIR, key + ".npy"), acc)
        print("PTCMP saved", key + ".npy", "mean", float(acc.mean()))
        # EXR evidence of this config's last frame (display-space chain).
        m.frameCapture.baseFilename = "pt-cornell-%d-b%d" % (spp_eff, max_bounces)
        m.frameCapture.capture()
        m.removeGraph(graph)
    print("PTCMP done")


main()
exit()
