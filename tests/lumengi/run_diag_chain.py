from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)


def lumen_graph(feats):
    graph = RenderGraph("DiagChain")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", feats), "LumenGI")
    for edge in (
        ("vbuffer", "vbuffer"), ("linearZ", "linearZ"), ("mvec", "mvec"),
        ("mvecW", "mvecW"), ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"), ("diffuseOpacity", "diffuseOpacity"), ("emissive", "emissive"),
    ):
        graph.addEdge("GBufferRT." + edge[0], "LumenGI." + edge[1])
    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.probeInterpolated")
    graph.markOutput("LumenGI.temporalFiltered")
    graph.markOutput("LumenGI.spatialFiltered")
    return graph


def chk(label, name):
    try:
        arr = m.activeGraph.get_output(name).to_numpy()[..., :3]
        import math
        print("CHAIN", label, name, "mean", float(arr.mean()), "max", float(arr.max()),
              "finite", math.isfinite(float(arr.min())))
    except Exception as e:
        print("CHAIN", label, name, "EXC", str(e)[:80])


m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.pause()
m.loadScene("test_scenes/cornell_box.pyscene")
m.resizeFrameBuffer(*RESOLUTION)

combos = [
    ("probe-only", {"useScreenTrace": True, "useScreenProbes": True}),
    ("probe+temporal", {"useScreenTrace": True, "useScreenProbes": True, "useTemporalFilter": True}),
    ("all", {"useScreenTrace": True, "useScreenProbes": True, "useTemporalFilter": True, "useSpatialFilter": True}),
]
for label, feats in combos:
    try:
        graph = lumen_graph(feats)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        for f in range(1, 33):
            m.clock.frame = f
            m.renderFrame()
        chk(label, "LumenGI.probeInterpolated")
        chk(label, "LumenGI.temporalFiltered")
        chk(label, "LumenGI.spatialFiltered")
        m.removeGraph(graph)
    except Exception as e:
        print("CHAIN", label, "EXC", str(e)[:120])
        try:
            m.removeGraph(graph)
        except Exception:
            pass

exit()
