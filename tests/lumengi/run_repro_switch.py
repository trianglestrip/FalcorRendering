from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)


def lumen_graph():
    graph = RenderGraph("Repro")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", {"useCacheLighting": True, "useSurfaceCache": True}), "LumenGI")
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
graph = lumen_graph()
m.addGraph(graph)
m.setActiveGraph(graph)
m.resizeFrameBuffer(*RESOLUTION)

m.loadScene("test_scenes/cornell_box.pyscene")
for f in range(1, 33):
    m.clock.frame = f
    m.renderFrame()
print("REPRO cornell_box done")

m.loadScene(os.path.abspath("tests/lumengi/scenes/cornell_pointlight.pyscene"))
for f in range(33, 65):
    m.clock.frame = f
    m.renderFrame()
print("REPRO cornell_pointlight done")

exit()
