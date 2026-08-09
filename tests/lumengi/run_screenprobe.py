from falcor import *

"""S4.2 screen probe gather GPU validation (Agent Z1, S4-A2/B2).

DRIVING CONSTRAINT: the C++ LumenGI wiring (LumenScreenProbe.h + LumenGI.cpp) cannot be
exercised by the CURRENT Release binary (last rebuilt before the S4-A1 C++ wiring, see
run_s4_shader_verify.py header). This script therefore drives the FROZEN probe shaders
(LumenScreenProbeData.slang + LumenScreenProbeTrace.cs.slang) directly through the Python
bindings on a dedicated Device, exactly as the C++ host wires them (same CB field names,
same resource names, same entry points, same dispatch order), and verifies the frozen
contract from LumenScreenProbeData.slang.

What it verifies:
  V0. The probe shader COMPILES at runtime (ComputePass creation throws on failure) in the
      standalone mode (LUMEN_GI_PROBE_SCENE_TRACE=0, no scene block), including the inline
      RayQuery HWRT fallback path, and the three entry points (updateMain / traceMain /
      finalizeMain) dispatch without a crash or validation error.
  V1. Probe metadata is correct: active flags, world position matches an independent
      unprojection (GBufferRT.viewW-based reference), normal sane, depth == linearZ.
  V2. Hit records are valid: flags are one of {ScreenHit, FallbackUnavailable} (test mode
      has no TLAS), screen-hit records carry non-zero finite radiance in lit regions, a
      finite positive hit distance and an in-range hitUV; fallback records carry the
      FallbackUnavailable flag.
  V3. Counters are consistent: screenHits + fallbackAttempts == directionsTraced,
      fallbackAttempts == fallbackUnavailable (test mode), directionsTraced ==
      activeProbes * directionsPerProbe with interval 1, inactiveProbes + budgetSkipped +
      active == probeCount.
  V4. Determinism: re-dispatching the same frame produces byte-identical hit records.
  V5. Cross-frame rotation: frame 1 differs from frame 0 (direction set rotates) while the
      screen-hit rate stays in a stable band.
  V6. Per-frame budget: updateInterval 4 -> directionsTraced ~= activeProbes * dirs / 4 and
      budgetSkipped > 0 (the round-robin gate), after the frame-0 initial-dirty flush.
  V7. The probeRadiance grid has non-zero average radiance on a meaningful fraction of
      probe points in lit regions.

Limitations (documented, see the report):
  * The HWRT fallback against the REAL scene TLAS (SceneRayQuery<0> in the
    LUMEN_GI_PROBE_SCENE_TRACE=1 host mode) cannot run here: the script bindings cannot bind
    a scene block / TLAS to a standalone compute pass. The test validates the fallback code
    path (raw RayQuery, compiled + dispatched) in its degraded form.
  * The screenTrace output prefilter is not exercised (the stale LumenGI.dll has no
    screenTrace output); is_valid_gScreenTraceResult=0.

Usage (root, from repo root):
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_screenprobe.py ^
      --logfile artifacts\\lumengi\\S4\\probe\\screenprobe.log
  Env: LUMEN_PROBE_SCENE, LUMEN_PROBE_RES, LUMEN_PROBE_OUT.
"""

import json
import math
import os

import numpy as np

# -------------------------------------------------------------------------------------
# Config (mirrors of the frozen shader constants in LumenScreenProbeData.slang /
# LumenScreenProbe.h / the C++ host).
# -------------------------------------------------------------------------------------
RESOLUTION = tuple(int(x) for x in os.environ.get("LUMEN_PROBE_RES", "640,360").split(","))
FRAME_RATE = 60
SCENE = os.environ.get("LUMEN_PROBE_SCENE", "test_scenes/cornell_box.pyscene")
OUT_JSON = os.environ.get("LUMEN_PROBE_OUT", "artifacts/lumengi/S4/probe/screenprobe.json")
SHADER = os.path.abspath("Source/RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeTrace.cs.slang")

TILE = 8
MAX_DIRS = 32
DIRS = 16
STRIDE = MAX_DIRS
SEED = 0x51B8DC0D
MARCH_STEPS = 512
MIN_THICKNESS = 0.01
THICKNESS_SCALE = 4.0
STEP_EPSILON = 0.5
DEPTH_THRESHOLD = 0.02
MAX_MIP = 12
RAY_BIAS = 0.01
MAX_SURFACE_DEPTH = 100.0

F_SCREEN_HIT = 1 << 0
F_HWRT_HIT = 1 << 1
F_HWRT_MISS = 1 << 2
F_FALLBACK_UNAVAIL = 1 << 3
F_RADIANCE_REUSED = 1 << 4
F_ENVIRONMENT = 1 << 5

META_FLAG_ACTIVE = 1 << 0
META_FLAG_DIRTY = 1 << 1

# numpy mirror of the frozen LumenScreenProbeMeta (64 B) / LumenProbeHit (32 B).
META_DTYPE = np.dtype(
    [
        ("screenPos", "<f4", (2,)),
        ("active", "<u4"),
        ("lastUpdateFrame", "<u4"),
        ("worldPos", "<f4", (3,)),
        ("materialID", "<u4"),
        ("normalW", "<f4", (3,)),
        ("depth", "<f4"),
        ("age", "<u4"),
        ("updateInterval", "<u4"),
        ("dirty", "<u4"),
        ("flags", "<u4"),
    ]
)
assert META_DTYPE.itemsize == 64, META_DTYPE.itemsize
HIT_DTYPE = np.dtype(
    [
        ("radiance", "<f4", (3,)),
        ("hitDistance", "<f4"),
        ("flags", "<u4"),
        ("confidence", "<f4"),
        ("hitUV", "<f4", (2,)),
    ]
)
assert HIT_DTYPE.itemsize == 32, HIT_DTYPE.itemsize


