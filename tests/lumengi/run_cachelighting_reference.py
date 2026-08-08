"""LumenGI S3 Gate reference asset (SKELETON): cache-lighting vs hit-lighting.

Role / purpose
--------------
Agent V (S3 Gate reference-compare asset, pure code/script round). S3 gate
requires "Surface Cache direct lighting 与 hit-lighting reference 在容差内"
(task.md 8, S3 gate bullet 1; task.md 15.1 reference protocol). LumenGI has two
paths that estimate a radiance field at a surface point:

  (a) S1 HWRT per-pixel hit lighting  -> LumenGI.diffuseRadianceHitDist
      (screen-res RGBA16F; RGB = unmodulated diffuse radiance, A = hit distance,
       LumenHardwareTrace.rt.slang:332).
  (b) S3 cache lighting direct term   -> LumenGI.cacheDirectRadiance
      (atlas-res RGBA16F; blitted from the radiance atlas whose RGB = average
       DIRECT radiance per cache texel, LumenGI.cpp:1415-1430,
       LumenSurfaceCacheLightingData.slang:302-306).

This script renders BOTH paths in the SAME graph / SAME frame (so the two
channels see identical camera, scene, lights, frame index and cache state) and
dumps the raw numpy arrays to artifacts/lumengi/S3/reference-compare/.
The offline comparison (tests/lumengi/compare_cachelighting.py, pure python)
reads them. GPU detail of the domain-mapping resample is left as S3_TODO below.

STATUS: SKELETON, runnable TODAY against the current integration state. When
the cacheDirectRadiance channel (or the S3_TODO resample output) is absent the
script degrades gracefully (prints hints + saves whatever exists + SKIP notes)
instead of crashing, matching the run_cachelighting.py probe convention. When
root integrates S3-B1 this becomes a real reference asset with only the
S3_TODO freeze points below.

Domain-mapping / comparison scheme (the core of this asset)
-----------------------------------------------------------
cacheDirectRadiance is a WORLD-SPACE atlas: 4096^2 texels, one texel per card
surface element, position/size determined by the card scene and page table, NOT
by the camera. diffuseRadianceHitDist is SCREEN-SPACE: one texel per pixel, a
camera-dependent subset of the same world surfaces. Direct per-pixel arithmetic
between the two arrays is therefore NOT meaningful. Chosen comparison scheme:

  PRIMARY (screen-aligned resample, GPU side -> S3_TODO):
    For every screen pixel, reconstruct the world position P (unproject
    GBufferRT.linearZ through the inverse view-projection; linearZ is already a
    LumenGI input), query the card scene world->card mapping to get the card
    local UV, then the page table (card -> atlas tile) to fetch the matching
    cacheDirectRadiance texel. Write a screen-res output
    "cacheDirectResampled" (RGB = cache direct radiance, A = lookup-valid).
    The result is pixel-aligned with diffuseRadianceHitDist and the offline
    analysis can run per-pixel relative RMSE / mean abs / quantiles / coverage.
    This is exactly the lookup LumenGI already needs for S4-A3
    (task.md 9, "Surface Cache Lookup Host"): same card-scene query, same
    page table. GPU detail (new optional output / debug-mode hook in LumenGI,
    tile padding edge handling, empty-atlas fallback) is S3_TODO by root +
    Agent M.
  FALLBACK (offline distribution/pre-flight, works with today's raw dumps):
    compare coverage and energy statistics domain-by-domain on the two raw
    arrays, masked per-domain by lit/non-zero texels, and (in
    compare_cachelighting.py) label every cross-domain number as NON-aligned /
    pre-flight, NOT gate-able. The gate itself requires the resample.

  Offline world->atlas reprojection in pure python is NOT recommended as the
  primary path: the world->card->atlas mapping (LumenCardScene transforms,
  LumenSurfaceCache page table) is Lumen-internal and would have to be
  re-implemented in numpy, duplicating the S3_TODO GPU pass for no benefit.

Transport-semantics caveat (S3_TODO alignment with root):
  As wired today cacheDirectRadiance carries the DIRECT term while
  diffuseRadianceHitDist carries the one-bounce INDIRECT term
  (LumenHardwareTrace.rt.slang:326: payload.radiance is the scattered diffuse
  transport). An energy comparison between the two is only physically
  meaningful once both channels evaluate the SAME transport at the surface
  point. Two compatible alignments (root picks at freeze time):
    - extend the reference side: add a per-pixel DIRECT-lighting reference
      channel (per-pixel LightBVH/Emissive/EnvMap NEE, i.e. the same samplers
      S3-B1 uses) and compare cache-direct against it; or
    - extend the cache side: expose the atlas INDIRECT term (the radiance
      atlas A channel, LumenSurfaceCacheLightingData.slang:302-306) alongside
      cacheDirectRadiance and compare that against diffuseRadianceHitDist.
  The metric machinery in compare_cachelighting.py is transport-agnostic: it
  compares whatever two RGB channels are fed to it. This script dumps
  cacheDirectRadiance + diffuseRadianceHitDist exactly as the brief requests;
  the S3_TODO alignment determines which comparison is the gate.

S3_TODO list (integration interface alignment; resolved by root + Agent M)
---------------------------------------------------------------------------
* S3_TODO: CONFIRM the cache-lighting toggle property ("useCacheLighting",
  LumenGI.cpp:53; unknown props only warn, LumenGI.cpp:200, so pre-S3 it is a
  no-op). Mirror of run_cachelighting.py.
* S3_TODO: CONFIRM cacheDirectRadiance remains the atlas channel name
  (LumenGI.h:243) and that RGB = direct radiance at atlas resolution.
* S3_TODO: implement the "cacheDirectResampled" screen-aligned resample output
  (primary comparison scheme above) and wire the S3_TODO hook in this script.
* S3_TODO: freeze the transport alignment (direct-vs-direct or
  indirect-vs-indirect, see caveat above) so the gate comparison is
  physically meaningful.
* S3_TODO: freeze the capture-frame policy (which warmed-up frame the arrays
  are read from) and confirm CACHE_WARMUP_FRAMES / SETTLE_FRAMES give a
  converged cache-direct atlas.

Usage (run by root on GPU, from the repo root, after S3 integration)
--------------------------------------------------------------------
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_cachelighting_reference.py ^
      --logfile artifacts\\lumengi\\S3\\reference-compare\\cachelighting-ref.log
(create artifacts\\lumengi\\S3\\reference-compare first. Pre-S3 the same
command is expected to produce SKIP notes and a warning, which is the
intended fast-fail.)

Scenes / loading
----------------
* cornell_box         : "test_scenes/cornell_box.pyscene" (media; emissive
                        "Light" quad is the only light).
* cornell_pointlight  : tests/lumengi/scenes/cornell_pointlight.pyscene, loaded
                        by ABSOLUTE PATH (run_analytic.py header caveat); its
                        only light is the analytic "LumenGITestPointLight".
One of these scenes has only an emissive light, the other only an analytic
light, so the cache-direct vs hit-lighting comparison covers both S3-B1
sampler classes (EmissiveLightSampler / LightBVHSampler).

Outputs (written by root on GPU; read by compare_cachelighting.py)
-------------------------------------------------------------------
artifacts/lumengi/S3/reference-compare/
  <tag>_diffuseRadianceHitDist.npy     screen-res (H, W, 4) float32 raw dump
  <tag>_cacheDirectRadiance.npy        atlas-res (A, A, 4) float32 raw dump
  <tag>_cacheDirectResampled.npy       S3_TODO: screen-res aligned dump
  cachelighting_manifest.json          sidecar with per-scene config/status
"""

