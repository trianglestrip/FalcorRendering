"""LumenGI S1 (Agent L): secondary-hit NEE success / coverage diagnostic.

Renders LumenGI on Cornell 640x360 frame 1 (same fixed camera and seed
schedule as run_pathreference.py) and dumps the raw intermediate outputs:

  * LumenGI.diffuseRadianceHitDist  -> gDiffuseRadianceHitDist
      RGB  = unmodulated radiance at the pixel (pre-primary-albedo)
      A    = hit distance of the SINGLE bounce ray
             (0 -> no primary hit; ~65504 (kLumenGIMissHitDistance) -> primary
             hit + bounce ray miss; in between -> bounce ray hit)
  * LumenGI.diffuseGI               -> final GI
  * LumenGI.debugOutput (EmissiveOnly debug mode)

The script then prints, as text evidence (no image input needed):

  - P(primary hit), P(bounce hit | primary hit)
  - P(nonzero bounce radiance | bounce hit)          <-- NEE success rate
  - region bands (rows x cols) of the nonzero-radiance mask
  - the same region bands for the PT(1024, b1)-PT(1024, b0) indirect mask
    loaded from the S1 reference-compare directory for direct comparison

Evidence arrays are saved to artifacts/lumengi/S1/nee-diag/*.npy.

Usage (from the repo root, artifacts dir must exist):
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe --device-type d3d12 ^
      --headless --precise --script tests\\lumengi\\run_nee_diag.py ^
      --logfile artifacts\\lumengi\\S1\\nee-diag\\nee-diag.log
"""

from falcor import *

RESOLUTION = (640, 360)
FRAME_RATE = 60
OUT_DIR = "artifacts/lumengi/S1/nee-diag"
PT_DIR = "artifacts/lumengi/S1/reference-compare"
MISS_DIST = 65504.0


def build_graph():
    graph = RenderGraph("LumenGIHitStats")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI"), "LumenGI")
    for edge in (
        ("vbuffer", "vbuffer"), ("linearZ", "linearZ"), ("mvec", "mvec"),
        ("mvecW", "mvecW"), ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"), ("diffuseOpacity", "diffuseOpacity"), ("emissive", "emissive"),
    ):
        graph.addEdge("GBufferRT." + edge[0], "LumenGI." + edge[1])
    graph.markOutput("LumenGI.diffuseRadianceHitDist")
    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.debugOutput")
    return graph


def lum(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def band_report(name, mask, h, w, row_band=30, col_band=40):
    print("BAND %s nonzero frac overall: %.4f" % (name, mask.mean()))
    rows = mask.reshape(-1, w)[:, :].mean(axis=1)
    for y in range(0, h, row_band):
        print("  rows %3d-%3d: %.3f" % (y, min(y + row_band - 1, h - 1), rows[y:y + row_band].mean()))
    cols = mask.reshape(-1, w)[:, :].mean(axis=0)
    for x in range(0, w, col_band):
        print("  cols %3d-%3d: %.3f" % (x, min(x + col_band - 1, w - 1), cols[x:x + col_band].mean()))


def main():
    import os
    import math
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

    print("DIAG useEnvLight", m.scene.renderSettings.useEnvLight,
          "useAnalyticLights", m.scene.renderSettings.useAnalyticLights,
          "useEmissiveLights", m.scene.renderSettings.useEmissiveLights)

    graph = build_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    m.clock.frame = 1
    m.renderFrame()

    rad = m.activeGraph.get_output("LumenGI.diffuseRadianceHitDist").to_numpy()
    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
    dbg = m.activeGraph.get_output("LumenGI.debugOutput").to_numpy()
    rad = np.ascontiguousarray(rad[..., :4], dtype="float32")
    gi = np.ascontiguousarray(gi[..., :3], dtype="float32")
    dbg = np.ascontiguousarray(dbg[..., :3], dtype="float32")
    np.save(os.path.join(OUT_DIR, "radianceHitDist_f1.npy"), rad)
    np.save(os.path.join(OUT_DIR, "diffuseGI_f1.npy"), gi)
    np.save(os.path.join(OUT_DIR, "debugEmissiveOnly_f1.npy"), dbg)
    print("DIAG saved radianceHitDist_f1.npy diffuseGI_f1.npy debugEmissiveOnly_f1.npy")

    h, w = rad.shape[0], rad.shape[1]
    hit_dist = rad[..., 3]
    radiance = rad[..., :3]
    rlum = lum(radiance)
    primary = hit_dist > 0.0
    bounce_hit = (hit_dist > 0.0) & (hit_dist < MISS_DIST)
    bounce_miss = hit_dist >= MISS_DIST
    nonz = rlum > 0.0

    print("DIAG primary-hit frac: %.4f" % primary.mean())
    print("DIAG bounce-hit frac (of all pixels): %.4f" % bounce_hit.mean())
    print("DIAG bounce-miss frac (primary but miss): %.4f" % bounce_miss.mean())
    print("DIAG bounce-hit | primary: %.4f" % (bounce_hit[primary].mean() if primary.any() else float("nan")))
    print("DIAG nonzero-radiance | bounce-hit: %.4f  (%d px)" % (
        nonz[bounce_hit].mean() if bounce_hit.any() else float("nan"),
        int(nonz[bounce_hit].sum()) if bounce_hit.any() else 0))
    print("DIAG nonzero-radiance | all pixels: %.4f" % nonz.mean())
    print("DIAG zero-radiance but bounce-hit: %.4f" % ((~nonz & bounce_hit).mean()))
    print("DIAG radiance max %.4f  mean-nonzero %.4f" % (
        float(radiance.max()), float(rlum[nonz].mean()) if nonz.any() else 0.0))
    print("DIAG hitDist min/max of bounce-hit: %.4f / %.4f" % (
        float(hit_dist[bounce_hit].min()), float(hit_dist[bounce_hit].max())))
    print("DIAG finite radiance: %s" % (math.isfinite(float(radiance.min())) and math.isfinite(float(radiance.max()))))

    # Stats on the primary-hit pixels only (background removed).
    prim_nonzero_frac = (nonz & primary).sum() / max(primary.sum(), 1)
    print("DIAG nonzero | primary: %.4f" % prim_nonzero_frac)

    band_report("LumenGI nonzero (all px)", nonz, h, w)

    b1 = np.load(os.path.join(PT_DIR, "pt_1024_b1_f1.npy"))
    b0 = np.load(os.path.join(PT_DIR, "pt_1024_b0_f1.npy"))
    bind = b1[..., :3] - b0[..., :3]
    bind_lum = lum(bind)
    band_report("PT-indirect nonzero (all px)", bind_lum > 0.0, h, w)
    print("DIAG PT-indirect mean over nonzero: %.4f" % (bind_lum[bind_lum > 0.0].mean() if (bind_lum > 0.0).any() else 0.0))

    # Per-pixel joint: where LumenGI is nonzero vs PT.
    a = nonz
    b = bind_lum > 0.0
    print("DIAG joint: LumenGI&PT %.4f  LumenGI-only %.4f  PT-only %.4f  neither %.4f" % (
        (a & b).mean(), (a & ~b).mean(), (~a & b).mean(), (~a & ~b).mean()))
    print("DIAG lumens mean where nonzero: %.4f" % float(rlum[nonz].mean()))

    exit()


main()
