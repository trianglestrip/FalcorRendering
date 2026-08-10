"""C1 Arcade cache-lighting isolation matrix.

This is a GPU/Mogwai test asset, not a build script.  It keeps the LumenGI
features fixed to the cache-lighting path and toggles the *scene* light
settings one at a time.  In particular, ``renderEnv`` is intentionally not
sent to LumenGI: it is not a LumenGI property and therefore cannot disable the
scene environment light.

The script is deliberately diagnostic.  Each case prints a JSON feature
manifest, renders a short settling window, and reports the first Python/GPU
exception without aborting the remaining cases.  Root should run it with a
unique Mogwai logfile; no build or subprocess is started here.

Environment overrides:

``LUMEN_CACHELIGHTING_RESOLUTION`` (or ``LUMEN_C1_RESOLUTION``)
    ``WIDTHxHEIGHT`` or ``WIDTH,HEIGHT``; default ``640x360``.
``LUMEN_CACHELIGHTING_LABEL`` (or ``LUMEN_C1_LABEL``)
    Prefix used in graph/case labels; default ``c1-arcade-cachelighting``.
``LUMEN_CACHELIGHTING_WARMUP``
    Frames rendered per case; default ``8``.
``LUMEN_CACHELIGHTING_ISOLATION_OUT``
    Optional JSON summary path.  No summary file is written when unset.
``LUMEN_CACHELIGHTING_CASES`` (or ``LUMEN_C1_CASES``)
    Optional comma-separated case names (for example ``all_on,env_off``).
    Selection always follows the declared ``LIGHT_CASES`` order; unset runs
    the complete matrix.
"""

from falcor import *

import json
import math
import os
import traceback


FRAME_RATE = 60
DEFAULT_RESOLUTION = (640, 360)
SCENE = "Arcade/Arcade.pyscene"


def _parse_resolution(value):
    """Parse an environment resolution without making malformed input fatal."""
    if not value:
        return DEFAULT_RESOLUTION
    try:
        tokens = value.lower().replace("x", ",").split(",")
        if len(tokens) != 2:
            raise ValueError("expected WIDTHxHEIGHT or WIDTH,HEIGHT")
        width, height = (int(token.strip()) for token in tokens)
        if width <= 0 or height <= 0:
            raise ValueError("dimensions must be positive")
        return (width, height)
    except Exception as exc:
        print("C1_CONFIG invalid resolution", repr(value), "using", DEFAULT_RESOLUTION, repr(exc))
        return DEFAULT_RESOLUTION


def _parse_positive_int(value, fallback):
    try:
        parsed = int(value)
        return parsed if parsed > 0 else fallback
    except Exception:
        return fallback


RESOLUTION = _parse_resolution(
    os.environ.get("LUMEN_CACHELIGHTING_RESOLUTION")
    or os.environ.get("LUMEN_C1_RESOLUTION")
)
OUTPUT_LABEL = (
    os.environ.get("LUMEN_CACHELIGHTING_LABEL")
    or os.environ.get("LUMEN_C1_LABEL")
    or "c1-arcade-cachelighting"
)
WARMUP_FRAMES = _parse_positive_int(
    os.environ.get("LUMEN_CACHELIGHTING_WARMUP"), 8
)
OUT_JSON = os.environ.get("LUMEN_CACHELIGHTING_ISOLATION_OUT", "").strip()


# Frozen C1 interface: the LumenGI feature set is held constant while the
# actual Scene::RenderSettings light switches cover every on/off combination.
# This isolates cache-lighting's environment, analytic, and emissive inputs
# without using the unrelated/unsupported LumenGI property ``renderEnv``.
LUMEN_FEATURES = {
    "useSurfaceCache": True,
    "useCacheLighting": True,
}

LIGHT_CASES = (
    ("all_on", {"useEnvLight": True, "useAnalyticLights": True, "useEmissiveLights": True}),
    ("env_off", {"useEnvLight": False, "useAnalyticLights": True, "useEmissiveLights": True}),
    ("analytic_off", {"useEnvLight": True, "useAnalyticLights": False, "useEmissiveLights": True}),
    ("emissive_off", {"useEnvLight": True, "useAnalyticLights": True, "useEmissiveLights": False}),
    ("env_analytic_off", {"useEnvLight": False, "useAnalyticLights": False, "useEmissiveLights": True}),
    ("env_emissive_off", {"useEnvLight": False, "useAnalyticLights": True, "useEmissiveLights": False}),
    ("analytic_emissive_off", {"useEnvLight": True, "useAnalyticLights": False, "useEmissiveLights": False}),
    ("all_off", {"useEnvLight": False, "useAnalyticLights": False, "useEmissiveLights": False}),
)


def _select_cases(value):
    """Return a declared-order subset from an optional case-name filter."""
    if not value or not value.strip():
        return LIGHT_CASES

    requested = {token.strip() for token in value.split(",") if token.strip()}
    known = {name for name, _ in LIGHT_CASES}
    unknown = sorted(requested - known)
    if unknown:
        print("C1_CONFIG unknown case names ignored", json.dumps(unknown, sort_keys=True))
    selected = tuple(case for case in LIGHT_CASES if case[0] in requested)
    if not selected:
        print("C1_CONFIG case filter selected no known cases", repr(value))
    return selected