from falcor import *
import os
import json

FRAME_RATE = 60
RESOLUTION = (640, 360)

# Atlas size, frozen by kLumenSurfaceCacheDefaultAtlasSize (LumenSurfaceCache.h:61)
# and re-normalized to whole tiles (LumenGI.cpp:156-158). Passed explicitly so
# the contract survives a future default change.
ATLAS_SIZE_TEXELS = 4096

# Frames rendered after scene setup before sampling: the S2 capture scheduler is
# budget-limited (default 64 pages/frame, LumenCaptureScheduler.h:106) and the
# S3-A1 lighting scheduler updates a limited page set per frame, so 64 frames
# is the S3-C1 convention (run_cachelighting.py CACHE_WARMUP_FRAMES).
# S3_TODO: freeze once S3-A1 budget is known.
CACHE_WARMUP_FRAMES = 64

# Extra frames after warmup before reading the arrays (S3_A1 settle lag).
# S3_TODO: freeze with root.
SETTLE_FRAMES = 8

# S3_TODO: confirm the cache-lighting toggle property name with root (mirror of
# run_cachelighting.py; unknown props only warn, LumenGI.cpp:200).
CACHE_LIGHTING_TOGGLE = "useCacheLighting"
USE_CACHE_LIGHTING = True

# Channels (see header). cacheDirectRadiance is the S3 gate channel;
# diffuseRadianceHitDist is the S1 HWRT hit-lighting reference.
HIT_REFERENCE_CHANNEL = "diffuseRadianceHitDist"
CACHE_DIRECT_CHANNEL = "cacheDirectRadiance"
# S3_TODO: optional screen-aligned resample output produced by a future LumenGI
# debug-mode hook (primary comparison scheme in the header). Absent today ->
# the script skips it without failing.
RESAMPLE_CHANNEL = "cacheDirectResampled"

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"
SCENE_POINTLIGHT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "cornell_pointlight.pyscene")
)

