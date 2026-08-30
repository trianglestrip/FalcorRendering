"""Runtime gate for the production Screen -> GDF -> HWRT probe router.

This gate is deliberately separate from the standalone view-ray GDF test.  It
reads ``screenProbeStats.gdfHits`` written by the unified probe-hit counter and
therefore cannot pass from ``gdfStats.sphereHit`` alone.
"""

from falcor import *

import json
import math
import os

import numpy as np


OUT_DIR = os.path.abspath(os.environ.get("LUMEN_GDF_PROBE_ROUTER_OUT", "artifacts/lumengi/C4/gdf-probe-router"))
SCENE = os.environ.get("LUMEN_GDF_PROBE_ROUTER_SCENE", "media/test_scenes/cornell_box.pyscene")
RESOLUTION = tuple(int(v) for v in os.environ.get("LUMEN_GDF_PROBE_ROUTER_RESOLUTION", "320x180").lower().replace("x", ",").split(","))
WARMUP = max(1, int(os.environ.get("LUMEN_GDF_PROBE_ROUTER_WARMUP", "4")))
TRACE_MODE = os.environ.get("LUMEN_GDF_PROBE_ROUTER_TRACE_MODE", "MeshSDF")
USE_GDF = os.environ.get("LUMEN_GDF_PROBE_ROUTER_USE_GDF", "1").strip().lower() not in ("0", "false", "off", "no")
USE_SURFACE_CACHE = os.environ.get("LUMEN_GDF_PROBE_ROUTER_USE_SURFACE_CACHE", "0").strip().lower() not in ("0", "false", "off", "no")
USE_CACHE_LIGHTING = os.environ.get("LUMEN_GDF_PROBE_ROUTER_USE_CACHE_LIGHTING", "0").strip().lower() not in ("0", "false", "off", "no")
USE_CACHE_CARD_GRID = os.environ.get("LUMEN_GDF_PROBE_ROUTER_USE_CACHE_CARD_GRID", "0").strip().lower() not in ("0", "false", "off", "no")
CACHE_LOOKUP_ENABLED = USE_SURFACE_CACHE and USE_CACHE_LIGHTING
SURFACE_CACHE_ATLAS_SIZE = max(16, int(os.environ.get("LUMEN_GDF_PROBE_ROUTER_ATLAS_SIZE", "4096")))
CAPTURE_MAX_PAGES = max(1, int(os.environ.get("LUMEN_GDF_PROBE_ROUTER_CAPTURE_MAX_PAGES", "64")))
MARK_GDF_TRACE = os.environ.get("LUMEN_GDF_PROBE_ROUTER_MARK_GDF_TRACE", "1").strip().lower() not in ("0", "false", "off", "no")
MARK_RAW_GI = os.environ.get("LUMEN_GDF_PROBE_ROUTER_MARK_RAW_GI", "1").strip().lower() not in ("0", "false", "off", "no")
MARK_PROBE_INTERPOLATED = os.environ.get("LUMEN_GDF_PROBE_ROUTER_MARK_PROBE_INTERPOLATED", "1").strip().lower() not in ("0", "false", "off", "no")


def _safe(v):
    if isinstance(v, dict):
        return {str(k): _safe(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_safe(x) for x in v]
    if isinstance(v, (float, np.floating)):
        return float(v) if math.isfinite(float(v)) else None
    if isinstance(v, (int, bool, str)) or v is None:
        return v
    return str(v)


def _graph():
    graph = RenderGraph("GDFProbeRouter")
    graph.addPass(createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}), "GBufferRT")
    graph.addPass(createPass("LumenGI", {
        "enabled": True,
        "traceMode": TRACE_MODE,
        "useGDF": USE_GDF,
        "useScreenProbes": True,
        "useScreenTrace": True,
        "useSurfaceCache": USE_SURFACE_CACHE,
        "useCacheLighting": USE_CACHE_LIGHTING,
        "useCacheCardGrid": USE_CACHE_CARD_GRID,
        "markGDFTrace": MARK_GDF_TRACE,
        "markRawGI": MARK_RAW_GI,
        "markProbeInterpolated": MARK_PROBE_INTERPOLATED,
        "surfaceCacheAtlasSize": SURFACE_CACHE_ATLAS_SIZE,
        "captureMaxPagesPerFrame": CAPTURE_MAX_PAGES,
        "probeDirectionsPerProbe": 16,
        "useTemporalFilter": True,
        "useSpatialFilter": True,
        "gdfLevelCount": 2,
        "gdfResolution": 64,
        "gdfBaseExtent": 4.0,
        "gdfTraceMaxSteps": 32,
        "gdfTraceMaxDistance": 20.0,
    }), "LumenGI")
    for channel in ("vbuffer", "linearZ", "mvec", "mvecW", "normWRoughnessMaterialID", "viewW", "diffuseOpacity", "emissive"):
        graph.addEdge("GBufferRT." + channel, "LumenGI." + channel)
    channels = ["diffuseGI", "resolvedDiffuseGI"]
    if MARK_PROBE_INTERPOLATED:
        channels.insert(1, "probeInterpolated")
    if MARK_RAW_GI:
        channels[1:1] = ["diffuseRadianceHitDist", "confidence"]
    if MARK_GDF_TRACE:
        channels.insert(3, "gdfTrace")
    for channel in channels:
        graph.markOutput("LumenGI." + channel)
    return graph


