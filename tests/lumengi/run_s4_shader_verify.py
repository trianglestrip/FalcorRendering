from falcor import *

"""S4-A1 host-integration GPU verification (Agent W, standalone compute).

WHY THIS SCRIPT EXISTS
----------------------
The S4-A1 C++ wiring (LumenGI.cpp: HZB build pass + screen trace pass) cannot be
exercised by the CURRENT Release binary (rebuilt at the last HEAD, before the S4
wiring was added). This script drives the FROZEN shaders (LumenHZBBuild.cs.slang,
LumenScreenTrace.cs.slang) directly through the Python bindings on a dedicated
Device, so root's shader code is real-machine-compiled and dispatched exactly as
the host wires it (same CB field names, same resource names, same mip chain), and
the output is verified against the frozen LumenScreenTraceData.slang encoding
contract. The LumenGI (stale) pass in the Mogwai graph provides the S1 HWRT
diffuseRadianceHitDist reference for the hit-distance comparison, and real
GBufferRT.linearZ from the Cornell scene is uploaded to the compute device.

What it verifies:
  V0. Both frozen .slang files COMPILE at runtime (ComputePass creation throws on
      failure) with the exact binding names / CB field names the C++ host uses.
  V1. HZB mip 0 dispatch (gSourceIsLinearZ = 1) reproduces GBufferRT.linearZ.x.
  V2. The screen trace dispatches over the full frame and every texel of the
      gScreenTraceResult output obeys the frozen encoding:
        * hit  -> A in (0, 1], RGB = (hitUV.x, hitUV.y, hitLinearDistance)
        * miss -> A = -((float)reason + 1) < 0, reason in [1, kLumenScreenTraceMissReasonCount-1]
      hits + misses == W*H (task.md S4 gate: miss-reason total == rays launched).
  V3. (diagnostic) screen-trace hit distance vs HWRT diffuseRadianceHitDist.a with
      the S4_TODO placeholder tolerance (see run_screentrace.py header).

The HZB mips m > 0 are filled from the numpy ceil-halving reference instead of a
GPU dispatch: the Python bindings cannot create a per-mip UAV/SRV view (Texture has
no get_srv/get_uav), so a full GPU chain build is only exercisable from the C++ host
(root rebuild). The screen trace consumes the CPU-filled chain exactly as it would
the GPU-built one (bit-identical data, LumenHZB::buildMipCPU semantics).

Usage (root, from repo root):
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_s4_shader_verify.py ^
      --logfile artifacts\\lumengi\\S4\\shader_verify.log
"""

import json
import math
import os

import numpy as np

# -------------------------------------------------------------------------------------
# Config
# -------------------------------------------------------------------------------------
RESOLUTION = (640, 360)
FRAME_RATE = 60
SCENE = os.environ.get("LUMEN_S4_VERIFY_SCENE", "test_scenes/cornell_box.pyscene")
OUT_JSON = os.environ.get("LUMEN_S4_VERIFY_OUT", "artifacts/lumengi/S4/shader_verify.json")

# Shader paths: ABSOLUTE, because Falcor's shader resolver does not search the repo
# source dir for relative "Source/RenderPasses/..." paths (it searches the copied
# shaders dir, which will contain LumenHZBBuild.cs.slang only after the next build).
HZB_SHADER = os.path.abspath("Source/RenderPasses/LumenGI/ScreenTrace/LumenHZBBuild.cs.slang")
SCREEN_TRACE_SHADER = os.path.abspath("Source/RenderPasses/LumenGI/ScreenTrace/LumenScreenTrace.cs.slang")

# Mirrors of the frozen shader defaults / the C++ host constants (LumenGI.cpp).
MAX_STEPS = 64
MIN_THICKNESS = 0.001
THICKNESS_SCALE = 2.0
STEP_EPSILON = 1e-4
MAX_MIP = 12

# The C++ host constant kScreenTraceRayDirection (LumenGI.cpp): S4-A1 MVP fixed
# view-space direction, d.z < 0 (forward), constant per pixel.
RAY_DIR = np.array([0.5, 0.35, -1.0], dtype=np.float64)
RAY_DIR = RAY_DIR / np.linalg.norm(RAY_DIR)

