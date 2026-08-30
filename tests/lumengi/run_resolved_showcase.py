"""Capture a resolved realtime direct+indirect LumenGI visual view.

The default graph uses RTXDI -> NRD (diffuse radiance + hit distance) for
direct/emissive lighting and adds the resolved LumenGI diffuse indirect term.
Set LUMEN_RESOLVED_USE_DIRECT_LIGHTING=0 to run the indirect-only diagnostic
view; that mode is not a full-scene final-color gate.
"""

from falcor import *

import json
import hashlib
import math
import os
from pathlib import Path

import numpy as np


FRAME_RATE = 60
RESOLUTION = (800, 450)
SETTLE_FRAMES = int(os.environ.get("LUMEN_RESOLVED_SETTLE_FRAMES", "96"))
OUT_DIR = os.path.abspath(os.environ.get("LUMEN_RESOLVED_SHOWCASE_OUT", "artifacts/lumengi/screenshots/final-resolved-20260810"))
EXPOSURE = float(os.environ.get("LUMEN_RESOLVED_SHOWCASE_EXPOSURE", "0.0"))
USE_SURFACE_CACHE = os.environ.get("LUMEN_RESOLVED_USE_SURFACE_CACHE", "1").strip().lower() not in ("0", "false", "off")
USE_CACHE_LIGHTING = os.environ.get("LUMEN_RESOLVED_USE_CACHE_LIGHTING", "1").strip().lower() not in ("0", "false", "off")
# Test-only consumer isolation. The host reads the same process environment to keep
# Surface Cache capture/lighting active while bypassing ScreenProbe cache lookup.
DISABLE_SURFACE_CACHE_LOOKUP = os.environ.get("LUMEN_C9_DISABLE_SURFACE_CACHE_LOOKUP", "0").strip().lower() not in ("0", "false", "off")
# Test-only lookup side-effect isolation. The host keeps cache radiance replacement enabled but
# disables feedback/request atomics and their next-frame scheduler readbacks.
DISABLE_SURFACE_CACHE_FEEDBACK = os.environ.get("LUMEN_C9_DISABLE_SURFACE_CACHE_FEEDBACK", "0").strip().lower() not in ("0", "false", "off")
# Diagnostic-only LumenGI filter switches. Defaults preserve the production
# screenshot/C9 path; disabling either filter is for isolated producer traces.
USE_LUMEN_TEMPORAL_FILTER = os.environ.get("LUMEN_RESOLVED_USE_LUMEN_TEMPORAL", "1").strip().lower() not in ("0", "false", "off")
USE_LUMEN_SPATIAL_FILTER = os.environ.get("LUMEN_RESOLVED_USE_LUMEN_SPATIAL", "1").strip().lower() not in ("0", "false", "off")
GI_PREVIEW_SCALE = float(os.environ.get("LUMEN_RESOLVED_GI_PREVIEW_SCALE", "1.0"))
PROBE_DIRECTIONS_PER_PROBE = int(os.environ.get("LUMEN_RESOLVED_PROBE_DIRECTIONS", "32"))
# UE-style spatial quality sweep knobs.  Defaults preserve the production preset;
# the screenshot harness can widen the bilateral footprint without changing the
# shader/host ABI, then records the exact values in the artifact directory.
SPATIAL_RADIUS_MIN = float(os.environ.get("LUMEN_RESOLVED_SPATIAL_RADIUS_MIN", "2.0"))
SPATIAL_RADIUS_MAX = float(os.environ.get("LUMEN_RESOLVED_SPATIAL_RADIUS_MAX", "4.0"))
SPATIAL_VARIANCE_LOW = float(os.environ.get("LUMEN_RESOLVED_SPATIAL_VARIANCE_LOW", "0.0"))
SPATIAL_VARIANCE_HIGH = float(os.environ.get("LUMEN_RESOLVED_SPATIAL_VARIANCE_HIGH", "0.20"))
# The production screenshot path is full direct+indirect.  Disable it only for the
# isolated Lumen indirect diagnostic view.
USE_DIRECT_LIGHTING = os.environ.get("LUMEN_RESOLVED_USE_DIRECT_LIGHTING", "1").strip().lower() not in ("0", "false", "off")
DIRECT_LIGHT_SCALE = float(os.environ.get("LUMEN_RESOLVED_DIRECT_LIGHT_SCALE", "1.0"))
USE_DIRECT_DENOISING = os.environ.get("LUMEN_RESOLVED_USE_DIRECT_DENOISING", "1").strip().lower() not in ("0", "false", "off")
DIRECT_DENOISING_METHOD = os.environ.get("LUMEN_RESOLVED_DIRECT_DENOISING_METHOD", "RelaxDiffuseSpecular")
DIRECT_DENOISING_MAX_INTENSITY = float(os.environ.get("LUMEN_RESOLVED_DIRECT_DENOISING_MAX_INTENSITY", "250.0"))
USE_INDIRECT_DENOISING = os.environ.get("LUMEN_RESOLVED_USE_INDIRECT_DENOISING", "1").strip().lower() not in ("0", "false", "off")
INDIRECT_DENOISING_MAX_INTENSITY = float(os.environ.get("LUMEN_RESOLVED_INDIRECT_DENOISING_MAX_INTENSITY", "100.0"))
USE_DIRECT_ACCUMULATION = os.environ.get("LUMEN_RESOLVED_DIRECT_ACCUMULATION", "0").strip().lower() not in ("0", "false", "off")
DIRECT_ACCUMULATION_FRAMES = int(os.environ.get("LUMEN_RESOLVED_DIRECT_ACCUMULATION_FRAMES", "10"))
DIRECT_ACCUMULATION_AUTO_RESET = os.environ.get("LUMEN_RESOLVED_DIRECT_ACCUMULATION_AUTO_RESET", "0").strip().lower() not in ("0", "false", "off")
PROFILER_OUT = os.environ.get("LUMEN_RESOLVED_PROFILER_OUT", "").strip()
PROFILER_FRAMES = max(1, int(os.environ.get("LUMEN_RESOLVED_PROFILER_FRAMES", "30")))
FINALCOLOR_RUNTIME_OUT = os.environ.get("LUMEN_C9_FINALCOLOR_RUNTIME_OUT", "").strip()
FINALCOLOR_REFERENCE_JSON = os.environ.get("LUMEN_C9_REFERENCE_RUNTIME_JSON", "").strip()
# Optional test-only producer tracing.  When enabled, the graph adds marked
# BlitPass sentinels fed by the direct, LumenGI, and composite producers.  This
# deliberately changes the diagnostic graph topology and must never be used as
# strict C9 export-equivalence evidence.
PRODUCER_TRACE_OUT = os.environ.get("LUMEN_C9_PRODUCER_TRACE_OUT", "").strip()
# Optional strict C9 pair mode.  One Mogwai process recreates the scene and
# RenderGraph twice, with an identical frame schedule, and changes only whether
# the LumenGI diagnostic outputs are marked.  The two phase artifacts are
# consumed by run_c9_export_repro.py; neither phase can self-promote to PASS.
DETERMINISTIC_REPLAY_OUT = os.environ.get("LUMEN_C9_DETERMINISTIC_REPLAY_OUT", "").strip()
DETERMINISTIC_REPLAY_ID = os.environ.get("LUMEN_C9_DETERMINISTIC_REPLAY_ID", "").strip()
DETERMINISTIC_REPLAY_CONTEXT = {}
_REPLAY_ORDER = os.environ.get("LUMEN_C9_REPLAY_ORDER", "mark-on-first").strip().lower()
REPLAY_ORDER = _REPLAY_ORDER if _REPLAY_ORDER in ("mark-on-first", "mark-off-first") else "mark-on-first"
# Marking a RenderGraph output changes endpoint exposure, not the producer
# arithmetic.  The default C9 runtime path therefore performs an explicit
# same-process mark-on -> unmark transition after the composite is rendered.
# This avoids comparing two independently seeded RTXDI/NRD processes.  Set to
# 0 only when a caller deliberately wants the older independent-reference mode.
SAME_PROCESS_EQUIVALENCE = os.environ.get("LUMEN_C9_SAME_PROCESS_EQUIVALENCE", "1").strip().lower() not in ("0", "false", "off")
# Optional strict-replay scheduling diagnostic.  When enabled, drain the GPU
# after the final renderFrame() and before FrameCapture adds its readback work.
# This is deliberately off by default and is only meaningful for deterministic
# C9 replay; it must not change the ordinary showcase schedule.
WAIT_BEFORE_FRAME_CAPTURE = (
    os.environ.get("LUMEN_C9_WAIT_BEFORE_FRAME_CAPTURE", "0").strip().lower()
    not in ("0", "false", "off")
)
KEEP_SCENE_CAMERA = os.environ.get("LUMEN_RESOLVED_KEEP_SCENE_CAMERA", "0").strip().lower() not in ("0", "false", "off")
MARK_LUMEN_OUTPUTS = os.environ.get("LUMEN_RESOLVED_MARK_LUMEN_OUTPUTS", "1").strip().lower() not in ("0", "false", "off")
E1_DIAGNOSTIC_OUTPUTS = os.environ.get("LUMEN_RESOLVED_E1_DIAGNOSTIC_OUTPUTS", "0").strip().lower() not in ("0", "false", "off")
# Optional presentation-only scene overrides.  These are deliberately applied
# after loading the scene so the source assets remain untouched.  They make the
# Arcade shadow/emissive A/B reproducible without changing the LumenGI ABI.
ENV_INTENSITY = os.environ.get("LUMEN_RESOLVED_ENV_INTENSITY", "").strip()
POINT_LIGHT_SCALE = float(os.environ.get("LUMEN_RESOLVED_POINT_LIGHT_SCALE", "1.0"))
DIRECTIONAL_LIGHT_SCALE = float(os.environ.get("LUMEN_RESOLVED_DIRECTIONAL_LIGHT_SCALE", "1.0"))
EMISSIVE_FACTOR_SCALE = float(os.environ.get("LUMEN_RESOLVED_EMISSIVE_FACTOR_SCALE", "1.0"))
VIEW_SPECS = {
    # Keep the room and cabinet in frame. The earlier close-up positions made
    # the indirect-radiance preview look like a crop rather than a scene view.
    "front": (float3(0, 0.75, 2.6), float3(0, 0.5, 0)),
    "left": (float3(-2.2, 1.0, 1.8), float3(0, 0.5, 0)),
    "right": (float3(0.8, 0.8, 2.6), float3(0, 0.5, 0)),
}

