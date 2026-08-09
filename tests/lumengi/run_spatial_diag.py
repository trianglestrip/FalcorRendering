from falcor import *

"""Quick diagnostic: spatial filter denoise effect on a noisy (post-cut) frame."""

import numpy as np

m.loadScene("test_scenes/cornell_box.pyscene")
m.resizeFrameBuffer(640, 360)
m.ui = False
m.clock.framerate = 60
m.clock.time = 0
m.clock.pause()
camera = m.scene.camera
camera.position = float3(0, 0.28, 1.2)
camera.target = float3(0, 0.28, 0)
camera.focalLength = 35.0

graph = RenderGraph("diag")
graph.addPass(createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}), "GBufferRT")
graph.addPass(createPass("LumenGI", {
    "enabled": True, "traceMode": "HardwareRT", "qualityPreset": "High",
    "useScreenTrace": True, "useScreenProbes": True,
    "useTemporalFilter": True, "useSpatialFilter": True,
}), "LumenGI")
for edge in [
    ("GBufferRT.vbuffer", "LumenGI.vbuffer"),
    ("GBufferRT.linearZ", "LumenGI.linearZ"),
    ("GBufferRT.mvec", "LumenGI.mvec"),
    ("GBufferRT.mvecW", "LumenGI.mvecW"),
    ("GBufferRT.normWRoughnessMaterialID", "LumenGI.normWRoughnessMaterialID"),
    ("GBufferRT.viewW", "LumenGI.viewW"),
    ("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity"),
    ("GBufferRT.emissive", "LumenGI.emissive"),
]:
    graph.addEdge(*edge)
for ch in ["probeInterpolated", "temporalFiltered", "temporalConfidence", "spatialFiltered"]:
    graph.markOutput("LumenGI." + ch)
m.addGraph(graph)
m.setActiveGraph(graph)


def grab(name):
    return np.asarray(m.activeGraph.get_output(name).to_numpy(), dtype=np.float32)


def rgb3(img):
    a = img[..., :3] if img.ndim == 3 and img.shape[-1] >= 3 else img
    return a[..., :3]


def box_mean(img, r):
    p = np.pad(img, ((r, r), (r, r)), mode="edge")
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    h, w = p.shape
    s = c[2*r:, 2*r:] - c[:h-2*r, 2*r:] - c[2*r:, :w-2*r] + c[:h-2*r, :w-2*r]
    return s / float((2*r+1)**2)


def lum(img):
    r = rgb3(img)
    return 0.2126*r[...,0] + 0.7152*r[...,1] + 0.0722*r[...,2]


def local_var(img, r=2):
    L = lum(img)
    m = box_mean(L, r)
    ms = box_mean(L*L, r)
    return np.maximum(ms - m*m, 0.0)

for i in range(12):
    m.clock.frame += 1
    m.renderFrame()

t = grab("LumenGI.temporalFiltered")
s = grab("LumenGI.spatialFiltered")
print("steady tail: mean |spatial-temporal| =", round(float(np.abs(rgb3(t)-rgb3(s)).mean()), 5))
print("  local var temporal =", round(float(local_var(t).mean()), 6),
      " spatial =", round(float(local_var(s).mean()), 6))

camera.position = float3(0.25, 0.4, 0.5)
camera.target = float3(0.15, 0.2, -0.1)
m.clock.frame += 1
m.renderFrame()
t = grab("LumenGI.temporalFiltered")
s = grab("LumenGI.spatialFiltered")
print("CUT frame: mean |spatial-temporal| =", round(float(np.abs(rgb3(t)-rgb3(s)).mean()), 5))
vt = local_var(t); vs = local_var(s)
print("  local var temporal =", round(float(vt.mean()), 6), " spatial =", round(float(vs.mean()), 6),
      " ratio =", round(float(vs.mean()/vt.mean()), 4))
# High-variance (noisy) pixel mask only
mask = vt > np.percentile(vt, 70)
print("  noisy-mask (p70+) variance ratio =", round(float(vs[mask].mean()/vt[mask].mean()), 4),
      " spatial mean/var:", round(float(vs[mask].mean()),5), round(float(vt[mask].mean()),5))
for i in range(6):
    m.clock.frame += 1
    m.renderFrame()
t = grab("LumenGI.temporalFiltered")
s = grab("LumenGI.spatialFiltered")
vt = local_var(t); vs = local_var(s)
print("settle+6: local var temporal =", round(float(vt.mean()), 6), " spatial =", round(float(vs.mean()), 6),
      " ratio =", round(float(vs.mean()/vt.mean()), 4))
exit()
