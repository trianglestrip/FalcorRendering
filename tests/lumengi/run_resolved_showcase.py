"""Capture a resolved LumenGI-only visual view for the final screenshot gate.

This intentionally displays LumenGI.resolvedDiffuseGI through ToneMapper.
It is a resolved indirect-radiance view, not a fabricated full-scene finalColor;
the production graph still needs a separate composite pass for that claim.
"""

from falcor import *

import math
import os


FRAME_RATE = 60
RESOLUTION = (800, 450)
SETTLE_FRAMES = 96
OUT_DIR = os.path.abspath(os.environ.get("LUMEN_RESOLVED_SHOWCASE_OUT", "artifacts/lumengi/screenshots/final-resolved-20260810"))
EXPOSURE = float(os.environ.get("LUMEN_RESOLVED_SHOWCASE_EXPOSURE", "1.5"))
VIEW_SPECS = {
    # Keep the room and cabinet in frame. The earlier close-up positions made
    # the indirect-radiance preview look like a crop rather than a scene view.
    "front": (float3(0, 0.75, 2.6), float3(0, 0.5, 0)),
    "left": (float3(-2.2, 1.0, 1.8), float3(0, 0.5, 0)),
    "right": (float3(1.2, 0.8, 2.2), float3(0, 0.5, 0)),
}


def _scenes():
    value = os.environ.get("LUMEN_RESOLVED_SHOWCASE_SCENES", "")
    if not value:
        return (
            ("cornell", "test_scenes/cornell_box.pyscene"),
            ("arcade", "Arcade/Arcade.pyscene"),
        )
    result = []
    for token in value.split(","):
        token = token.strip()
        if not token:
            continue
        if "=" in token:
            label, path = token.split("=", 1)
        else:
            path = token
            label = os.path.splitext(os.path.basename(path))[0]
        result.append((label.strip(), path.strip()))
    return tuple(result)


def _views():
    value = os.environ.get("LUMEN_RESOLVED_SHOWCASE_VIEWS", "front")
    selected = tuple(token.strip() for token in value.split(",") if token.strip() in VIEW_SPECS)
    return selected or ("front",)


def _graph():
    graph = RenderGraph("LumenResolvedShowcase")
    graph.addPass(
        createPass("GBufferRT", {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True}),
        "GBufferRT",
    )
    graph.addPass(
        createPass(
            "LumenGI",
            {
                "enabled": True,
                "useSurfaceCache": True,
                "useCacheLighting": True,
                "useScreenTrace": True,
                "useScreenProbes": True,
                "useTemporalFilter": True,
                "useSpatialFilter": True,
                "debugMode": "None",
            },
        ),
        "LumenGI",
    )
    graph.addPass(
        createPass(
            "ToneMapper",
            {
                "autoExposure": False,
                "exposureCompensation": EXPOSURE,
            },
        ),
        "ToneMapperDisplay",
    )
    # A screenshot-only preview composite: the GBuffer diffuse reflectance
    # keeps the scene readable while resolvedDiffuseGI supplies the indirect
    # contribution. This is deliberately not exported as LumenGI.finalColor.
    graph.addPass(
        createPass("Composite", {"mode": "Add", "scaleA": 0.35, "scaleB": 1.0}),
        "ResolvedCompositePreview",
    )
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
    for channel in (
        "diffuseGI",
        "probeInterpolated",
        "temporalFiltered",
        "temporalMoments",
        "spatialFiltered",
        "filteredVariance",
        "resolvedDiffuseGI",
    ):
        graph.markOutput("LumenGI." + channel)
    graph.addEdge("GBufferRT.diffuseOpacity", "ResolvedCompositePreview.A")
    graph.addEdge("LumenGI.resolvedDiffuseGI", "ResolvedCompositePreview.B")
    graph.markOutput("ResolvedCompositePreview.out")
    graph.addEdge("ResolvedCompositePreview.out", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    return graph


def _capture(label, scene_path, view_name):
    m.loadScene(scene_path)
    m.resizeFrameBuffer(*RESOLUTION)
    position, target = VIEW_SPECS[view_name]
    m.scene.camera.position = position
    m.scene.camera.target = target
    m.scene.camera.up = float3(0, 1, 0)
    m.scene.camera.focalLength = 35.0
    graph = _graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    m.frameCapture.outputDir = OUT_DIR
    m.frameCapture.baseFilename = label + "-" + view_name + "-resolved"
    for frame in range(1, SETTLE_FRAMES + 1):
        m.clock.frame = frame
        m.renderFrame()
    m.frameCapture.capture()
    resolved = m.activeGraph.get_output("LumenGI.resolvedDiffuseGI").to_numpy()[..., :3]
    display = m.activeGraph.get_output("ToneMapperDisplay.dst").to_numpy()[..., :3]
    print(
        "RESOLVED_SHOWCASE",
        label,
        view_name,
        "gi_mean",
        float(resolved.mean()),
        "gi_max",
        float(resolved.max()),
        "display_mean",
        float(display.mean()),
        "display_max",
        float(display.max()),
        "finite",
        bool(math.isfinite(float(resolved.min())) and math.isfinite(float(resolved.max()))),
        "nonnegative",
        bool(float(resolved.min()) >= 0.0),
    )
    m.removeGraph(graph)


m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.pause()
os.makedirs(OUT_DIR, exist_ok=True)
for label, scene_path in _scenes():
    for view_name in _views():
        _capture(label, scene_path, view_name)
print("RESOLVED_SHOWCASE done", OUT_DIR)
exit()
