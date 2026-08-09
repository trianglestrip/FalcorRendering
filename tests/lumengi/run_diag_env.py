from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)


def lumen_graph(feats):
    graph = RenderGraph("DiagEnv")
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
    return graph


m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.pause()
m.loadScene("Arcade/Arcade.pyscene")
m.resizeFrameBuffer(*RESOLUTION)

base = {"useSurfaceCache": True, "useCacheLighting": True}
for label, extra in (
    ("cl-env-on", {}),
    ("cl-env-off", {"renderEnv": False}),
):
    try:
        feats = dict(base)
        feats.update(extra)
        graph = lumen_graph(feats)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        for f in range(1, 17):
            m.clock.frame = f
            m.renderFrame()
        print("DIAGENV", label, "OK")
        m.removeGraph(graph)
    except Exception as e:
        print("DIAGENV", label, "EXC", str(e)[:120])
        try:
            m.removeGraph(graph)
        except Exception:
            pass

exit()