OUT_DIR = "artifacts/lumengi/S3/reference-compare"

# Same fixed camera as run_reference.py / run_pathreference.py (survives scene
# file edits).
FIXED_CAMERA_POSITION = float3(0, 0.28, 1.2)
FIXED_CAMERA_TARGET = float3(0, 0.28, 0)
FIXED_CAMERA_UP = float3(0, 1, 0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0


def json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if value == value and value not in (float("inf"), float("-inf")) else None
    if isinstance(value, (list, tuple)):
        return [json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): json_safe(item) for key, item in value.items()}
    return str(value)


def write_json(path, payload):
    path = os.path.abspath(path)
    out_dir = os.path.dirname(path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(json_safe(payload), f, indent=2, sort_keys=True, allow_nan=False)
        f.write("\n")
    os.replace(temp, path)


def build_graph(extra_outputs):
    """GBufferRT -> LumenGI with useSurfaceCache + reserved cache-lighting
    toggle. Marks the extra outputs (hit reference is always marked)."""
    graph = RenderGraph("LumenGICacheRef")
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
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "useSurfaceCache": True,
                "surfaceCacheAtlasSize": ATLAS_SIZE_TEXELS,
                # useCacheLighting is S3_TODO (see header); unknown props only warn.
                CACHE_LIGHTING_TOGGLE: USE_CACHE_LIGHTING,
            },
        ),
        "LumenGI",
    )
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
    # Always: the S1 HWRT hit-lighting reference (screen-res).
    graph.markOutput("LumenGI." + HIT_REFERENCE_CHANNEL)
    for name in extra_outputs:
        # S3_TODO: cacheDirectRadiance may not exist pre-S3 (graph compile
        # fails on the first render and we fall back; see probe below). The
        # RESAMPLE_CHANNEL is likewise absent until root implements it.
        graph.markOutput("LumenGI." + name)
    return graph


def _setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0
    camera = m.scene.camera
    camera.position = FIXED_CAMERA_POSITION
    camera.target = FIXED_CAMERA_TARGET
    camera.up = FIXED_CAMERA_UP
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH


def probe_graph(scene_path):
    """Try to run with the most complete output set first, fall back when a
    channel (or graph compile) is missing. Returns (available_map, graph).
    available_map: {channel: bool}."""
    # Order matters: most complete first, least complete last.
    candidates = (
        (CACHE_DIRECT_CHANNEL, RESAMPLE_CHANNEL),
        (CACHE_DIRECT_CHANNEL,),
        (),
    )
    last_exc = None
    for extra in candidates:
        try:
            graph = build_graph(extra)
            m.addGraph(graph)
            m.setActiveGraph(graph)
            _setup_scene(scene_path)
            m.clock.frame = 1
            m.renderFrame()
            available = {
                CACHE_DIRECT_CHANNEL: CACHE_DIRECT_CHANNEL in extra,
                RESAMPLE_CHANNEL: RESAMPLE_CHANNEL in extra,
            }
            print(
                "CACHEREF probe ok", list(extra),
                "| cacheDirectRadiance", available[CACHE_DIRECT_CHANNEL],
                "| cacheDirectResampled", available[RESAMPLE_CHANNEL],
            )
            return available, graph
        except Exception as exc:  # pragma: no cover - pre-S3 path
            last_exc = exc
            try:
                m.removeGraph(graph)
            except Exception:
                pass
            print(
                "CACHEREF WARNING output set %s unavailable (pre-S3 expected): %s"
                % (list(extra), str(exc))
            )
    print(
        "CACHEREF WARNING all output sets failed; last error: %s "
        "(graph degraded to hit-reference only)" % str(last_exc)
    )
    graph = build_graph(())
    m.addGraph(graph)
    m.setActiveGraph(graph)
    _setup_scene(scene_path)
    m.clock.frame = 1
    m.renderFrame()
    return {CACHE_DIRECT_CHANNEL: False, RESAMPLE_CHANNEL: False}, graph