CASE_FILTER = (
    os.environ.get("LUMEN_CACHELIGHTING_CASES")
    or os.environ.get("LUMEN_C1_CASES")
    or ""
)
SELECTED_LIGHT_CASES = _select_cases(CASE_FILTER)


def _lumen_graph():
    graph = RenderGraph(OUTPUT_LABEL + "Graph")
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    graph.addPass(createPass("LumenGI", dict(LUMEN_FEATURES)), "LumenGI")
    for channel in (
        "vbuffer",
        "linearZ",
        "mvec",
        "mvecW",
        "normWRoughnessMaterialID",
        "viewW",
        "diffuseOpacity",
        "emissive",
    ):
        graph.addEdge("GBufferRT." + channel, "LumenGI." + channel)
    graph.markOutput("LumenGI.diffuseGI")
    return graph


def _set_scene_lights(settings):
    render_settings = m.scene.renderSettings
    for name, enabled in settings.items():
        # These are the Scene render switches consumed by LumenGI's shader
        # defines.  Keep this list explicit so a typo cannot silently become a
        # pass property (the historical ``renderEnv`` mistake).
        if name not in ("useEnvLight", "useAnalyticLights", "useEmissiveLights"):
            raise ValueError("unsupported scene-light setting: " + str(name))
        setattr(render_settings, name, bool(enabled))


def _sample_diffuse_gi():
    try:
        image = m.activeGraph.get_output("LumenGI.diffuseGI").to_numpy()
        values = image[..., :3] if getattr(image, "ndim", 0) >= 3 else image
        if values.size == 0:
            return {"min": 0.0, "max": 0.0, "mean": 0.0, "finite": True, "nonnegative": True}
        minimum = float(values.min())
        maximum = float(values.max())
        mean = float(values.mean())
        return {
            "min": minimum,
            "max": maximum,
            "mean": mean,
            "finite": bool(math.isfinite(minimum) and math.isfinite(maximum) and math.isfinite(mean)),
            "nonnegative": bool(minimum >= 0.0),
        }
    except Exception as exc:
        return {"sample_error": repr(exc)}


def _manifest(label, settings):
    return {
        "script": "run_cachelighting_isolation.py",
        "label": label,
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "warmup_frames": WARMUP_FRAMES,
        "lumen_gi_features": dict(LUMEN_FEATURES),
        "scene_render_settings": dict(settings),
        "renderEnv_property_used": False,
        "isolation_note": "scene renderSettings switches are authoritative; renderEnv is not a LumenGI property",
    }


def _write_summary(records):
    if not OUT_JSON:
        return
    path = os.path.abspath(OUT_JSON)
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(
            {
                "script": "run_cachelighting_isolation.py",
                "output_label": OUTPUT_LABEL,
                "resolution": list(RESOLUTION),
                "warmup_frames": WARMUP_FRAMES,
                "selected_cases": [name for name, _ in SELECTED_LIGHT_CASES],
                "records": records,
            },
            stream,
            indent=2,
            sort_keys=True,
            allow_nan=False,
        )
        stream.write("\n")
    print("C1_SUMMARY wrote", path)


def main():
    print(
        "C1_CONFIG",
        json.dumps(
            {
                "output_label": OUTPUT_LABEL,
                "resolution": list(RESOLUTION),
                "warmup_frames": WARMUP_FRAMES,
                "scene": SCENE,
                "lumen_gi_features": dict(LUMEN_FEATURES),
                "renderEnv_property_used": False,
                "selected_cases": [name for name, _ in SELECTED_LIGHT_CASES],
            },
            sort_keys=True,
        ),
    )

    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.loadScene(SCENE)
    m.resizeFrameBuffer(*RESOLUTION)

    graph = _lumen_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    records = []
    frame = 0
    try:
        for case_name, settings in SELECTED_LIGHT_CASES:
            manifest = _manifest(case_name, settings)
            print("C1_MANIFEST", json.dumps(manifest, sort_keys=True))
            record = dict(manifest)
            try:
                _set_scene_lights(settings)
                for _ in range(WARMUP_FRAMES):
                    frame += 1
                    m.clock.frame = frame
                    m.renderFrame()
                record["status"] = "OK"
                record["diffuseGI"] = _sample_diffuse_gi()
                print("C1_CASE", case_name, "OK", json.dumps(record["diffuseGI"], sort_keys=True))
            except Exception as exc:
                record["status"] = "EXC"
                record["exception"] = repr(exc)
                print("C1_CASE", case_name, "EXC", repr(exc))
                traceback.print_exc()
            records.append(record)
    finally:
        # Leave the scene in the documented baseline state if Mogwai continues
        # after this script (and make failures in cleanup non-fatal).
        try:
            _set_scene_lights({"useEnvLight": True, "useAnalyticLights": True, "useEmissiveLights": True})
        except Exception as exc:
            print("C1_CLEANUP EXC", repr(exc))
        try:
            m.removeGraph(graph)
        except Exception as exc:
            print("C1_CLEANUP removeGraph EXC", repr(exc))
    _write_summary(records)
    print(
        "C1_DONE",
        OUTPUT_LABEL,
        "cases",
        len(records),
        "selected",
        json.dumps([name for name, _ in SELECTED_LIGHT_CASES]),
    )


main()
exit()
