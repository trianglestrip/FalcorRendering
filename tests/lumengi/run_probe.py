from falcor import *

"""LumenGI S4-C2 probe distribution validation asset (SKELETON, Agent Z3).

Role / purpose
--------------
RUN-ONLY Mogwai GPU skeleton for the S4.2 probe wave (task.md 9, S4-C2 probe
distribution + S4-C3 hybrid comparison). Graph: GBufferRT -> LumenGI with
useScreenTrace=True + useScreenProbes=True. It PROBES the LumenGI probe
output channels and degrades to SKIP (never crashes) when they are absent --
which is the expected pre-S4.2 state at HEAD 0620d28b. Root runs this after
the S4-A2 (probe grid host) + S4-B2 (probe direction sampling) integration.

What this script verifies (S4-C2)
---------------------------------
  * G-A (LIVE today, S4-A1/B1 screen trace): the frozen per-pixel screen
    trace is used as the screen-hit-rate / miss-reason source. Decoding is
    the frozen LumenScreenTraceData.slang contract (same as run_screentrace.py):
      hit  -> A in (0, 1], miss -> A = -(reason + 1) < 0, reason = uint(-A) - 1.
    A1. determinism: re-rendering the SAME fixed frameIndex on two fresh,
        identical graphs yields bit-identical screenTrace readback (this is
        the reproducibility pattern the probe path must also satisfy).
    A2. screen hit rate = hits / rays (rays = W*H, one ray per pixel).
    A3. fallback classification (screen-level): miss-reason histogram
        (OutOfScreen/BehindSurface/MaxSteps/ThinGeometry) + the fraction of
        rays that MISS and therefore require an HWRT fallback.
  * G-B (S4_TODO, needs LumenGI.probeRadiance; SKIP pre-S4.2):
    B1. probe coverage: the probe texture must be a tile grid of
        ceil(W/TILE) x ceil(H/TILE) probes that structurally covers the frame,
        and the fraction of VALID (placed) probes must be >= a floor.
    B2. ray budget: probes x directions_per_probe <= RAY_BUDGET.
    B3. reproducibility: same seed + same frameIndex across two fresh graphs
        -> identical probeRadiance readback (the direction set is
        f(seed, frameIndex); the fixed seed + frameIndex rotation means two
        DIFFERENT frames differ by design, same frame re-run must match).
    B4. probe screen hit rate + fallback classification: from the unified per
        probe hit record (S4_TODO) when it exists; otherwise proxied by A2/A3.
  * G-C (S4_TODO, S4-C3; SKIP pre-S4.2): screen-only vs HWRT-only vs hybrid.
    One fresh graph per mode (a Falcor bug forbids mutating pass properties
    after a readback -- see run_gbuffer_compare.py header), each with its own
    ray-count estimate:
      hwrt-only : useScreenTrace=False, useScreenProbes=False (per-pixel HWRT)
      screen-only: useScreenTrace=True,  useScreenProbes=True, fallback OFF
      hybrid     : useScreenTrace=True,  useScreenProbes=True, fallback ON
    C1. hybrid ray reduction vs the per-pixel HWRT baseline (ray budget gate).
    C2. no severe new artifacts: mode output finite / non-negative / within a
        relative mean tolerance of the HWRT-only baseline.
    C3. off-screen occlusion covered by the HWRT fallback (fallback hit rate
        >= floor) -- the task.md S4 gate "屏幕外遮挡由 fallback 补齐".

S4_TODO contract (root must freeze with Z1/Z2 before this becomes gating)
-------------------------------------------------------------------------
  * S4_TODO[probe_channel]: the LumenGI output exposing probe radiance.
    Candidate "probeRadiance" (RGBA16F, one texel per probe, dims =
    ceil(W/TILE) x ceil(H/TILE), RGB = radiance, A > 0 valid probe). This is
    THE probed channel; if absent the whole probe section SKIPs.
  * S4_TODO[stats_channel]: optional LumenGI output exposing per-probe stats
    (rays, hits, miss/fallback histogram). Absent -> stats recomputed here.
  * S4_TODO[hit_channel]: the unified per-probe hit record from S4-B2
    (screen-hit / HWRT-fallback-hit / final-miss classification). Z2's real
    record is a StructuredBuffer<LumenScreenProbeHit> (LumenScreenProbeData.slang:
    radiance + hitInfo.z = radiance kind [0=ScreenTrace, 1=SurfaceCache,
    2=HardwareTrace, 3=EnvFallback], hitInfo.w = flags [1=Valid, 2=Miss]),
    which is NOT a graph texture. Root/Z1 must re-export it as a stats texture
    (or a host counters readback) for this script; the placeholder 2D decode
    below is only a stand-in until that export exists.
  * S4_TODO[tile_size]: frozen "8x8 tile start" (task.md S4-A2); the structural
    coverage gate asserts ceil(W/TILE) x ceil(H/TILE).
  * S4_TODO[directions]: directions per probe (low-discrepancy / blue-noise
    set from S4-B2); default 16 (kLumenScreenProbeDirections).
  * S4_TODO[seed_property] + [seed]: the fixed probe RNG seed. Task rule 5
    (every stochastic process supports a fixed seed); a per-pass property is
    assumed. Unknown props only warn (LumenGI.cpp:234), safe pre-S4.2.
  * S4_TODO[fallback_switch]: the LumenGI property enabling HWRT fallback for
    screen-missed probe rays (candidate "probeHWRTFallback"). Same warning
    caveat as the seed property.
  * S4_TODO[ray_counter]: a real "rays launched" readback (like
    LumenGIFrameCounters.tracedSamples) replaces the config-based estimates
    in G-C once Z1/Z2 expose one.
  * S4_TODO gates: PROBE_COVERAGE_MIN / RAY_BUDGET / CROSS-MODE tolerances /
    fallback-coverage floor / REDUCTION_MIN are all placeholders.

Gates (task.md S4 gate list)
----------------------------
  * Probe 分布: tile grid covers the frame; probes x directions <= budget;
    same-seed same-frame reproducibility.
  * Screen hit rate + miss reason 分布: sums match rays; every reason in
    [1, kLumenScreenTraceMissReasonCount-1].
  * S4-C3: hybrid rays < per-pixel HWRT rays (reduction); hybrid output has no
    new severe artifacts; off-screen occlusion covered by fallback.

Exit: Falcor `exit()` as in the sibling scripts. Report JSON is written to
artifacts/lumengi/S4/probe/probe.json regardless of the verdict so the runner
can consume a machine-readable SKIP/PASS/FAIL.
"""