def _texture_health(graph, name):
    try:
        a = np.asarray(graph.get_output("LumenGI." + name).to_numpy(), dtype=np.float64)
        return {
            "finite": bool(np.isfinite(a).all()),
            "nonnegative": bool(np.nanmin(a) >= 0.0) if a.size else False,
            "mean": float(np.nanmean(a)) if a.size else None,
            "max": float(np.nanmax(a)) if a.size else None,
            "shape": list(a.shape),
        }
    except Exception as exc:
        return {"status": "BLOCKED", "error": str(exc)}


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    m.loadScene(SCENE)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.pause()
    m.clock.frame = 0
    graph = _graph()
    m.addGraph(graph)

    frames = []
    render_error = None
    try:
        for frame in range(1, WARMUP + 1):
            m.clock.frame = frame
            m.renderFrame()
            lumen_pass = graph.getPass("LumenGI")
            stats = dict(lumen_pass.screenProbeStats)
            try:
                cache_stats = dict(lumen_pass.surfaceCacheStats)
            except Exception:
                cache_stats = {}
            frames.append({
                "frame": frame,
                "screenProbeStats": _safe(stats),
                "surfaceCacheStats": _safe(cache_stats),
                "outputs": {
                    name: _texture_health(graph, name)
                    for name in (("diffuseGI", "resolvedDiffuseGI") + (("probeInterpolated",) if MARK_PROBE_INTERPOLATED else ()) + (("diffuseRadianceHitDist", "confidence") if MARK_RAW_GI else ()) + (("gdfTrace",) if MARK_GDF_TRACE else ()))
                },
            })
    except Exception as exc:
        render_error = str(exc)

    last = frames[-1] if frames else {}
    stats = last.get("screenProbeStats", {})
    cache_stats = last.get("surfaceCacheStats", {})
    route_enabled = float(stats.get("gdfRouteEnabled", 0.0)) > 0.5
    gdf_hits = float(stats.get("gdfHits", 0.0))
    cache_lookup_hits = float(stats.get("cacheLookupHits", 0.0))
    finite = all(v.get("finite", False) and v.get("nonnegative", False) for v in last.get("outputs", {}).values()) if last else False
    if USE_GDF:
        cache_gate = (not CACHE_LOOKUP_ENABLED) or cache_lookup_hits > 0
        status = "PASS" if render_error is None and route_enabled and gdf_hits > 0 and finite and cache_gate else "BLOCKED"
    else:
        status = "PASS" if render_error is None and not route_enabled and finite else "BLOCKED"
    report = {
        "schema": "LumenGI.GDFProbeRouter.v2",
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "warmup": WARMUP,
        "traceMode": TRACE_MODE,
        "useGDF": USE_GDF,
        "useSurfaceCache": USE_SURFACE_CACHE,
        "useCacheLighting": USE_CACHE_LIGHTING,
        "useCacheCardGrid": USE_CACHE_CARD_GRID,
        "markGDFTrace": MARK_GDF_TRACE,
        "markRawGI": MARK_RAW_GI,
        "markProbeInterpolated": MARK_PROBE_INTERPOLATED,
        "status": status,
        "renderError": render_error,
        "routeContract": "Screen -> GDF -> HWRT",
        "routeEvidence": {
            "gdfRouteEnabled": route_enabled,
            "gdfHits": gdf_hits,
            "gdfMisses": stats.get("gdfMisses"),
            "fallbackHits": stats.get("fallbackHits"),
            "cacheLookupHits": cache_lookup_hits,
            "cacheLookupAttempts": stats.get("cacheLookupAttempts"),
            "cachePageRejects": stats.get("cachePageRejects"),
            "cacheCoverageRejects": stats.get("cacheCoverageRejects"),
            "cacheMetadataRejects": stats.get("cacheMetadataRejects"),
            "cardGridCandidateCount": cache_stats.get("cardGridCandidateCount"),
            "cardGridOverflowCells": cache_stats.get("cardGridOverflowCells"),
            "cardGridCardsIndexed": cache_stats.get("cardGridCardsIndexed"),
            "cardGridIndexedCards": cache_stats.get("cardGridCardsIndexed"),
            "cardGridMissingCards": cache_stats.get("cardGridMissingCards"),
            "cardGridCardCount": cache_stats.get("cardCount", cache_stats.get("cards")),
            "cacheVisibilityRejects": stats.get("cacheVisibilityRejects"),
            "cacheAuthoritativeWhenEnabled": (not CACHE_LOOKUP_ENABLED) or cache_lookup_hits > 0,
        },
        "frames": frames,
        "limitations": ["GDF hit radiance is authoritative from Surface Cache only when cache lookup and cache lighting are enabled and a valid page covers the hit; cache-miss directions retain the existing fallback resolver."],
    }
    path = os.path.join(OUT_DIR, "gdf-probe-router.json")
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_safe(report), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    m.removeGraph(graph)
    m.unloadScene()
    print("GDF_PROBE_ROUTER", status, path)
    if render_error:
        print("GDF_PROBE_ROUTER_ERROR", render_error)


main()
