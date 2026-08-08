from falcor import *


RESOLUTION = (640, 360)
FRAME_RATE = 60


def create_lumen_graph(debug_mode):
    graph = RenderGraph("LumenGIDiag")
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
    graph.addPass(createPass("LumenGI", {"debugMode": debug_mode}), "LumenGI")

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
    graph.markOutput("LumenGI.debugOutput")
    return graph


m.addGraph(create_lumen_graph("IndirectOnly"))
m.loadScene("test_scenes/cornell_box.pyscene")
m.resizeFrameBuffer(*RESOLUTION)
m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.time = 0
m.clock.pause()
m.frameCapture.baseFilename = "lumengi-diag-cornell"

print("DIAG scene.renderSettings.useEnvLight:", m.scene.renderSettings.useEnvLight)
print("DIAG scene.renderSettings.useAnalyticLights:", m.scene.renderSettings.useAnalyticLights)
print("DIAG scene.renderSettings.useEmissiveLights:", m.scene.renderSettings.useEmissiveLights)

frame = 0
for capture_frame in (1, 8):
    while frame < capture_frame:
        frame += 1
        m.clock.frame = frame
        m.renderFrame()

try:
    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
    print("DIAG diffuseGI shape:", gi.shape, "min:", gi.min(), "max:", gi.max(), "mean:", gi.mean())
    import math
    print("DIAG diffuseGI finite:", math.isfinite(float(gi.min())) and math.isfinite(float(gi.max())))
except Exception as e:
    print("DIAG to_numpy failed:", e)

exit()