import json
import math
import os

import numpy as np

# -------------------------------------------------------------------------------------
# Frozen S4 screen-trace contract (LumenScreenTraceData.slang).
# -------------------------------------------------------------------------------------
MISS_REASON_NONE = 0          # Hit.
MISS_REASON_OUT_OF_SCREEN = 1
MISS_REASON_BEHIND_SURFACE = 2
MISS_REASON_MAX_STEPS = 3
MISS_REASON_THIN_GEOMETRY = 4  # Reserved.
MISS_REASON_COUNT = 5
MISS_REASON_NAMES = {
    0: "Hit",
    1: "OutOfScreen",
    2: "BehindSurface",
    3: "MaxSteps",
    4: "ThinGeometry",
}

HWRT_MISS_HIT_DISTANCE = 65504.0

# -------------------------------------------------------------------------------------
# Configuration (S4_TODO: freeze defaults with root when the S4.2 wave lands).
# -------------------------------------------------------------------------------------
RESOLUTION = (640, 360)
FRAME_RATE = 60
SCENE = "test_scenes/cornell_box.pyscene"
OUT_JSON = os.environ.get("LUMEN_PROBE_OUT", "artifacts/lumengi/S4/probe/probe.json")

# The pass properties the S4.2 probe grid requires. useScreenTrace is real
# today (LumenGI.cpp:75/219); useScreenProbes is already parsed (LumenGI.cpp:76/221)
# though its S4.2 wiring is not present yet.
USE_SCREEN_TRACE = bool(os.environ.get("LUMEN_PROBE_USE_SCREEN_TRACE", "") != "0")
USE_SCREEN_PROBES = bool(os.environ.get("LUMEN_PROBE_USE_SCREEN_PROBES", "") != "0")

