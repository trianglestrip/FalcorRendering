from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)


def create_lumen_graph():
    graph = RenderGraph("LumenGIArcadetoggle")
    graph.addPass(
        createPass(
            "GBufferRT",
            {
                "samplePattern": "Center",
                "sampleCount": 1,
                "useAlphaTest": True,
            },
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI"), "LumenGI")

    graph.addEdge("GBufferRT.vbuffer", "LumenGI.vbuffer")
    graph.addEdge("GBufferRT.linearZ", "LumenGI.linearZ")
    graph.addEdge("GBufferRT.mvec", "LumenGI.mvec")
    graph.addEdge("GBufferRT.mvecW", "LumenGI.mvecW")
    graph.addEdge("GBufferRT.normWRoughnessMaterialID", "LumenGI.normWRoughnessMaterialID")
    graph.addEdge("GBufferRT.viewW", "LumenGI.viewW")
    graph.addEdge("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity")
    graph.addEdge("GBufferRT.emissive", "LumenGI.emissive")

    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.confidence")
    return graph


def stats(label):
    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
    import math

    print(
        "TOGGLE",
        label,
        "min",
        float(gi.min()),
        "max",
        float(gi.max()),
        "mean",
        float(gi.mean()),
        "finite",
        math.isfinite(float(gi.min())) and math.isfinite(float(gi.max())),
    )


m.addGraph(create_lumen_graph())
m.loadScene("Arcade/Arcade.pyscene")
m.resizeFrameBuffer(*RESOLUTION)
m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.time = 0
m.clock.pause()

for frame in range(1, 9):
    m.clock.frame = frame
    m.renderFrame()
stats("arcade-all")

m.scene.renderSettings.useEnvLight = False
for frame in range(9, 17):
    m.clock.frame = frame
    m.renderFrame()
stats("arcade-no-env")

m.scene.renderSettings.useEmissiveLights = False
for frame in range(17, 25):
    m.clock.frame = frame
    m.renderFrame()
stats("arcade-no-env-no-emissive")

m.scene.renderSettings.useAnalyticLights = False
for frame in range(25, 33):
    m.clock.frame = frame
    m.renderFrame()
stats("arcade-all-off")

exit()
