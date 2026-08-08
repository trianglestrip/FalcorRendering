"""LumenGI S2 placeholder: card placement overlay debug runner (SKELETON).

Role / purpose
--------------
S2 integration-run script (Agent C2): after S2 (Surface Cache / Cards) is
integrated, root runs this on GPU to render the card placement overlay debug
view and capture the overlay + stats.

STATUS: SKELETON ONLY - do NOT run before S2 integration. The pieces marked
S2_TODO below do not exist yet in the LumenGI pass:
  * debugMode "CardsOverlay" (LumenGIDebug.cs.slang visualizer) - missing
  * per-card placement statistics (host counters) - missing
If run before S2, createPass("LumenGI", {"debugMode": "CardsOverlay", ...})
fails at graph build time; that failure is the intended fast-fail so nobody
mistakes the skeleton for a passing gate.

After S2 integration, expected changes (root/Agent B, NOT C2):
  * Wire "CardsOverlay" into LumenGIDebug.cs.slang (follow the existing
    Normal/LinearDepth/... visualizer pattern).
  * Expose card placement counters (page count, bounds coverage, atlas
    occupancy) on the pass (C++ host) and print them here.

Usage (after S2 integration, root, from the repo root):
    build\windows-vs2022\bin\Release\Mogwai.exe --device-type d3d12 ^
      --headless --precise --script tests\lumengi\run_cards_debug.py ^
      --logfile artifacts\lumengi\S2\cards-debug.log
"""

from falcor import *

RESOLUTION = (640, 360)
FRAME_RATE = 60
CAPTURE_FRAMES = (1, 8)

# S2_TODO: debugMode value must be added by S2 (LumenGIDebug.cs.slang).
CARDS_OVERLAY_DEBUG_MODE = "CardsOverlay"
CARDS_OVERLAY_ENABLED = True

FIXED_CAMERA_POSITION = float3(0, 0.28, 1.2)
FIXED_CAMERA_TARGET = float3(0, 0.28, 0)
FIXED_CAMERA_UP = float3(0, 1, 0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0


def create_cards_graph():
    graph = RenderGraph("LumenGICardsDebug")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    # useSurfaceCache exists as a LumenGI property since S1 (LumenGI.cpp:45);
    # S2 makes it actually switch the trace path.
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "useSurfaceCache": CARDS_OVERLAY_ENABLED,
                "debugMode": CARDS_OVERLAY_DEBUG_MODE,
            },
        ),
        "LumenGI",
    )

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


def cards_stats(label):
    """S2_TODO: replace with real per-card placement counters once exposed.

    Current body only checks the overlay buffer is finite and nonzero, which
    is a smoke check, not a placement correctness check.
    """
    import math

    out = m.activeGraph.get_output("LumenGI.debugOutput").to_numpy()
    out = out[..., :3]
    finite = math.isfinite(float(out.min())) and math.isfinite(float(out.max()))
    mean = float(out.mean())
    print("CARDS", label, "shape", out.shape, "mean", mean, "finite", finite)
    return finite


def main():
    m.addGraph(create_cards_graph())
    m.loadScene("test_scenes/cornell_box.pyscene")
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()

    camera = m.scene.camera
    camera.position = FIXED_CAMERA_POSITION
    camera.target = FIXED_CAMERA_TARGET
    camera.up = FIXED_CAMERA_UP
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH

    frame = 0
    for capture_frame in CAPTURE_FRAMES:
        while frame < capture_frame:
            frame += 1
            m.clock.frame = frame
            m.renderFrame()
        cards_stats("frame%d" % capture_frame)
        m.frameCapture.capture()


main()
exit()