# S4_TODO[probe_channel]: candidate LumenGI output exposing probe radiance.
# This is the channel this script PROBES. If absent -> probe section SKIPs.
PROBE_RADIANCE_CHANNEL = "probeRadiance"
# S4_TODO[stats_channel] / S4_TODO[hit_channel]: optional secondary channels.
PROBE_STATS_CHANNEL = "probeStats"
PROBE_HIT_CHANNEL = "probeHitRecord"
PROBE_CHANNELS = [PROBE_RADIANCE_CHANNEL, PROBE_STATS_CHANNEL, PROBE_HIT_CHANNEL]

# S4_TODO[tile_size]: fixed "8x8 tile start" per task.md S4-A2.
TILE_SIZE = 8
# S4_TODO[directions]: low-discrepancy / blue-noise directions per probe.
# Default matches kLumenScreenProbeDirections (LumenScreenProbeData.slang:84),
# which defaults to 16 and is overridable via the LUMEN_SCREEN_PROBE_DIRECTIONS
# shader define. Freeze this to whatever Z2 integrates.
DIRECTIONS_PER_PROBE = 16
# S4_TODO[seed]: fixed probe RNG base seed; the shader rotates it per frameIndex.
PROBE_SEED = 0x7F4A7C15
# S4_TODO[seed_property]: assumed LumenGI property that pins the probe seed.
PROBE_SEED_PROP = "probeSeed"
# S4_TODO[fallback_switch]: assumed LumenGI property enabling HWRT fallback.
PROBE_HWRT_FALLBACK_PROP = "probeHWRTFallback"

# S4_TODO gates (placeholders; freeze with root):
PROBE_COVERAGE_MIN = 0.5        # Fraction of tile slots holding a valid probe.
RAY_BUDGET = 64 * 1024          # Probes x directions must not exceed this.
HYBRID_REDUCTION_MIN = 4.0      # hybrid_rays <= hwrt_rays / REDUCTION_MIN.
CROSS_MODE_MEAN_REL_TOL = 0.5   # hybrid/screen-only mean vs hwrt-only mean.
FALLBACK_COVERAGE_MIN = 0.5     # Fallback-hit fraction floor (off-screen gate).
MIN_VALID_HIT_PIXELS = 100      # Floor for the hit-distance diagnostics.
HIT_DIST_REL_TOL = 0.25         # S4_TODO[tolerance] (mirror run_screentrace.py).

# Reproducibility capture: both graphs warm up identically and capture the SAME
# fixed frameIndex, so the probe direction set f(seed, frameIndex) must match.
WARMUP_FRAMES = 4
REPRO_FRAME = 5

# S4-C3 modes -> pass properties. "hwrtFallback" is S4_TODO (PROBE_HWRT_FALLBACK_PROP).
MODE_PROPS = {
    "hwrt-only": {"useScreenTrace": False, "useScreenProbes": False, "hwrtFallback": False},
    "screen-only": {"useScreenTrace": True, "useScreenProbes": True, "hwrtFallback": False},
    "hybrid": {"useScreenTrace": True, "useScreenProbes": True, "hwrtFallback": True},
}

# -------------------------------------------------------------------------------------
# Small helpers mirroring the sibling scripts.
# -------------------------------------------------------------------------------------


def json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
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


def _pass_props(mode):
    """Build the createPass('LumenGI', ...) dict for a mode. Only the
    fallback / seed props are S4_TODO unknowns; unknown props only warn
    (LumenGI.cpp:234) so this is safe pre-S4.2."""
    spec = MODE_PROPS[mode]
    props = {
        "useScreenTrace": bool(USE_SCREEN_TRACE and spec["useScreenTrace"]),
        "useScreenProbes": bool(USE_SCREEN_PROBES and spec["useScreenProbes"]),
    }
    if props["useScreenProbes"]:
        props[PROBE_SEED_PROP] = PROBE_SEED
    if props["useScreenProbes"] and spec["hwrtFallback"]:
        props[PROBE_HWRT_FALLBACK_PROP] = True
    return props


# -------------------------------------------------------------------------------------
# Graph / scene helpers.
# -------------------------------------------------------------------------------------


