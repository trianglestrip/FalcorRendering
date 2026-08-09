from falcor import *

"""S4.3 probe integrate + interpolate GPU validation (Agent Z4, S4-A3/B3).

DRIVING CONSTRAINT: the C++ LumenGI wiring (integrate/interpolate host dispatch in
LumenGI.cpp) cannot be exercised by the CURRENT Release binary (last rebuilt at the
S4.2 commit 96dfaa7f; S4.3 is a no-build wave). This script therefore drives the
S4.3 shaders (LumenScreenProbeIntegrate.cs.slang, LumenScreenProbeInterpolate.cs.slang)
AND the frozen S4.2 trace shaders DIRECTLY through the Python bindings on a dedicated
Device, exactly as the C++ host wires them (same CB field names, same resource names,
same entry points, same dispatch order), following the run_screenprobe.py precedent.

Pipeline under test (dispatch order == the C++ host):
    updateMain -> traceMain -> finalizeMain -> integrateMain -> interpolateMain
with:
    * a REAL GBufferRT render (linearZ + oct-encoded normWRoughnessMaterialID + viewW)
      for the probe metadata/march inputs,
    * the real S1 per-pixel diffuseRadianceHitDist from the stale LumenGI as the
      screen-hit radiance (S4.2 degradation).

What it verifies:
  V0. All three shaders COMPILE at runtime (ComputePass creation throws on failure):
      LumenScreenProbeTrace.cs.slang (update/trace/finalize), LumenScreenProbeIntegrate.cs.slang
      (main), LumenScreenProbeInterpolate.cs.slang (main); all five entry points dispatch
      without a crash or D3D12 validation error (debug layer on).
  V1. Integrate output (RGB = incident irradiance E, A = confidence) at the probe
      tile-center texels is finite / non-negative; A in [0,1]; a meaningful fraction of
      probes in lit regions carry E > 0; E matches a numpy mirror of the shader's
      mode-0 cosine estimator (π/N Σ clamp(Li)) within tolerance.
  V2. Interpolate output (full-res RGBA16F: RGB = E, A = confidence) is finite /
      non-negative; A in [0,1]; lit regions carry mean E > 0.
  V3. Interpolate does not cross-blend across depth / normal boundaries
      (run_probe_interp.py V2 edge-side-consistency metric, same thresholds).
  V4. Determinism: re-dispatching the same frame produces byte-identical integrate AND
      interpolate outputs.
  V5. No S4.2 regression: the probe hit records / counters / finalize radiance-grid
      invariants from run_screenprobe.py hold on the same pipeline (frame 0, interval 1).

Evidence: JSON report + logs written under artifacts/lumengi/S4/probe/.

Usage (root, from repo root):
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_probe_integrate_interp.py ^
      --logfile artifacts\\lumengi\\S4\\probe\\probe_integrate_interp.log
  Env: LUMEN_INTERP_SCENE, LUMEN_INTERP_RES, LUMEN_INTERP_OUT,
       LUMEN_INTERP_CAM_POS / LUMEN_INTERP_CAM_TGT (camera override).
"""

import json
import math
import os

import numpy as np

# -------------------------------------------------------------------------------------
# Config (mirrors of the frozen shader constants / host defaults).
# -------------------------------------------------------------------------------------
RESOLUTION = tuple(int(x) for x in os.environ.get("LUMEN_INTERP_RES", "640,360").split(","))
FRAME_RATE = 60
SCENE = os.environ.get("LUMEN_INTERP_SCENE", "test_scenes/cornell_box.pyscene")
OUT_JSON = os.environ.get("LUMEN_INTERP_OUT", "artifacts/lumengi/S4/probe/probe_integrate_interp.json")

SHADER_TRACE = os.path.abspath("Source/RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeTrace.cs.slang")
SHADER_INTEGRATE = os.path.abspath("Source/RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeIntegrate.cs.slang")
SHADER_INTERPOLATE = os.path.abspath("Source/RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeInterpolate.cs.slang")

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

# S4.3 weight defaults (host LumenGI.cpp defaults / Z2 task spec).
WEIGHT_MODE = 0
INTERP_DEPTH_THRESHOLD = 0.02
INTERP_DEPTH_SIGMA_INV = 4.0
INTERP_NORMAL_EXPONENT = 8.0
INTERP_MATERIAL_MISMATCH = 0.05
INTERP_FALLBACK_CONF_SCALE = 0.25

MAX_RADIANCE = 10000.0
PI = 3.14159265358979

F_SCREEN_HIT = 1 << 0
F_HWRT_HIT = 1 << 1
F_HWRT_MISS = 1 << 2
F_FALLBACK_UNAVAIL = 1 << 3
F_RADIANCE_REUSED = 1 << 4
F_ENVIRONMENT = 1 << 5

META_FLAG_ACTIVE = 1 << 0
META_FLAG_DIRTY = 1 << 1

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

# Boundary gates (mirror run_probe_interp.py V2).
LOGZ_EDGE = 0.5
NORMAL_EDGE_ANGLE = 0.5
CROSS_BLEND_MAX_FRACTION = 0.2
MIN_EDGE_PIXELS = 200


def log(msg):
    print(msg, flush=True)


