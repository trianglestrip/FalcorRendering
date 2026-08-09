from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)


def lumen_graph(trace_mode):
    graph = RenderGraph("DiagAll")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "useSurfaceCache": True,
                "useCacheLighting": True,
                "useScreenTrace": True,
                "useScreenProbes": True,
                "useTemporalFilter": True,
                "useSpatialFilter": True,
                "traceMode": trace_mode,
            },
        ),
        "LumenGI",
    )
    for edge in (
        ("vbuffer", "vbuffer"), ("linearZ", "linearZ"), ("mvec", "mvec"),
        ("mvecW", "mvecW"), ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"), ("diffuseOpacity", "diffuseOpacity"), ("emissive", "emissive"),
    ):
        graph.addEdge("GBufferRT." + edge[0], "LumenGI." + edge[1])
    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.spatialFiltered")
    graph.markOutput("LumenGI.temporalFiltered")
    graph.markOutput("LumenGI.probeInterpolated")
    graph.markOutput("LumenGI.screenTrace")
    graph.markOutput("LumenGI.cacheDirectRadiance")
    return graph


m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.pause()
m.loadScene("test_scenes/cornell_box.pyscene")
m.resizeFrameBuffer(*RESOLUTION)

for mode in ("HardwareRT", "MeshSDF", "Hybrid"):
    try:
        graph = lumen_graph(mode)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        for f in range(1, 9):
            m.clock.frame = f
            m.renderFrame()
        print("DIAGALL", mode, "OK")
        m.removeGraph(graph)
    except Exception as e:
        print("DIAGALL", mode, "EXC", str(e)[:200])
        try:
            m.removeGraph(graph)
        except Exception:
            pass

exit()
