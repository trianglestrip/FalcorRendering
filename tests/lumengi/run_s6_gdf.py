"""LumenGI S6 GPU verification: Mesh SDF data pipeline + GDF compose + sphere trace.

Agent Z13, S6 mainline integration. Verifies, on real hardware (D3D12):
  * S6-A data pipeline: scene static instances -> LumenMeshSDFScene (disk cache + built-in
    box-SDF builder) -> atlas -> GPU atlas, reported through the "gdfStats" binding.
  * S6-B3 GDF compose: the clipmap textures are filled each frame (dirty-region compose).
  * S6-B4 sphere trace: TraceMode=MeshSDF makes the GDF sphere trace the PRIMARY path and
    writes diffuseGI / diffuseRadianceHitDist / confidence; Hybrid keeps HWRT and writes the
    optional "gdfTrace" diagnostic channel.
  * The screen + SDF pipeline renders N frames with no crash / no NaN, and the sphere-trace
    hit statistics (gdfTrace A channel + gdfStats.sphereHitRate) are plausible for Cornell.

Report: artifacts/lumengi/S6/gpu/run_s6_gdf.json (verdicts) + captured EXRs.
"""

from falcor import *
import json
import math
import os
import traceback

FRAME_RATE = 60
RESOLUTION = (640, 360)
SCENE = "test_scenes/cornell_box.pyscene"
WARMUP = 4
REPRO_FRAME = 8

OUT_JSON = os.environ.get("LUMEN_S6_GDF_OUT", "artifacts/lumengi/S6/gpu/run_s6_gdf.json")

# A per-run disk cache so the built volumes are reproducible and not shared with other runs.
os.environ["LUMEN_MSDF_CACHE_DIR"] = os.environ.get(
    "LUMEN_S6_GDF_CACHE", "artifacts/lumengi/S6/gpu/msdf-cache"
)


def build_graph(mode, mark_debug=True):
    graph = RenderGraph("LumenGIS6")
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
                "traceMode": mode,          # "MeshSDF" (software primary) or "Hybrid".
                "useGDF": True,
                "meshSDFResolution": 32,    # per-mesh volume voxels (longest axis).
                "gdfLevelCount": 2,         # dynamic near + static far.
                "gdfResolution": 64,
                "gdfBaseExtent": 4.0,
                "gdfTraceMaxSteps": 32,
                "gdfTraceMaxDistance": 20.0,
                "gdfEmptyDistanceScale": 8.0,
            },
        ),
        "LumenGI",
    )

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
    graph.markOutput("LumenGI.gdfTrace")
    if mark_debug:
        graph.markOutput("LumenGI.debugOutput")
    return graph


def render_frames(graph, base_name):
    """Render a deterministic frame sequence and capture the marked outputs at REPRO_FRAME."""
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.frameCapture.baseFilename = base_name
    frame = 0
    for capture_frame in (WARMUP, REPRO_FRAME):
        while frame < capture_frame:
            frame += 1
            m.clock.frame = frame
            m.renderFrame()
        m.frameCapture.capture()


def grab(name):
    return m.activeGraph.get_output(name).to_numpy()


def finite_stats(arr):
    import numpy as np
    a = np.asarray(arr, dtype=np.float32)
    finite = np.isfinite(a).all()
    nans = int(np.isnan(a).sum())
    return {
        "finite": bool(finite),
        "nanCount": nans,
        "min": float(a.min()),
        "max": float(a.max()),
        "mean": float(a.mean()),
    }


def sphere_trace_stats(numpy_arr):
    """gdfTrace channel: R = t/tMax, G = |SDF|/voxel at surface, B = t, A = hit?1:0."""
    import numpy as np
    a = np.asarray(numpy_arr, dtype=np.float32)
    hit_mask = a[..., 3] > 0.5
    total = hit_mask.size
    hits = int(hit_mask.sum())
    hit_rate = hits / float(total) if total else 0.0
    hit_t = a[..., 2][hit_mask]
    mean_t = float(hit_t.mean()) if hits else 0.0
    max_t = float(hit_t.max()) if hits else 0.0
    surface_sdf = a[..., 1][hit_mask]
    mean_surface_sdf = float(surface_sdf.mean()) if hits else 0.0
    return {
        "hitFraction": hit_rate,
        "hits": hits,
        "total": total,
        "meanHitT": mean_t,
        "maxHitT": max_t,
        "meanSurfaceSDFVoxels": mean_surface_sdf,
    }


