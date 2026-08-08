from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)


def lumen_graph(use_cl):
    graph = RenderGraph("ReproToggle")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", {"useSurfaceCache": True, "useCacheLighting": use_cl}), "LumenGI")
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
graph = lumen_graph(True)
m.addGraph(graph)
m.setActiveGraph(graph)
m.resizeFrameBuffer(*RESOLUTION)

m.loadScene(os.path.abspath("tests/lumengi/scenes/cornell_pointlight.pyscene"))
for f in range(1, 33):
    m.clock.frame = f
    m.renderFrame()
print("REPRO pointlight 32 done")

m.scene.renderSettings.useAnalyticLights = False
for f in range(33, 49):
    m.clock.frame = f
    m.renderFrame()
print("REPRO analytic off 16 done")

m.scene.renderSettings.useAnalyticLights = True
for f in range(49, 65):
    m.clock.frame = f
    m.renderFrame()
print("REPRO analytic on 16 done")

exit()