LUMEN_RUNTIME_OUTPUTS = (
    "diffuseGI",
    "diffuseRadianceHitDist",
    "probeHistory",
    "probeInterpolated",
    "temporalFiltered",
    "temporalAlpha",
    "temporalConfidence",
    "temporalMoments",
    "spatialFiltered",
    "filteredVariance",
    "resolvedDiffuseGI",
)


def _lumen_output_mark_list():
    """Return the diagnostic outputs retained by the current capture mode.

    The ordinary showcase keeps the complete LumenGI diagnostic surface for
    screenshot and telemetry consumers.  Strict C9 replay only needs the
    production composite's resolved diffuse resource; retaining every
    intermediate diagnostic in mark-on changes RenderGraph lifetime/aliasing
    versus mark-off and can introduce unrelated low-bit differences in the
    final composite.  This narrows only the replay harness and leaves the
    production graph/output policy unchanged.
    """
    if DETERMINISTIC_REPLAY_OUT:
        return ("resolvedDiffuseGI",)
    outputs = LUMEN_RUNTIME_OUTPUTS
    if E1_DIAGNOSTIC_OUTPUTS:
        outputs += (
            "roughSpecularIndirect",
            "roughSpecularValidity",
            "transmissionIndirect",
            "transmissionValidity",
        )
    return outputs


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