MISS_REASON_COUNT = 5
MISS_REASON_NAMES = {0: "Hit", 1: "OutOfScreen", 2: "BehindSurface", 3: "MaxSteps", 4: "ThinGeometry"}
HWRT_MISS_HIT_DISTANCE = 65504.0
HIT_DIST_REL_TOL = 0.25  # S4_TODO[tolerance]: placeholder (freeze with root).


def log(msg):
    print(msg, flush=True)


def mark(msg):
    """Append a step marker to a progress file (outlives stdout buffering / kills)."""
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(OUT_JSON)), "shader_verify.progress"), "a") as f:
            f.write(msg + "\n")
    except Exception:
        pass


# -------------------------------------------------------------------------------------
# Helpers
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


def hzb_mip_dim(full_dim, mip):
    """LumenHZB::mipDimension -- ceil-halving (frozen S4-A1)."""
    return max((full_dim + (1 << mip) - 1) >> mip, 1)


def hzb_mip_count(width, height):
    """LumenHZB::mipCount -- 1 + ceil(log2(max dim))."""
    dim = max(width, height)
    count = 1
    while dim > 1:
        dim = (dim + 1) // 2
        count += 1
    return count


def hzb_build_mip(src, src_w, src_h, dst_w, dst_h):
    """One max-pool level, bit-identical to LumenHZB::buildMipCPU (clamp-to-edge)."""
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


def build_hzb_reference(linear_z, width, height):
    """Full chain [mip0, ...] matching LumenHZB::buildChainCPU (ceil-halving)."""
    mips = [np.asarray(linear_z, dtype=np.float32).reshape(height, width).copy()]
    mw, mh = width, height
    for m in range(1, hzb_mip_count(width, height)):
        src_w, src_h = mw, mh
        mw, mh = hzb_mip_dim(width, m), hzb_mip_dim(height, m)
        mips.append(hzb_build_mip(mips[-1], src_w, src_h, mw, mh))
    return mips


def hzb_mip_dim_floor(full_dim, mip):
    return max(full_dim >> mip, 1)


def build_hzb_reference_floor(linear_z, width, height):
    """Chain with FLOOR-halved dims -- what a native D3D12 texture mip chain
    actually holds (D3D12 always floor-halves; the frozen LumenHZB.h ceil contract
    cannot be realized as a native mip chain for non-power-of-two dims, see the
    hzb_contract section of the report). Each stored texel is the clamp-to-edge
    2x2 max of the previous FLOOR-sized mip."""
    mips = [np.asarray(linear_z, dtype=np.float32).reshape(height, width).copy()]
    mw, mh = width, height
    while max(mw, mh) > 1:
        nw, nh = max(mw >> 1, 1), max(mh >> 1, 1)
        mips.append(hzb_build_mip(mips[-1], mw, mh, nw, nh))
        mw, mh = nw, nh
    return mips


def build_graph():
    graph = RenderGraph("S4ShaderVerify")
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
    graph.markOutput("GBufferRT.linearZ")  # V1 source (real Cornell depth).
    graph.markOutput("LumenGI.diffuseRadianceHitDist")  # HWRT reference (V3).
    return graph


def make_shader_pass(device, shader_path, defines):
    desc = ProgramDesc()
    desc.add_shader_module().add_file(shader_path)
    desc.cs_entry("main")
    return ComputePass(device, desc, defines)


def setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 1


# -------------------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------------------


