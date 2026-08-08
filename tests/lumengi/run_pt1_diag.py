"""LumenGI S1 (Agent L): fair single-sample PathTracer baseline (GPU).

LumenGI.diffuseGI uses exactly ONE cosine-weighted bounce ray per pixel with
ONE NEE sample at the bounce vertex (per-pixel PRNG). PathTracer's C2
reference averaged 256/1024 spp per pixel, so its 42.8% indirect coverage is
a multi-sample coverage and cannot be compared 1:1 with LumenGI's
single-sample coverage.

This script renders the SAME physical quantity with the SAME sample count:
PT(maxSurfaceBounces=1) - PT(maxSurfaceBounces=0) at samplesPerPixel=1,
frame 1, fixedSeed=1337. With useRussianRoulette=False and identical seeds
for the b1/b0 pair, the shared primary segments cancel exactly and the
difference is the per-pixel single-path one-bounce indirect estimate -
pixelwise the same estimator as LumenGI.diffuseGI (independent RNG stream).

Outputs to artifacts/lumengi/S1/nee-diag/:
  pt1_b1_f1.npy, pt1_b0_f1.npy (linear HDR), printed coverage statistics.

Usage:
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe --device-type d3d12 ^
      --headless --precise --script tests\\lumengi\\run_pt1_diag.py ^
      --logfile artifacts\\lumengi\\S1\\nee-diag\\pt1-diag.log
"""

from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)
OUT_DIR = "artifacts/lumengi/S1/nee-diag"
BASE_SEED = 1337


def build_graph(max_bounces):
    graph = RenderGraph("PTSingleSampleB%d" % max_bounces)
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
                "samplesPerPixel": 1,
                "maxSurfaceBounces": max_bounces,
                "useRussianRoulette": False,
                "fixedSeed": BASE_SEED,
            },
        ),
        "PT",
    )
    graph.addEdge("GBufferRT.vbuffer", "PT.vbuffer")
    graph.addEdge("GBufferRT.mvec", "PT.mvec")
    graph.addEdge("GBufferRT.viewW", "PT.viewW")
    graph.markOutput("PT.color")
    return graph


def grab(name):
    arr = m.activeGraph.get_output(name).to_numpy()
    return arr[..., :3].astype("float32")


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

    camera = m.scene.camera
    camera.position = float3(0, 0.28, 1.2)
    camera.target = float3(0, 0.28, 0)
    camera.up = float3(0, 1, 0)
    camera.focalLength = 35.0

    lum = lambda x: x[..., 0] * 0.2126 + x[..., 1] * 0.7152 + x[..., 2] * 0.0722

    for b in (1, 0):
        graph = build_graph(b)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        pt = graph.getPass("PT")
        pt.fixedSeed = BASE_SEED
        m.clock.frame = 1
        m.renderFrame()
        arr = grab("PT.color")
        np.save(os.path.join(OUT_DIR, "pt1_b%d_f1.npy" % b), arr)
        l = lum(arr)
        print("PT1 b%d: nonzero frac %.4f  mean %.6f  max %.4f" % (b, (l > 0).mean(), l.mean(), l.max()))
        m.removeGraph(graph)

    b1 = np.load(os.path.join(OUT_DIR, "pt1_b1_f1.npy"))
    b0 = np.load(os.path.join(OUT_DIR, "pt1_b0_f1.npy"))
    bind = b1 - b0
    lbind = lum(bind)
    np.save(os.path.join(OUT_DIR, "pt1_indirect_f1.npy"), bind)
    print("PT1 indirect(b1-b0): nonzero frac %.4f  mean-over-nonzero %.6f  max %.4f" % (
        (lbind > 0).mean(),
        lbind[lbind > 0].mean() if (lbind > 0).any() else 0.0,
        lbind.max()))

    lumens = np.load(os.path.join(OUT_DIR, "diffuseGI_f1.npy"))[..., :3]
    llum = lum(lumens)
    a = llum > 0.0
    b = lbind > 0.0
    print("PT1 vs LumenGI joint: both %.4f  lumens-only %.4f  pt1-only %.4f  neither %.4f" % (
        (a & b).mean(), (a & ~b).mean(), (~a & b).mean(), (~a & ~b).mean()))
    print("PT1 coverage %.4f | LumenGI coverage %.4f | ratio %.3f" % (
        b.mean(), a.mean(), b.mean() / (a.mean() or 1e-9)))
    # cosine on intersection
    both = a & b
    if both.any():
        an = np.linalg.norm(lumens[both], axis=-1)
        bn = np.linalg.norm(bind[both], axis=-1)
        cos = np.clip(np.sum(lumens[both] * bind[both], axis=-1) / np.maximum(an * bn, 1e-12), -1, 1)
        print("PT1 intersection: pixels %d  cosine_mean %.4f" % (both.sum(), cos.mean()))
    exit()


main()