def read_channel(channel, available):
    """Read a marked output as float32 numpy (H, W, C). None when unavailable."""
    if not available:
        return None
    try:
        arr = m.activeGraph.get_output("LumenGI." + channel).to_numpy()
        return arr.astype("float32")
    except Exception as exc:
        print("CACHEREF WARNING read %s failed: %s" % (channel, str(exc)))
        return None


def render_and_dump(scene_tag, available):
    """Render warmup + settle frames, then dump every channel to npy.
    Returns a per-channel dump summary dict."""
    for _ in range(CACHE_WARMUP_FRAMES + SETTLE_FRAMES):
        m.clock.frame += 1
        m.renderFrame()

    import numpy as np

    records = {}
    for channel in (HIT_REFERENCE_CHANNEL, CACHE_DIRECT_CHANNEL, RESAMPLE_CHANNEL):
        ok = channel == HIT_REFERENCE_CHANNEL or available.get(channel, False)
        arr = read_channel(channel, ok)
        if arr is None:
            records[channel] = None
            continue
        fname = "%s_%s.npy" % (scene_tag, channel)
        np.save(os.path.join(OUT_DIR, fname), arr)
        rec = {
            "file": fname,
            "shape": list(arr.shape),
            "min": float(arr.min()),
            "max": float(arr.max()),
            "mean": float(arr.mean()),
        }
        records[channel] = rec
        print(
            "CACHEREF saved", fname,
            "shape", arr.shape,
            "min", rec["min"],
            "max", rec["max"],
            "mean", rec["mean"],
        )
    return records


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    manifest = {
        "script": "run_cachelighting_reference.py",
        "status": "skeleton",
        "resolution": list(RESOLUTION),
        "atlas_size_texels": ATLAS_SIZE_TEXELS,
        "warmup_frames": CACHE_WARMUP_FRAMES,
        "settle_frames": SETTLE_FRAMES,
        "cache_lighting_toggle": CACHE_LIGHTING_TOGGLE,
        "camera": {
            "position": [0, 0.28, 1.2],
            "target": [0, 0.28, 0],
            "up": [0, 1, 0],
            "focal_length": 35.0,
        },
        "scenes": {},
    }

    for scene_tag, scene_path in (
        ("cornell", SCENE_CORNELL),
        ("cornell_pointlight", SCENE_POINTLIGHT),
    ):
        print("CACHEREF scene", scene_tag, scene_path)
        available, graph = probe_graph(scene_path)
        records = render_and_dump(scene_tag, available)
        manifest["scenes"][scene_tag] = {
            "scene_path": scene_path,
            "channels": records,
        }
        if not available[CACHE_DIRECT_CHANNEL]:
            print(
                "CACHEREF VERDICT", scene_tag,
                "SKIP (cacheDirectRadiance channel unavailable, pre-S3)"
            )
        if not available[RESAMPLE_CHANNEL]:
            print(
                "CACHEREF NOTE", scene_tag,
                "cacheDirectResampled absent (S3_TODO resample); offline "
                "comparison will be DISTRIBUTION/pre-flight only, not aligned"
            )
        m.removeGraph(graph)

    write_json(os.path.join(OUT_DIR, "cachelighting_manifest.json"), manifest)
    print("CACHEREF wrote", os.path.abspath(os.path.join(OUT_DIR, "cachelighting_manifest.json")))
    print("CACHEREF done")


main()
exit()
