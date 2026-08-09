from falcor import *

"""S4-A1 HZB build-correctness GPU-side verification skeleton (Agent R2).

The HZB is an INTERNAL LumenGI texture (R32F, mip chain built by
LumenHZBBuild.cs.slang from GBufferRT.linearZ). It is not a graph output, so
this script cannot read it directly today. After S4-A1 integration, root wires
an observation path and this script runs the check. Until then it PROBES for
the observation channel and SKIPs (never crashes).

HZB CONTRACT (LumenHZB.h / LumenHZBBuild.cs.slang, frozen):
  * mip m dims  = max(ceil(dim / 2^m), 1x1)   (CEIL halving, S4-A1 fixed by root; every mip
    fully covers its parent so the max chain is conservative for hierarchical tracing).
  * mip 0       = full-resolution linear depth == GBufferRT.linearZ.x.
  * mip m+1 (x, y) = MAX of mip m texels
        (2x, 2y), (2x+1, 2y), (2x, 2y+1), (2x+1, 2y+1),
    with out-of-range coordinates CLAMPED to the source dims - 1 (edge texel
    duplicated). mipCount = 1 + ceil(log2(max(width, height))).

  The CPU mirror buildChainCPU/buildMipCPU in LumenHZB.h is the bit-identical
  reference; build_hzb_reference() below re-implements exactly that in numpy
  so the GPU readback can be compared pixel-exact (the max semantics: every
  mip value is the 2x2 max of its parent).

HOW ROOT INTEGRATES THE OBSERVATION PATH (S4_TODO[channel] contract):
  Option A (preferred): add an OPTIONAL LumenGI output channel, e.g. "hzb",
  that exposes the R32F mip chain. Falcor Texture.to_numpy() reads one mip /
  array slice, so expose the chain as a 2D ARRAY texture (mipCount layers,
  layer i = mip i, each padded to the mip0 dims) OR add per-mip debug views
  (debugMode entry) that blit mip i into an RGBA view. The script below handles
  both a (count, H, W) array readback and a flat per-mip decode; freeze the
  exact exposure with root at S4_A1.
  Option B: a readback binding on the internal texture + a CPU copy in the pass
  (like mpLumenGICountersReadback). The script's check_mip_chain() is then fed
  directly.

VERIFICATION (task.md S4 gate + HZB max semantics):
  V1. mip 0 == GBufferRT.linearZ.x  (bit-identical, within fp readback noise).
  V2. For every mip m >= 1: GPU mip m == numpy reference build_hzb_reference()
      (i.e. each texel is the 2x2 max of the previous mip, clamp-to-edge).
  V3. Structural: mip dims and mipCount match LumenHZB::makeCreateParams.

S4_TODO markers:
  * S4_TODO[channel]: freeze the HZB observation channel name / exposure format
    with root (candidate "hzb" array channel). Absent -> SKIP.
  * S4_TODO[tolerance]: fp readback of R32F is exact (round-trip), but if root
    exposes the chain through a re-encoded debug view (RGBA16F / 8-bit), a
    tolerance must be frozen here. Default 0.0 (bit-exact) for the R32F path.
  * S4_TODO[linearz]: V1 needs GBufferRT.linearZ readback; if root prefers the
    HZB pass to export its own mip0 copy, use that instead.

Exit: Falcor `exit()`; report JSON written to artifacts/lumengi/S4/hzb.json.
"""

import json
import math
import os

import numpy as np

# -------------------------------------------------------------------------------------
# Configuration (S4_TODO: freeze with root).
# -------------------------------------------------------------------------------------
RESOLUTION = (640, 360)
FRAME_RATE = 60
SCENE = "test_scenes/cornell_box.pyscene"
OUT_JSON = os.environ.get("LUMEN_HZB_OUT", "artifacts/lumengi/S4/hzb.json")

# S4_TODO[channel]: candidate LumenGI output channel exposing the HZB chain.
HZB_CHANNEL = "hzb"
# Tolerance for V1/V2. 0.0 == bit-exact (R32F readback path); raise it only if
# root exposes the chain through a lossy debug view (S4_TODO[tolerance]).
HZB_TOL = 0.0


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
# HZB reference (numpy mirror of LumenHZB::buildMipCPU / buildChainCPU).
# -------------------------------------------------------------------------------------


def hzb_mip_dim(full_dim, mip):
    """max(ceil(fullDim / 2^mip), 1) - LumenHZB::mipDimension (ceil-halving, frozen S4-A1)."""
    return max((full_dim + (1 << mip) - 1) >> mip, 1)