def to_hw(arr, width, height):
    """Ensure a channel readback is in (height, width[, ...]) numpy layout. Some Falcor
    binaries return textures width-major (width, height, ...); transpose when that is the
    case (unambiguous whenever width != height, which is true for all our resolutions)."""
    arr = np.asarray(arr)
    if arr.ndim >= 2 and arr.shape[0] == width and arr.shape[1] == height and width != height:
        return arr.transpose((1, 0) + tuple(range(2, arr.ndim)))
    return arr


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
# HZB CPU reference (native floor-halved max chain, matches LumenHZB buildMipCPU).
# -------------------------------------------------------------------------------------


def hzb_mip_dim_floor(full_dim, mip):
    return max(full_dim >> mip, 1)


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
            dst[y, x] = max(max(src[by, bx], src[by, ex]), max(src[ey, bx], src[ey, ex]))
    return dst


def build_hzb_reference_floor(linear_z, width, height):
    mips = [np.asarray(linear_z, dtype=np.float32).reshape(height, width).copy()]
    mw, mh = width, height
    while max(mw, mh) > 1:
        nw, nh = max(mw >> 1, 1), max(mh >> 1, 1)
        mips.append(hzb_build_mip(mips[-1], mw, mh, nw, nh))
        mw, mh = nw, nh
    return mips


# -------------------------------------------------------------------------------------
# Graph: real GBufferRT + stale LumenGI per-pixel GI (same as run_screenprobe.py).
# -------------------------------------------------------------------------------------


def build_graph():
    graph = RenderGraph("S4ProbeInterp")
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
            {"enabled": True, "traceMode": "HardwareRT", "qualityPreset": "High", "useSurfaceCache": False},
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
    if os.environ.get("LUMEN_INTERP_CAM_POS") and os.environ.get("LUMEN_INTERP_CAM_TGT"):
        cam = m.scene.camera
        cam.position = float3(*[float(v) for v in os.environ["LUMEN_INTERP_CAM_POS"].split(",")])
        cam.target = float3(*[float(v) for v in os.environ["LUMEN_INTERP_CAM_TGT"].split(",")])


def make_shader_pass(device, shader_path, entry, defines):
    desc = ProgramDesc()
    desc.add_shader_module().add_file(shader_path)
    desc.cs_entry(entry)
    return ComputePass(device, desc, defines)


def read_struct(buffer, dtype, count):
    raw = np.asarray(buffer.to_numpy(), dtype=np.uint8).reshape(-1)
    return raw[: count * dtype.itemsize].view(dtype)


# -------------------------------------------------------------------------------------
# Oct normal decode / unpack of the RGB10A2 normWRoughnessMaterialID channel.
# -------------------------------------------------------------------------------------


def decode_normal(matid_raw, h, w):
    """Returns (xy_unorm[h,w,2] in [0,1], w[h,w] in [0,1]) from the RGB10A2 channel.
    Handles packed uint8/uint32 and float readbacks (mirrors run_probe_interp.py)."""
    arr = np.asarray(matid_raw)
    if arr.dtype == np.uint8:
        if arr.ndim == 1:
            packed = arr.view(np.uint32).reshape(h, w)
        else:
            packed = arr.reshape(-1, 4).view(np.uint32).reshape(h, w)
        xy = np.stack(
            [
                ((packed >> 0) & 0x3FF).astype(np.float64) / 1023.0,
                ((packed >> 10) & 0x3FF).astype(np.float64) / 1023.0,
            ],
            axis=-1,
        )
        w8 = ((packed >> 30) & 0x3).astype(np.float64) / 3.0
        return xy, w8
    arr = np.asarray(matid_raw, dtype=np.float64)
    return arr[..., :2], arr[..., 3]


def oct_to_ndir(xy_unorm):
    p = xy_unorm.astype(np.float64) * 2.0 - 1.0
    n = np.zeros(p.shape[:-1] + (3,), dtype=np.float64)
    n[..., 0] = p[..., 0]
    n[..., 1] = p[..., 1]
    n[..., 2] = 1.0 - np.abs(p[..., 0]) - np.abs(p[..., 1])
    wrap = n[..., 2] < 0.0
    xy = np.zeros_like(p[..., :2])
    xy[..., 0] = (1.0 - np.abs(p[..., 1])) * np.sign(p[..., 0])
    xy[..., 1] = (1.0 - np.abs(p[..., 0])) * np.sign(p[..., 1])
    n[..., :2][wrap] = xy[wrap]
    norm = np.sqrt(np.sum(n * n, axis=-1))
    return n / (norm[..., np.newaxis] + 1e-12)


# -------------------------------------------------------------------------------------
# CPU mirror of the integrate mode-0 estimator (matches the shader exactly).
# -------------------------------------------------------------------------------------


