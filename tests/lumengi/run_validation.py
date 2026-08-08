from falcor import *


FRAME_RATE = 60
DEFAULT_RESOLUTION = (640, 360)
SMALL_RESOLUTION = (320, 180)
WARMUP_FRAMES = 16
RESIZE_FRAMES = 8


def create_lumen_graph():
    graph = RenderGraph("LumenGIValidation")
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
    graph.addEdge(
        "GBufferRT.normWRoughnessMaterialID",
        "LumenGI.normWRoughnessMaterialID",
    )
    graph.addEdge("GBufferRT.viewW", "LumenGI.viewW")
    graph.addEdge("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity")
    graph.addEdge("GBufferRT.emissive", "LumenGI.emissive")

    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.confidence")
    graph.markOutput("LumenGI.debugOutput")
    return graph


def render_frames_and_capture(name, frame_count, resolution):
    m.resizeFrameBuffer(*resolution)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.frameCapture.baseFilename = name

    for frame in range(1, frame_count + 1):
        m.clock.frame = frame
        m.renderFrame()
    m.frameCapture.capture()


m.addGraph(create_lumen_graph())
m.loadScene("Arcade/Arcade.pyscene")

render_frames_and_capture(
    "lumengi-validation-default",
    WARMUP_FRAMES,
    DEFAULT_RESOLUTION,
)

# Exercise camera history invalidation before reallocating screen-sized resources.
camera = m.scene.camera
camera.position = camera.position + float3(0.15, 0.0, 0.0)
camera.target = camera.target + float3(0.15, 0.0, 0.0)
render_frames_and_capture(
    "lumengi-validation-camera",
    RESIZE_FRAMES,
    DEFAULT_RESOLUTION,
)

render_frames_and_capture(
    "lumengi-validation-small",
    RESIZE_FRAMES,
    SMALL_RESOLUTION,
)
render_frames_and_capture(
    "lumengi-validation-restored",
    RESIZE_FRAMES,
    DEFAULT_RESOLUTION,
)

exit()