def hzb_mip_count(width, height):
    """1 + ceil(log2(max(width, height))) - LumenHZB::mipCount (ceil-halving)."""
    dim = max(width, height)
    count = 1
    while dim > 1:
        dim = (dim + 1) // 2
        count += 1
    return count


def hzb_build_mip(src, src_w, src_h, dst_w, dst_h):
    """One max-pool level, bit-identical to LumenHZB::buildMipCPU:
    dst texel (x, y) = max of the 2x2 block in src at (2x, 2y)..(2x+1, 2y+1),
    clamped to the source dims - 1. src is a (src_h, src_w) float array."""
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
    """Full chain: mip0 = linearZ (identity), mip m+1 = max-pool of mip m.
    Returns [np (mh, mw) float32, ...], one per mip, matching LumenHZB
    buildChainCPU. Raises ValueError when the mip chain is degenerate (the
    pyramid shape breaks below 2x2; the frozen contract stops at dims 1x1)."""
    if width < 1 or height < 1:
        raise ValueError("invalid HZB dims %dx%d" % (width, height))
    mips = []
    mip0 = np.asarray(linear_z, dtype=np.float32).reshape(height, width)
    mips.append(mip0.copy())
    mw, mh = width, height
    for m in range(1, hzb_mip_count(width, height)):
        src_w, src_h = mw, mh
        mw, mh = hzb_mip_dim(width, m), hzb_mip_dim(height, m)
        mips.append(hzb_build_mip(mips[-1], src_w, src_h, mw, mh))
    return mips


# -------------------------------------------------------------------------------------
# Graph / probe.
# -------------------------------------------------------------------------------------


def create_lumen_graph(mark_hzb):
    graph = RenderGraph("LumenGIHZBCheck")
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
                "enabled": True,
                "traceMode": "HardwareRT",
                "qualityPreset": "High",
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
    graph.markOutput("GBufferRT.linearZ")  # V1 source (S4_TODO[linearz]).
    if mark_hzb:
        # S4_TODO[channel]: freeze the exposure format with root (see header).
        graph.markOutput("LumenGI." + HZB_CHANNEL)
    return graph


def _setup_scene(scene_path):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 1


def probe_hzb(scene_path):
    """Probe the HZB observation channel. Returns (available, graph).

    graph.markOutput('LumenGI.hzb') throws immediately when the channel is
    absent, so the whole probe body (including create_lumen_graph) is inside the
    try; on failure we rebuild without the channel and render once (SKIP path).
    """
    graph = None
    try:
        graph = create_lumen_graph(mark_hzb=True)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(scene_path)
        m.clock.frame = 1
        m.renderFrame()
        return True, graph
    except Exception as exc:  # pragma: no cover - pre-S4 path
        print(
            "HZBCHECK WARNING HZB observation channel 'LumenGI.%s' not available "
            "(pre-S4-A1 integration expected); channel absent -> %s" % (HZB_CHANNEL, str(exc))
        )
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass
        graph = create_lumen_graph(mark_hzb=False)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene(scene_path)
        m.clock.frame = 1
        m.renderFrame()
        return False, graph


# -------------------------------------------------------------------------------------
# Decode + verification.
# -------------------------------------------------------------------------------------


def decode_hzb_chain(tex, width, height):
    """Decode the exposed HZB chain into [np (mh, mw) float32, ...], one per mip.

    Handles the two candidate exposures (S4_TODO[channel] freeze):
      * (count, H, W) array texture - layer i == mip i (each layer padded to the
        mip0 dims; cropped here to the mip dims).
      * flat RGBA debug view (H, W, 4) at mip0 resolution - not the preferred
        path; V2 then decodes the block-encoded max values. Kept as a stub with
        a clear error so root implements it when the freeze lands.
    Returns (mips, format_note)."""
    arr = np.asarray(tex, dtype=np.float32)
    note = "array"
    if arr.ndim == 3 and arr.shape[0] == hzb_mip_count(width, height) and arr.shape[1] == height and arr.shape[2] == width:
        mips = []
        for m in range(hzb_mip_count(width, height)):
            mh, mw = hzb_mip_dim(height, m), hzb_mip_dim(width, m)
            mips.append(arr[m, :mh, :mw])
        return mips, note
    raise NotImplementedError(
        "S4_TODO[channel]: HZB chain exposure format not frozen yet. Received "
        "array shape %s; expected (mipCount=%d, H=%d, W=%d) array texture. Freeze "
        "the exposure with root, then extend decode_hzb_chain()."
        % (arr.shape, hzb_mip_count(width, height), height, width)
    )