def integrate_cpu_reference(hits, dirs, stride, max_radiance):
    """Per-probe mode-0 cosine estimate: E = π/N Σ clamp(Li) over valid dirs,
    conf = mean(c) * valid/N. Mirrors LumenScreenProbeIntegrate.cs.slang main."""
    n_probe = hits.shape[0] // stride
    E = np.zeros((n_probe, 3), dtype=np.float64)
    conf = np.zeros(n_probe, dtype=np.float64)
    valid_frac = np.zeros(n_probe, dtype=np.float64)
    for i in range(n_probe):
        rec = hits[i * stride : i * stride + dirs]
        flags = rec["flags"]
        valid = (flags & (F_SCREEN_HIT | F_HWRT_HIT)) != 0
        li = np.maximum(rec["radiance"].astype(np.float64), 0.0)
        finite = np.isfinite(li).all(axis=1)
        li = np.where(finite[:, None], li, 0.0)
        li = np.minimum(li, max_radiance)
        keep = valid & li.any(axis=1)
        if not np.any(keep):
            continue
        w = PI / float(dirs)
        E[i] = np.minimum(np.maximum(np.sum(w * li[keep], axis=0), 0.0), max_radiance)
        conf[i] = (float(np.sum(rec["confidence"][keep])) / float(np.sum(keep))) * (float(np.sum(keep)) / float(dirs))
        valid_frac[i] = float(np.sum(keep)) / float(dirs)
    return E, conf, valid_frac


# -------------------------------------------------------------------------------------
# Interpolate output validation (V1/V2/V3; mirrors run_probe_interp.py metrics).
# -------------------------------------------------------------------------------------


def neighbor_max_grad(field):
    field = np.asarray(field, dtype=np.float64)
    p = np.pad(field, ((1, 1), (1, 1)) + ((0, 0),) * (field.ndim - 2), mode="edge")
    grad = np.zeros_like(field)
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        other = p[1 + dy : 1 + dy + field.shape[0], 1 + dx : 1 + dx + field.shape[1]]
        grad = np.maximum(grad, np.abs(field - other))
    return grad


def normal_edge_mask(nrm):
    good = np.isfinite(nrm).all(axis=-1)
    grad = np.zeros(nrm.shape[:-1], dtype=np.float64)
    p = np.pad(nrm, ((1, 1), (1, 1), (0, 0)), mode="edge")
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        other = p[1 + dy : 1 + dy + nrm.shape[0], 1 + dx : 1 + dx + nrm.shape[1]]
        dot = np.clip(np.sum(nrm * other, axis=-1), -1.0, 1.0)
        grad = np.maximum(grad, np.arccos(dot))
    return (grad > NORMAL_EDGE_ANGLE) & good


def edge_side_cross_dominant(gi_lum, lz, nrm, edge, good, radius=2):
    h, w = gi_lum.shape
    pad = lambda f, r: np.pad(f, ((r, r), (r, r)) + ((0, 0),) * (f.ndim - 2), mode="edge")
    gi_p = pad(gi_lum, radius)
    lz_p = pad(lz, radius)
    nrm_p = pad(nrm, radius)
    good_p = pad(good, radius)
    edge_p = pad(edge, radius)
    same_sum = np.zeros_like(gi_lum, dtype=np.float64)
    same_cnt = np.zeros_like(gi_lum, dtype=np.int32)
    cross_sum = np.zeros_like(gi_lum, dtype=np.float64)
    cross_cnt = np.zeros_like(gi_lum, dtype=np.int32)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if max(abs(dy), abs(dx)) != radius:
                continue
            n_gi = gi_p[radius + dy : radius + dy + h, radius + dx : radius + dx + w]
            n_lz = lz_p[radius + dy : radius + dy + h, radius + dx : radius + dx + w]
            n_nrm = nrm_p[radius + dy : radius + dy + h, radius + dx : radius + dx + w]
            n_good = good_p[radius + dy : radius + dy + h, radius + dx : radius + dx + w]
            n_edge = edge_p[radius + dy : radius + dy + h, radius + dx : radius + dx + w]
            same_side = (
                n_good
                & ~n_edge
                & (np.abs(lz - n_lz) <= LOGZ_EDGE)
                & (np.arccos(np.clip(np.sum(nrm * n_nrm, axis=-1), -1.0, 1.0)) <= NORMAL_EDGE_ANGLE)
            )
            cross_side = n_good & ~n_edge & ~same_side
            same_sum += np.where(same_side, n_gi, 0.0)
            same_cnt += same_side.astype(np.int32)
            cross_sum += np.where(cross_side, n_gi, 0.0)
            cross_cnt += cross_side.astype(np.int32)
    measurable = edge & (same_cnt > 0) & (cross_cnt > 0)
    same_mean = same_sum / np.maximum(same_cnt, 1)
    cross_mean = cross_sum / np.maximum(cross_cnt, 1)
    d_same = np.abs(gi_lum - same_mean)
    d_cross = np.abs(gi_lum - cross_mean)
    return measurable & (d_cross <= d_same)


# -------------------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------------------


