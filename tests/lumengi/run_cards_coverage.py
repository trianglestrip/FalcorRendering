"""LumenGI S2/S3 validation asset: Surface Cache card coverage over frames.

Role / purpose
--------------
Agent N (Test-Tooling) verification asset. Reads the Surface Cache coverage
series from the LumenGI pass output channel "cardCoverage" (R16F), which Agent
M is adding as an OPTIONAL graph output next to the existing capture path
(S2-C1 coverage/residency evidence, task.md 7). The script writes the
coverage-vs-frame time series to JSON so root can inspect the residency
plateau and assert the S2/S3 coverage gates without a GPU on hand.

ROBUST FALLBACK (required by the task): the cardCoverage channel may not exist
yet (Agent M still integrating) or may be disabled. The script probes it and
degrades gracefully:
  * If the graph cannot compile with the channel marked, it rebuilds without
    the channel and continues rendering the S2 capture path, printing a clear
    "channel not present" hint. It never crashes.
  * If the channel exists but a per-frame read fails, that frame's sample is
    recorded as null and rendering continues.

STATUS: RUN-ONLY. Prints stats and VERDICT lines, never exits non-zero; root
evaluates the verdicts at the S2/S3 gates.

Coverage contract (interface alignment with Agent M)
----------------------------------------------------
* Channel name : "cardCoverage" (LumenGI graph output), format R16F, values in
  [0,1] per texel. S3_TODO: confirm with root/Agent M whether the channel needs
  a pass property to enable it (e.g. "coverageDebug": true) and whether the
  final frozen metric is the texel mean, the "fraction of lit/valid texels" or
  a coverage-weighted page statistic. This script reports both the mean and the
  lit fraction so the freeze has both numbers.
* Metric      : per-sample mean over the whole channel + lit fraction (fraction
  of texels > 0). Series written to JSON (LUMEN_COVERAGE_OUT).
* Gate        : PLACEHOLDER final-coverage threshold (COVERAGE_FINAL_MIN).
  task.md 15.4 freezes a related but distinct S1 trace-coverage gate
  (ratio >= 0.9, absolute >= 0.15); the cardCoverage absolute threshold is
  frozen with root at S2/S3 (S3_TODO below).

Usage (run by root on GPU, from the repo root)
----------------------------------------------
    build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
      --device-type d3d12 --headless --precise ^
      --script tests\\lumengi\\run_cards_coverage.py ^
      --logfile artifacts\\lumengi\\S2\\cards-coverage.log
(create artifacts\\lumengi\\S2 first.)

Known pitfalls encoded here
---------------------------
* mark_output on a channel the pass does not reflect does NOT throw at mark
  time (RenderGraph::markOutput, RenderGraph.cpp:548); the failure surfaces at
  the first render (graph compile). Hence the probe wraps the FIRST render in
  try/except and rebuilds the graph without the channel on failure.
* m.activeGraph.get_output(name) throws when the output is not a marked graph
  output (RenderGraph.cpp:623); each per-frame read is guarded.
"""

from falcor import *
import math
import os
import json

FRAME_RATE = 60
RESOLUTION = (640, 360)

# Frames over which the coverage series is sampled. Cornell has 7 instances x 6
# faces = 42 cards; the S2 capture budget is 64 pages/frame by default, so the
# atlas should reach full residency well within this window.
TOTAL_FRAMES = 128
SAMPLE_INTERVAL_FRAMES = 8

# The optional coverage output channel Agent M is adding (R16F).
COVERAGE_CHANNEL = "cardCoverage"

# S3_TODO: freeze the final-coverage gate with root. Placeholder absolute
# coverage floor (see header; task.md 15.4 documents the related 0.15 floor).
COVERAGE_FINAL_MIN = 0.15

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"

OUT_JSON = os.environ.get("LUMEN_COVERAGE_OUT", "artifacts/lumengi/S2/cards-coverage.json")


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


def create_lumen_graph(mark_coverage):
    graph = RenderGraph("LumenGICoverage")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", {"useSurfaceCache": True}), "LumenGI")
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
    graph.markOutput("LumenGI.confidence")
    if mark_coverage:
        graph.markOutput("LumenGI." + COVERAGE_CHANNEL)
    return graph


