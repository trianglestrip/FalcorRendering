from falcor import *

FRAME_RATE = 60
RESOLUTION = (800, 450)


def lumen_graph(features):
    graph = RenderGraph("DiagArcade")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", features), "LumenGI")
    for edge in (
        ("vbuffer", "vbuffer"), ("linearZ", "linearZ"), ("mvec", "mvec"),
        ("mvecW", "mvecW"), ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"), ("diffuseOpacity", "diffuseOpacity"), ("emissive", "emissive"),
    ):
        graph.addEdge("GBufferRT." + edge[0], "LumenGI." + edge[1])
    graph.markOutput("LumenGI.diffuseGI")
    return graph


m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.pause()
m.loadScene("Arcade/Arcade.pyscene")
m.resizeFrameBuffer(*RESOLUTION)

combos = [
    ("baseline", {}),
    ("+surfaceCache", {"useSurfaceCache": True}),
    ("+cacheLighting", {"useSurfaceCache": True, "useCacheLighting": True}),
    ("+screenTrace", {"useSurfaceCache": True, "useCacheLighting": True, "useScreenTrace": True}),
    ("+probes", {"useSurfaceCache": True, "useCacheLighting": True, "useScreenTrace": True, "useScreenProbes": True}),
    ("+temporal", {"useSurfaceCache": True, "useCacheLighting": True, "useScreenTrace": True, "useScreenProbes": True, "useTemporalFilter": True}),
    ("all", {"useSurfaceCache": True, "useCacheLighting": True, "useScreenTrace": True, "useScreenProbes": True, "useTemporalFilter": True, "useSpatialFilter": True}),
]
for label, feats in combos:
    try:
        graph = lumen_graph(feats)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        for f in range(1, 9):
            m.clock.frame = f
            m.renderFrame()
        print("DIAGARC", label, "OK")
        m.removeGraph(graph)
    except Exception as e:
        print("DIAGARC", label, "EXC", str(e)[:120])
        try:
            m.removeGraph(graph)
        except Exception:
            pass

exit()
