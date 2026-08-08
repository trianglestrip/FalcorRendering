"""LumenGI S1 Gate validation asset: analytic point-light path check.

Role / purpose
--------------
Agent F (Test-Tooling) verification asset, run by root on GPU (Mogwai headless).
Verifies that the LumenGI indirect path samples analytic (point) lights:
  1. With the analytic light enabled, diffuseGI is non-zero.
  2. With renderSettings.useAnalyticLights=False (all other lights also off
     in this scene), diffuseGI drops to ~zero ("black scene" invariant).
Renders >= 8 frames after every renderSettings change before sampling,
because the scene's active-light list can lag the settings change by a few
frames (Scene::beginFrame detects the settings diff and Scene::update
refreshes active lights; the pass observes the updated scene each frame).

Status: RUN-ONLY. Prints stats and VERDICT lines but never exits non-zero;
root evaluates the verdicts at the S1 Gate.

Usage (run by root, from the repo root)
---------------------------------------
    build\windows-vs2022\bin\Release\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\lumengi\run_analytic.py ^
      --logfile artifacts\lumengi\S1\analytic.log
(create artifacts\lumengi\S1 first.)

Scene / loading
---------------
* New scene: tests/lumengi/scenes/cornell_pointlight.pyscene -- a Cornell Box
  whose ONLY light source is the analytic PointLight "LumenGITestPointLight"
  (the emissive panel of the original cornell_box was removed on purpose so
  the analytic-off state can actually reach ~zero).
* The scene lives outside media/ (media is a packman link; no writes there)
  and is loaded by ABSOLUTE PATH. AssetResolver::resolvePath accepts absolute
  existing paths (findFileInDirectories, Source/Falcor/Core/Platform/OS.cpp).
  Alternative: FALCOR_MEDIA_FOLDERS=tests\lumengi\scenes;... set BEFORE Mogwai
  starts (it is read once at static init, too late to set from the script).

Verified APIs used (pybind, from source)
----------------------------------------
* scene.renderSettings.useAnalyticLights = bool
  (Scene::RenderSettings def_readwrite, Scene.cpp:4293; scene.renderSettings
  def_property get/set, Scene.cpp:4332)
* m.scene.getLight("LumenGITestPointLight") -> PointLight
  (Scene::getLightByName overload, Scene.cpp:4336; Light.position property
  def_property, Light.cpp:415)
* SceneBuilder.addLight(light) in the pyscene
  (SceneBuilder.cpp:2993; there is NO "addAnalyticLight" binding anywhere)

Known pitfalls encoded here
---------------------------
* Settings change -> active-light refresh lag: render >= 8 frames after each
  toggle before sampling (TOGGLE_WARMUP_FRAMES = 8).
* renderSettings is a value type; direct property writes work (proven in
  run_toggle.py), the full-object reassign pattern is also documented in
  test_LumenGILighting.py. Direct writes are used here.
"""

from falcor import *
import os

FRAME_RATE = 60
RESOLUTION = (640, 360)

# Frames rendered after each renderSettings change before stats sampling.
TOGGLE_WARMUP_FRAMES = 8

# Analytic-on mean must be above this absolute level to prove the analytic
# path is actually contributing.
ON_MIN_MEAN = 1e-4

# Analytic-off mean must be below this absolute level ("~zero").
OFF_MAX_MEAN = 1e-3

# Also report the ratio on_mean / off_mean; the strict gate is absolute.

SCENE_PATH = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)


def create_lumen_graph():
    graph = RenderGraph("LumenGIAnalytic")
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


def diffuse_gi_stats(label):
    import math

    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
    # RGB only: alpha channel is hardcoded to 1.0 in the trace shader
    # (LumenHardwareTrace.rt.slang:318 float4(diffuseGI, 1.f)); including it
    # would bias the mean by +0.25 and defeat the analytic-off "~zero" gate.
    gi = gi[..., :3]
    vmin = float(gi.min())
    vmax = float(gi.max())
    vmean = float(gi.mean())
    finite = math.isfinite(vmin) and math.isfinite(vmax)
    nonneg = vmin >= 0.0
    print(
        "ANALYTIC",
        label,
        "min", vmin,
        "max", vmax,
        "mean", vmean,
        "finite", finite,
        "nonnegative", nonneg,
    )
    return vmin, vmax, vmean, finite, nonneg


def render_and_stats(label, frames):
    stats = None
    for _ in range(frames):
        m.clock.frame += 1
        m.renderFrame()
        stats = diffuse_gi_stats(label)
    return stats


m.addGraph(create_lumen_graph())
m.loadScene(SCENE_PATH)
m.resizeFrameBuffer(*RESOLUTION)
m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.time = 0
m.clock.pause()
m.clock.frame = 0

point_light = m.scene.getLight("LumenGITestPointLight")
print("ANALYTIC scene:", SCENE_PATH)
print("ANALYTIC point light:", point_light.name, "active:", point_light.active, "position:", point_light.position, "intensity:", point_light.intensity)
print("ANALYTIC initial renderSettings:", m.scene.renderSettings)

# State 1: all lights on -> analytic path must be non-zero.
m.scene.renderSettings.useEnvLight = False
m.scene.renderSettings.useEmissiveLights = False
m.scene.renderSettings.useAnalyticLights = True
_, _, on_mean, on_finite, on_nonneg = render_and_stats("analytic-on", TOGGLE_WARMUP_FRAMES)

# State 2: analytic off -> scene is black -> output must be ~zero.
m.scene.renderSettings.useAnalyticLights = False
_, _, off_mean, off_finite, off_nonneg = render_and_stats("analytic-off", TOGGLE_WARMUP_FRAMES)

# State 3: restore, verify the contribution comes back (settings round-trip).
m.scene.renderSettings.useAnalyticLights = True
_, _, on2_mean, on2_finite, on2_nonneg = render_and_stats("analytic-on-again", TOGGLE_WARMUP_FRAMES)

verdicts = [
    ("analytic path non-zero", on_mean > ON_MIN_MEAN),
    ("analytic off ~zero", off_mean < OFF_MAX_MEAN),
    ("analytic restore non-zero", on2_mean > ON_MIN_MEAN),
    ("no NaN/Inf", on_finite and off_finite and on2_finite),
    ("no negative radiance", on_nonneg and off_nonneg and on2_nonneg),
]
for name, ok in verdicts:
    print("ANALYTIC VERDICT", name, "PASS" if ok else "FAIL")
print("ANALYTIC VERDICT ratios on/off", (on_mean / off_mean) if off_mean > 0 else "inf")
print("ANALYTIC VERDICT on-again/on", (on2_mean / on_mean) if on_mean > 0 else "n/a")

exit()