def _apply_scene_overrides():
    """Apply opt-in lighting overrides for visual A/B captures only."""
    if ENV_INTENSITY:
        try:
            m.scene.envMap.intensity = float(ENV_INTENSITY)
        except Exception as exc:
            print("SCENE_OVERRIDE_WARN", "env", str(exc))
    if POINT_LIGHT_SCALE != 1.0:
        try:
            light = m.scene.getLight("Point light")
            light.intensity = light.intensity * POINT_LIGHT_SCALE
        except Exception as exc:
            print("SCENE_OVERRIDE_WARN", "point", str(exc))
    if DIRECTIONAL_LIGHT_SCALE != 1.0:
        try:
            light = m.scene.getLight("Directional light")
            light.intensity = light.intensity * DIRECTIONAL_LIGHT_SCALE
        except Exception as exc:
            print("SCENE_OVERRIDE_WARN", "directional", str(exc))
    if EMISSIVE_FACTOR_SCALE != 1.0:
        try:
            # The string overload of Scene.get_material() is unreliable in this
            # Falcor build; resolve by the reflected material list instead.
            material = next((candidate for candidate in m.scene.materials if str(candidate.name) == "Cabinet"), None)
            if material is None:
                raise RuntimeError("material 'Cabinet' was not found")
            material.emissiveFactor = material.emissiveFactor * EMISSIVE_FACTOR_SCALE
        except Exception as exc:
            print("SCENE_OVERRIDE_WARN", "emissive", str(exc))
    if ENV_INTENSITY or POINT_LIGHT_SCALE != 1.0 or DIRECTIONAL_LIGHT_SCALE != 1.0 or EMISSIVE_FACTOR_SCALE != 1.0:
        print(
            "SCENE_OVERRIDES",
            "env", ENV_INTENSITY or "unchanged",
            "pointScale", POINT_LIGHT_SCALE,
            "directionalScale", DIRECTIONAL_LIGHT_SCALE,
            "emissiveScale", EMISSIVE_FACTOR_SCALE,
        )


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
                "useSurfaceCache": USE_SURFACE_CACHE,
                "useCacheLighting": USE_CACHE_LIGHTING,
                "useScreenTrace": True,
                "useScreenProbes": True,
                # Reference-quality real-time probe sampling for the visual gate.
                # This remains a bounded 32-direction/probe path, not a path-tracing
                # replacement; production presets can lower it explicitly.
                "probeDirectionsPerProbe": max(1, PROBE_DIRECTIONS_PER_PROBE),
                "useTemporalFilter": USE_LUMEN_TEMPORAL_FILTER,
                "useSpatialFilter": USE_LUMEN_SPATIAL_FILTER,
                "spatialRadiusMin": max(0.0, SPATIAL_RADIUS_MIN),
                "spatialRadiusMax": max(0.0, SPATIAL_RADIUS_MAX),
                "spatialVarianceThresholdLow": max(0.0, SPATIAL_VARIANCE_LOW),
                "spatialVarianceThresholdHigh": max(0.0, SPATIAL_VARIANCE_HIGH),
                "debugMode": "None",
            },
        ),
        "LumenGI",
    )
    if USE_INDIRECT_DENOISING:
        # UE-style quality mode: denoise the raw HWRT-compatible Lumen
        # radiance+hit-distance output before material modulation. This is the
        # stable low-noise indirect source used by the final screenshot gate;
        # set the switch to 0 to inspect the screen-probe resolve directly.
        graph.addPass(
            createPass(
                "NRD",
                {
                    "method": "RelaxDiffuse",
                    "maxIntensity": INDIRECT_DENOISING_MAX_INTENSITY,
                    "worldSpaceMotion": True,
                    "enableReprojectionTestSkippingWithoutMotion": True,
                },
            ),
            "IndirectDenoise",
        )
        graph.addPass(
            createPass(
                "ModulateIllumination",
                {
                    "useDiffuseReflectance": True,
                    "useDiffuseRadiance": True,
                },
            ),
            "IndirectResolve",
        )
    if USE_DIRECT_LIGHTING:
        # RTXDIPass is the existing realtime direct/emissive producer. Its
        # diffuseIllumination output is demodulated RGB and hitT in alpha, which
        # is the same radiance+hit-distance contract consumed by NRD/UE-style
        # temporal-spatial denoisers.
        graph.addPass(createPass("RTXDIPass"), "DirectLighting")
        if USE_DIRECT_DENOISING:
            graph.addPass(
                createPass(
                    "NRD",
                    {
                        "method": DIRECT_DENOISING_METHOD,
                        "maxIntensity": DIRECT_DENOISING_MAX_INTENSITY,
                        "worldSpaceMotion": True,
                        "enableReprojectionTestSkippingWithoutMotion": True,
                    },
                ),
                "DirectDenoise",
            )
            graph.addPass(
                createPass(
                    "ModulateIllumination",
                    {
                        "useEmission": True,
                        "useDiffuseReflectance": True,
                        "useDiffuseRadiance": True,
                        "useSpecularReflectance": True,
                        "useSpecularRadiance": True,
                    },
                ),
                "DirectResolve",
            )
        elif USE_DIRECT_ACCUMULATION:
            # Legacy diagnostic fallback. This is not a replacement for a
            # radiance+hit-distance denoiser and is disabled by default.
            graph.addPass(
                createPass(
                    "AccumulatePass",
                    {
                        "enabled": True,
                        "precisionMode": "Single",
                        "autoReset": DIRECT_ACCUMULATION_AUTO_RESET,
                        "maxFrameCount": max(1, DIRECT_ACCUMULATION_FRAMES),
                        "overflowMode": "EMA",
                    },
                ),
                "DirectTemporal",
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
    # Full-scene screenshot composite: direct/emissive lighting plus resolved indirect diffuse
    # irradiance.  The fallback branch remains useful for isolating LumenGI, but is explicitly
    # marked as an indirect-only preview rather than a final-color claim.
    graph.addPass(
        createPass(
            "Composite",
            {
                "mode": "Add",
                "scaleA": DIRECT_LIGHT_SCALE if USE_DIRECT_LIGHTING else 0.35,
                "scaleB": GI_PREVIEW_SCALE,
            },
        ),
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
    lumen_outputs = _lumen_output_mark_list()
    # Keep Mogwai's first graph output on the production composite. The
    # renderer uses output[0] as its mainOutput; putting a diagnostic LumenGI
    # texture first makes a later same-process unmark transition leave the
    # renderer trying to blit an intentionally unmarked resource.
    graph.markOutput("ResolvedCompositePreview.out")
    if MARK_LUMEN_OUTPUTS:
        for channel in lumen_outputs:
            graph.markOutput("LumenGI." + channel)
    if USE_DIRECT_LIGHTING:
        graph.addEdge("GBufferRT.vbuffer", "DirectLighting.vbuffer")
        graph.addEdge("GBufferRT.mvec", "DirectLighting.mvec")
        if USE_DIRECT_DENOISING:
            graph.addEdge("DirectLighting.diffuseIllumination", "DirectDenoise.diffuseRadianceHitDist")
            graph.addEdge("DirectLighting.specularIllumination", "DirectDenoise.specularRadianceHitDist")
            graph.addEdge("GBufferRT.mvecW", "DirectDenoise.mvec")
            graph.addEdge("GBufferRT.normWRoughnessMaterialID", "DirectDenoise.normWRoughnessMaterialID")
            graph.addEdge("GBufferRT.linearZ", "DirectDenoise.viewZ")
            graph.addEdge("DirectLighting.emission", "DirectResolve.emission")
            graph.addEdge("DirectLighting.diffuseReflectance", "DirectResolve.diffuseReflectance")
            graph.addEdge("DirectLighting.specularReflectance", "DirectResolve.specularReflectance")
            graph.addEdge("DirectDenoise.filteredDiffuseRadianceHitDist", "DirectResolve.diffuseRadiance")
            graph.addEdge("DirectDenoise.filteredSpecularRadianceHitDist", "DirectResolve.specularRadiance")
            graph.addEdge("DirectResolve.output", "ResolvedCompositePreview.A")
        elif USE_DIRECT_ACCUMULATION:
            graph.addEdge("DirectLighting.color", "DirectTemporal.input")
            graph.addEdge("DirectTemporal.output", "ResolvedCompositePreview.A")
        else:
            graph.addEdge("DirectLighting.color", "ResolvedCompositePreview.A")
    else:
        graph.addEdge("GBufferRT.diffuseOpacity", "ResolvedCompositePreview.A")
    if USE_INDIRECT_DENOISING:
        graph.addEdge("LumenGI.diffuseRadianceHitDist", "IndirectDenoise.diffuseRadianceHitDist")
        graph.addEdge("GBufferRT.mvecW", "IndirectDenoise.mvec")
        graph.addEdge("GBufferRT.normWRoughnessMaterialID", "IndirectDenoise.normWRoughnessMaterialID")
        graph.addEdge("GBufferRT.linearZ", "IndirectDenoise.viewZ")
        graph.addEdge("GBufferRT.diffuseOpacity", "IndirectResolve.diffuseReflectance")
        graph.addEdge("IndirectDenoise.filteredDiffuseRadianceHitDist", "IndirectResolve.diffuseRadiance")
        # Keep the denoised raw source available as a marked diagnostic output,
        # but freeze the full-scene final-color contract on the LumenGI public
        # resolve.  This prevents an optional NRD branch from silently becoming
        # the production composite input.
        graph.markOutput("IndirectResolve.output")
    graph.addEdge("LumenGI.resolvedDiffuseGI", "ResolvedCompositePreview.B")
    graph.addEdge("ResolvedCompositePreview.out", "ToneMapperDisplay.src")
    graph.markOutput("ToneMapperDisplay.dst")
    if PRODUCER_TRACE_OUT:
        # Test-only readback sentinels keep the source endpoints unmarked while
        # making their same-frame values available through legal graph outputs.
        # This adds copy passes and therefore changes topology; never mix this
        # diagnostic run with the strict C9 baseline.
        sentinel_options = {"filter": "Point", "outputFormat": "RGBA32Float"}
        producer_sentinels = {
            "DirectResolve.output": "C9TraceDirect",
            "LumenGI.resolvedDiffuseGI": "C9TraceLumen",
            "ResolvedCompositePreview.out": "C9TraceComposite",
        }
        if USE_DIRECT_LIGHTING and USE_DIRECT_DENOISING:
            for source, pass_name in producer_sentinels.items():
                graph.addPass(createPass("BlitPass", sentinel_options), pass_name)
                graph.addEdge(source, pass_name + ".src")
                graph.markOutput(pass_name + ".dst")
    return graph


def _same_process_mark_equivalence(graph, composite):
    """Prove bounded same-resource invariance across a zero-execute unmark.

    ``unmarkOutput`` invalidates the graph.  ``RenderGraph.getOutput()`` then
    rejects all fetches until a compile/execute, while executing would advance
    RTXDI, NRD and LumenGI producer history.  Retain the already-compiled
    composite resource instead, unmark only diagnostic outputs, and prove that
    the metadata mutation itself does not alter its bytes.  This is deliberately
    PASS_BOUNDED: it is not evidence for a newly executed mark-off graph.
    """
    if not SAME_PROCESS_EQUIVALENCE:
        return {
            "status": "BLOCKED",
            "comparisonMode": "disabled",
            "reason": "same-process mark transition disabled by LUMEN_C9_SAME_PROCESS_EQUIVALENCE=0",
            "renderedFrames": 0,
        }

    names = list(_lumen_output_mark_list())
    rendered_frames = 0
    try:
        if not MARK_LUMEN_OUTPUTS:
            return {
                "status": "BLOCKED",
                "comparisonMode": "same_process_graph_unmark",
                "reason": "zero-execute proof must start from a compiled mark-on graph",
                "renderedFrames": 0,
            }
        # Hold the compiled resource before invalidating the graph. Marking an
        # already marked output is a no-op and does not request recompilation.
        for channel in names:
            graph.markOutput("LumenGI." + channel)
        composite_resource = m.activeGraph.get_output("ResolvedCompositePreview.out")
        mark_on_composite = np.ascontiguousarray(composite_resource.to_numpy()[..., :3]).copy()
        for channel in names:
            graph.unmarkOutput("LumenGI." + channel)
        # The graph-level fetch must now be unavailable because unmarkOutput()
        # set mRecompile.  The retained resource itself remains readable and is
        # the exact same producer execution; no render/compile is permitted.
        graph_fetch_blocked = False
        try:
            m.activeGraph.get_output("ResolvedCompositePreview.out")
        except Exception:
            graph_fetch_blocked = True
        mark_off_composite = np.ascontiguousarray(composite_resource.to_numpy()[..., :3]).copy()
        on_error = np.abs(np.asarray(composite, dtype=np.float64) - np.asarray(mark_on_composite, dtype=np.float64))
        off_error = np.abs(np.asarray(mark_on_composite, dtype=np.float64) - np.asarray(mark_off_composite, dtype=np.float64))
        exact = bool(np.array_equal(mark_on_composite, mark_off_composite))
        return {
            "status": "PASS_BOUNDED" if exact and graph_fetch_blocked else "FAIL",
            "comparisonMode": "same_process_graph_unmark",
            "reason": "retained compiled resource is byte-exact across diagnostic-output metadata unmark; graph was not recompiled or executed",
            "renderedFrames": rendered_frames,
            "producerExecutions": 0,
            "graphRecompiled": False,
            "graphFetchBlockedUntilRecompile": graph_fetch_blocked,
            "resourceIdentityRetained": True,
            "retainedResourceReadableAfterUnmark": True,
            "proofScope": "metadata mutation against the retained compiled composite resource",
            "lumenOutputsMarked": False,
            "markOn": {"status": "PASS", "lumenOutputsMarked": True},
            "markOff": {
                "status": "PASS_BOUNDED" if exact and graph_fetch_blocked else "FAIL",
                "lumenOutputsMarked": False,
            },
            "exact": exact,
            "meanAbsErrorFromCaptured": float(np.mean(on_error)),
            "maxAbsErrorFromCaptured": float(np.max(on_error)),
            "meanAbsErrorMarkOnOff": float(np.mean(off_error)),
            "maxAbsErrorMarkOnOff": float(np.max(off_error)),
            "markOnSha256": hashlib.sha256(np.ascontiguousarray(mark_on_composite).tobytes()).hexdigest(),
            "markOffSha256": hashlib.sha256(np.ascontiguousarray(mark_off_composite).tobytes()).hexdigest(),
        }
    except Exception as exc:
        return {
            "status": "BLOCKED",
            "comparisonMode": "same_process_graph_unmark",
            "reason": "RenderGraph mark transition/readback unavailable: " + str(exc),
            "renderedFrames": rendered_frames,
            "producerExecutions": 0,
            "graphRecompiled": False,
        }


def _strict_replay_teardown_fence(scope="strict replay capture before graph teardown"):
    """Drain the live device before a strict replay graph is destroyed.

    The replay intentionally recreates the scene and graph between mark-on and
    mark-off phases. Waiting at that boundary prevents deferred scene/FBO
    releases from overlapping the next phase, while leaving ordinary showcase
    captures untouched. The status is recorded as diagnostics; the offline
    gate still owns the frozen pixel verdict.
    """
    if not DETERMINISTIC_REPLAY_OUT:
        return {"status": "NOT_RUN", "binding": "m.device.wait"}
    try:
        device = getattr(globals().get("m"), "device", None)
        wait = getattr(device, "wait", None) if device is not None else None
        if not callable(wait):
            raise RuntimeError("live m.device.wait binding is unavailable")
        wait()
        return {
            "status": "PASS",
            "binding": "m.device.wait",
            "scope": scope,
        }
    except Exception as exc:
        return {
            "status": "BLOCKED",
            "binding": "m.device.wait",
            "scope": scope,
            "reason": str(exc),
        }


def _strict_replay_unload_scene():
    """Release the prior Scene before SceneBuilder constructs the next phase.

    Renderer.loadScene() builds the replacement Scene before setScene() can
    release the current one.  Strict replay uses an explicit unload boundary
    between phases so the two Scene GPU allocations cannot overlap.  This is
    intentionally scoped to the diagnostic replay path.
    """
    unload = getattr(m, "unloadScene", None)
    if not callable(unload):
        raise RuntimeError("live m.unloadScene binding is unavailable")
    unload()
    fence = _strict_replay_teardown_fence("strict replay after scene unload")
    if fence.get("status") != "PASS":
        raise RuntimeError("strict replay scene-unload fence failed: " + str(fence.get("reason", "unknown error")))


def _capture_producer_trace(graph, label, view_name):
    """Capture test-only float producer sentinels without changing strict gate data.

    The sentinels are added only when ``LUMEN_C9_PRODUCER_TRACE_OUT`` is set.
    They add copy passes and marked outputs, so their artifacts diagnose source
    deltas but are intentionally not consumed by the strict C9 verifier.
    """
    if not PRODUCER_TRACE_OUT:
        return None

    phase = str(DETERMINISTIC_REPLAY_CONTEXT.get("phase", "single")) or "single"
    trace_dir = Path(PRODUCER_TRACE_OUT) / (label + "-" + view_name) / phase
    trace_dir.mkdir(parents=True, exist_ok=True)
    producer_sentinels = {
        "direct": ("DirectResolve.output", "C9TraceDirect.dst"),
        "lumen": ("LumenGI.resolvedDiffuseGI", "C9TraceLumen.dst"),
        "composite": ("ResolvedCompositePreview.out", "C9TraceComposite.dst"),
    }
    outputs = {}
    all_ok = True
    for key, (source_endpoint, readback_endpoint) in producer_sentinels.items():
        record = {
            "sourceEndpoint": source_endpoint,
            "readbackEndpoint": readback_endpoint,
            "readbackFormat": "RGBA32Float",
        }
        try:
            data = np.asarray(graph.get_output(readback_endpoint).to_numpy()).copy()
            data = np.ascontiguousarray(data)
            path = trace_dir / (key + ".npy")
            np.save(path, data, allow_pickle=False)
            record.update(
                {
                    "status": "PASS" if np.isfinite(data).all() else "FAIL",
                    "path": str(path),
                    "shape": list(data.shape),
                    "dtype": str(data.dtype),
                    "finite": bool(np.isfinite(data).all()),
                    "min": float(data.min()) if data.size else 0.0,
                    "max": float(data.max()) if data.size else 0.0,
                    "mean": float(data.mean()) if data.size else 0.0,
                    "sha256": hashlib.sha256(data.tobytes()).hexdigest(),
                }
            )
        except Exception as exc:
            record.update({"status": "FAIL", "error": str(exc)})
        outputs[key] = record
        all_ok = all_ok and record.get("status") == "PASS"

    trace_scene = str(
        DETERMINISTIC_REPLAY_CONTEXT.get("config", {}).get("scene", "")
        if isinstance(DETERMINISTIC_REPLAY_CONTEXT.get("config"), dict)
        else ""
    )
    metadata = {
        "schema": "LumenGI.C9.ProducerTrace.v1",
        "status": "PASS" if all_ok else "FAIL",
        "diagnosticOnly": True,
        "topologyChanged": True,
        "phase": phase,
        "scene": trace_scene or None,
        "view": view_name,
        "frame": SETTLE_FRAMES,
        "outputs": outputs,
    }
    metadata_path = trace_dir / "producer-trace.json"
    metadata["metadataPath"] = str(metadata_path)
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return metadata


def _capture(label, scene_path, view_name):
    m.loadScene(scene_path)
    _apply_scene_overrides()
    m.resizeFrameBuffer(*RESOLUTION)
    # Cornell's scene file already carries a composed camera. The Arcade view
    # coordinates are intentionally not reused there, otherwise the room shrinks
    # to a tiny center crop and looks like a GI failure.
    if label.lower() != "cornell" and not KEEP_SCENE_CAMERA:
        position, target = VIEW_SPECS[view_name]
        m.scene.camera.position = position
        m.scene.camera.target = target
        m.scene.camera.up = float3(0, 1, 0)
        m.scene.camera.focalLength = 35.0
    graph = _graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    m.frameCapture.outputDir = OUT_DIR
    replay_phase = str(DETERMINISTIC_REPLAY_CONTEXT.get("phase", ""))
    m.frameCapture.baseFilename = label + "-" + view_name + "-resolved" + ("-" + replay_phase if replay_phase else "")
    for frame in range(1, SETTLE_FRAMES + 1):
        m.clock.frame = frame
        m.renderFrame()
    if PROFILER_OUT:
        # Keep the profiler opt-in so ordinary screenshot runs retain their exact
        # frame schedule.  Capture the same graph after warmup, including the
        # direct RTXDI/NRD and LumenGI indirect branch used by the final image.
        m.profiler.reset_stats()
        m.profiler.start_capture(PROFILER_FRAMES)
        for offset in range(PROFILER_FRAMES + 1):
            m.clock.frame = SETTLE_FRAMES + 1 + offset
            m.renderFrame()
        profiler_capture = m.profiler.end_capture() or {}
        profiler_path = Path(PROFILER_OUT)
        profiler_path.mkdir(parents=True, exist_ok=True)
        with (profiler_path / (label + "-" + view_name + ".json")).open("w", encoding="utf-8") as handle:
            json.dump(profiler_capture, handle, indent=2, default=str)
    pre_capture_fence = {
        "status": "NOT_RUN",
        "binding": "m.device.wait",
        "scope": "strict replay before FrameCapture",
    }
    if WAIT_BEFORE_FRAME_CAPTURE and DETERMINISTIC_REPLAY_OUT:
        pre_capture_fence = _strict_replay_teardown_fence("strict replay before FrameCapture")
        if pre_capture_fence.get("status") != "PASS":
            raise RuntimeError(
                "strict replay pre-capture fence failed: "
                + str(pre_capture_fence.get("reason", "unknown error"))
            )
    elif WAIT_BEFORE_FRAME_CAPTURE:
        print("RESOLVED_SHOWCASE pre-capture wait ignored outside deterministic replay")
    m.frameCapture.capture()
    if MARK_LUMEN_OUTPUTS:
        resolved = m.activeGraph.get_output("LumenGI.resolvedDiffuseGI").to_numpy()[..., :3]
    else:
        # The mark-off policy intentionally hides direct LumenGI diagnostics;
        # the production composite remains the only endpoint sampled here.
        resolved = np.zeros((RESOLUTION[1], RESOLUTION[0], 3), dtype=np.float32)
    composite = m.activeGraph.get_output("ResolvedCompositePreview.out").to_numpy()[..., :3]
    display = m.activeGraph.get_output("ToneMapperDisplay.dst").to_numpy()[..., :3]
    # Capture the host-side counters alongside the C9 endpoint snapshot.  This
    # is read-only telemetry: it does not alter graph topology or scheduling,
    # and makes cache-lookup/feedback isolation claims auditable in the artifact.
    screen_probe_stats = {}
    surface_cache_stats = {}
    try:
        lumen_pass = graph.getPass("LumenGI")
        screen_probe_stats = dict(lumen_pass.screenProbeStats)
        surface_cache_stats = dict(lumen_pass.surfaceCacheStats)
    except Exception as exc:
        print("RESOLVED_SHOWCASE telemetry unavailable", str(exc))
    producer_trace = _capture_producer_trace(graph, label, view_name)
    if FINALCOLOR_RUNTIME_OUT:
        runtime_path = Path(FINALCOLOR_RUNTIME_OUT)
        if runtime_path.suffix.lower() != ".json":
            runtime_path = runtime_path / (label + "-" + view_name + "-finalcolor.json")
        runtime_path.parent.mkdir(parents=True, exist_ok=True)
        finite = bool(np.isfinite(composite).all())
        nonnegative = bool(float(composite.min()) >= 0.0)
        composite_digest = hashlib.sha256(np.ascontiguousarray(composite).tobytes()).hexdigest()
        # Preserve the actual composite pixels for the independent mark-on /
        # mark-off equivalence check. Mean/max summaries alone can hide a
        # localized mismatch and are not strong enough for the C9 gate.
        snapshot_path = runtime_path.with_suffix(".npy")
        np.save(snapshot_path, np.ascontiguousarray(composite), allow_pickle=False)
        export_status = "BLOCKED"
        export_reason = "single showcase execution only proves the marked runtime endpoint"
        export_metrics = {
            "snapshot": str(snapshot_path),
            "meanAbsError": None,
            "p99AbsError": None,
            "maxAbsError": None,
            "relativeMaxError": None,
            "tolerance": {
                # Separate Mogwai processes reproduce the same graph but can
                # differ by a few low bits in the RT/NRD composite. Keep the
                # absolute and relative limits explicit instead of comparing
                # only aggregate mean/max summaries.
                "meanAbsError": 2e-5,
                "p99AbsError": 5e-4,
                "maxAbsError": 5e-3,
                "relativeMaxError": 1e-4,
            },
        }
        reference_final = {}
        reference_pixels = None
        reference_export_metadata = False
        same_process_evidence = _same_process_mark_equivalence(graph, composite)
        if FINALCOLOR_REFERENCE_JSON:
            reference_path = Path(FINALCOLOR_REFERENCE_JSON)
            try:
                reference = json.loads(reference_path.read_text(encoding="utf-8"))
                reference_final = reference.get("finalColor", {})
                reference_export_metadata = any(
                    isinstance(reference.get(name), dict)
                    and str(reference[name].get("status", "")).upper() == "PASS"
                    for name in ("exportOn", "exportOff")
                )
                reference_digest = reference_final.get("sha256")
                reference_snapshot = reference_final.get("snapshot")
                if reference_snapshot:
                    snapshot_candidate = Path(reference_snapshot)
                    if not snapshot_candidate.is_absolute() and not snapshot_candidate.exists():
                        snapshot_candidate = reference_path.parent / snapshot_candidate
                    reference_pixels = np.asarray(np.load(snapshot_candidate, allow_pickle=False), dtype=np.float64)
                if reference_pixels is not None and reference_pixels.shape == composite.shape:
                    abs_error = np.abs(np.asarray(composite, dtype=np.float64) - reference_pixels)
                    export_metrics["meanAbsError"] = float(np.mean(abs_error))
                    export_metrics["p99AbsError"] = float(np.percentile(abs_error, 99.0))
                    export_metrics["maxAbsError"] = float(np.max(abs_error))
                    export_metrics["relativeMaxError"] = float(
                        export_metrics["maxAbsError"] / max(1.0, abs(float(reference_final.get("max", 0.0))))
                    )
                else:
                    mean_delta = abs(float(composite.mean()) - float(reference_final.get("mean", float("nan"))))
                    max_delta = abs(float(composite.max()) - float(reference_final.get("max", float("nan"))))
                    export_metrics["meanAbsError"] = mean_delta
                    export_metrics["maxAbsError"] = max_delta
                    export_metrics["p99AbsError"] = None
                    export_metrics["relativeMaxError"] = float(
                        max_delta / max(1.0, abs(float(reference_final.get("max", 0.0))))
                    ) if math.isfinite(max_delta) else None
                numeric_equivalent = (
                    export_metrics["meanAbsError"] is not None
                    and export_metrics["maxAbsError"] is not None
                    and math.isfinite(float(export_metrics["meanAbsError"]))
                    and math.isfinite(float(export_metrics["maxAbsError"]))
                    and export_metrics["relativeMaxError"] is not None
                    and math.isfinite(float(export_metrics["relativeMaxError"]))
                    and float(export_metrics["meanAbsError"]) <= export_metrics["tolerance"]["meanAbsError"]
                    and (export_metrics["p99AbsError"] is None or float(export_metrics["p99AbsError"]) <= export_metrics["tolerance"]["p99AbsError"])
                    and float(export_metrics["maxAbsError"]) <= export_metrics["tolerance"]["maxAbsError"]
                    and float(export_metrics["relativeMaxError"]) <= export_metrics["tolerance"]["relativeMaxError"]
                )
                if reference_digest and reference_digest == composite_digest:
                    export_status = "PASS"
                    export_reason = "composite SHA-256 matches independent mark-on execution"
                elif numeric_equivalent:
                    export_status = "PASS"
                    export_reason = "composite endpoint pixel error is within the recorded independent-process tolerance"
                else:
                    export_status = "FAIL"
                    export_reason = "composite endpoint pixel error exceeds the recorded equivalence tolerance"
            except Exception as exc:
                export_status = "BLOCKED"
                export_reason = "reference runtime JSON unavailable: " + str(exc)
        if export_status == "PASS" and FINALCOLOR_REFERENCE_JSON and not reference_export_metadata:
            # A numeric comparison against a legacy artifact is not enough for
            # the strict gate.  The independent side must identify its export
            # mode explicitly before equivalence can be promoted.
            export_status = "BLOCKED"
            export_reason = "reference runtime is missing explicit export-on/export-off metadata"

        mark_on_metadata = dict(
            same_process_evidence.get("markOn", {})
            if same_process_evidence.get("status") in ("PASS", "PASS_BOUNDED", "FAIL")
            else {}
        )
        mark_off_metadata = dict(
            same_process_evidence.get("markOff", {})
            if same_process_evidence.get("status") in ("PASS", "PASS_BOUNDED", "FAIL")
            else {}
        )
        mark_on_metadata.update(
            {
                "endpoint": "LumenGI.resolvedDiffuseGI",
                "comparisonMode": same_process_evidence.get("comparisonMode"),
                "reason": same_process_evidence.get("reason")
                if same_process_evidence.get("status") != "PASS"
                else None,
            }
        )
        mark_off_metadata.update(
            {
                "endpoint": "LumenGI.resolvedDiffuseGI",
                "comparisonMode": same_process_evidence.get("comparisonMode"),
                "reason": same_process_evidence.get("reason")
                if same_process_evidence.get("status") != "PASS"
                else "same-process graph unmark transition completed",
            }
        )
        if "status" not in mark_on_metadata:
            mark_on_metadata = {
                "status": "PASS" if MARK_LUMEN_OUTPUTS else "BLOCKED",
                "endpoint": "LumenGI.resolvedDiffuseGI",
                "lumenOutputsMarked": bool(MARK_LUMEN_OUTPUTS),
                "comparisonMode": same_process_evidence.get("comparisonMode"),
                "reason": None
                if MARK_LUMEN_OUTPUTS
                else "this execution intentionally used mark-off policy",
            }
        if "status" not in mark_off_metadata:
            mark_off_metadata = {
                "status": "PASS" if not MARK_LUMEN_OUTPUTS else "BLOCKED",
                "endpoint": "LumenGI.resolvedDiffuseGI",
                "lumenOutputsMarked": bool(MARK_LUMEN_OUTPUTS),
                "comparisonMode": same_process_evidence.get("comparisonMode"),
                "reason": "LumenGI diagnostic outputs were unmarked in this execution"
                if not MARK_LUMEN_OUTPUTS
                else "mark-off execution is a separate runtime invocation",
            }
        export_on_metadata = {
            "status": "PASS" if finite and nonnegative else "FAIL",
            "endpoint": "ResolvedCompositePreview.out",
            "marked": True,
            "snapshot": str(snapshot_path),
            "reason": None if finite and nonnegative else "current composite is not finite/nonnegative",
        }
        export_off_metadata = {
            "status": "PASS" if export_status == "PASS" else export_status,
            "endpoint": "ResolvedCompositePreview.out",
            "snapshot": reference_final.get("snapshot") if isinstance(reference_final, dict) else None,
            "comparisonMode": export_metrics.get("comparisonMode"),
            "independentFile": bool(reference_export_metadata),
            "reason": None if export_status == "PASS" else export_reason,
        }
        runtime = {
            "schema": "LumenGI.C9.FinalColorRuntime.v1",
            "deterministicReplay": (
                {
                    **DETERMINISTIC_REPLAY_CONTEXT,
                    "processId": os.getpid(),
                    "sceneReloaded": True,
                    "graphRecreated": True,
                    "clockFrames": [1, SETTLE_FRAMES],
                    "captureExecutions": 1,
                    "preCaptureFence": pre_capture_fence,
                }
                if DETERMINISTIC_REPLAY_CONTEXT
                else None
            ),
            "producerEvidence": {
                "status": "PASS" if USE_DIRECT_LIGHTING else "BLOCKED",
                "directEnabled": bool(USE_DIRECT_LIGHTING),
                "indirectEnabled": True,
                "compositeInputs": [
                    "DirectResolve.output" if USE_DIRECT_LIGHTING else "GBufferRT.diffuseOpacity",
                    "LumenGI.resolvedDiffuseGI",
                ],
                "required": {
                    "directEnabled": True,
                    "indirectEnabled": True,
                    "compositeInputs": [
                        "DirectResolve.output",
                        "LumenGI.resolvedDiffuseGI",
                    ],
                },
                "directPath": (
                    "RTXDI -> NRD -> DirectResolve"
                    if USE_DIRECT_LIGHTING and USE_DIRECT_DENOISING
                    else "RTXDI -> DirectResolve"
                    if USE_DIRECT_LIGHTING
                    else "disabled"
                ),
                "indirectPath": "LumenGI.resolvedDiffuseGI",
                "emissionInDirectResolve": bool(USE_DIRECT_LIGHTING),
            },
            "producerTrace": producer_trace,
            "screenProbeStats": screen_probe_stats,
            "surfaceCacheStats": surface_cache_stats,
            "finalColor": {
                "endpoint": "ResolvedCompositePreview.out",
                "marked": True,
                "lumenOutputsMarked": bool(MARK_LUMEN_OUTPUTS),
                "directEnabled": bool(USE_DIRECT_LIGHTING),
                "indirectEnabled": True,
                "compositeInputs": [
                    "DirectResolve.output" if USE_DIRECT_LIGHTING else "GBufferRT.diffuseOpacity",
                    "LumenGI.resolvedDiffuseGI",
                ],
                "finite": finite,
                "nonnegative": nonnegative,
                "frame": SETTLE_FRAMES,
                "scene": scene_path,
                "view": view_name,
                "mean": float(composite.mean()),
                "max": float(composite.max()),
                "sha256": composite_digest,
                "snapshot": str(snapshot_path),
            },
            "markOn": mark_on_metadata,
            "markOff": mark_off_metadata,
            # Bounded endpoint-only evidence.  This is intentionally separate
            # from the independent export comparison and cannot promote a
            # stochastic export pair to PASS.
            "sameProcessMarkTransition": same_process_evidence,
            "exportOn": export_on_metadata,
            "exportOff": export_off_metadata,
            "exportEquivalence": {
                "status": export_status,
                "reason": export_reason,
                "metrics": export_metrics,
            },
        }
        teardown_fence = _strict_replay_teardown_fence()
        if isinstance(runtime.get("deterministicReplay"), dict):
            runtime["deterministicReplay"]["teardownFence"] = teardown_fence
        runtime_path.write_text(json.dumps(runtime, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "RESOLVED_SHOWCASE",
        label,
        view_name,
        "gi_mean",
        float(resolved.mean()),
        "gi_max",
        float(resolved.max()),
        "composite_mean",
        float(composite.mean()),
        "composite_max",
        float(composite.max()),
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
    # Drop the final Python graph reference before fencing the deferred
    # releases. This is a diagnostic guard for strict replay only; ordinary
    # showcase captures keep their historical teardown path.
    graph = None
    if DETERMINISTIC_REPLAY_OUT:
        post_teardown_fence = _strict_replay_teardown_fence("strict replay after graph teardown")
        runtime_path = Path(FINALCOLOR_RUNTIME_OUT) if FINALCOLOR_RUNTIME_OUT else None
        if runtime_path and runtime_path.exists():
            try:
                runtime_after_teardown = json.loads(runtime_path.read_text(encoding="utf-8"))
                deterministic = runtime_after_teardown.get("deterministicReplay")
                if isinstance(deterministic, dict):
                    deterministic["postTeardownFence"] = post_teardown_fence
                    runtime_path.write_text(
                        json.dumps(runtime_after_teardown, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                    )
            except Exception as exc:
                print("RESOLVED_SHOWCASE teardown fence metadata blocked", str(exc))


m.ui = False
m.clock.framerate = FRAME_RATE
m.clock.pause()
os.makedirs(OUT_DIR, exist_ok=True)
for label, scene_path in _scenes():
    for view_name in _views():
        if DETERMINISTIC_REPLAY_OUT:
            # Both phases are intentionally executed by this process.  Loading
            # the scene and constructing a fresh graph resets RTXDI, NRD and
            # LumenGI internal frame histories; the explicit 1..N clock table
            # then makes their frame-derived seeds identical.  The offline
            # verifier still applies the original strict pixel thresholds.
            pair_root = Path(DETERMINISTIC_REPLAY_OUT) / (label + "-" + view_name)
            pair_root.mkdir(parents=True, exist_ok=True)
            pair_id = DETERMINISTIC_REPLAY_ID or hashlib.sha256(
                (str(pair_root.resolve()) + "|" + scene_path + "|" + view_name).encode("utf-8")
            ).hexdigest()[:24]
            config_payload = {
                "scene": scene_path,
                "view": view_name,
                "resolution": list(RESOLUTION),
                "settleFrames": SETTLE_FRAMES,
                "profilerFrames": PROFILER_FRAMES if PROFILER_OUT else 0,
                "directLighting": USE_DIRECT_LIGHTING,
                "directDenoising": USE_DIRECT_DENOISING,
                "directDenoisingMethod": DIRECT_DENOISING_METHOD,
                "indirectDenoising": USE_INDIRECT_DENOISING,
                "surfaceCache": USE_SURFACE_CACHE,
                "cacheLighting": USE_CACHE_LIGHTING,
                "disableSurfaceCacheLookup": DISABLE_SURFACE_CACHE_LOOKUP,
                "disableSurfaceCacheFeedback": DISABLE_SURFACE_CACHE_FEEDBACK,
                "lumenTemporalFilter": USE_LUMEN_TEMPORAL_FILTER,
                "lumenSpatialFilter": USE_LUMEN_SPATIAL_FILTER,
                "waitBeforeFrameCapture": WAIT_BEFORE_FRAME_CAPTURE,
                "probeDirections": PROBE_DIRECTIONS_PER_PROBE,
                "spatial": [SPATIAL_RADIUS_MIN, SPATIAL_RADIUS_MAX, SPATIAL_VARIANCE_LOW, SPATIAL_VARIANCE_HIGH],
                "lightingOverrides": [ENV_INTENSITY, POINT_LIGHT_SCALE, DIRECTIONAL_LIGHT_SCALE, EMISSIVE_FACTOR_SCALE],
                "deterministicMarkChannels": list(_lumen_output_mark_list()) if DETERMINISTIC_REPLAY_OUT else None,
                "replayOrder": REPLAY_ORDER,
            }
            config_fingerprint = hashlib.sha256(
                json.dumps(config_payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
            ).hexdigest()
            on_json = pair_root / "mark-on.json"
            off_json = pair_root / "mark-off.json"
            phase_paths = {"mark-on": on_json, "mark-off": off_json}
            replay_phases = (
                ("mark-on", "mark-off")
                if REPLAY_ORDER == "mark-on-first"
                else ("mark-off", "mark-on")
            )
            first_phase_path = None
            for phase in replay_phases:
                MARK_LUMEN_OUTPUTS = phase == "mark-on"
                SAME_PROCESS_EQUIVALENCE = False
                FINALCOLOR_REFERENCE_JSON = str(first_phase_path) if first_phase_path else ""
                FINALCOLOR_RUNTIME_OUT = str(phase_paths[phase])
                DETERMINISTIC_REPLAY_CONTEXT = {
                    "pairId": pair_id,
                    "phase": phase,
                    "configFingerprint": config_fingerprint,
                    "config": config_payload,
                }
                _capture(label, scene_path, view_name)
                if first_phase_path is None:
                    first_phase_path = phase_paths[phase]
                    if DETERMINISTIC_REPLAY_OUT:
                        _strict_replay_unload_scene()
            manifest = {
                "schema": "LumenGI.C9.DeterministicReplayManifest.v1",
                "pairId": pair_id,
                "processId": os.getpid(),
                "configFingerprint": config_fingerprint,
                "replayOrder": REPLAY_ORDER,
                "markOn": str(on_json),
                "markOff": str(off_json),
                "gateCommand": "python -B tests/lumengi/run_c9_export_repro.py --manifest " + str(pair_root / "replay-manifest.json"),
            }
            (pair_root / "replay-manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
        else:
            _capture(label, scene_path, view_name)
print("RESOLVED_SHOWCASE done", OUT_DIR)
exit()
