from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)


def lumen_graph():
    graph = RenderGraph("ReproT")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", {"useSurfaceCache": True, "useCacheLighting": True}), "LumenGI")
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
print("REPRO cornell 32 done")

m.scene.renderSettings.useEmissiveLights = False
for f in range(33, 41):
    m.clock.frame = f
    m.renderFrame()
print("REPRO emissive off done")

m.scene.renderSettings.useEmissiveLights = True
for f in range(41, 49):
    m.clock.frame = f
    m.renderFrame()
print("REPRO emissive on done")

m.removeGraph(graph)
graph2 = lumen_graph()
m.addGraph(graph2)
m.setActiveGraph(graph2)
m.loadScene(os.path.abspath("tests/lumengi/scenes/cornell_pointlight.pyscene"))
for f in range(49, 65):
    m.clock.frame = f
    m.renderFrame()
print("REPRO switch pointlight done")

exit()