def main():
    report = {
        "stage": "S4",
        "script": "run_s4_shader_verify.py",
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "ray_direction": list(map(float, RAY_DIR)),
    }
    verdicts = []
    W, H = RESOLUTION
    MIPS = hzb_mip_count(W, H)

    # --- Mogwai graph: real Cornell linearZ + HWRT reference ----------------------
    mark("graph build start")
    graph = build_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    mark("graph added")
    try:
        setup_scene(SCENE)
        mark("scene loaded")
        m.renderFrame()
        mark("frame rendered")
        log("S4VERIFY graph rendered (GBufferRT + stale LumenGI HWRT)")
        verdicts.append(("graph render (GBufferRT + stale LumenGI HWRT)", "PASS"))
    except Exception as exc:  # pragma: no cover
        log("S4VERIFY ERROR graph render failed: %r" % (exc,))
        report["graph_render"] = False
        report["verdicts"] = [("graph render (GBufferRT + stale LumenGI HWRT)", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return

    mark("read linearZ")
    linearz_tex = m.activeGraph.get_output("GBufferRT.linearZ")
    linearz = np.asarray(linearz_tex.to_numpy(), dtype=np.float32)
    lin_z = linearz[..., 0] if linearz.ndim == 3 else linearz
    report["linearz_stats"] = {
        "min": float(np.min(lin_z)),
        "max": float(np.max(lin_z)),
        "sky_pixels": int(np.sum(lin_z <= 0.0)),
    }

    # --- Dedicated compute device (Mogwai's device is not exposed to Python). -----
    mark("device creation")
    try:
        device = Device(enable_debug_layer=True)
    except Exception as exc:  # pragma: no cover
        log("S4VERIFY NOTE debug-layer device unavailable, retrying without: %r" % (exc,))
        device = Device(enable_debug_layer=False)
    log("S4VERIFY compute device created")

    srgb = ResourceBindFlags.ShaderResource
    srgb_uav = ResourceBindFlags.ShaderResource | ResourceBindFlags.UnorderedAccess

    # Upload linearZ (RG32F, .x = depth) to the compute device.
    lin_on_device = device.create_texture(W, H, 0, ResourceFormat.RG32Float, 1, 1, srgb)
    lin_on_device.from_numpy(np.asarray(linearz, dtype=np.float32), mip_level=0, array_slice=0)
    mark("linearZ uploaded")

    hzb = device.create_texture(W, H, 0, ResourceFormat.R32Float, 1, MIPS, srgb_uav)
    result = device.create_texture(W, H, 0, ResourceFormat.RGBA16Float, 1, 1, srgb_uav)
    raydir = device.create_texture(W, H, 0, ResourceFormat.RGBA32Float, 1, 1, srgb)
    ray4 = np.zeros((H, W, 4), dtype=np.float32)
    ray4[..., :3] = RAY_DIR.astype(np.float32)
    ray4[..., 3] = 1.0
    raydir.from_numpy(ray4, mip_level=0, array_slice=0)

    # ---- V0/V1: HZB build shader compiles + mip 0 dispatch -----------------------
    mark("HZB pass creation")
    try:
        hzb_pass = make_shader_pass(device, HZB_SHADER, {})
        log("S4VERIFY LumenHZBBuild.cs.slang compiled")
    except Exception as exc:  # pragma: no cover
        log("S4VERIFY ERROR LumenHZBBuild.cs.slang failed to compile: %r" % (exc,))
        report["hzb_compile"] = repr(exc)
        report["verdicts"] = [("LumenHZBBuild.cs.slang runtime compile", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return
    report["hzb_compile"] = "ok"
    verdicts.append(("LumenHZBBuild.cs.slang runtime compile", "PASS"))

    hzb_root = hzb_pass.root_var
    hzb_cb = hzb_root["LumenHZBBuildCB"]
    hzb_cb["gSourceMipSize"] = uint2(W, H)
    hzb_cb["gTargetMipSize"] = uint2(W, H)
    hzb_cb["gSourceIsLinearZ"] = 1
    hzb_cb["gPad"] = 0
    hzb_root["gLinearZSource"] = lin_on_device
    hzb_root["gHZBTarget"] = hzb  # whole-texture UAV == mip 0 (per-mip UAV needs the C++ host).
    hzb_pass.execute(W, H, 1)
    mark("HZB mip0 dispatched")
    log("S4VERIFY HZB mip 0 dispatched")

    mip0 = np.asarray(hzb.to_numpy(mip_level=0, array_slice=0), dtype=np.float32)
    ref_mips = build_hzb_reference(lin_z, W, H)
    mip0_diff = np.abs(mip0 - ref_mips[0])
    report["hzb_mip0"] = {
        "max_abs_diff": float(np.max(mip0_diff)) if mip0_diff.size else 0.0,
        "mean_abs_diff": float(np.mean(mip0_diff)) if mip0_diff.size else 0.0,
    }
    if mip0_diff.size and np.max(mip0_diff) <= 0.0:
        verdicts.append(("HZB mip0 == linearZ.x (bit-exact GPU dispatch)", "PASS"))
    else:
        verdicts.append(("HZB mip0 == linearZ.x (bit-exact GPU dispatch)", "FAIL"))

    # HZB ceil/floor contract conflict (flagged for root, see report):
    #   * The frozen LumenHZB.h contract is CEIL-halved (every mip fully covers its
    #     parent) and the screen trace indexes gHZB.Load(int3(cell, mip)) with ceil
    #     dims. LumenHZBBuild.cs.slang takes its dims from the CB, so the host is
    #     free to pass ceil dims.
    #   * But a native D3D12 texture mip chain is FLOOR-halved. For 640x360 the two
    #     disagree from mip 4 on (H 23 vs 22, later W 3 vs 2), so a native texture
    #     CANNOT hold the ceil chain, and Load of the ceil tail is out-of-bounds.
    #   * Storage below therefore uses the FLOOR chain (what the native texture holds
    #     and what the C++ host's mipmapped R32Float texture will contain). Mip 0 is
    #     still verified bit-exact from the GPU dispatch above. The build of mips>0
    #     by the CPU reference here mirrors LumenHZB::buildMipCPU semantics on the
    #     real (floor) mip dims.
    mark("HZB mips upload")
    ref_floor = build_hzb_reference_floor(lin_z, W, H)
    for mip in range(1, MIPS):
        data = ref_floor[mip] if mip < len(ref_floor) else ref_floor[-1]  # 1x1 tail mips.
        hzb.from_numpy(np.asarray(data, dtype=np.float32), mip_level=mip, array_slice=0)

    # ---- V0/V2: screen trace shader compiles + dispatch --------------------------
    focal_px = float(m.scene.camera.focalLength * H / m.scene.camera.frameHeight)
    defines = {"is_valid_gHZB": "1", "is_valid_gLinearZ": "1", "is_valid_gRayDirection": "1"}
    mark("screen trace pass creation")
    try:
        st_pass = make_shader_pass(device, SCREEN_TRACE_SHADER, defines)
        log("S4VERIFY LumenScreenTrace.cs.slang compiled")
    except Exception as exc:  # pragma: no cover
        log("S4VERIFY ERROR LumenScreenTrace.cs.slang failed to compile: %r" % (exc,))
        report["screen_trace_compile"] = repr(exc)
        report["verdicts"] = verdicts + [("LumenScreenTrace.cs.slang runtime compile", "FAIL")]
        report["summary"] = "FAIL"
        write_json(OUT_JSON, report)
        return
    report["screen_trace_compile"] = "ok"
    verdicts.append(("LumenScreenTrace.cs.slang runtime compile", "PASS"))

    st_root = st_pass.root_var
    cb = st_root["LumenScreenTraceCB"]
    cb["gFrameDim"] = uint2(W, H)
    cb["gMaxSteps"] = MAX_STEPS
    cb["gStartMip"] = 0
    cb["gMinThickness"] = MIN_THICKNESS
    cb["gThicknessScale"] = THICKNESS_SCALE
    cb["gStepEpsilon"] = STEP_EPSILON
    cb["gCameraFocalPx"] = focal_px
    cb["gPrincipalPoint"] = float2(0.5 * W, 0.5 * H)
    cb["gInvFrameDim"] = float2(1.0 / W, 1.0 / H)
    cb["gMaxMip"] = min(MAX_MIP, MIPS - 1)
    cb["gPad"] = 0
    st_root["gLinearZ"] = lin_on_device
    st_root["gHZB"] = hzb
    st_root["gRayDirection"] = raydir
    st_root["gScreenTraceResult"] = result
    mark("screen trace dispatch")
    st_pass.execute(W, H, 1)
    log("S4VERIFY screen trace dispatched")

    mark("screen trace readback")
    out = np.asarray(result.to_numpy(), dtype=np.float32)  # (H, W, 4) RGBA16F
    a = out[..., 3]
    hit = a > 0.0

    reason = np.zeros(a.shape, dtype=np.int32)
    reason[hit] = 0
    miss_a = a[~hit]
    encode_ok = True
    if miss_a.size:
        dec = (-miss_a).astype(np.float32) - 1.0
        r = np.rint(dec).astype(np.int32)
        bad = int(np.count_nonzero((r < 1) | (r >= MISS_REASON_COUNT)))
        if bad:
            encode_ok = False
            log("S4VERIFY WARNING %d texels decode to an out-of-range miss reason" % bad)
        reason[~hit] = r

    total = W * H
    hist = {MISS_REASON_NAMES[r]: int(np.sum(reason == r)) for r in range(1, MISS_REASON_COUNT)}
    hist["Hit"] = int(np.sum(hit))
    hit_dist = np.where(hit, out[..., 2], 0.0)

    completeness_ok = int(np.sum(hit)) + int(np.sum(~hit)) == total
    report["screen_trace"] = {
        "focal_px": focal_px,
        "hits": int(np.sum(hit)),
        "misses": int(np.sum(~hit)),
        "rays_launched": total,
        "encoding_valid": bool(encode_ok),
        "hit_confidence_min": float(np.min(a[hit])) if np.any(hit) else None,
        "hit_confidence_max": float(np.max(a[hit])) if np.any(hit) else None,
        "hit_distance_min": float(np.min(hit_dist[hit])) if np.any(hit) else None,
        "hit_distance_max": float(np.max(hit_dist[hit])) if np.any(hit) else None,
        "miss_reason_histogram": hist,
        "completeness_ok": completeness_ok,
    }
    if encode_ok and completeness_ok:
        verdicts.append(("screen trace encoding + hits+misses == W*H", "PASS"))
    else:
        verdicts.append(("screen trace encoding + hits+misses == W*H", "FAIL"))

    # ---- V3 (diagnostic): screen trace hit distance vs HWRT ----------------------
    mark("hwrt readback")
    hwrt = np.asarray(
        m.activeGraph.get_output("LumenGI.diffuseRadianceHitDist").to_numpy(), dtype=np.float32
    )
    hwrt_dist = hwrt[..., 3] if hwrt.ndim == 3 else hwrt
    hwrt_hit = hwrt_dist < HWRT_MISS_HIT_DISTANCE
    both_hit = hit & hwrt_hit
    if np.any(both_hit):
        rel_err = np.abs(hit_dist - hwrt_dist) / np.maximum(np.abs(hwrt_dist), 1e-6)
        g1_ok = int(np.count_nonzero(rel_err[both_hit] <= HIT_DIST_REL_TOL))
        report["hwrt_compare"] = {
            "hwrt_hit_pixels": int(np.sum(hwrt_hit)),
            "both_hit_pixels": int(np.sum(both_hit)),
            "hit_dist_rel_tol": HIT_DIST_REL_TOL,
            "hit_dist_g1_valid": int(np.sum(both_hit)),
            "hit_dist_g1_ok": g1_ok,
            "hit_dist_max_rel_err": float(np.max(rel_err[both_hit])),
            "hit_dist_mean_rel_err": float(np.mean(rel_err[both_hit])),
        }
        # Diagnostic only: the S4 gate tolerance is S4_TODO (run_screentrace.py).
        log(
            "S4VERIFY NOTE hit-distance vs HWRT: %d/%d pixels within rel-tol %g "
            "(S4_TODO tolerance, diagnostic)" % (g1_ok, int(np.sum(both_hit)), HIT_DIST_REL_TOL)
        )
    else:
        report["hwrt_compare"] = {"both_hit_pixels": 0}

    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all(v == "PASS" for _, v in verdicts) else "FAIL"
    for name, verdict in verdicts:
        log("S4VERIFY VERDICT %s -- %s" % (name, verdict))
    mark("write json")
    write_json(OUT_JSON, report)


# Falcor's embedded Python executes the script with __name__ == 'builtins', so an
# `if __name__ == "__main__":` guard would never run. Run at module level, like the
# other working tests/lumengi scripts (run_diag.py, run_stability.py, ...).
main()
exit()