def check_hzb(actual_mips, reference_mips, tol):
    """Per-mip comparison: max-abs-diff and PASS/FAIL per mip + aggregate."""
    rows = []
    for m, (act, ref) in enumerate(zip(actual_mips, reference_mips)):
        act = np.asarray(act, dtype=np.float32)
        ref = np.asarray(ref, dtype=np.float32)
        if act.shape != ref.shape:
            rows.append(
                {
                    "mip": m,
                    "dims": list(act.shape),
                    "expected_dims": list(ref.shape),
                    "max_abs_diff": None,
                    "passed": False,
                    "note": "dims mismatch",
                }
            )
            continue
        if act.size == 0:
            rows.append({"mip": m, "dims": list(act.shape), "max_abs_diff": 0.0, "passed": True, "note": "empty"})
            continue
        diff = np.abs(act - ref)
        rows.append(
            {
                "mip": m,
                "dims": list(act.shape),
                "max_abs_diff": float(np.max(diff)),
                "mean_abs_diff": float(np.mean(diff)),
                "passed": bool(np.max(diff) <= tol),
                "note": "ok",
            }
        )
    return rows


def main(scene_path, out_json):
    available, graph = probe_hzb(scene_path)
    report = {
        "stage": "S4",
        "script": "run_hzb_check.py",
        "scene": scene_path,
        "resolution": list(RESOLUTION),
        "hzb_channel": HZB_CHANNEL if available else None,
        "hzb_available": available,
        "expected_mip_count": hzb_mip_count(*RESOLUTION),
    }

    if not available:
        report["verdicts"] = [("HZB observation channel unavailable", "SKIP")]
        report["summary"] = "SKIP"
        for name, verdict in report["verdicts"]:
            print("HZBCHECK VERDICT", name, verdict)
        print(
            "HZBCHECK SKIP pre-S4-A1 integration (observation channel '%s' absent; "
            "root must expose the HZB chain - see S4_TODO[channel])" % HZB_CHANNEL
        )
        write_json(out_json, report)
        return

    # V1: GBufferRT.linearZ readback (RG32F; .x = linear depth in meters).
    lin = m.activeGraph.get_output("GBufferRT.linearZ").to_numpy()
    lin = np.asarray(lin, dtype=np.float32)
    lin_z = lin[..., 0] if lin.ndim == 3 else lin

    # S4_TODO[channel]: read the frozen HZB exposure.
    hzb_tex = m.activeGraph.get_output("LumenGI.%s" % HZB_CHANNEL).to_numpy()
    actual_mips, exposure_note = decode_hzb_chain(hzb_tex, *RESOLUTION)

    # Structural (V3): mip count must match LumenHZB::mipCount.
    expected_count = hzb_mip_count(*RESOLUTION)
    structural_ok = len(actual_mips) == expected_count
    structural = {
        "expected_mip_count": expected_count,
        "actual_mip_count": len(actual_mips),
        "passed": structural_ok,
    }

    # Reference chain (V1+V2).
    reference_mips = build_hzb_reference(lin_z, *RESOLUTION)

    # V1: mip 0 must equal linearZ.x.
    mip0_row = check_hzb(actual_mips[:1], reference_mips[:1], HZB_TOL)[0]
    # V2: all mips >= 1 must equal the max-pool reference.
    coarse_rows = check_hzb(actual_mips[1:], reference_mips[1:], HZB_TOL)
    mip_rows = [mip0_row] + coarse_rows

    all_pass = structural_ok and all(r["passed"] for r in mip_rows)
    verdicts = []
    verdicts.append(("HZB structural (mipCount == 1+ceil(log2 max dim))", "PASS" if structural_ok else "FAIL"))
    verdicts.append(("HZB mip0 == linearZ.x (max-abs <= %g)" % HZB_TOL, "PASS" if mip0_row["passed"] else "FAIL"))
    verdicts.append(
        (
            "HZB max semantics (each mip == 2x2 max of parent, clamp-to-edge, tol %g)" % HZB_TOL,
            "PASS" if all(r["passed"] for r in coarse_rows) else "FAIL",
        )
    )

    report["exposure"] = exposure_note
    report["structural"] = structural
    report["mips"] = mip_rows
    report["verdicts"] = [(name, v) for name, v in verdicts]
    report["summary"] = "PASS" if all_pass else "FAIL"

    for name, verdict in verdicts:
        print("HZBCHECK VERDICT", name, verdict)
    write_json(out_json, report)


# --- entry point ---------------------------------------------------------------
# NOTE (Agent W, S4-A1): Falcor's embedded Python executes the script with
# __name__ == 'builtins', so an `if __name__ == "__main__":` guard never runs.
# Call main() unconditionally like the other working run_*.py scripts.
main(SCENE, OUT_JSON)
exit()