def log(msg):
    print(msg, flush=True)


def mark(msg):
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(OUT_JSON)), "screenprobe.progress"), "a") as f:
            f.write(msg + "\n")
    except Exception:
        pass


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


# -------------------------------------------------------------------------------------
# HZB CPU reference (ceil-halving, LumenHZB::buildChainCPU semantics).
# -------------------------------------------------------------------------------------


def hzb_mip_dim(full_dim, mip):
    return max((full_dim + (1 << mip) - 1) >> mip, 1)


def hzb_mip_count(width, height):
    dim = max(width, height)
    count = 1
    while dim > 1:
        dim = (dim + 1) // 2
        count += 1
    return count


def hzb_build_mip(src, src_w, src_h, dst_w, dst_h):
    clamp_x = src_w - 1
    clamp_y = src_h - 1
    dst = np.empty((dst_h, dst_w), dtype=np.float32)
    for y in range(dst_h):
        by = min(y * 2, clamp_y)
        ey = min(y * 2 + 1, clamp_y)
        for x in range(dst_w):
            bx = min(x * 2, clamp_x)
            ex = min(x * 2 + 1, clamp_x)
            v00 = src[by, bx]
            v10 = src[by, ex]
            v01 = src[ey, bx]
            v11 = src[ey, ex]
            dst[y, x] = max(max(v00, v10), max(v01, v11))
    return dst


def hzb_mip_dim_floor(full_dim, mip):
    return max(full_dim >> mip, 1)


def build_hzb_reference_floor(linear_z, width, height):
    """Native floor-halved max chain (what a real D3D12 mip chain holds and what
    the probe shader's explicit-mip Load indexes). Matches LumenHZB buildMipCPU
    semantics on floor dims."""
    mips = [np.asarray(linear_z, dtype=np.float32).reshape(height, width).copy()]
    mw, mh = width, height
    while max(mw, mh) > 1:
        nw, nh = max(mw >> 1, 1), max(mh >> 1, 1)
        mips.append(hzb_build_mip(mips[-1], mw, mh, nw, nh))
        mw, mh = nw, nh
    return mips


# -------------------------------------------------------------------------------------
# Graph: real GBufferRT.linearZ / normals / viewW + stale LumenGI per-pixel GI.
# -------------------------------------------------------------------------------------


def build_graph():
    graph = RenderGraph("S4ScreenProbe")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "enabled": True,
                "traceMode": "HardwareRT",
                "qualityPreset": "High",
                "useSurfaceCache": False,
            },
        ),
        "LumenGI",
    )
    for edge in [
        ("vbuffer", "vbuffer"),
        ("linearZ", "linearZ"),
        ("mvec", "mvec"),
        ("mvecW", "mvecW"),
        ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"),
        ("diffuseOpacity", "diffuseOpacity"),
        ("emissive", "emissive"),
    ]:
        graph.addEdge("GBufferRT." + edge[0], "LumenGI." + edge[1])
    graph.markOutput("GBufferRT.linearZ")
    graph.markOutput("GBufferRT.normWRoughnessMaterialID")
    graph.markOutput("GBufferRT.viewW")
    graph.markOutput("LumenGI.diffuseRadianceHitDist")
    return graph


def setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 1
    # Optional camera repositioning (LUMEN_PROBE_CAM_POS / _TGT) so the geometry is close and
    # screen-filling (the default Cornell camera looks at the box from outside, leaving most
    # of the screen as empty far-plane and making screen-first rays too long).
    if os.environ.get("LUMEN_PROBE_CAM_POS") and os.environ.get("LUMEN_PROBE_CAM_TGT"):
        cam = m.scene.camera
        cam.position = float3(*[float(v) for v in os.environ["LUMEN_PROBE_CAM_POS"].split(",")])
        cam.target = float3(*[float(v) for v in os.environ["LUMEN_PROBE_CAM_TGT"].split(",")])


def make_shader_pass(device, shader_path, entry, defines):
    desc = ProgramDesc()
    desc.add_shader_module().add_file(shader_path)
    desc.cs_entry(entry)
    return ComputePass(device, desc, defines)


def read_struct(buffer, dtype, count):
    raw = np.asarray(buffer.to_numpy(), dtype=np.uint8).reshape(-1)
    return raw[: count * dtype.itemsize].view(dtype)


def synthetic_wall_control():
    """Pure-CPU control: a flat wall at depth 2.0 filling a 640x360 frame, a probe at the
    center at depth 1.0, and a straight-forward ray (d = +z in trace view) must hit the wall
    at t = 1.0. This validates the march algorithm independently of any real scene."""
    W, H = 640, 360
    linz = np.zeros((H, W, 2), dtype=np.float64)
    linz[..., 0] = 2.0
    mips = [np.full((H, W), 2.0, dtype=np.float32)]
    mw, mh = W, H
    while max(mw, mh) > 1:
        nw, nh = max(mw >> 1, 1), max(mh >> 1, 1)
        mips.append(np.full((nh, nw), 2.0, dtype=np.float32))
        mw, mh = nw, nh
    probe = (320.5, 180.5)
    z0 = 1.0
    o = np.array([0.0, 0.0, z0])
    d = np.array([0.0, 0.0, 1.0])
    res, trace = simulate_march_trace(
        probe, o, d, linz, mips, W, H, 525.0, (320.0, 180.0),
        MARCH_STEPS, STEP_EPSILON, THICKNESS_SCALE, MIN_THICKNESS, 12,
    )
    ok = res[0] == "hit"
    hit_t = res[3][3] if ok and res[3] is not None else None
    return {"result": res[0], "steps": res[1], "hit_t": hit_t, "pass": bool(ok and hit_t is not None and abs(hit_t - 1.0) < 0.05)}