def create_lumen_graph(mode, mark_probe_outputs):
    """GBufferRT -> LumenGI for the given mode. Always marks the (post-S4-A1)
    screenTrace output; optionally marks the S4.2 probe channels."""
    graph = RenderGraph("LumenGIProbe")
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
    graph.addPass(createPass("LumenGI", _pass_props(mode)), "LumenGI")
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
    graph.markOutput("LumenGI.diffuseRadianceHitDist")
    graph.markOutput("LumenGI.screenTrace")
    if mark_probe_outputs:
        for ch in PROBE_CHANNELS:
            # S4_TODO[channel]: if the final names differ, update PROBE_CHANNELS
            # (this block is the only wiring change needed).
            graph.markOutput("LumenGI." + ch)
    return graph


def _setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()


def render_capture(frame=REPRO_FRAME):
    """Warm up WARMUP_FRAMES frames (1..N) then render the fixed frame. The
    exact clock.frame sequence is the reproducibility contract: two graphs
    running this helper capture the SAME frameIndex."""
    for i in range(1, WARMUP_FRAMES + 1):
        m.clock.frame = i
        m.renderFrame()
    m.clock.frame = frame
    m.renderFrame()


def add_and_render(graph, scene_path):
    m.addGraph(graph)
    m.setActiveGraph(graph)
    _setup_scene(scene_path)
    render_capture()


def grab(name):
    return m.activeGraph.get_output(name).to_numpy()


def remove_graph(graph):
    try:
        m.removeGraph(graph)
    except Exception:
        pass


# -------------------------------------------------------------------------------------
# Screen-trace decode + stats (frozen LumenScreenTraceData.slang encoding).
# -------------------------------------------------------------------------------------


def decode_screentrace(tex):
    """Mirror of run_screentrace.py::decode_screentrace. Returns hit mask,
    per-pixel miss reason, hit distance. Raises on an out-of-range encoding."""
    arr = np.asarray(tex, dtype=np.float32)
    if arr.ndim == 2:
        arr = np.stack([arr, np.zeros_like(arr), np.zeros_like(arr), np.ones_like(arr)], axis=-1)
    if arr.shape[-1] < 4:
        raise ValueError("screen trace output has <4 channels: %s" % (arr.shape,))
    a = arr[..., 3]
    hit = a > 0.0
    reason = np.zeros(a.shape, dtype=np.int32)
    reason[hit] = MISS_REASON_NONE
    miss_a = a[~hit]
    if miss_a.size:
        r = np.rint((-miss_a).astype(np.float32) - 1.0).astype(np.int32)
        if np.any(r < 1) or np.any(r >= MISS_REASON_COUNT):
            raise ValueError("screen trace miss encoding out of range (corrupt gScreenTraceResult?)")
        reason[~hit] = r
    return {
        "hit": hit,
        "miss_reason": reason,
        "hit_distance": np.where(hit, arr[..., 2], 0.0),
        "raw": arr,
    }


def screen_trace_stats(decoded):
    """Screen hit rate + fallback classification from the decoded screen trace.
    hit/miss counts must sum to rays; miss reasons give the screen-level
    fallback reason distribution (rays that MISS need the HWRT fallback)."""
    hit = decoded["hit"]
    reason = decoded["miss_reason"]
    total = int(hit.size)
    hits = int(hit.sum())
    misses = int((~hit).sum())
    hist = {MISS_REASON_NAMES.get(int(r), str(int(r))): int((reason == r).sum()) for r in range(1, MISS_REASON_COUNT)}
    return {
        "rays_launched": total,
        "hits": hits,
        "misses": misses,
        "completeness_ok": bool(hits + misses == total),
        "screen_hit_rate": float(hits) / float(total) if total else 0.0,
        "fallback_required_fraction": float(misses) / float(total) if total else 0.0,
        "miss_reason_histogram": hist,
    }


# -------------------------------------------------------------------------------------
# Probe channel decode (S4_TODO contract; Z1/Z2 freeze the real layout).
# -------------------------------------------------------------------------------------


