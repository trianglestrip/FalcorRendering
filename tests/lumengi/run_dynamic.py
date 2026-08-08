"""LumenGI S1 Gate validation asset: dynamic regression skeleton.

Role / purpose
--------------
Agent F (Test-Tooling) verification asset, run by root on GPU (Mogwai headless).
Exercises the scene-update / history-reset path of LumenGI:
  * fixed camera trajectory: step-by-step translation AND orbit (script loop
    mutating camera.position/target), so history invalidation is hit
    continuously;
  * camera cut: an instantaneous jump to a distant pose;
  * moving light: PointLight.position stepped per frame (runtime light-move
    API, see test_LumenGILighting.py for the intensity-step precedent).
For every frame it prints diffuseGI statistics plus the inter-frame change
rate (mean abs diff between consecutive frames), and asserts the S1
invariants: no NaN/Inf, no negative radiance, and no unbounded energy growth
(last static-frame mean < first static-frame mean * threshold).

Status: RUN-ONLY. Prints stats and VERDICT lines, never exits non-zero; no
golden-image comparison (that is S1-C2's image-test round, task.md 6).

Usage (run by root, from the repo root)
---------------------------------------
    build\windows-vs2022\bin\Release\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\lumengi\run_dynamic.py ^
      --logfile artifacts\lumengi\S1\dynamic.log
(create artifacts\lumengi\S1 first.)

Determinism contract
--------------------
* Fixed camera path: constants below; orbit is computed around the initial
  target with fixed radius/step angles. Frame rate 60, frame counter
  advanced manually, resolution 640x360.
* Fixed seed: LumenGI has no host seed property (PRNG seeded per pixel and
  frame index internally); dynamic sequences are therefore repeatable in
  structure but not bit-exact across runs -- acceptable for this regression.
* Exposure: linear HDR outputs, no ToneMapper in this graph (fixed exposure
  is only needed for the PathTracer reference comparison, see run_reference.py).

Verified APIs used
------------------
* camera.position / camera.target / camera.up / camera.focalLength
  (Camera pybind def_property, Camera.cpp:421-435)
* m.scene.getLight("LumenGITestPointLight").position
  (Scene::getLightByName, Scene.cpp:4336; PointLight.position, Light.cpp:415)
* m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()

Known pitfalls encoded here
---------------------------
* After every scene mutation (camera/light), the pass needs a few frames to
  observe the update and reset history; per-state stats are sampled at the
  LAST frame of each state's frame block.
* Moving the camera by writing position/target triggers IScene::UpdateFlags;
  the pass resets its history on scene updates (LumenGI.cpp onSceneUpdates).
"""

from falcor import *
import math
import os

FRAME_RATE = 60
RESOLUTION = (640, 360)

# --- Camera trajectory constants (phase A: cornell_box, emissive-lit) ---
CAM_START_POS = float3(0, 0.28, 1.2)
CAM_START_TARGET = float3(0, 0.28, 0)
CAM_UP = float3(0, 1, 0)
CAM_FOCAL_LENGTH = 35.0

ORBIT_STEPS = 6          # orbit frames
ORBIT_STEP_DEG = 8.0     # degrees of orbit per frame
SLIDE_STEPS = 6          # translation frames
SLIDE_DELTA = float3(0.02, 0.01, -0.015)
CUT_POS = float3(0.25, 0.4, 0.5)    # camera cut destination
CUT_TARGET = float3(0.15, 0.2, -0.1)

STATIC_FRAMES = 16       # fixed camera: energy-divergence window
ENERGY_GROWTH_THRESHOLD = 5.0  # last_mean < first_mean * threshold

# --- Light movement constants (phase B: cornell_pointlight scene) ---
LIGHT_MOVE_STEPS = 5
LIGHT_MOVE_DELTA = float3(0.03, -0.02, 0.02)

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"
SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)

prev_gi = None


def create_lumen_graph():
    graph = RenderGraph("LumenGIDynamic")
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


