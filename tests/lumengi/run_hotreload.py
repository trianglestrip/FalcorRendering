from falcor import *

"""Hot-reload / scene-reload / resize smoke sequence for LumenGI.

Loads the main LumenGI graph (scripts/LumenGI.py), then exercises, in order:
  1. Baseline render at 1280x720.
  2. Resize to 960x540, render, resize back.
  3. Scene reload (same pyscene path -> new Scene object -> setScene ->
     trace program rebuild + history reset).
  4. Scene switch (cornell_box -> Arcade) and switch back.
  5. Graph reload equivalent (m.addGraph of a fresh graph with the same
     name replaces the old graph: Renderer::addGraph removes the existing
     graph first, then re-adds -> full graph recompile).

Each step renders frames and captures output EXRs (outputDir +
baseFilename.<output>.<frame>.exr) so the caller can assert non-empty,
non-crash per step.

Program-level hot reload (F5 -> ProgramManager::reloadAllPrograms ->
RenderPass::onHotReload) is NOT reachable from the Python API in headless
mode: Mogwai binds no script function for reloadAllPrograms / onHotReload,
and headless mode has no window to inject the F5 key event. The equivalent
driven here (scene reload + graph re-add) exercises the same pass-side
recovery paths in LumenGI (setScene/createTraceProgram, onHotReload-style
program/var invalidation, resetHistory). Manual windowed F5 check is
documented as the remaining gap in the phase report.
"""

FRAME_RATE = 60


def run_frames(name, frame_count, resolution, caption):
    m.resizeFrameBuffer(*resolution)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.frameCapture.baseFilename = name
    for _ in range(frame_count):
        m.clock.frame = m.clock.frame + 1
        m.renderFrame()
    m.frameCapture.capture()


def load_lumen_graph():
    m.script("scripts/LumenGI.py")
    g = m.getGraph("LumenGI")
    if g is None:
        raise RuntimeError("LumenGI graph not found after scripts/LumenGI.py")
    m.setActiveGraph(g)
    return g


# 1. Baseline: load graph + Cornell Box.
g = load_lumen_graph()
m.loadScene("test_scenes/cornell_box.pyscene")
run_frames("hotreload-baseline", 8, (1280, 720), "baseline")

# 2. Resize down and back up.
run_frames("hotreload-resize-small", 4, (960, 540), "resize 960x540")
run_frames("hotreload-resize-restore", 4, (1280, 720), "resize restore")

# 3. Scene reload (same scene, new Scene instance).
m.loadScene("test_scenes/cornell_box.pyscene")
run_frames("hotreload-scene-reload", 8, (1280, 720), "scene reload")

# 4. Scene switch to Arcade and back.
m.loadScene("Arcade/Arcade.pyscene")
run_frames("hotreload-scene-arcade", 8, (1280, 720), "scene switch Arcade")
m.loadScene("test_scenes/cornell_box.pyscene")
run_frames("hotreload-scene-restore", 8, (1280, 720), "scene switch back")

# 5. Graph reload equivalent: re-add fresh graph with same name.
g2 = load_lumen_graph()
run_frames("hotreload-graph-reload", 8, (1280, 720), "graph reload")

exit()
