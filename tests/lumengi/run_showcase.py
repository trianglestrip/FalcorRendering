from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)
OUT_DIR = os.path.abspath("artifacts/lumengi/showcase")
TONE_MAPPER = {"autoExposure": False, "exposureCompensation": 0.0}


def lumen_graph(debug_mode="None"):
    graph = RenderGraph("Showcase")
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
                "debugMode": debug_mode,
            },
        ),
        "LumenGI",
    )
    graph.addPass(createPass("ToneMapper", dict(TONE_MAPPER)), "ToneMapperDisplay")
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
    graph.addEdge("LumenGI.spatialFiltered", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph


def snap(scene, label, settle_frames=96):
    m.loadScene(scene)
    m.resizeFrameBuffer(*RESOLUTION)
    m.frameCapture.outputDir = OUT_DIR
    m.frameCapture.baseFilename = label
    graph = lumen_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    for f in range(1, settle_frames + 1):
        m.clock.frame = f
        m.renderFrame()
    m.frameCapture.capture()
    import math

    gi = m.activeGraph.get_output("LumenGI.spatialFiltered").to_numpy()[..., :3]
    print("SHOWCASE", label, "mean", float(gi.mean()), "max", float(gi.max()),
          "finite", math.isfinite(float(gi.min())) and math.isfinite(float(gi.max())),
          "nonneg", float(gi.min()) >= 0.0)
    m.removeGraph(graph)


m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.pause()
os.makedirs(OUT_DIR, exist_ok=True)

snap("test_scenes/cornell_box.pyscene", "cornell-gi")

snap(os.path.abspath("tests/lumengi/scenes/emissive_glow.pyscene"), "emissive-glow")
print("SHOWCASE done")
exit()