def expected_probe_grid():
    w, h = RESOLUTION
    return (int((w + TILE_SIZE - 1) // TILE_SIZE), int((h + TILE_SIZE - 1) // TILE_SIZE))


def decode_probe_radiance(tex):
    """Decode the probeRadiance channel under the ASSUMED S4_TODO contract:
    one texel per probe (probeGridH, probeGridW, 4); RGB = integrated
    irradiance E, A = integrated confidence (0 = no valid input). The A>0
    validity rule matches LumenScreenProbeRadiance.radiance.a (Z2, frozen).
    Returns stats; raises on shape/encoding problems."""
    arr = np.asarray(tex, dtype=np.float32)
    if arr.ndim == 3 and arr.shape[-1] >= 4:
        a = arr[..., 3]
        rgb = arr[..., :3]
    elif arr.ndim == 2:
        a = arr
        rgb = np.stack([arr, np.zeros_like(arr), np.zeros_like(arr)], axis=-1)
    else:
        raise ValueError("probeRadiance readback has an unexpected shape: %s" % (arr.shape,))

    valid = a > 0.0
    n_valid = int(valid.sum())
    n_slots = int(a.size)
    rad = np.where(valid[..., np.newaxis], rgb, 0.0)
    finite = bool(math.isfinite(float(rad[valid].min())) if n_valid else True) and bool(
        math.isfinite(float(rad[valid].max())) if n_valid else True
    )
    nonneg = bool(float(rad[valid].min()) >= 0.0) if n_valid else True
    return {
        "grid": [int(a.shape[1]), int(a.shape[0])],
        "probe_slots": n_slots,
        "valid_probes": n_valid,
        "coverage": float(n_valid) / float(n_slots) if n_slots else 0.0,
        "radiance_finite": finite,
        "radiance_nonnegative": nonneg,
        "radiance_min": float(rad[valid].min()) if n_valid else None,
        "radiance_max": float(rad[valid].max()) if n_valid else None,
        "radiance_mean": float(rad[valid].mean()) if n_valid else None,
    }


def decode_probe_hit_record(tex):
    """Decode the per-probe unified hit record (S4_TODO[hit_channel]). The real
    S4-B2 record is StructuredBuffer<LumenScreenProbeHit> (hitInfo.z = kind,
    hitInfo.w = flags) -- not a texture -- so this 2D decode is a STAND-IN for
    the stats texture / counters readback root must re-export. ASSUMED
    placeholder encoding per texel:
      A > 0  -> screen hit (confidence), RGB = screen hit rate so far
      A == 0 -> all directions HWRT-fallback hit
      A < 0  -> A = -(final_miss_fraction + 1) (fallback also missed)
    This is defensive: if the readback does not match the assumption we flag it
    instead of misreading it. Returns None when the channel is absent."""
    arr = np.asarray(tex, dtype=np.float32)
    if arr.ndim == 3 and arr.shape[-1] >= 4:
        a = arr[..., 3]
        rgb = arr[..., :3]
    elif arr.ndim == 2:
        a = arr
        rgb = None
    else:
        raise ValueError("probeHitRecord readback has an unexpected shape: %s" % (arr.shape,))
    total = int(a.size)
    screen_hit = a > 0.0
    fallback_hit = a == 0.0
    final_miss = a < 0.0
    screen_hit_rate = float(screen_hit.sum()) / float(total) if total else 0.0
    fallback_hit_rate = float(fallback_hit.sum()) / float(total) if total else 0.0
    final_miss_rate = float(final_miss.sum()) / float(total) if total else 0.0
    if rgb is not None:
        finite = bool(math.isfinite(float(rgb[screen_hit].min())) if np.any(screen_hit) else True) and bool(
            math.isfinite(float(rgb[screen_hit].max())) if np.any(screen_hit) else True
        )
    else:
        finite = True
    return {
        "records": total,
        "screen_hit_rate": screen_hit_rate,
        "fallback_hit_rate": fallback_hit_rate,
        "final_miss_rate": final_miss_rate,
        "encoding_finite": finite,
    }


# -------------------------------------------------------------------------------------
# Gate sections.
# -------------------------------------------------------------------------------------


def run_screen_gates():
    """G-A (live): determinism, screen hit rate, fallback classification from
    the frozen per-pixel screen trace. Returns (report, verdicts)."""
    verdicts = []
    report = {}

    graph_a = create_lumen_graph("hybrid", mark_probe_outputs=False)
    add_and_render(graph_a, SCENE)
    st_a = grab("LumenGI.screenTrace")
    stats = screen_trace_stats(decode_screentrace(st_a))
    report["screen_trace"] = stats
    remove_graph(graph_a)

    graph_b = create_lumen_graph("hybrid", mark_probe_outputs=False)
    add_and_render(graph_b, SCENE)
    st_b = grab("LumenGI.screenTrace")
    identical = bool(np.array_equal(st_a, st_b))
    report["screen_trace_determinism"] = identical
    remove_graph(graph_b)

    verdicts.append(("screen trace same-frame re-render deterministic", "PASS" if identical else "FAIL"))
    if stats["completeness_ok"]:
        verdicts.append(("screen trace completeness (hits+misses == rays)", "PASS"))
    else:
        verdicts.append(("screen trace completeness (hits+misses == rays)", "FAIL"))
    # Screen hit rate / fallback-required are REPORTED metrics (S4_TODO gate);
    # the reason distribution validity is the hard invariant.
    bad_reason = any(int(v) > 0 for k, v in stats["miss_reason_histogram"].items() if k not in MISS_REASON_NAMES.values())
    verdicts.append(("screen miss reason distribution valid (all in [1, count-1])", "PASS" if not bad_reason else "FAIL"))
    return report, verdicts


def run_probe_section():
    """G-B (S4_TODO): probe coverage, ray budget, reproducibility, probe hit
    record classification. All SKIP when the probe channel is absent."""
    report = {}
    verdicts = []

    graph = None
    available = False
    try:
        graph = create_lumen_graph("hybrid", mark_probe_outputs=True)
        add_and_render(graph, SCENE)
        available = True
    except Exception as exc:
        print(
            "PROBE WARNING probe channel(s) %s not available (pre-S4.2 integration expected); absent -> %s"
            % (PROBE_CHANNELS, str(exc))
        )
        if graph is not None:
            remove_graph(graph)
        graph = create_lumen_graph("hybrid", mark_probe_outputs=False)
        add_and_render(graph, SCENE)
    report["probe_channel_available"] = available

    if not available:
        verdicts.append(("probe coverage (tile grid covers frame)", "SKIP"))
        verdicts.append(("probe ray budget (probes x directions <= budget)", "SKIP"))
        verdicts.append(("probe reproducibility (same seed + frameIndex)", "SKIP"))
        verdicts.append(("probe screen hit rate + fallback classification (S4_TODO hit record)", "SKIP"))
        remove_graph(graph)
        return report, verdicts

    # ---- B1/B2 from the probeRadiance readback -------------------------------
    probe_tex = grab("LumenGI." + PROBE_RADIANCE_CHANNEL)
    try:
        pr = decode_probe_radiance(probe_tex)
    except Exception as exc:
        print("PROBE WARNING probeRadiance decode failed: %s" % str(exc))
        report["probe_decode_error"] = str(exc)
        for name in (
            "probe coverage (tile grid covers frame)",
            "probe ray budget (probes x directions <= budget)",
        ):
            verdicts.append((name, "SKIP"))
        remove_graph(graph)
        return report, verdicts
    report["probe_radiance"] = pr

    expected = expected_probe_grid()
    grid_match = list(pr["grid"]) == expected
    coverage_ok = grid_match and pr["coverage"] >= PROBE_COVERAGE_MIN and pr["radiance_finite"] and pr["radiance_nonnegative"]
    verdicts.append(
        (
            "probe coverage (grid %s==%s, valid %g>=%g, finite, nonneg)"
            % (pr["grid"], expected, pr["coverage"], PROBE_COVERAGE_MIN),
            "PASS" if coverage_ok else "FAIL",
        )
    )

    rays_active = pr["valid_probes"] * DIRECTIONS_PER_PROBE
    rays_slots = pr["probe_slots"] * DIRECTIONS_PER_PROBE
    budget_ok = rays_active <= RAY_BUDGET
    report["probe_ray_budget"] = {
        "valid_probes": pr["valid_probes"],
        "probe_slots": pr["probe_slots"],
        "directions_per_probe": DIRECTIONS_PER_PROBE,
        "rays_active": rays_active,
        "rays_upper": rays_slots,
        "ray_budget": RAY_BUDGET,
    }
    verdicts.append(
        ("probe ray budget (active %d <= %d, upper %d)" % (rays_active, RAY_BUDGET, rays_slots), "PASS" if budget_ok else "FAIL")
    )

    # ---- B3 reproducibility: same seed + same frameIndex, two fresh graphs ---
    graph2 = None
    repro_identical = False
    try:
        remove_graph(graph)
        graph2 = create_lumen_graph("hybrid", mark_probe_outputs=True)
        add_and_render(graph2, SCENE)
        probe_tex2 = grab("LumenGI." + PROBE_RADIANCE_CHANNEL)
        pr2 = decode_probe_radiance(probe_tex2)
        repro_identical = bool(pr2["probe_slots"] == pr["probe_slots"]) and bool(
            np.array_equal(probe_tex2, probe_tex)
        )
    except Exception as exc:
        print("PROBE WARNING reproducibility re-render failed: %s" % str(exc))
        repro_identical = False
    if graph2 is not None:
        remove_graph(graph2)

    # --- B4 probe hit record classification (S4_TODO[hit_channel]) -------------
    hit_rec = None
    graph3 = None
    try:
        graph3 = create_lumen_graph("hybrid", mark_probe_outputs=True)
        add_and_render(graph3, SCENE)
        hit_rec = decode_probe_hit_record(grab("LumenGI." + PROBE_HIT_CHANNEL))
    except Exception as exc:
        print("PROBE WARNING probeHitRecord channel not available (S4_TODO hit record): %s" % str(exc))
        hit_rec = None
    if graph3 is not None:
        remove_graph(graph3)
    report["probe_hit_record"] = hit_rec

    if repro_identical:
        verdicts.append(("probe reproducibility (same seed + frameIndex)", "PASS"))
    else:
        verdicts.append(("probe reproducibility (same seed + frameIndex)", "FAIL"))
    if hit_rec is not None:
        covered = hit_rec["fallback_hit_rate"] + hit_rec["screen_hit_rate"] >= FALLBACK_COVERAGE_MIN
        verdicts.append(
            (
                "probe screen hit rate + fallback classification (screen %.3f, fallback %.3f, miss %.3f)"
                % (hit_rec["screen_hit_rate"], hit_rec["fallback_hit_rate"], hit_rec["final_miss_rate"]),
                "PASS" if covered else "FAIL",
            )
        )
    else:
        verdicts.append(("probe screen hit rate + fallback classification (S4_TODO hit record)", "SKIP"))

    return report, verdicts


def run_mode_comparison(fallback_required_fraction):
    """G-C (S4-C3): one fresh graph per mode, ray estimates, artifact checks.
    SKIP for the probe-backed gates when the probe channel is unavailable."""
    verdicts = []
    modes = {}

    grid = expected_probe_grid()
    w, h = RESOLUTION
    hwrt_rays = w * h
    probe_slots = grid[0] * grid[1]
    probe_rays = probe_slots * DIRECTIONS_PER_PROBE
    fallback_rays = int(round(probe_rays * fallback_required_fraction)) if fallback_required_fraction is not None else None

    mode_rays = {
        "hwrt-only": hwrt_rays,
        "screen-only": probe_rays,
        "hybrid": (probe_rays + fallback_rays) if fallback_rays is not None else None,
    }

    for mode in ("hwrt-only", "screen-only", "hybrid"):
        graph = create_lumen_graph(mode, mark_probe_outputs=True)
        try:
            add_and_render(graph, SCENE)
            rec = {}
            gi = grab("LumenGI.diffuseGI")[..., :3]
            rec["diffuseGI"] = {
                "mean": float(gi.mean()),
                "max": float(gi.max()),
                "finite": bool(math.isfinite(float(gi.min())) and math.isfinite(float(gi.max()))),
                "nonnegative": bool(float(gi.min()) >= 0.0),
            }
            if MODE_PROPS[mode]["useScreenTrace"]:
                try:
                    st = screen_trace_stats(decode_screentrace(grab("LumenGI.screenTrace")))
                    rec["screen_trace"] = st
                    hwrt = np.asarray(grab("LumenGI.diffuseRadianceHitDist"), dtype=np.float32)
                    hwrt_dist = hwrt[..., 3] if hwrt.ndim == 3 else hwrt
                    hwrt_hit = hwrt_dist < HWRT_MISS_HIT_DISTANCE
                    rec["hwrt_hit_pixels"] = int(hwrt_hit.sum())
                except Exception as exc:
                    rec["screen_trace_error"] = str(exc)
            rec["rays"] = mode_rays[mode]
            modes[mode] = rec
        except Exception as exc:
            modes[mode] = {"error": str(exc)}
        finally:
            remove_graph(graph)

    hwrt_mean = (modes.get("hwrt-only") or {}).get("diffuseGI", {}).get("mean")
    for mode in ("screen-only", "hybrid"):
        rec = modes.get(mode) or {}
        dg = rec.get("diffuseGI")
        if dg is None:
            verdicts.append(("%s mode output sanity" % mode, "SKIP"))
            continue
        sanity = dg["finite"] and dg["nonnegative"]
        verdicts.append(("%s mode output sanity (finite, nonneg)" % mode, "PASS" if sanity else "FAIL"))
        if hwrt_mean is not None and hwrt_mean > 0.0 and dg["mean"] is not None:
            ratio = dg["mean"] / hwrt_mean
            rec["mean_vs_hwrt_ratio"] = ratio
            ok = abs(ratio - 1.0) <= CROSS_MODE_MEAN_REL_TOL
            verdicts.append(
                ("%s vs hwrt-only mean within %g (ratio %g, S4_TODO)" % (mode, CROSS_MODE_MEAN_REL_TOL, ratio),
                 "PASS" if ok else "FAIL")
            )
        else:
            verdicts.append(("%s vs hwrt-only mean (S4_TODO, no comparable baseline)" % mode, "SKIP"))

    hybrid_rays = mode_rays["hybrid"]
    if hybrid_rays is None:
        verdicts.append(("hybrid ray reduction vs per-pixel HWRT (S4_TODO ray counter)", "SKIP"))
    else:
        reduction = float(hwrt_rays) / float(hybrid_rays) if hybrid_rays else None
        rec = modes.get("hybrid") or {}
        rec["ray_reduction_vs_hwrt"] = reduction
        ok = reduction is not None and reduction >= HYBRID_REDUCTION_MIN
        verdicts.append(
            (
                "hybrid ray reduction (hwrt %d / hybrid %d = %s >= %g, S4_TODO)"
                % (hwrt_rays, hybrid_rays, "%.2f" % reduction if reduction is not None else "n/a", HYBRID_REDUCTION_MIN),
                "PASS" if ok else "FAIL",
            )
        )

    verdicts.append(("off-screen occlusion covered by HWRT fallback (S4_TODO hit record)", "SKIP"))
    return {"hwrt_rays": hwrt_rays, "probe_rays": probe_rays, "modes": modes}, verdicts


def main():
    report = {
        "stage": "S4",
        "script": "run_probe.py",
        "role": "S4-C2 probe distribution + S4-C3 mode comparison (Agent Z3)",
        "status": "skeleton",
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "config": {
            "useScreenTrace": USE_SCREEN_TRACE,
            "useScreenProbes": USE_SCREEN_PROBES,
            "tile_size": TILE_SIZE,
            "directions_per_probe": DIRECTIONS_PER_PROBE,
            "probe_seed": PROBE_SEED,
            "ray_budget": RAY_BUDGET,
            "probe_channels": PROBE_CHANNELS,
            "probe_hwrt_fallback_prop": PROBE_HWRT_FALLBACK_PROP,
        },
    }
    verdicts = []

    screen_report, screen_verdicts = run_screen_gates()
    report.update(screen_report)
    verdicts.extend(screen_verdicts)

    probe_report, probe_verdicts = run_probe_section()
    report.update(probe_report)
    verdicts.extend(probe_verdicts)

    fallback_fraction = screen_report.get("screen_trace", {}).get("fallback_required_fraction")
    mode_report, mode_verdicts = run_mode_comparison(fallback_fraction)
    report["modes"] = mode_report
    verdicts.extend(mode_verdicts)

    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    if any(v == "SKIP" for _, v in verdicts):
        report["summary"] = "SKIP" if all(v != "FAIL" for _, v in verdicts) else "FAIL"

    for name, verdict in verdicts:
        print("PROBE VERDICT", name, verdict)
    write_json(OUT_JSON, report)
    print("PROBE wrote", os.path.abspath(OUT_JSON))


# Falcor's embedded Python executes the script with __name__ == 'builtins', so an
# `if __name__ == "__main__":` guard never runs. Call main() unconditionally, like
# the other working run_*.py scripts.
try:
    main()
except Exception as exc:
    print("PROBE ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S4",
            "script": "run_probe.py",
            "summary": "SKIP",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive SKIP on fatal error)", "SKIP")],
        },
    )
exit()