def main():
    W, H = RESOLUTION
    report = {
        "stage": "S4.3",
        "script": "run_probe_integrate_interp.py",
        "role": "probe integrate + interpolate GPU validation (Agent Z4)",
        "scene": SCENE,
        "resolution": [W, H],
        "tileSize": TILE,
        "directionsPerProbe": DIRS,
        "hitRecordStride": STRIDE,
        "seed": SEED,
        "mode": "standalone (LUMEN_GI_PROBE_SCENE_TRACE=0 for trace, =1 for integrate/interpolate)",
        "config": {
            "weightMode": WEIGHT_MODE,
            "interpDepthThreshold": INTERP_DEPTH_THRESHOLD,
            "interpDepthSigmaInv": INTERP_DEPTH_SIGMA_INV,
            "interpNormalExponent": INTERP_NORMAL_EXPONENT,
            "interpMaterialMismatchWeight": INTERP_MATERIAL_MISMATCH,
            "interpFallbackConfidenceScale": INTERP_FALLBACK_CONF_SCALE,
        },
    }
    verdicts = []

    # --- Mogwai graph: real GBufferRT + stale LumenGI per-pixel GI -------------------
    graph = build_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    try:
        setup_scene(SCENE)
        m.renderFrame()
        log("PROBEINTERP graph rendered (GBufferRT + stale LumenGI)")
        verdicts.append(("graph render (GBufferRT + stale LumenGI)", "PASS"))
    except Exception as exc:  # pragma: no cover
        log("PROBEINTERP ERROR graph render failed: %r" % (exc,))
        report["graph_render"] = False
        report["verdicts"] = [("graph render (GBufferRT + stale LumenGI)", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return

    linearz = to_hw(np.asarray(m.activeGraph.get_output("GBufferRT.linearZ").to_numpy(), dtype=np.float32), W, H)
    lin_z = linearz[..., 0] if linearz.ndim == 3 else linearz
    matid_raw = to_hw(m.activeGraph.get_output("GBufferRT.normWRoughnessMaterialID").to_numpy(), W, H)
    vieww = to_hw(np.asarray(m.activeGraph.get_output("GBufferRT.viewW").to_numpy(), dtype=np.float32), W, H)
    if vieww.ndim == 3:
        vieww = vieww[..., :3]
    gbuffer_gi = to_hw(np.asarray(m.activeGraph.get_output("LumenGI.diffuseRadianceHitDist").to_numpy(), dtype=np.float32), W, H)
    if gbuffer_gi.ndim == 3:
        gbuffer_gi = gbuffer_gi[..., :3]

    # Adopt the ACTUAL rendered dims (defensive: the framebuffer may not equal RESOLUTION).
    H = int(lin_z.shape[0])
    W = int(lin_z.shape[1])
    report["resolution"] = [W, H]
    grid_x = (W + TILE - 1) // TILE
    grid_y = (H + TILE - 1) // TILE
    probe_count = grid_x * grid_y
    report["probeGridDims"] = [grid_x, grid_y]
    report["probeCount"] = probe_count
    report["gbuffer"] = {
        "linearz_shape": [H, W],
        "linearz_max": float(np.max(lin_z)),
        "sky_pixels": int(np.sum(lin_z <= 0.0)),
        "gi_max": float(np.max(gbuffer_gi)) if gbuffer_gi.size else None,
        "gi_nonzero_frac": float(np.mean(gbuffer_gi > 0.01)) if gbuffer_gi.size else 0.0,
    }

    # GBuffer oct normal + material ID (float [0,1]).
    h_px, w_px = int(lin_z.shape[0]), int(lin_z.shape[1])
    oct_xy, mat_w = decode_normal(matid_raw, h_px, w_px)
    pixel_normals = oct_to_ndir(oct_xy)

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

    # --- Dedicated compute device (D3D12 debug layer for validation errors) ----------
    try:
        device = Device(enable_debug_layer=True)
    except Exception as exc:  # pragma: no cover
        log("PROBEINTERP NOTE debug-layer device unavailable, retrying without: %r" % (exc,))
        device = Device(enable_debug_layer=False)

    srgb = ResourceBindFlags.ShaderResource
    srgb_uav = ResourceBindFlags.ShaderResource | ResourceBindFlags.UnorderedAccess

    lin_on_device = device.create_texture(W, H, 0, ResourceFormat.RG32Float, 1, 1, srgb)
    lin_on_device.from_numpy(np.asarray(linearz, dtype=np.float32), mip_level=0, array_slice=0)

    mips = build_hzb_reference_floor(lin_z, W, H)
    mip_count = len(mips)
    hzb_tex = device.create_texture(W, H, 0, ResourceFormat.R32Float, 1, mip_count, srgb_uav)
    for mm in range(mip_count):
        hzb_tex.from_numpy(np.asarray(mips[mm], dtype=np.float32), mip_level=mm, array_slice=0)

    # GBuffer normal emulation: RGBA32F with the real oct-encoded xy + material ID in w
    # (the Z1 trace decodes meta.normalW = oct_to_ndir(octXY); the interpolate pass uses the
    # same decode for the pixel normal).
    normal_data = np.zeros((H, W, 4), dtype=np.float32)
    normal_data[..., 0:2] = oct_xy.astype(np.float32)
    normal_data[..., 3] = mat_w.astype(np.float32)
    normal_tex = device.create_texture(W, H, 0, ResourceFormat.RGBA32Float, 1, 1, srgb)
    normal_tex.from_numpy(normal_data, mip_level=0, array_slice=0)

    gi_tex = device.create_texture(W, H, 0, ResourceFormat.RGBA16Float, 1, 1, srgb)
    gi_float = np.zeros((H, W, 4), dtype=np.float16)
    gi_float[..., :3] = gbuffer_gi.astype(np.float16)
    gi_tex.from_numpy(gi_float, mip_level=0, array_slice=0)

    # --- Probe buffers / textures ------------------------------------------------------
    meta_buf = device.create_structured_buffer(META_DTYPE.itemsize, probe_count, srgb_uav, MemoryType.DeviceLocal)
    hit_buf = device.create_structured_buffer(HIT_DTYPE.itemsize, probe_count * STRIDE, srgb_uav, MemoryType.DeviceLocal)
    counter_buf = device.create_structured_buffer(32, 1, srgb_uav, MemoryType.DeviceLocal)
    debug_buf = device.create_structured_buffer(16, probe_count * 4, srgb_uav, MemoryType.DeviceLocal)
    radiance_tex = device.create_texture(W, H, 0, ResourceFormat.RGBA16Float, 1, 1, srgb_uav)
    gi_out_tex = device.create_texture(W, H, 0, ResourceFormat.RGBA16Float, 1, 1, srgb_uav)

    metas0 = np.zeros(probe_count, dtype=META_DTYPE)
    for gy in range(grid_y):
        for gx in range(grid_x):
            i = gy * grid_x + gx
            metas0[i]["screenPos"] = (gx * TILE + TILE * 0.5 + 0.5, gy * TILE + TILE * 0.5 + 0.5)
    meta_buf.from_numpy(metas0.view(np.uint8))
    hit_buf.from_numpy(np.zeros(probe_count * STRIDE * HIT_DTYPE.itemsize, dtype=np.uint8))
    counter_buf.from_numpy(np.zeros(32, dtype=np.uint8))
    radiance_tex.from_numpy(np.zeros((H, W, 4), dtype=np.float16), mip_level=0, array_slice=0)
    gi_out_tex.from_numpy(np.zeros((H, W, 4), dtype=np.float16), mip_level=0, array_slice=0)

    # --- Shader passes ----------------------------------------------------------------
    trace_defines = {
        "is_valid_gScreenTraceResult": "0",
        "is_valid_gDiffuseRadianceHitDist": "1",
        "is_valid_gNormalRoughnessMaterialID": "1",
        "is_valid_gProbeRadiance": "1",
        "is_valid_gProbeTLAS": "0",
        "is_valid_gProbeDebug": "1",
    }
    probe_compute_defines = {
        "LUMEN_GI_PROBE_SCENE_TRACE": "1",  # skips the data module's gProbeTLAS declaration.
        "is_valid_gProbeMeta": "1",
        "is_valid_gProbeHitRecords": "1",
        "is_valid_gProbeRadiance": "1",
        "is_valid_gLinearZ": "1",
        "is_valid_gNormalRoughnessMaterialID": "1",
        "is_valid_gGIOutput": "1",
    }
    try:
        p_update = make_shader_pass(device, SHADER_TRACE, "updateMain", trace_defines)
        p_trace = make_shader_pass(device, SHADER_TRACE, "traceMain", trace_defines)
        p_finalize = make_shader_pass(device, SHADER_TRACE, "finalizeMain", trace_defines)
        log("PROBEINTERP LumenScreenProbeTrace.cs.slang compiled (3 entry points)")
        verdicts.append(("LumenScreenProbeTrace.cs.slang compile (update/trace/finalize)", "PASS"))
    except Exception as exc:  # pragma: no cover
        log("PROBEINTERP ERROR trace shader failed to compile: %r" % (exc,))
        report["trace_compile"] = repr(exc)
        report["verdicts"] = verdicts + [("LumenScreenProbeTrace.cs.slang compile (update/trace/finalize)", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return
    report["trace_compile"] = "ok"

    try:
        p_integrate = make_shader_pass(device, SHADER_INTEGRATE, "main", probe_compute_defines)
        log("PROBEINTERP LumenScreenProbeIntegrate.cs.slang compiled")
        verdicts.append(("LumenScreenProbeIntegrate.cs.slang compile", "PASS"))
    except Exception as exc:  # pragma: no cover
        log("PROBEINTERP ERROR integrate shader failed to compile: %r" % (exc,))
        report["integrate_compile"] = repr(exc)
        report["verdicts"] = verdicts + [("LumenScreenProbeIntegrate.cs.slang compile", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return
    report["integrate_compile"] = "ok"

    try:
        p_interpolate = make_shader_pass(device, SHADER_INTERPOLATE, "main", probe_compute_defines)
        log("PROBEINTERP LumenScreenProbeInterpolate.cs.slang compiled")
        verdicts.append(("LumenScreenProbeInterpolate.cs.slang compile", "PASS"))
    except Exception as exc:  # pragma: no cover
        log("PROBEINTERP ERROR interpolate shader failed to compile: %r" % (exc,))
        report["interpolate_compile"] = repr(exc)
        report["verdicts"] = verdicts + [("LumenScreenProbeInterpolate.cs.slang compile", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return
    report["interpolate_compile"] = "ok"

    def bind_trace_cb(root, frame_index, interval, dirs):
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
        cb["gWeightMode"] = WEIGHT_MODE

    def bind_trace_resources(root):
        root["gLinearZ"] = lin_on_device
        root["gHZBMips"] = hzb_tex
        root["gNormalRoughnessMaterialID"] = normal_tex
        root["gDiffuseRadianceHitDist"] = gi_tex
        root["gProbeMeta"] = meta_buf
        root["gProbeHitRecords"] = hit_buf
        root["gProbeCounters"] = counter_buf
        root["gProbeRadiance"] = radiance_tex
        root["gProbeDebug"] = debug_buf

    def bind_probe_compute(root, bind_goutput):
        # Same shared CB fields the C++ host fills for the integrate/interpolate passes.
        cb = root["LumenScreenProbeCB"]
        cb["gFrameDim"] = uint2(W, H)
        cb["gFrameIndex"] = 0
        cb["gDirectionsPerProbe"] = DIRS
        cb["gProbeGridDims"] = uint2(grid_x, grid_y)
        cb["gMaxHitRecordStride"] = STRIDE
        cb["gUpdateInterval"] = 1
        cb["gSeed"] = SEED
        cb["gCameraFocalPx"] = focal_px
        cb["gPrincipalPoint"] = float2(principal[0], principal[1])
        cb["gInvFrameDim"] = float2(1.0 / W, 1.0 / H)
        cb["gCameraPosW"] = float3(pos[0], pos[1], pos[2])
        cb["gCameraRightW"] = float3(right[0], right[1], right[2])
        cb["gCameraUpW"] = float3(upv[0], upv[1], upv[2])
        cb["gCameraForwardW"] = float3(fwd[0], fwd[1], fwd[2])
        cb["gProbeCount"] = probe_count
        cb["gWeightMode"] = WEIGHT_MODE
        root["gLinearZ"] = lin_on_device
        root["gNormalRoughnessMaterialID"] = normal_tex
        root["gProbeMeta"] = meta_buf
        root["gProbeHitRecords"] = hit_buf
        root["gProbeRadiance"] = radiance_tex
        if bind_goutput:
            interp_cb = root["LumenScreenProbeInterpolateCB"]
            interp_cb["gProbeGridDim"] = uint2(grid_x, grid_y)
            interp_cb["gDepthThreshold"] = INTERP_DEPTH_THRESHOLD
            interp_cb["gDepthSigmaInv"] = INTERP_DEPTH_SIGMA_INV
            interp_cb["gNormalExponent"] = INTERP_NORMAL_EXPONENT
            interp_cb["gMaterialMismatchWeight"] = INTERP_MATERIAL_MISMATCH
            interp_cb["gFallbackConfidenceScale"] = INTERP_FALLBACK_CONF_SCALE
            interp_cb["gPad"] = float2(0.0, 0.0)
            root["gGIOutput"] = gi_out_tex

    update_threads = ((probe_count + 63) // 64) * 64
    trace_threads = ((probe_count * DIRS + 63) // 64) * 64
    interp_threads_x = ((W + 7) // 8) * 8
    interp_threads_y = ((H + 7) // 8) * 8

    def dispatch(frame_index, interval, dirs):
        counter_buf.from_numpy(np.zeros(32, dtype=np.uint8))
        hit_buf.from_numpy(np.zeros(probe_count * STRIDE * HIT_DTYPE.itemsize, dtype=np.uint8))
        radiance_tex.from_numpy(np.zeros((H, W, 4), dtype=np.float16), mip_level=0, array_slice=0)
        gi_out_tex.from_numpy(np.zeros((H, W, 4), dtype=np.float16), mip_level=0, array_slice=0)
        for p in (p_update, p_trace, p_finalize):
            bind_trace_cb(p.root_var, frame_index, interval, dirs)
            bind_trace_resources(p.root_var)
        tt = ((probe_count * dirs + 63) // 64) * 64
        p_update.execute(update_threads, 1, 1)
        p_trace.execute(tt, 1, 1)
        p_finalize.execute(update_threads, 1, 1)
        # S4.3 integrate + interpolate (mirrors the C++ host order).
        bind_probe_compute(p_integrate.root_var, False)
        p_integrate.execute(update_threads, 1, 1)
        bind_probe_compute(p_interpolate.root_var, True)
        p_interpolate.execute(interp_threads_x, interp_threads_y, 1)
        device.wait()

    def read_back():
        metas = read_struct(meta_buf, META_DTYPE, probe_count)
        hits = read_struct(hit_buf, HIT_DTYPE, probe_count * STRIDE)
        ctr = read_struct(counter_buf, np.dtype([("c", "<u4", (8,))]), 1)["c"][0]
        rad = np.asarray(radiance_tex.to_numpy(), dtype=np.float32)
        gi = np.asarray(gi_out_tex.to_numpy(), dtype=np.float32)
        return metas, hits, ctr, rad, gi

    # ---- Frame 0 (interval 1: all probes) -------------------------------------------
    dispatch(0, 1, DIRS)
    metas, hits, ctr, rad, gi = read_back()
    log("PROBEINTERP frame 0 dispatched + read back")

    # --- V5 / S4.2 regression: counters + hit records + finalize grid -----------------
    active = metas["active"].astype(bool)
    rec_flags = hits["flags"]
    screen_mask = (rec_flags & F_SCREEN_HIT) != 0
    fb_mask = (rec_flags & F_FALLBACK_UNAVAIL) != 0
    hwrt_mask = (rec_flags & (F_HWRT_HIT | F_HWRT_MISS)) != 0
    touched = rec_flags != 0
    unexpected = touched & ~(screen_mask | fb_mask | hwrt_mask)
    records_written = int(np.sum(touched))
    c_ok = (
        ctr[0] + ctr[1] == ctr[7]
        and ctr[1] == ctr[4]
        and ctr[7] == int(np.sum(active)) * DIRS
        and ctr[5] == int(probe_count - np.sum(active))
        and ctr[6] == 0
    )
    records_ok = (
        records_written == int(np.sum(active)) * DIRS
        and np.sum(unexpected) == 0
        and (np.sum(screen_mask) + np.sum(fb_mask)) == records_written
    )
    report["s42_regression"] = {
        "active_probes": int(np.sum(active)),
        "counters": [int(v) for v in ctr],
        "counters_ok": bool(c_ok),
        "records_written": records_written,
        "records_ok": bool(records_ok),
    }
    if c_ok and records_ok:
        verdicts.append(("S4.2 regression: counters + hit records consistent (frame 0, interval 1)", "PASS"))
    else:
        verdicts.append(("S4.2 regression: counters + hit records consistent (frame 0, interval 1)", "FAIL"))

    # --- V1: integrate output ----------------------------------------------------------
    px = metas["screenPos"][:, 0].astype(int)
    py = metas["screenPos"][:, 1].astype(int)
    in_frame = (px >= 0) & (px < W) & (py >= 0) & (py < H)
    rad_at = np.zeros((probe_count, 4), dtype=np.float32)
    valid_pos = in_frame & active
    if np.any(valid_pos):
        rad_at[valid_pos] = rad[py[valid_pos], px[valid_pos]]

    E_gpu = rad_at[:, :3].astype(np.float64)
    conf_gpu = rad_at[:, 3].astype(np.float64)
    E_cpu, conf_cpu, valid_frac_cpu = integrate_cpu_reference(hits, DIRS, STRIDE, MAX_RADIANCE)

    integ_finite = bool(np.all(np.isfinite(E_gpu[active]))) if np.any(active) else True
    integ_nonneg = bool(np.min(E_gpu[active]) >= 0.0) if np.any(active) else True
    conf_in_range = bool(np.all((conf_gpu[active] >= 0.0) & (conf_gpu[active] <= 1.0))) if np.any(active) else True

    lit = active & valid_pos & (E_gpu.max(axis=-1) > 0.05)
    lit_fraction = float(np.mean(lit)) if np.any(active) else 0.0

    # CPU-reference comparison over ACTIVE in-frame probes that the shader produced E>0 for.
    # The gating error is measured on STRONG probes (E_cpu.max >= 0.01): near-zero irradiance
    # values are dominated by RGBA16F half-float quantization (11-bit mantissa), which inflates
    # the relative error of tiny records without indicating an estimator mismatch. The all-mask
    # error is still reported as a diagnostic.
    cmp_mask = active & valid_pos & (E_cpu.max(axis=-1) > 0.0)
    strong_mask = cmp_mask & (E_cpu.max(axis=-1) >= 0.01)
    max_rel_err = None
    mean_rel_err = None
    max_rel_err_strong = None
    if np.any(cmp_mask):
        denom = np.maximum(E_cpu[cmp_mask], 1e-6)
        rel = np.abs(E_gpu[cmp_mask] - E_cpu[cmp_mask]) / denom
        max_rel_err = float(np.max(rel))
        mean_rel_err = float(np.mean(rel))
    if np.any(strong_mask):
        denom_s = np.maximum(E_cpu[strong_mask], 1e-6)
        rel_s = np.abs(E_gpu[strong_mask] - E_cpu[strong_mask]) / denom_s
        max_rel_err_strong = float(np.max(rel_s))
    integ_cpu_match = max_rel_err_strong is not None and max_rel_err_strong < 0.05

    report["integrate"] = {
        "finite": integ_finite,
        "nonnegative": integ_nonneg,
        "confidence_in_range": conf_in_range,
        "active_probes": int(np.sum(active)),
        "lit_probe_fraction_gt_0_05": lit_fraction,
        "E_max": float(np.max(E_gpu[active])) if np.any(active) else None,
        "E_mean_active": float(np.mean(E_gpu[active])) if np.any(active) else None,
        "conf_mean_active": float(np.mean(conf_gpu[active])) if np.any(active) else None,
        "cpu_ref_max_rel_err_all": max_rel_err,
        "cpu_ref_mean_rel_err": mean_rel_err,
        "cpu_ref_max_rel_err_strong": max_rel_err_strong,
        "cpu_ref_strong_count": int(np.sum(strong_mask)),
    }
    if integ_finite and integ_nonneg and conf_in_range and integ_cpu_match and lit_fraction > 0.01:
        verdicts.append(("integrate output finite/non-neg, conf in [0,1], matches CPU mode-0 estimator (strong-probe max rel err < 5%)", "PASS"))
    else:
        verdicts.append(("integrate output finite/non-neg, conf in [0,1], matches CPU mode-0 estimator (strong-probe max rel err < 5%)", "FAIL"))

    # --- V2: interpolate output --------------------------------------------------------
    gi_rgb = gi[..., :3].astype(np.float64)
    gi_conf = gi[..., 3].astype(np.float64)
    good_px = (lin_z > 0.0) & np.isfinite(lin_z)
    interp_finite = bool(np.all(np.isfinite(gi_rgb[good_px]))) if np.any(good_px) else True
    interp_nonneg = bool(np.min(gi_rgb[good_px]) >= 0.0) if np.any(good_px) else True
    gi_conf_in_range = bool(np.all((gi_conf[good_px] >= 0.0) & (gi_conf[good_px] <= 1.0))) if np.any(good_px) else True
    lit_mean = float(np.mean(gi_rgb[good_px & (gi_rgb.max(axis=-1) > 0.05)])) if np.any(good_px) else 0.0
    lit_frac = float(np.mean(good_px & (gi_rgb.max(axis=-1) > 0.05))) if np.any(good_px) else 0.0

    report["interpolate"] = {
        "shape": [int(gi.shape[0]), int(gi.shape[1])],
        "finite": interp_finite,
        "nonnegative": interp_nonneg,
        "confidence_in_range": gi_conf_in_range,
        "lit_mean_E": lit_mean,
        "lit_fraction": lit_frac,
        "E_max": float(np.max(gi_rgb[good_px])) if np.any(good_px) else None,
        "conf_mean_surface": float(np.mean(gi_conf[good_px])) if np.any(good_px) else None,
    }
    shape_ok = [int(gi.shape[0]), int(gi.shape[1])] == [H, W]
    if shape_ok and interp_finite and interp_nonneg and gi_conf_in_range and lit_mean > 0.0 and lit_frac > 0.01:
        verdicts.append(("interpolate output full-res, finite/non-neg, conf in [0,1], lit regions carry E", "PASS"))
    else:
        verdicts.append(("interpolate output full-res, finite/non-neg, conf in [0,1], lit regions carry E", "FAIL"))

    # --- V3: boundary cross-blend (mirror run_probe_interp.py V2) ----------------------
    lz = np.log2(np.maximum(lin_z.astype(np.float64), 1e-6))
    lz[~good_px] = 0.0
    depth_edge = (neighbor_max_grad(lz) > LOGZ_EDGE) & good_px
    normal_edge = normal_edge_mask(pixel_normals) & good_px
    edge = (depth_edge | normal_edge) & good_px
    n_edge = int(edge.sum())
    gi_lum = gi_rgb[..., 0] * 0.2126 + gi_rgb[..., 1] * 0.7152 + gi_rgb[..., 2] * 0.0722
    cross_dominant = edge_side_cross_dominant(gi_lum, lz, pixel_normals, edge, good_px)
    n_dominant = int(cross_dominant.sum())
    cross_blend_fraction = float(n_dominant) / float(n_edge) if n_edge > 0 else None
    cross_blend_ok = None
    if n_edge >= MIN_EDGE_PIXELS:
        cross_blend_ok = cross_blend_fraction <= CROSS_BLEND_MAX_FRACTION
    report["boundary"] = {
        "depth_edge_pixels": int(depth_edge.sum()),
        "normal_edge_pixels": int(normal_edge.sum()),
        "edge_pixels": n_edge,
        "cross_side_dominant_pixels": n_dominant,
        "cross_blend_fraction": cross_blend_fraction,
        "cross_blend_ok": cross_blend_ok,
        "min_edge_pixels": MIN_EDGE_PIXELS,
        "max_fraction": CROSS_BLEND_MAX_FRACTION,
    }
    if cross_blend_ok is True:
        verdicts.append(("interpolate no cross-blend across depth/normal edges (%.3f <= %.2f)" % (cross_blend_fraction, CROSS_BLEND_MAX_FRACTION), "PASS"))
    elif cross_blend_ok is None:
        verdicts.append(("interpolate no cross-blend across depth/normal edges (too few edge px: %d)" % n_edge, "SKIP"))
    else:
        verdicts.append(("interpolate no cross-blend across depth/normal edges (%.3f > %.2f)" % (cross_blend_fraction, CROSS_BLEND_MAX_FRACTION), "FAIL"))

    # --- V4: determinism ----------------------------------------------------------------
    dispatch(0, 1, DIRS)
    _, _, _, rad2, gi2 = read_back()
    det_integrate = bool(np.array_equal(rad2.view(np.uint8), rad.view(np.uint8)))
    det_interpolate = bool(np.array_equal(gi2.view(np.uint8), gi.view(np.uint8)))
    report["determinism"] = {
        "integrate_identical": det_integrate,
        "interpolate_identical": det_interpolate,
    }
    if det_integrate and det_interpolate:
        verdicts.append(("determinism: re-dispatch byte-identical integrate + interpolate outputs", "PASS"))
    else:
        verdicts.append(("determinism: re-dispatch byte-identical integrate + interpolate outputs", "FAIL"))

    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    if any(v == "SKIP" for _, v in verdicts):
        report["summary"] = "SKIP" if all(v != "FAIL" for _, v in verdicts) else "FAIL"
    for name, verdict in verdicts:
        log("PROBEINTERP VERDICT %s -- %s" % (name, verdict))
    write_json(OUT_JSON, report)
    log("PROBEINTERP wrote %s" % os.path.abspath(OUT_JSON))


try:
    main()
except Exception as exc:
    print("PROBEINTERP ERROR script failed: %r" % (exc,))
    write_json(
        OUT_JSON,
        {
            "stage": "S4.3",
            "script": "run_probe_integrate_interp.py",
            "summary": "FAIL",
            "fatal_error": str(exc),
            "verdicts": [("script ran (defensive FAIL on fatal error)", "FAIL")],
        },
    )
exit()