def render_one(label):
    """Render one frame and return (stats-dict, per-pixel diff vs previous)."""
    global prev_gi
    m.clock.frame += 1
    m.renderFrame()
    gi = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
    # RGB only: alpha channel is hardcoded to 1.0 in the trace shader
    # (LumenHardwareTrace.rt.slang:318 float4(diffuseGI, 1.f)).
    gi = gi[..., :3]
    vmin = float(gi.min())
    vmax = float(gi.max())
    vmean = float(gi.mean())
    finite = math.isfinite(vmin) and math.isfinite(vmax)
    nonneg = vmin >= 0.0
    change = None
    if prev_gi is not None and prev_gi.shape == gi.shape:
        change = float(abs(gi - prev_gi).mean())
    prev_gi = gi
    print(
        "DYNAMIC", label,
        "frame", m.clock.frame,
        "min", vmin,
        "max", vmax,
        "mean", vmean,
        "finite", finite,
        "nonnegative", nonneg,
        "framediff", change,
    )
    return {"min": vmin, "max": vmax, "mean": vmean, "finite": finite, "nonneg": nonneg}


def render_block(label, count):
    stats = None
    for _ in range(count):
        stats = render_one(label)
    return stats


def orbit_camera(camera, angle_deg):
    """Orbit camera.position around camera.target on the Y axis."""
    rad = math.radians(angle_deg)
    r = camera.position - camera.target
    rx = r.x * math.cos(rad) - r.z * math.sin(rad)
    rz = r.x * math.sin(rad) + r.z * math.cos(rad)
    camera.position = camera.target + float3(rx, r.y, rz)


m.addGraph(create_lumen_graph())
m.resizeFrameBuffer(*RESOLUTION)
m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.time = 0
m.clock.pause()
m.clock.frame = 0

# ---------------------------------------------------------------- Phase A ---
m.loadScene(SCENE_CORNELL)
camera = m.scene.camera
camera.position = CAM_START_POS
camera.target = CAM_START_TARGET
camera.up = CAM_UP
camera.focalLength = CAM_FOCAL_LENGTH

render_block("warmup", 8)

# Fixed camera: energy-divergence window (S1 invariant: static energy plateau).
static_first = render_one("static-first")
static_first_mean = static_first["mean"]
for _ in range(STATIC_FRAMES - 1):
    static_last = render_one("static")
static_last_mean = static_last["mean"]
static_ok = static_last_mean < static_first_mean * ENERGY_GROWTH_THRESHOLD
print("DYNAMIC VERDICT static-energy-plateau PASS" if static_ok else "DYNAMIC VERDICT static-energy-plateau FAIL",
      "first_mean", static_first_mean, "last_mean", static_last_mean, "growth_ratio", (static_last_mean / static_first_mean) if static_first_mean > 0 else "n/a")

# Camera trajectory: orbit then slide (history invalidation per frame).
for i in range(ORBIT_STEPS):
    orbit_camera(camera, ORBIT_STEP_DEG)
    render_block("orbit-%d" % i, 1)
for i in range(SLIDE_STEPS):
    camera.position = camera.position + SLIDE_DELTA
    camera.target = camera.target + SLIDE_DELTA
    render_block("slide-%d" % i, 1)

# Camera cut: instantaneous jump -> one-frame history reset expected.
camera.position = CUT_POS
camera.target = CUT_TARGET
cut_stats = render_block("camera-cut", 4)

# ---------------------------------------------------------------- Phase B ---
# Moving analytic light (only possible on the point-light scene).
m.loadScene(SCENE_POINTLIGHT)
prev_gi = None
point_light = m.scene.getLight("LumenGITestPointLight")
print("DYNAMIC light:", point_light.name, "position", point_light.position)
render_block("light-warmup", 4)
for i in range(LIGHT_MOVE_STEPS):
    point_light.position = point_light.position + LIGHT_MOVE_DELTA
    render_block("light-move-%d" % i, 1)

# ---------------------------------------------------------------- Verdicts ---
verdicts = [
    ("no NaN/Inf", static_last["finite"] and cut_stats["finite"]),
    ("no negative radiance", static_last["nonneg"] and cut_stats["nonneg"]),
    ("static energy does not diverge", static_ok),
]
for name, ok in verdicts:
    print("DYNAMIC VERDICT", name, "PASS" if ok else "FAIL")

exit()