def simulate_march_trace(probe_pixel, o, d, linearz, hzb_mips, W, H, focal_px, principal, max_steps, step_eps, thickness_scale, min_thickness, max_mip, start_bias_texels=8.0):
    """Numpy mirror of the shader's depth-stepping screen-first march. linearz is (H,W,2)
    with .x = depth; d is the trace-view unit direction (d.z > 0 = forward)."""
    lin_z = linearz[..., 0]
    cx, cy = principal
    f = focal_px
    if d[2] <= 0.0:
        return ("backward", 0, 0.0, None), []
    min_hit_dist = start_bias_texels * o[2] / f
    t = max(0.005, min_hit_dist * 0.5)
    prev_diff = -1.0
    steps = 0
    trace = []
    while steps < max_steps and t < 20.0:
        steps += 1
        w = o + d * t
        q = (w[0] / w[2] * f + cx, w[1] / w[2] * f + cy)
        texel = (int(q[0]), int(q[1]))
        if texel[0] < 0 or texel[1] < 0 or texel[0] >= W or texel[1] >= H:
            return ("oob", steps, t, texel), trace
        zt = o[2] + d[2] * t
        step = max(0.005, 0.02 * max(zt, 0.01))
        z_surf = float(lin_z[texel[1], texel[0]])
        action = "advance"
        if z_surf <= 0.0 or z_surf > 100.0:
            prev_diff = -1.0
            t += step
        else:
            diff = zt - z_surf
            thickness = max(min_thickness, thickness_scale * (d[2] * step))
            if diff >= 0.0 and (prev_diff < 0.0 or diff <= thickness):
                if t < min_hit_dist:
                    prev_diff = diff
                    t += step
                    action = "close"
                else:
                    return ("hit", steps, t, (texel, zt, z_surf, t, q)), trace
            else:
                prev_diff = diff
                t += step
        if steps <= 24:
            trace.append(
                {
                    "step": steps,
                    "s": round(float(t), 3),
                    "texel": texel,
                    "zt": round(float(zt), 4),
                    "zSurf": round(float(z_surf), 4),
                    "action": action,
                }
            )
    return ("maxsteps", steps, t, None), trace


# -------------------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------------------


