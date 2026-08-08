from falcor import *

FRAME_RATE = 60
RESOLUTION = (640, 360)


def create_lumen_graph(use_surface_cache):
    graph = RenderGraph("LumenGIS2Verify")
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
    graph.addPass(createPass("LumenGI", {"useSurfaceCache": use_surface_cache}), "LumenGI")

    for edge in [
        ("GBufferRT.vbuffer", "LumenGI.vbuffer"),
        ("GBufferRT.linearZ", "LumenGI.linearZ"),
        ("GBufferRT.mvec", "LumenGI.mvec"),
        ("GBufferRT.mvecW", "LumenGI.mvecW"),
        ("GBufferRT.normWRoughnessMaterialID", "LumenGI.normWRoughnessMaterialID"),
        ("GBufferRT.viewW", "LumenGI.viewW"),
        ("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity"),
        ("GBufferRT.emissive", "LumenGI.emissive"),
    ]:
        graph.addEdge(*edge)

    graph.markOutput("LumenGI.diffuseGI")
    graph.markOutput("LumenGI.confidence")
    return graph


def stats(label):
    import math

    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()[..., :3]
    print(
        "S2VERIFY",
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


m.addGraph(create_lumen_graph(True))
m.loadScene("test_scenes/cornell_box.pyscene")
m.resizeFrameBuffer(*RESOLUTION)
m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.time = 0
m.clock.pause()

for frame in range(1, 9):
    m.clock.frame = frame
    m.renderFrame()
stats("surfacecache-on-frame8")

# Scene reload must not crash or leak (page table / cards rebuilt).
m.loadScene("test_scenes/cornell_box.pyscene")
for frame in range(9, 17):
    m.clock.frame = frame
    m.renderFrame()
stats("surfacecache-on-reloaded")

# Resize must not crash.
m.resizeFrameBuffer(320, 180)
for frame in range(17, 25):
    m.clock.frame = frame
    m.renderFrame()
stats("surfacecache-on-resized")
m.resizeFrameBuffer(*RESOLUTION)

# Toggle off: baseline path must keep working.
m.activeGraph.update_pass("LumenGI", {"useSurfaceCache": False})
for frame in range(25, 33):
    m.clock.frame = frame
    m.renderFrame()
stats("surfacecache-off")

exit()
