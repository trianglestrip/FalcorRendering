from falcor import *

"""GBuffer debug-view comparison for LumenGI (one mode per process).

For a single LumenGI.debugMode (None/Normal/LinearDepth/Motion/MaterialID),
render one fixed frame, then compare LumenGI.debugOutput pixel-by-pixel
against the GBufferRT source channels it claims to visualize:

  * Normal       : oct_to_ndir_unorm(normWRoughnessMaterialID.xy) * 0.5 + 0.5
  * LinearDepth  : saturate(log2(1 + max(|linearZ.r|, 0)) / 16)
  * Motion       : (saturate(0.5 + mvec.xy * 16), saturate(length(mvec.xy) * 32))
  * MaterialID   : pseudocolor(round(saturate(normWRoughnessMaterialID.w) * 3))
  * None         : (0, 0, 0, 1)

Reference decodes are reimplemented here with numpy, exactly mirroring
LumenGIDebug.cs.slang. The comparison is done on the GPU texture readback
(Texture.to_numpy) so no file-format round-trip is involved.

Each invocation handles exactly one mode (argv[1]) and writes:

  * artifacts/lumengi/S0/gbuffer/mode-<mode>.json  (metrics)
  * artifacts/lumengi/S0/gbuffer/mode-<mode>-dbg.npy / -linearZ.npy /
    -mvec.npy / -matid.npy                          (raw channel readbacks)

The runner then merges the per-mode JSONs into gbuffer-compare.json.

One mode per process is required by a Falcor API interaction bug found while
building this test: calling RenderPass::setProperties-equivalent
(RenderGraph::updatePass) after a Texture.to_numpy() readback and rendering
the next frame deterministically raises an access violation inside
Falcor.dll. Recreating the pass at graph-build time (this script) avoids the
mid-lifecycle pass replacement entirely.

Scene: test_scenes/cornell_box.pyscene, fixed 640x360, fixed clock frame.

Note: GBufferRT.linearZ/mvec (RG32F) and normWRoughnessMaterialID (RGB10A2)
cannot be exported to EXR by Falcor's frame capture (Bitmap::saveImage only
supports 32-bit/channel RGB/RGBA or 16-bit RGBA), so the evidence is the raw
GPU readbacks (.npy) plus the metric report, both pixel-exact.
"""

import json
import os
import sys

import numpy as np

FRAME_RATE = 60
RESOLUTION = (640, 360)
OUT_DIR = "artifacts/lumengi/S0/gbuffer"
MODES = ["None", "Normal", "LinearDepth", "Motion", "MaterialID"]


def build_graph(mode):
    g = RenderGraph("LumenGIGBufferCompare")
    gbuffer = createPass("GBufferRT", {
        "samplePattern": "Center",
        "sampleCount": 1,
        "useAlphaTest": True,
    })
    g.addPass(gbuffer, "GBufferRT")
    lumen = createPass("LumenGI", {
        "enabled": True,
        "traceMode": "HardwareRT",
        "qualityPreset": "High",
        "debugMode": mode,
    })
    g.addPass(lumen, "LumenGI")
    for channel in [
        "vbuffer",
        "linearZ",
        "mvec",
        "mvecW",
        "normWRoughnessMaterialID",
        "viewW",
        "diffuseOpacity",
        "emissive",
    ]:
        g.addEdge("GBufferRT.%s" % channel, "LumenGI.%s" % channel)
    g.markOutput("GBufferRT.linearZ")
    g.markOutput("GBufferRT.mvec")
    g.markOutput("GBufferRT.normWRoughnessMaterialID")
    g.markOutput("LumenGI.debugOutput")
    return g