def probe_coverage(scene_path):
    """Try running with the cardCoverage channel marked. Returns (available,
    graph). On graph-compile failure (channel not integrated yet) rebuilds
    without the channel and keeps going."""
    graph = create_lumen_graph(mark_coverage=True)
    m.addGraph(graph)
    m.setActiveGraph(graph)
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0
    try:
        m.clock.frame = 1
        m.renderFrame()
        return True, graph
    except Exception as exc:
        print(
            "COVERAGE WARNING channel 'LumenGI.%s' not present (Agent M still "
            "integrating); skipping coverage sampling -> %s" % (COVERAGE_CHANNEL, str(exc))
        )
        m.removeGraph(graph)
        graph = create_lumen_graph(mark_coverage=False)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        m.loadScene(scene_path)
        m.resizeFrameBuffer(*RESOLUTION)
        m.ui = False
        m.clock.framerate = FRAME_RATE
        m.clock.time = 0
        m.clock.pause()
        m.clock.frame = 0
        m.clock.frame = 1
        m.renderFrame()
        return False, graph


def sample_coverage():
    """Read the cardCoverage channel (R16F). Returns a stats dict or None on
    any failure (the caller treats None as 'no sample this frame')."""
    try:
        tex = m.activeGraph.get_output("LumenGI." + COVERAGE_CHANNEL)
        arr = tex.to_numpy()
    except Exception as exc:
        print("COVERAGE WARNING read failed: %s" % str(exc))
        return None
    flat = arr.reshape(-1)
    if flat.size == 0:
        return None
    vals = flat.astype(float)
    lit = vals > 0.0
    return {
        "coverage_mean": float(vals.mean()),
        "lit_fraction": float(lit.mean()),
        "min": float(vals.min()),
        "max": float(vals.max()),
        "finite": bool(math.isfinite(float(vals.min())) and math.isfinite(float(vals.max()))),
    }


def main():
    available, graph = probe_coverage(SCENE_CORNELL)
    print("COVERAGE channel", COVERAGE_CHANNEL, "available", available)

    series = []
    for frame in range(2, TOTAL_FRAMES + 1):
        m.clock.frame = frame
        m.renderFrame()
        if frame % SAMPLE_INTERVAL_FRAMES == 0:
            sample = sample_coverage() if available else None
            if sample is not None:
                print(
                    "COVERAGE frame", frame,
                    "mean", sample["coverage_mean"],
                    "lit", sample["lit_fraction"],
                    "finite", sample["finite"],
                )
            series.append(
                {
                    "frame": frame,
                    "coverage_mean": sample["coverage_mean"] if sample else None,
                    "lit_fraction": sample["lit_fraction"] if sample else None,
                    "min": sample["min"] if sample else None,
                    "max": sample["max"] if sample else None,
                    "finite": sample["finite"] if sample else None,
                }
            )

    # Final-coverage gate (S3_TODO placeholder). When the channel is absent the
    # verdict is SKIP, not FAIL, so the S2 gate is not falsely blocked pre-M.
    final = None
    for s in reversed(series):
        if s["coverage_mean"] is not None:
            final = s["coverage_mean"]
            break
    if available and final is not None:
        ok = final >= COVERAGE_FINAL_MIN
        print(
            "COVERAGE VERDICT final-coverage>=%.3f" % COVERAGE_FINAL_MIN,
            "PASS" if ok else "FAIL",
            "final_mean", final,
        )
    else:
        print("COVERAGE VERDICT final-coverage SKIP (channel unavailable, pre-M)")

    write_json(
        OUT_JSON,
        {
            "schema_version": 1,
            "script": "run_cards_coverage.py",
            "channel": COVERAGE_CHANNEL,
            "channel_available": available,
            "scene": SCENE_CORNELL,
            "total_frames": TOTAL_FRAMES,
            "sample_interval_frames": SAMPLE_INTERVAL_FRAMES,
            "coverage_final_min_placeholder": COVERAGE_FINAL_MIN,
            "series": series,
        },
    )
    print("COVERAGE wrote", os.path.abspath(OUT_JSON))


main()
exit()