def run_mode(mode, report, verdicts):
    print("=" * 70)
    print("S6 GPU run: traceMode = %s" % mode)
    print("=" * 70)
    graph = None
    entry = {}
    try:
        graph = build_graph(mode)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        m.loadScene(SCENE)
        render_frames(graph, "s6-%s" % mode.lower())

        entry["renderedFrames"] = WARMUP + 1
        entry["renderOk"] = True

        # S6 scriptable stats binding.
        try:
            stats = dict(m.activeGraph.getPass("LumenGI").gdfStats)
            entry["gdfStats"] = {k: float(v) for k, v in stats.items()}
        except Exception as exc:
            entry["gdfStats_error"] = str(exc)

        # Sphere-trace diagnostic channel (written in both modes; MeshSDF also writes the S1
        # outputs). Its A channel is the hit mask and R is t/tMax.
        try:
            trace_arr = grab("LumenGI.gdfTrace")
            st = sphere_trace_stats(trace_arr)
            entry["sphereTrace"] = st
            entry["gdfTraceFinite"] = finite_stats(trace_arr)
        except Exception as exc:
            entry["gdfTrace_error"] = str(exc)

        # MeshSDF primary-path outputs must be finite and non-empty.
        for ch in ("LumenGI.diffuseGI", "LumenGI.confidence"):
            try:
                arr = grab(ch)
                fs = finite_stats(arr)
                entry[ch + "_finite"] = fs
                entry[ch + "_nonZero"] = bool((arr != 0).any())
            except Exception as exc:
                entry[ch + "_error"] = str(exc)

        # Verdicts.
        stats = entry.get("gdfStats", {})
        if mode == "MeshSDF":
            verdicts.append(
                (
                    "meshSDFDataPipeline",
                    stats.get("registeredMeshes", 0) >= 1 and stats.get("residentInstances", 0) >= 1,
                    "registeredMeshes=%s residentInstances=%s"
                    % (stats.get("registeredMeshes"), stats.get("residentInstances")),
                )
            )
            verdicts.append(
                (
                    "sphereTraceRan",
                    stats.get("sphereTraced", 0) > 0,
                    "sphereTraced=%s" % stats.get("sphereTraced"),
                )
            )
            st = entry.get("sphereTrace", {})
            verdicts.append(
                (
                    "sphereTraceHitsPlausible",
                    st.get("hitFraction", 0.0) > 0.15 and st.get("meanHitT", 0.0) > 0.05,
                    "hitFraction=%s meanHitT=%s" % (st.get("hitFraction"), st.get("meanHitT")),
                )
            )
            verdicts.append(
                (
                    "sdfPrimaryWritesOutputs",
                    entry.get("LumenGI.diffuseGI_nonZero", False),
                    "diffuseGI_nonZero=%s" % entry.get("LumenGI.diffuseGI_nonZero"),
                )
            )
            verdicts.append(
                (
                    "noNaN",
                    entry.get("LumenGI.diffuseGI_finite", {}).get("finite", False)
                    and entry.get("gdfTraceFinite", {}).get("finite", False),
                    "diffuseGI_finite=%s gdfTrace_finite=%s"
                    % (
                        entry.get("LumenGI.diffuseGI_finite", {}).get("finite"),
                        entry.get("gdfTraceFinite", {}).get("finite"),
                    ),
                )
            )
        else:  # Hybrid
            verdicts.append(
                (
                    "hybridRenders",
                    entry.get("renderOk", False),
                    "renderOk=%s" % entry.get("renderOk"),
                )
            )
            verdicts.append(
                (
                    "hybridGdfDiagnostic",
                    entry.get("sphereTrace", {}).get("hitFraction", 0.0) > 0.0,
                    "hitFraction=%s" % entry.get("sphereTrace", {}).get("hitFraction"),
                )
            )
    except Exception as exc:
        entry["gpu_error"] = "%s\n%s" % (str(exc), traceback.format_exc())
        verdicts.append((("mode_%s_no_crash" % mode), False, str(exc)))
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception:
                pass

    report[mode] = entry
    return verdicts


def main():
    report = {}
    verdicts = []
    run_mode("MeshSDF", report, verdicts)
    run_mode("Hybrid", report, verdicts)

    os.makedirs(os.path.dirname(OUT_JSON), exist_ok=True)
    report["verdicts"] = [
        {"name": n, "pass": bool(p), "detail": d} for (n, p, d) in verdicts
    ]
    report["allPass"] = all(p for (_, p, _) in verdicts)
    with open(OUT_JSON, "w") as f:
        json.dump(report, f, indent=2)
    print("S6 GPU report written to %s" % OUT_JSON)
    print("VERDICTS:")
    for (n, p, d) in verdicts:
        print("  [%s] %s: %s" % ("PASS" if p else "FAIL", n, d))
    print("ALL PASS: %s" % report["allPass"])
    exit()


main()