def decode_oct_unorm(p):
    p = p * 2.0 - 1.0
    n = np.zeros(p.shape[:-1] + (3,), dtype=np.float64)
    n[..., 0] = p[..., 0]
    n[..., 1] = p[..., 1]
    n[..., 2] = 1.0 - np.abs(p[..., 0]) - np.abs(p[..., 1])
    wrap = n[..., 2] < 0.0
    xy = np.zeros_like(p[..., :2], dtype=np.float64)
    xy[..., 0] = (1.0 - np.abs(p[..., 1])) * np.sign(p[..., 0])
    xy[..., 1] = (1.0 - np.abs(p[..., 0])) * np.sign(p[..., 1])
    n[..., :2][wrap] = xy[wrap]
    norm = np.sqrt(np.sum(n * n, axis=-1))
    return n / (norm[..., np.newaxis] + 1e-12)


def jenkins_hash(a):
    a = np.array(a, dtype=np.uint32)
    a = (a + np.uint32(0x7ED55D16)) + (a << np.uint32(12))
    a = (a ^ np.uint32(0xC761C23C)) ^ (a >> np.uint32(19))
    a = (a + np.uint32(0x165667B1)) + (a << np.uint32(5))
    a = (a + np.uint32(0xD3A2646C)) ^ (a << np.uint32(9))
    a = (a + np.uint32(0xFD7046C5)) + (a << np.uint32(3))
    a = (a ^ np.uint32(0xB55A4F09)) ^ (a >> np.uint32(16))
    return a


def pseudocolor(values):
    h = jenkins_hash(values)
    r = (h & np.uint32(0xFF)).astype(np.float64) / 255.0
    g = ((h >> np.uint32(8)) & np.uint32(0xFF)).astype(np.float64) / 255.0
    b = ((h >> np.uint32(16)) & np.uint32(0xFF)).astype(np.float64) / 255.0
    return np.stack([r, g, b], axis=-1)


def compare(name, expected, actual, valid, tol=5e-3):
    expected = np.asarray(expected, dtype=np.float64)
    actual = np.asarray(actual, dtype=np.float64)
    if expected.shape != actual.shape:
        raise ValueError("%s: shape mismatch %s vs %s" % (name, expected.shape, actual.shape))
    both_nan = np.isnan(expected) & np.isnan(actual)
    diff = np.abs(expected - actual)
    diff[both_nan] = 0.0
    ok = diff <= tol
    if ok.ndim == 3:
        ok = np.all(ok, axis=-1)
    v = np.asarray(valid, dtype=bool)
    n_valid = int(v.sum())
    n_ok = int((ok & v).sum())
    n_nan = int(both_nan.sum())
    frac = n_ok / float(n_valid) if n_valid else 1.0
    return {
        "mode": name,
        "valid_pixels": n_valid,
        "matched_pixels": n_ok,
        "nan_pixels": n_nan,
        "match_fraction": round(frac, 6),
        "max_abs_diff": float(np.max(np.abs(diff))),
        "mean_abs_diff": float(np.mean(np.abs(diff))),
        "tolerance": tol,
        "passed": frac >= 0.999,
    }


