from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)
OUT_DIR = os.path.abspath("artifacts/lumengi/screenshots")
FIXED_TONE_MAPPER = {"autoExposure": False, "exposureCompensation": 0.0}
FIXED_CAMERA_POSITION = float3(0, 0.28, 1.2)
FIXED_CAMERA_TARGET = float3(0, 0.28, 0)
FIXED_CAMERA_UP = float3(0, 1, 0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0


def lumen_graph(debug_mode="None"):
    graph = RenderGraph("LumenShot")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", {"debugMode": debug_mode}), "LumenGI")
    graph.addPass(createPass("ToneMapper", dict(FIXED_TONE_MAPPER)), "ToneMapperDisplay")
    for edge in (
        ("vbuffer", "vbuffer"), ("linearZ", "linearZ"), ("mvec", "mvec"),
        ("mvecW", "mvecW"), ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"), ("diffuseOpacity", "diffuseOpacity"), ("emissive", "emissive"),
    ):
        graph.addEdge("GBufferRT." + edge[0], "LumenGI." + edge[1])
    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.debugOutput")
    graph.addEdge("LumenGI.diffuseGI", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph


def pt_graph(max_bounces):
    graph = RenderGraph("PTShot")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "PathTracer",
            {
                "samplesPerPixel": 16,
                "maxSurfaceBounces": max_bounces,
                "useRussianRoulette": False,
                "fixedSeed": 1337,
            },
        ),
        "PT",
    )
    graph.addPass(createPass("ToneMapper", dict(FIXED_TONE_MAPPER)), "ToneMapperDisplay")
    graph.addEdge("GBufferRT.vbuffer", "PT.vbuffer")
    graph.addEdge("GBufferRT.mvec", "PT.mvec")
    graph.addEdge("GBufferRT.viewW", "PT.viewW")
    graph.markOutput("PT.color")
    graph.addEdge("PT.color", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph


def snap(label, frames=1):
    m.frameCapture.outputDir = OUT_DIR
    m.frameCapture.baseFilename = label
    m.frameCapture.capture()
    print("SHOT", label)


def render_lumen(scene, debug_mode, label):
    graph = lumen_graph(debug_mode)
    m.addGraph(graph)
    m.setActiveGraph(graph)
    m.clock.frame = 1
    m.renderFrame()
    snap(label)
    m.removeGraph(graph)


def render_pt(scene, bounces, label, spp_eff):
    graph = pt_graph(bounces)
    m.addGraph(graph)
    m.setActiveGraph(graph)
    pt = graph.getPass("PT")
    frames = spp_eff // 16
    for f in range(frames):
        pt.fixedSeed = 1337 + f
        m.clock.frame = f + 1
        m.renderFrame()
    snap(label)
    m.removeGraph(graph)


def fix_camera():
    camera = m.scene.camera
    camera.position = FIXED_CAMERA_POSITION
    camera.target = FIXED_CAMERA_TARGET
    camera.up = FIXED_CAMERA_UP
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()

    # Cornell: LumenGI indirect GI + PT full reference + debug view.
    m.loadScene("test_scenes/cornell_box.pyscene")
    m.resizeFrameBuffer(*RESOLUTION)
    fix_camera()
    render_lumen("cornell", "None", "cornell-lumengi-diffuseGI")
    render_lumen("cornell", "IndirectOnly", "cornell-lumengi-indirect-debug")
    render_pt("cornell", 4, "cornell-pt-reference", 64)

    # Arcade: LumenGI + PT reference (env/analytic/emissive lights).
    m.loadScene("Arcade/Arcade.pyscene")
    m.resizeFrameBuffer(*RESOLUTION)
    fix_camera()
    render_lumen("arcade", "None", "arcade-lumengi-diffuseGI")
    render_lumen("arcade", "IndirectOnly", "arcade-lumengi-indirect-debug")
    render_pt("arcade", 4, "arcade-pt-reference", 64)
    print("SHOTS done")


main()
exit()