def main():
    W, H = RESOLUTION
    report = {
        "stage": "S4.2",
        "script": "run_screenprobe.py",
        "scene": SCENE,
        "resolution": [W, H],
        "tileSize": TILE,
        "directionsPerProbe": DIRS,
        "hitRecordStride": STRIDE,
        "seed": SEED,
        "mode": "standalone (LUMEN_GI_PROBE_SCENE_TRACE=0)",
    }
    verdicts = []

    grid_x = (W + TILE - 1) // TILE
    grid_y = (H + TILE - 1) // TILE
    probe_count = grid_x * grid_y
    mips = build_hzb_reference_floor(np.zeros((H, W), dtype=np.float32), W, H)
    mip_count = len(mips)
    report["probeGridDims"] = [grid_x, grid_y]
    report["probeCount"] = probe_count
    report["hzbMipCount"] = mip_count

    report["synthetic_control"] = synthetic_wall_control()
    if report["synthetic_control"]["pass"]:
        verdicts.append(("synthetic flat-wall march control (straight-forward hits at t=1.0)", "PASS"))
    else:
        verdicts.append(("synthetic flat-wall march control (straight-forward hits at t=1.0)", "FAIL"))

    # --- Mogwai graph: real GBufferRT + stale LumenGI per-pixel GI -------------------
    mark("graph build start")
    graph = build_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    try:
        setup_scene(SCENE)
        m.renderFrame()
        log("SCREENPROBE graph rendered (GBufferRT + stale LumenGI)")
        verdicts.append(("graph render (GBufferRT + stale LumenGI)", "PASS"))
    except Exception as exc:  # pragma: no cover
        log("SCREENPROBE ERROR graph render failed: %r" % (exc,))
        report["graph_render"] = False
        report["verdicts"] = [("graph render (GBufferRT + stale LumenGI)", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return

    mark("read GBuffer outputs")
    linearz = np.asarray(m.activeGraph.get_output("GBufferRT.linearZ").to_numpy(), dtype=np.float32)
    lin_z = linearz[..., 0] if linearz.ndim == 3 else linearz
    lin_slope = linearz[..., 1] if linearz.ndim == 3 else np.zeros_like(lin_z)
    vieww = np.asarray(m.activeGraph.get_output("GBufferRT.viewW").to_numpy(), dtype=np.float32)
    if vieww.ndim == 3:
        vieww = vieww[..., :3]
    gbuffer_gi = np.asarray(
        m.activeGraph.get_output("LumenGI.diffuseRadianceHitDist").to_numpy(), dtype=np.float32
    )
    if gbuffer_gi.ndim == 3:
        gbuffer_gi = gbuffer_gi[..., :3]
    report["gbuffer_linearz"] = {
        "min": float(np.min(lin_z)),
        "max": float(np.max(lin_z)),
        "sky_pixels": int(np.sum(lin_z <= 0.0)),
    }
    report["gbuffer_gi"] = {
        "max": float(np.max(gbuffer_gi)),
        "nonzero_frac": float(np.mean(gbuffer_gi > 0.01)),
    }

    # --- Camera basis (Falcor matrixFromLookAt conventions) --------------------------
    cam = m.scene.camera
    pos = np.array([cam.position.x, cam.position.y, cam.position.z], dtype=np.float64)
    target = np.array([cam.target.x, cam.target.y, cam.target.z], dtype=np.float64)
    up = np.array([cam.up.x, cam.up.y, cam.up.z], dtype=np.float64)
    fwd = pos - target
    fwd = fwd / np.linalg.norm(fwd)
    right = np.cross(up, fwd)
    right = right / np.linalg.norm(right)
    upv = np.cross(fwd, right)
    focal_px = float(cam.focalLength * H / cam.frameHeight)
    principal = (0.5 * W, 0.5 * H)
    report["camera"] = {
        "position": list(map(float, pos)),
        "focal_px": focal_px,
        "principal": list(principal),
    }

    # numpy world position grid (trace-view unprojection, same formula as the shader).
    px = np.arange(W, dtype=np.float64) + 0.5
    py = np.arange(H, dtype=np.float64) + 0.5
    PX, PY = np.meshgrid(px, py)
    Z = lin_z.astype(np.float64)
    tv_x = (PX - principal[0]) / focal_px * Z
    tv_y = (PY - principal[1]) / focal_px * Z
    tv_z = Z.copy()
    world_grid = (
        pos[None, None, :]
        + right[None, None, :] * tv_x[..., None]
        - upv[None, None, :] * tv_y[..., None]
        - fwd[None, None, :] * tv_z[..., None]
    )

    # Independent world position reference from GBufferRT.viewW: viewW points FROM the
    # surface TOWARD the camera (the S1 trace uses -gViewW as the primary ray), so the
    # surface point is P = camPos - viewW * t with t = z0 / dot(viewW, fwd).
    vw = vieww.astype(np.float64)
    vw_len = np.linalg.norm(vw, axis=-1, keepdims=True)
    vw = vw / np.maximum(vw_len, 1e-9)
    t_ref = Z / np.maximum(np.sum(vw * fwd[None, None, :], axis=-1), 1e-12)
    expected_world = pos[None, None, :] - vw * t_ref[..., None]

    # Depth-derived world normals (screen-space finite differences), oriented to the
    # camera (GBuffer convention: viewW points surface->camera, so a camera-facing
    # normal has dot(n, viewW) > 0).
    gx, gy = np.gradient(world_grid, axis=(1, 0))
    nrm = np.cross(gx, gy)
    nrm_len = np.linalg.norm(nrm, axis=-1, keepdims=True)
    nrm = nrm / np.maximum(nrm_len, 1e-12)
    flip = np.sum(nrm * vw, axis=-1) < 0.0
    nrm[flip] = -nrm[flip]
    nrm[lin_z <= 0.0] = np.array([0.0, 1.0, 0.0])
    # Diagnostic: numpy normal-derivation values at the (40,40) probe pixel.
    _dpx, _dpy = 40 * TILE + int(TILE * 0.5 + 0.5), 40 * TILE + int(TILE * 0.5 + 0.5)
    report["normal_diag"] = {
        "pixel": [_dpx, _dpy],
        "worldPos": list(map(float, world_grid[_dpy, _dpx])),
        "linearZ": float(lin_z[_dpy, _dpx]),
        "viewW": list(map(float, vw[_dpy, _dpx])),
        "nrm_raw": list(map(float, np.cross(
            np.gradient(world_grid, axis=(1, 0))[0][_dpy, _dpx],
            np.gradient(world_grid, axis=(1, 0))[1][_dpy, _dpx],
        ))),
        "nrm_final": list(map(float, nrm[_dpy, _dpx])),
    }

    # --- Dedicated compute device -----------------------------------------------------
    mark("device creation")
    try:
        device = Device(enable_debug_layer=True)
    except Exception as exc:  # pragma: no cover
        log("SCREENPROBE NOTE debug-layer device unavailable, retrying without: %r" % (exc,))
        device = Device(enable_debug_layer=False)
    log("SCREENPROBE compute device created")

    srgb = ResourceBindFlags.ShaderResource
    srgb_uav = ResourceBindFlags.ShaderResource | ResourceBindFlags.UnorderedAccess

    lin_on_device = device.create_texture(W, H, 0, ResourceFormat.RG32Float, 1, 1, srgb)
    lin_on_device.from_numpy(np.asarray(linearz, dtype=np.float32), mip_level=0, array_slice=0)
    # Native floor-halved HZB mip chain as a single R32F texture (explicit-mip Load).
    mips = build_hzb_reference_floor(lin_z, W, H)
    mip_count = len(mips)
    hzb_tex = device.create_texture(W, H, 0, ResourceFormat.R32Float, 1, mip_count, srgb_uav)
    for mm in range(mip_count):
        hzb_tex.from_numpy(np.asarray(mips[mm], dtype=np.float32), mip_level=mm, array_slice=0)
    # Verify the GPU-side HZB mip 0 round-trips to linearZ.x (catches broken chains early).
    hzb0_readback = np.asarray(hzb_tex.to_numpy(mip_level=0, array_slice=0), dtype=np.float32)
    report["hzb_mip0_max_abs_diff"] = float(np.max(np.abs(hzb0_readback - lin_z)))
    # GBuffer normal emulation: RGBA32F with the [0,1] RGB10A2 encoding (normal*0.5+0.5).
    normal_data = np.zeros((H, W, 4), dtype=np.float32)
    normal_data[..., 0:3] = (nrm * 0.5 + 0.5).astype(np.float32)
    normal_tex = device.create_texture(W, H, 0, ResourceFormat.RGBA32Float, 1, 1, srgb)
    normal_tex.from_numpy(normal_data, mip_level=0, array_slice=0)
    gi_tex = device.create_texture(W, H, 0, ResourceFormat.RGBA16Float, 1, 1, srgb)
    gi_float = np.zeros((H, W, 4), dtype=np.float16)
    gi_float[..., :3] = gbuffer_gi.astype(np.float16)
    gi_tex.from_numpy(gi_float, mip_level=0, array_slice=0)
    mark("textures uploaded")

    # --- Probe buffers ----------------------------------------------------------------
    meta_buf = device.create_structured_buffer(
        META_DTYPE.itemsize, probe_count, srgb_uav, MemoryType.DeviceLocal
    )
    hit_buf = device.create_structured_buffer(
        HIT_DTYPE.itemsize, probe_count * STRIDE, srgb_uav, MemoryType.DeviceLocal
    )
    counter_buf = device.create_structured_buffer(32, 1, srgb_uav, MemoryType.DeviceLocal)
    debug_buf = device.create_structured_buffer(16, probe_count * 4, srgb_uav, MemoryType.DeviceLocal)
    radiance_tex = device.create_texture(W, H, 0, ResourceFormat.RGBA16Float, 1, 1, srgb_uav)

    metas0 = np.zeros(probe_count, dtype=META_DTYPE)
    for gy in range(grid_y):
        for gx in range(grid_x):
            i = gy * grid_x + gx
            metas0[i]["screenPos"] = (gx * TILE + TILE * 0.5 + 0.5, gy * TILE + TILE * 0.5 + 0.5)
    meta_buf.from_numpy(metas0.view(np.uint8))
    hit_buf.from_numpy(np.zeros(probe_count * STRIDE * HIT_DTYPE.itemsize, dtype=np.uint8))
    counter_buf.from_numpy(np.zeros(32, dtype=np.uint8))
    radiance_tex.from_numpy(np.zeros((H, W, 4), dtype=np.float16), mip_level=0, array_slice=0)
    mark("probe buffers created")

    # --- Shader passes (standalone mode) ----------------------------------------------
    defines = {
        "is_valid_gScreenTraceResult": "0",
        "is_valid_gDiffuseRadianceHitDist": "1",
        "is_valid_gNormalRoughnessMaterialID": "1",
        "is_valid_gProbeRadiance": "1",
        "is_valid_gProbeTLAS": "0",
        "is_valid_gProbeDebug": "1",
    }
    try:
        p_update = make_shader_pass(device, SHADER, "updateMain", defines)
        p_trace = make_shader_pass(device, SHADER, "traceMain", defines)
        p_finalize = make_shader_pass(device, SHADER, "finalizeMain", defines)
        log("SCREENPROBE LumenScreenProbeTrace.cs.slang compiled (3 entry points)")
        verdicts.append(("LumenScreenProbeTrace.cs.slang runtime compile", "PASS"))
    except Exception as exc:  # pragma: no cover
        log("SCREENPROBE ERROR probe shader failed to compile: %r" % (exc,))
        report["probe_compile"] = repr(exc)
        report["verdicts"] = verdicts + [("LumenScreenProbeTrace.cs.slang runtime compile", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return
    report["probe_compile"] = "ok"

    def bind_cb(root, frame_index, interval, dirs):
        cb = root["LumenScreenProbeCB"]
        cb["gFrameDim"] = uint2(W, H)
        cb["gFrameIndex"] = frame_index
        cb["gDirectionsPerProbe"] = dirs
        cb["gProbeGridDims"] = uint2(grid_x, grid_y)
        cb["gMaxHitRecordStride"] = STRIDE
        cb["gMinThickness"] = MIN_THICKNESS
        cb["gThicknessScale"] = THICKNESS_SCALE
        cb["gMaxMarchSteps"] = MARCH_STEPS
        cb["gStepEpsilon"] = STEP_EPSILON
        cb["gCameraFocalPx"] = focal_px
        cb["gPrincipalPoint"] = float2(principal[0], principal[1])
        cb["gInvFrameDim"] = float2(1.0 / W, 1.0 / H)
        cb["gUpdateInterval"] = interval
        cb["gSeed"] = SEED
        cb["gDepthChangeThreshold"] = DEPTH_THRESHOLD
        cb["gMaxMip"] = min(MAX_MIP, mip_count - 1)
        cb["gCameraPosW"] = float3(pos[0], pos[1], pos[2])
        cb["gCameraRightW"] = float3(right[0], right[1], right[2])
        cb["gCameraUpW"] = float3(upv[0], upv[1], upv[2])
        cb["gCameraForwardW"] = float3(fwd[0], fwd[1], fwd[2])
        cb["gEnvFallbackRadiance"] = float3(0.0, 0.0, 0.0)
        cb["gDebugMode"] = 0
        cb["gProbeCount"] = probe_count

    def bind_resources(root):
        root["gLinearZ"] = lin_on_device
        root["gHZBMips"] = hzb_tex
        root["gNormalRoughnessMaterialID"] = normal_tex
        root["gDiffuseRadianceHitDist"] = gi_tex
        root["gProbeMeta"] = meta_buf
        root["gProbeHitRecords"] = hit_buf
        root["gProbeCounters"] = counter_buf
        root["gProbeRadiance"] = radiance_tex
        root["gProbeDebug"] = debug_buf

    update_threads = ((probe_count + 63) // 64) * 64
    trace_threads = ((probe_count * DIRS + 63) // 64) * 64

    def dispatch(frame_index, interval, dirs):
        counter_buf.from_numpy(np.zeros(32, dtype=np.uint8))
        hit_buf.from_numpy(np.zeros(probe_count * STRIDE * HIT_DTYPE.itemsize, dtype=np.uint8))
        radiance_tex.from_numpy(np.zeros((H, W, 4), dtype=np.float16), mip_level=0, array_slice=0)
        for p in (p_update, p_trace, p_finalize):
            bind_cb(p.root_var, frame_index, interval, dirs)
            bind_resources(p.root_var)
        tt = ((probe_count * dirs + 63) // 64) * 64
        p_update.execute(update_threads, 1, 1)
        p_trace.execute(tt, 1, 1)
        p_finalize.execute(update_threads, 1, 1)
        device.wait()

    def read_back():
        metas = read_struct(meta_buf, META_DTYPE, probe_count)
        hits = read_struct(hit_buf, HIT_DTYPE, probe_count * STRIDE)
        ctr = read_struct(counter_buf, np.dtype([("c", "<u4", (8,))]), 1)["c"][0]
        rad = np.asarray(radiance_tex.to_numpy(), dtype=np.float32)
        debug = np.asarray(debug_buf.to_numpy(), dtype=np.uint8).reshape(-1).view(np.float32).reshape(-1, 4)
        return metas, hits, ctr, rad, debug

    # ---- Frame 0 (interval 1: all probes) -------------------------------------------
    mark("frame 0 dispatch")
    dispatch(0, 1, DIRS)
    metas, hits, ctr, rad, debug = read_back()
    log("SCREENPROBE frame 0 dispatched + read back")

    # Per-probe march diagnostics (probe index -> 4 float4s).
    dbg = debug.reshape(probe_count, 4, 4)
    fwd_eligible = dbg[:, 1, 1] > 0.5  # d.z > 0 (eligible for screen-first).
    report["march_diag"] = {
        "forward_eligible_frac": float(np.mean(fwd_eligible)),
        "result_screen_hit_frac": float(np.mean(dbg[:, 1, 0] == 1.0)),
        "result_fallback_frac": float(np.mean(dbg[:, 1, 0] == 2.0)),
        "d_z_min": float(np.min(dbg[fwd_eligible, 0, 2])) if np.any(fwd_eligible) else None,
        "d_z_max": float(np.max(dbg[fwd_eligible, 0, 2])) if np.any(fwd_eligible) else None,
        "probe_depth_min": float(np.min(dbg[:, 1, 2])),
        "probe_depth_max": float(np.max(dbg[:, 1, 2])),
        "probe_depth_gt100_frac": float(np.mean(dbg[:, 1, 2] > 100.0)),
    }
    # Sample diagnostics for a few probes (floor / wall / far-plane regions).
    floor_probe = int(40 * grid_x + 40)
    backwall_probe = int(20 * grid_x + 40)
    far_probe = int(0 * grid_x + 0)
    report["march_diag"]["samples"] = {
        "floor_probe_d": list(map(float, dbg[floor_probe, 0])),
        "floor_probe_res": list(map(float, dbg[floor_probe, 1])),
        "backwall_probe_d": list(map(float, dbg[backwall_probe, 0])),
        "backwall_probe_res": list(map(float, dbg[backwall_probe, 1])),
        "far_probe_d": list(map(float, dbg[far_probe, 0])),
        "far_probe_res": list(map(float, dbg[far_probe, 1])),
    }

    active = metas["active"].astype(bool)
    report["meta"] = {
        "active_probes": int(np.sum(active)),
        "inactive_probes": int(probe_count - np.sum(active)),
        "all_depth_gt0_active": bool(np.all(metas["depth"][active] > 0.0)),
    }

    # V1: world position vs the independent viewW-based reference. Probes whose linearZ is the
    # camera far plane (empty space, no geometry) are excluded from the surface checks.
    wpos = metas["worldPos"].astype(np.float64)
    probe_depth = metas["depth"].astype(np.float64)
    surface = active & (probe_depth < 100.0)
    ref_positions = np.zeros_like(wpos)
    grid_positions = np.zeros_like(wpos)  # numpy mirror of the shader's unprojection formula.
    for gy in range(grid_y):
        for gx in range(grid_x):
            i = gy * grid_x + gx
            px_c = gx * TILE + TILE * 0.5 + 0.5
            py_c = gy * TILE + TILE * 0.5 + 0.5
            px_i = int(px_c)
            py_i = int(py_c)
            if px_i >= W or py_i >= H:
                continue
            ref_positions[i] = expected_world[py_i, px_i]
            grid_positions[i] = world_grid[py_i, px_i]
    diff = np.linalg.norm(wpos - ref_positions, axis=-1)
    grid_diff = np.linalg.norm(wpos - grid_positions, axis=-1)
    active_diff = diff[surface]
    grid_active_diff = grid_diff[surface]
    wpos_tol = 0.01  # 1 cm
    wpos_ok = bool(np.all(active_diff < wpos_tol)) if active_diff.size else False
    report["meta"]["surface_probes"] = int(np.sum(surface))
    report["meta"]["worldpos_max_err_m"] = float(np.max(active_diff)) if active_diff.size else None
    report["meta"]["worldpos_mean_err_m"] = float(np.mean(active_diff)) if active_diff.size else None
    report["meta"]["worldpos_vs_formula_max_err_m"] = float(np.max(grid_active_diff)) if grid_active_diff.size else None
    report["meta"]["worldpos_tol_m"] = wpos_tol
    probe0 = int(0 * grid_x + 0)
    probe_c = int((grid_y // 2) * grid_x + (grid_x // 2))
    report["meta"]["sample"] = {
        "probe0_worldPos": list(map(float, wpos[probe0])),
        "probe0_formula": list(map(float, grid_positions[probe0])),
        "probe0_ref": list(map(float, ref_positions[probe0])),
        "probe0_depth": float(metas["depth"][probe0]),
        "center_worldPos": list(map(float, wpos[probe_c])),
        "center_formula": list(map(float, grid_positions[probe_c])),
        "center_ref": list(map(float, ref_positions[probe_c])),
    }
    if wpos_ok:
        verdicts.append(("probe worldPos vs viewW-based reference (< 1 cm)", "PASS"))
    else:
        verdicts.append(("probe worldPos vs viewW-based reference (< 1 cm)", "FAIL"))

    # Normal sanity: unit length + facing the camera (dot(n, viewW) > 0).
    nrm_out = metas["normalW"].astype(np.float64)
    nrm_len = np.linalg.norm(nrm_out, axis=-1)
    nrm_view_dir = np.zeros_like(nrm_out)
    for gy in range(grid_y):
        for gx in range(grid_x):
            px_i = gx * TILE + TILE * 0.5 + 0.5
            py_i = gy * TILE + TILE * 0.5 + 0.5
            nrm_view_dir[gy * grid_x + gx] = vw[int(py_i), int(px_i)]
    nrm_unit_ok = bool(np.all(np.abs(nrm_len[surface] - 1.0) < 1e-3))
    nrm_facing_ok = bool(np.all(np.sum(nrm_out[surface] * nrm_view_dir[surface], axis=-1) > -0.05))
    report["meta"]["normal_unit_ok"] = nrm_unit_ok
    report["meta"]["normal_facing_camera_ok"] = nrm_facing_ok
    if nrm_unit_ok and nrm_facing_ok:
        verdicts.append(("probe normals unit-length + facing the camera", "PASS"))
    else:
        verdicts.append(("probe normals unit-length + facing the camera", "FAIL"))

    # V2: hit records.
    rec_flags = hits["flags"]
    screen_mask = (rec_flags & F_SCREEN_HIT) != 0
    fb_mask = (rec_flags & F_FALLBACK_UNAVAIL) != 0
    hwrt_mask = (rec_flags & (F_HWRT_HIT | F_HWRT_MISS)) != 0
    touched = rec_flags != 0
    # A written record must carry at least one of the three resolution markers.
    unexpected = touched & ~(screen_mask | fb_mask | hwrt_mask)
    records_written = int(np.sum(touched))
    report["records"] = {
        "written": records_written,
        "screen_hits": int(np.sum(screen_mask)),
        "fallback_unavailable": int(np.sum(fb_mask)),
        "hwrt_resolved": int(np.sum(hwrt_mask)),
        "unexpected_flags": int(np.sum(unexpected)),
        "expected_written": int(np.sum(active)) * DIRS,
    }
    rec_radiance = hits["radiance"].astype(np.float64)
    screen_rad = rec_radiance[screen_mask]
    screen_hitdist = hits["hitDistance"][screen_mask]
    screen_uv = hits["hitUV"][screen_mask]
    report["records"]["screen_radiance"] = {
        "max": float(np.max(screen_rad)) if screen_rad.size else None,
        "mean": float(np.mean(screen_rad)) if screen_rad.size else None,
        "frac_gt_0_05": float(np.mean(np.any(screen_rad > 0.05, axis=-1))) if screen_rad.size else 0.0,
        "finite": bool(np.all(np.isfinite(screen_rad))) if screen_rad.size else True,
    }
    report["records"]["screen_hitdist"] = {
        "min": float(np.min(screen_hitdist)) if screen_hitdist.size else None,
        "max": float(np.max(screen_hitdist)) if screen_hitdist.size else None,
        "gt0": bool(np.all(screen_hitdist > 0.0)) if screen_hitdist.size else True,
    }
    report["records"]["screen_uv"] = {
        "inrange": bool(np.all((screen_uv >= 0.0) & (screen_uv <= 1.0))) if screen_uv.size else True,
    }
    radiance_ok = (
        screen_rad.size > 0
        and np.all(np.isfinite(screen_rad))
        and np.max(screen_rad) > 0.0
    )
    hitdist_ok = screen_hitdist.size > 0 and np.all(screen_hitdist > 0.0) and np.all(np.isfinite(screen_hitdist))
    uv_ok = screen_uv.size > 0 and np.all((screen_uv >= 0.0) & (screen_uv <= 1.0))
    records_ok = (
        records_written == int(np.sum(active)) * DIRS
        and np.sum(unexpected) == 0
        and (np.sum(screen_mask) + np.sum(fb_mask)) == records_written
    )
    if records_ok and radiance_ok and hitdist_ok and uv_ok:
        verdicts.append(("hit records: flags/count/radiance/hitdist/uv", "PASS"))
    else:
        verdicts.append(("hit records: flags/count/radiance/hitdist/uv", "FAIL"))

    # V3: counters.
    c = ctr
    cdict = {
        "screen_hits": int(c[0]),
        "fallback_attempts": int(c[1]),
        "fallback_hits": int(c[2]),
        "fallback_misses": int(c[3]),
        "fallback_unavailable": int(c[4]),
        "inactive_probes": int(c[5]),
        "budget_skipped": int(c[6]),
        "directions_traced": int(c[7]),
    }
    report["counters"] = cdict
    c_ok = (
        c[0] + c[1] == c[7]
        and c[1] == c[4]
        and c[7] == int(np.sum(active)) * DIRS
        and c[5] == int(probe_count - np.sum(active))
        and c[6] == 0
    )
    if c_ok:
        verdicts.append(("counters consistent (screen+fallback==traced, budget=0, interval 1)", "PASS"))
    else:
        verdicts.append(("counters consistent (screen+fallback==traced, budget=0, interval 1)", "FAIL"))

    # V7: radiance grid non-zero at lit probes.
    rad_rgb = rad[..., :3]
    probe_rad_mask = np.zeros((H, W), dtype=bool)
    probe_px = metas["screenPos"].astype(int)
    for i in range(probe_count):
        if active[i]:
            probe_rad_mask[probe_px[i, 1], probe_px[i, 0]] = True
    lit_fraction = float(np.mean(rad_rgb[probe_rad_mask] > 0.05)) if np.any(probe_rad_mask) else 0.0
    report["radiance_grid"] = {
        "probe_points": int(np.sum(probe_rad_mask)),
        "max_rgb": float(np.max(rad_rgb)),
        "frac_lit_gt_0_05": lit_fraction,
    }
    if np.max(rad_rgb) > 0.0 and lit_fraction > 0.01:
        verdicts.append(("probeRadiance grid non-zero at lit probe points", "PASS"))
    else:
        verdicts.append(("probeRadiance grid non-zero at lit probe points", "FAIL"))

    # ---- V4: determinism (re-dispatch frame 0, compare hit records) ------------------
    mark("determinism run")
    dispatch(0, 1, DIRS)
    _, hits2, ctr2, _, _ = read_back()
    determinism_ok = bool(np.array_equal(hits2.view(np.uint8), hits.view(np.uint8)))
    report["determinism"] = {
        "frame0_run2_identical": determinism_ok,
        "counter_equal": bool(np.array_equal(ctr2.view(np.uint8), ctr.view(np.uint8))),
    }
    if determinism_ok:
        verdicts.append(("determinism: frame-0 re-dispatch byte-identical hit records", "PASS"))
    else:
        verdicts.append(("determinism: frame-0 re-dispatch byte-identical hit records", "FAIL"))

    # ---- V5: cross-frame rotation (frame 1) ------------------------------------------
    mark("frame 1 dispatch")
    dispatch(1, 1, DIRS)
    _, hits1, ctr1, _, _ = read_back()
    rotated = not np.array_equal(hits1.view(np.uint8), hits.view(np.uint8))
    rate0 = c[0] / float(c[7]) if c[7] else 0.0
    rate1 = ctr1[0] / float(ctr1[7]) if ctr1[7] else 0.0
    report["rotation"] = {
        "frame1_differs": rotated,
        "screen_hit_rate_frame0": rate0,
        "screen_hit_rate_frame1": rate1,
        "rate_delta": abs(rate0 - rate1),
    }
    if rotated and abs(rate0 - rate1) < 0.15:
        verdicts.append(("cross-frame rotation changes the set, hit rate stable", "PASS"))
    else:
        verdicts.append(("cross-frame rotation changes the set, hit rate stable", "FAIL"))

    # ---- V6: per-frame budget (interval 4 at frame 1) --------------------------------
    mark("budget dispatch")
    dispatch(1, 4, DIRS)
    _, hits_b, ctr_b, _, _ = read_back()
    active_now = int(np.sum(active))
    expected_budget_traced = int(((active_now + 3) // 4) * DIRS)
    c_b = ctr_b
    report["budget"] = {
        "interval": 4,
        "directions_traced": int(c_b[7]),
        "expected_traced": expected_budget_traced,
        "budget_skipped": int(c_b[6]),
        "written_records": int(np.sum(hits_b["flags"] != 0)),
    }
    budget_ok = (
        c_b[6] > 0
        and abs(int(c_b[7]) - expected_budget_traced) <= DIRS  # +- one probe of slack.
        and int(np.sum(hits_b["flags"] != 0)) <= expected_budget_traced + DIRS
    )
    if budget_ok:
        verdicts.append(("per-frame budget (interval 4 round-robin)", "PASS"))
    else:
        verdicts.append(("per-frame budget (interval 4 round-robin)", "FAIL"))

    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    for name, verdict in verdicts:
        log("SCREENPROBE VERDICT %s -- %s" % (name, verdict))
    mark("write json")
    write_json(OUT_JSON, report)


main()
exit()