def main(mode):
    g = build_graph(mode)
    m.addGraph(g)
    m.setActiveGraph(g)
    m.loadScene("test_scenes/cornell_box.pyscene")
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 1

    os.makedirs(OUT_DIR, exist_ok=True)

    m.renderFrame()

    dbg = g.getOutput("LumenGI.debugOutput").to_numpy()
    lin = g.getOutput("GBufferRT.linearZ").to_numpy()
    mvec = g.getOutput("GBufferRT.mvec").to_numpy()
    matid_raw = g.getOutput("GBufferRT.normWRoughnessMaterialID").to_numpy()

    np.save(os.path.join(OUT_DIR, "mode-%s-dbg.npy" % mode), np.asarray(dbg))
    np.save(os.path.join(OUT_DIR, "mode-%s-linearZ.npy" % mode), np.asarray(lin))
    np.save(os.path.join(OUT_DIR, "mode-%s-mvec.npy" % mode), np.asarray(mvec))
    np.save(os.path.join(OUT_DIR, "mode-%s-matid.npy" % mode), np.asarray(matid_raw))

    if matid_raw.dtype == np.uint8:
        if matid_raw.ndim == 1:
            packed = matid_raw.view(np.uint32).reshape(lin.shape[0], lin.shape[1])
        else:
            packed = matid_raw.reshape(-1, 4).view(np.uint32).reshape(lin.shape[0], lin.shape[1])
        matid = np.stack([
            ((packed >> 0) & 0x3FF).astype(np.float64) / 1023.0,
            ((packed >> 10) & 0x3FF).astype(np.float64) / 1023.0,
            ((packed >> 20) & 0x3FF).astype(np.float64) / 1023.0,
            ((packed >> 30) & 0x3).astype(np.float64) / 3.0,
        ], axis=-1)
    else:
        matid = matid_raw.astype(np.float64)
        if matid.ndim == 3 and matid.shape[2] == 3:
            matid = np.concatenate([matid, np.ones_like(matid[..., :1])], axis=-1)

    dbg_f = dbg.astype(np.float64)
    if dbg_f.ndim == 2:
        dbg_f = np.stack([dbg_f, np.zeros_like(dbg_f), np.zeros_like(dbg_f), np.ones_like(dbg_f)], axis=-1)

    lin_f = lin.astype(np.float64)
    mvec_f = mvec.astype(np.float64)
    lin_r = lin_f[..., 0] if lin_f.ndim == 3 else lin_f
    mvec_xy = mvec_f[..., :2] if mvec_f.ndim == 3 else np.stack([mvec_f[..., 0], mvec_f[..., 1]], axis=-1)

    sky = np.isnan(lin_r) | np.isinf(lin_r)
    valid = ~sky

    h, w = dbg_f.shape[0], dbg_f.shape[1]

    if mode == "None":
        expected = np.zeros((h, w, 4))
        expected[..., 3] = 1.0
        res = compare(mode, expected, dbg_f, np.ones((h, w), dtype=bool))
    elif mode == "Normal":
        expected = decode_oct_unorm(matid[..., :2]) * 0.5 + 0.5
        expected = np.concatenate([expected, np.ones((h, w, 1))], axis=-1)
        res = compare(mode, expected, dbg_f, np.ones((h, w), dtype=bool))
    elif mode == "LinearDepth":
        z = np.where(np.isnan(lin_r) | np.isinf(lin_r), 0.0, np.maximum(np.abs(lin_r), 0.0))
        expected = np.clip(np.log2(1.0 + z) / 16.0, 0.0, 1.0)
        res = compare(mode, expected[..., np.newaxis], dbg_f[..., 0:1], valid)
    elif mode == "Motion":
        mv = np.where(np.isnan(mvec_xy), 0.0, mvec_xy)
        exp_rgb = np.zeros((h, w, 3))
        exp_rgb[..., :2] = np.clip(0.5 + mv * 16.0, 0.0, 1.0)
        exp_rgb[..., 2] = np.clip(np.sqrt(np.sum(mv * mv, axis=-1)) * 32.0, 0.0, 1.0)
        mv_valid = ~(np.isnan(mvec_xy[..., 0]) | np.isnan(mvec_xy[..., 1]))
        res = compare(mode, exp_rgb, dbg_f[..., :3], mv_valid)
    elif mode == "MaterialID":
        material_id = np.round(np.clip(matid[..., 3], 0.0, 1.0) * 3.0).astype(np.uint32)
        expected = pseudocolor(material_id)
        res = compare(mode, expected, dbg_f[..., :3], np.ones((h, w), dtype=bool))

    report = {
        "resolution": list(RESOLUTION),
        "frame": int(m.clock.frame),
        "mode": mode,
        "result": res,
    }
    report_path = os.path.join(OUT_DIR, "mode-%s.json" % mode)
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)

    print("MODE %s: %s" % (mode, json.dumps(res)))
    exit()


mode = os.environ.get("LUMEN_GBUFFER_MODE")
if mode not in MODES:
    print("usage: set LUMEN_GBUFFER_MODE to one of %s" % MODES)
    sys.exit(2)
main(mode)
