from falcor import *


FRAME_RATE = 60
RESOLUTION = (640, 360)
CAPTURE_FRAMES = (1, 8)


def create_lumen_graph():
    graph = RenderGraph("LumenGISmoke")
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


def render_capture_frames(name, capture_frames, resolution):
    m.resizeFrameBuffer(*resolution)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.frameCapture.baseFilename = name

    frame = 0
    for capture_frame in capture_frames:
        while frame < capture_frame:
            frame += 1
            m.clock.frame = frame
            m.renderFrame()
        m.frameCapture.capture()


m.addGraph(create_lumen_graph())
m.loadScene("test_scenes/cornell_box.pyscene")
render_capture_frames("lumengi-smoke-cornell", CAPTURE_FRAMES, RESOLUTION)

exit()
