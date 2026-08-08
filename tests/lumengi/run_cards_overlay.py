from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)
OUT_DIR = os.path.abspath("artifacts/lumengi/S2/gate")


def lumen_graph(debug_mode):
    graph = RenderGraph("CardsOverlayShot")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", {"useSurfaceCache": True, "debugMode": debug_mode}), "LumenGI")
    for edge in (
        ("vbuffer", "vbuffer"), ("linearZ", "linearZ"), ("mvec", "mvec"),
        ("mvecW", "mvecW"), ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"), ("diffuseOpacity", "diffuseOpacity"), ("emissive", "emissive"),
    ):
        graph.addEdge("GBufferRT." + edge[0], "LumenGI." + edge[1])
    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.debugOutput")
    graph.markOutput("LumenGI.cardCoverage")
    return graph


def render(scene_path, label):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.frameCapture.outputDir = OUT_DIR
    m.frameCapture.baseFilename = label
    graph = lumen_graph("CardsOverlay")
    m.addGraph(graph)
    m.setActiveGraph(graph)
    for frame in range(1, 17):
        m.clock.frame = frame
        m.renderFrame()
    m.frameCapture.capture()
    import math
    out = m.activeGraph.get_output("LumenGI.debugOutput").to_numpy()[..., :3]
    cov = m.activeGraph.get_output("LumenGI.cardCoverage").to_numpy()[..., 0]
    print("OVERLAY", label, "debug mean", float(out.mean()), "coverage", float(cov.mean()),
          "finite", math.isfinite(float(out.mean())))
    m.removeGraph(graph)


render("test_scenes/cornell_box.pyscene", "s2-cornell-cards-overlay")
render("Arcade/Arcade.pyscene", "s2-arcade-cards-overlay")
exit()
