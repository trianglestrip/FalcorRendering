"""C6 runtime gate: Surface Cache lookup effect and invalidation/budget matrix.

This is a Mogwai-only runner.  It keeps one fixed scene, camera, resolution and
frame-index seed schedule while executing four isolated cases in series:

``lookup_on``
    ``useSurfaceCache=True`` and ``useCacheLighting=True``.  This is the
    cache-backed screen-probe lookup path (``LUMEN_GI_PROBE_CACHE_LOOKUP``).
``lookup_off``
    Both cache switches are disabled.  This is the HWRT/screen-probe control.
``invalidate``
    Cache lookup is enabled, then the same scene is loaded again halfway through
    the checkpoint schedule.  The report records the post-reload cache reset and
    compares the re-warmed output with ``lookup_on``.
``low_budget``
    Cache lookup is enabled with ``captureMaxPagesPerFrame=1``.  This exercises
    the bounded capture path and records whether coverage/energy changes while
    keeping all outputs finite and non-negative.

The LumenGI pass has no host seed property.  Reproducibility is therefore defined
by the same scene/camera and the same explicit frame-index sequence in every
case; the JSON records this as ``seed_schedule`` instead of pretending that an
unbound ``seed`` property was applied.

The script never edits host/shader code and does not run a build.  Run it after a
Release build through Mogwai, with a unique ``LUMEN_C6_OUT`` directory per run.

Environment (optional):

    LUMEN_C6_OUT=artifacts/lumengi/C6/surfacecache-effect.json
    LUMEN_C6_SCENE=test_scenes/cornell_box.pyscene
    LUMEN_C6_RESOLUTION=640x360
    LUMEN_C6_CHECKPOINTS=1,8,16
    LUMEN_C6_INVALIDATE_AT=8
    LUMEN_C6_LOW_BUDGET=1
    LUMEN_C6_TINY_ATLAS=1
    LUMEN_C6_ATLAS_SIZE=64
    LUMEN_C6_TINY_MIN_FRAMES=72
    LUMEN_C6_PRESSURE_PHASES=1       # optional; recommend TINY_MIN_FRAMES=144
    LUMEN_C6_DIRTY_PRESSURE=1        # optional; mutate real material/geometry state
    LUMEN_C6_DIRTY_MODE=auto         # auto, material, or geometry
    LUMEN_C6_DIRTY_INTERVAL=8        # frames between mutation batches
    LUMEN_C6_DIRTY_BATCH=8           # materials/instances changed per mutation
    LUMEN_C6_SAMPLE_EVERY_FRAME=1    # optional; capture explicit N and N+1 samples
    LUMEN_C6_CASES=lookup_on,lookup_off,invalidate,low_budget
    LUMEN_C6_SAVE_ARRAYS=0

The numeric gate is intentionally diagnostic rather than a fake pass threshold:
all channels must be finite/non-negative, cache stats must be available and
valid, and each enabled-vs-control comparison must report a finite difference.
The C6 plan owns the final image-quality threshold once the cache lookup contract
is frozen.

The camera-partition schedule can be checked without Falcor::

    python tests/lumengi/run_surfacecache_effect.py --self-test

This fixture validates that tiny mode visits twelve distinct scheduled views;
it does not fabricate request, eviction, or stale-texel telemetry.
"""

import sys

try:
    from falcor import *

    FALCOR_AVAILABLE = True
    FALCOR_IMPORT_ERROR = None
except Exception as _falcor_error:  # pragma: no cover - exercised outside Mogwai.
    FALCOR_AVAILABLE = False
    FALCOR_IMPORT_ERROR = repr(_falcor_error)

import json
import math
import os

import numpy as np


FRAME_RATE = 60
DEFAULT_RESOLUTION = (640, 360)
DEFAULT_CHECKPOINTS = (1, 8, 16)
DEFAULT_CASES = ("lookup_on", "lookup_off", "invalidate", "low_budget")

# C6 page-lifecycle telemetry is a host contract, not an image-derived heuristic.
# Keep aliases explicit so a report can show the exact source key that satisfied each
# field.  A missing field is BLOCKED; this runner must never infer generation/state or
# stale-owner behavior from GI pixels.
PAGE_TELEMETRY_SCHEMA_VERSION = "C6-page-metadata-v1"
PAGE_TELEMETRY_ALIASES = {
    "pageGeneration": (
        "pageGeneration",
        "pageGenerationEpoch",
        "pageMetadataGeneration",
        "maxPageGeneration",
        "maxPageMetadataGeneration",
    ),
    "sceneGeneration": (
        "surfaceCacheSceneGeneration",
        "sceneGeneration",
        "surfaceCacheGeneration",
    ),
    "resetCount": (
        "surfaceCacheResetCount",
        "resetCount",
        "surfaceCacheResets",
    ),
    "feedbackHits": (
        "surfaceCacheFeedbackHits",
        "feedbackHits",
    ),
    "feedbackPages": (
        "surfaceCacheFeedbackPages",
        "feedbackPages",
    ),
    "feedbackDedup": (
        "surfaceCacheFeedbackDedup",
        "feedbackDedup",
    ),
    "feedbackStaleRejects": (
        "surfaceCacheFeedbackStaleRejects",
        "feedbackStaleRejects",
    ),
    "requestRaw": ("surfaceCacheRequestRaw", "requestRaw"),
    "requestCards": ("surfaceCacheRequestCards", "requestCards"),
    "requestReasonUnmapped": (
        "surfaceCacheRequestReasonUnmapped",
        "requestReasonUnmapped",
    ),
    "requestReasonStaleOwner": (
        "surfaceCacheRequestReasonStaleOwner",
        "requestReasonStaleOwner",
    ),
    "requestReasonMetadataInvalid": (
        "surfaceCacheRequestReasonMetadataInvalid",
        "requestReasonMetadataInvalid",
    ),
    "requestReasonVisibilityInvalid": (
        "surfaceCacheRequestReasonVisibilityInvalid",
        "requestReasonVisibilityInvalid",
    ),
    "requestDedup": (
        "surfaceCacheRequestDedup",
        "requestDedup",
        "requestDeduplications",
        "dedupedRequests",
        "schedRequestDedup",
    ),
    "requestStaleRejects": ("surfaceCacheRequestStaleRejects", "requestStaleRejects"),
    "requestCaptureCompleted": (
        "surfaceCacheRequestCaptureCompleted",
        "requestCaptureCompleted",
    ),
    "requestObservedFrame": ("requestObservedFrame",),
    "requestCaptureFrame": ("requestCaptureFrame",),
    "pageMetadataAllocated": ("pageMetadataAllocated",),
    "pageMetadataTouched": ("pageMetadataTouched",),
    "pageMetadataInvalid": ("pageMetadataInvalid",),
    "pageMetadataPending": (
        "pageMetadataPending",
        "metadataPending",
        "pendingPageMetadata",
    ),
    "pageMetadataReady": (
        "pageMetadataReady",
        "metadataReady",
        "readyPageMetadata",
    ),
    "generationMismatchRejects": (
        "generationMismatchRejects",
        "pageGenerationRejects",
        "generationRejects",
    ),
    "stateMismatchRejects": (
        "stateMismatchRejects",
        "pageStateRejects",
        "invalidPageStateRejects",
    ),
    "staleOwnerRejects": (
        "staleOwnerRejects",
        "stalePageOwnerRejects",
        "cardPageGenerationRejects",
        "pageOwnerGenerationRejects",
    ),
    "lastUsed": (
        "lastUsed",
        "lastUsedPages",
        "lastUsedPageCount",
        "pageLastUsedCount",
        "lastUsedFrame",
        "lastUsedPageFrame",
        "pageLastUsedFrame",
    ),
    "evictions": ("evictions", "pageEvictions", "evictedPages"),
    "pageClearCommands": (
        "pageClearCommands",
        "surfaceCachePageClearCommands",
    ),
    "pageClearTexels": (
        "pageClearTexels",
        "surfaceCachePageClearTexels",
    ),
    "staleTexelSentinel": (
        "staleTexelSentinel",
        "surfaceCacheStaleTexelSentinel",
    ),
    # Allocator identity is deliberately exported as host telemetry.  These
    # fields are optional in older builds, but when present they let the C6
    # fixture distinguish a page reuse from a mere aggregate counter change.
    "lastAllocatedPageID": ("lastAllocatedPageID",),
    "lastAllocatedGeneration": ("lastAllocatedGeneration",),
    "lastAllocatedFrame": ("lastAllocatedFrame",),
    "lastEvictedPageID": ("lastEvictedPageID",),
    "lastEvictedGeneration": ("lastEvictedGeneration",),
    "lastEvictedFrame": ("lastEvictedFrame",),
    "lastTouchedPageID": ("lastTouchedPageID",),
    "lastTouchedFrame": ("lastTouchedFrame",),
}
PAGE_TELEMETRY_REQUIRED_FIELDS = tuple(PAGE_TELEMETRY_ALIASES.keys())

# Frozen fields consumed by the independent C6 N -> N+1 checker.  Runtime
# surfaceCacheStats historically exposed the request counters with a
# ``surfaceCache`` prefix while the page counters were already canonical.  The
# runner keeps the original source keys in ``pageTelemetry`` and additionally
# writes these canonical names into each sample's ``stats`` object.  This is a
# lossless normalization step; it never derives values from GI outputs.
C6_FROZEN_FIELDS = (
    "pageMetadataPending",
    "pageMetadataReady",
    "requestRaw",
    "requestCards",
    "requestCaptureCompleted",
    "cacheLookupHits",
    "generationMismatchRejects",
    "stateMismatchRejects",
    "staleOwnerRejects",
)
C6_FROZEN_ALIASES = {
    name: PAGE_TELEMETRY_ALIASES[name]
    for name in C6_FROZEN_FIELDS
    if name in PAGE_TELEMETRY_ALIASES
}
C6_FROZEN_ALIASES["cacheLookupHits"] = (
    "cacheLookupHits",
)

# Card IDs are not exported by the current LumenGI stats binding.  Keep a
# separate optional alias set so that a future host can provide exact card/page
# pairs without making all older reports fail schema normalization.
CARD_ID_ALIASES = (
    "lastRequestedCardID",
    "lastCapturedCardID",
    "lastCardID",
    "cardID",
)

SCENE = os.environ.get("LUMEN_C6_SCENE", "test_scenes/cornell_box.pyscene")
OUT_JSON = os.environ.get(
    "LUMEN_C6_OUT", "artifacts/lumengi/C6/surfacecache-effect.json"
)
SAVE_ARRAYS = os.environ.get("LUMEN_C6_SAVE_ARRAYS", "0") == "1"
LOW_BUDGET = max(1, int(os.environ.get("LUMEN_C6_LOW_BUDGET", "1")))
INVALIDATE_AT = max(1, int(os.environ.get("LUMEN_C6_INVALIDATE_AT", "8")))
TINY_ATLAS = os.environ.get("LUMEN_C6_TINY_ATLAS", "0") == "1"
ATLAS_SIZE = max(16, int(os.environ.get("LUMEN_C6_ATLAS_SIZE", "64")))
PRESSURE_PHASES = os.environ.get("LUMEN_C6_PRESSURE_PHASES", "0") == "1"
SAMPLE_EVERY_FRAME = os.environ.get("LUMEN_C6_SAMPLE_EVERY_FRAME", "0") == "1"
TAIL_FRAMES = max(4, int(os.environ.get("LUMEN_C6_TAIL_FRAMES", "4")))
CACHE_CARD_GRID = os.environ.get("LUMEN_C6_CACHE_CARD_GRID", "0") == "1"
DIRTY_PRESSURE = os.environ.get("LUMEN_C6_DIRTY_PRESSURE", "0") == "1"
DIRTY_MODE = os.environ.get("LUMEN_C6_DIRTY_MODE", "auto").strip().lower()
if DIRTY_MODE not in ("auto", "material", "geometry"):
    DIRTY_MODE = "auto"
DIRTY_MUTATION_INTERVAL = max(1, int(os.environ.get("LUMEN_C6_DIRTY_INTERVAL", "8")))
DIRTY_MUTATION_BATCH = max(1, int(os.environ.get("LUMEN_C6_DIRTY_BATCH", "8")))
_default_tiny_min_frames = "144" if PRESSURE_PHASES else "72"
TINY_MIN_FRAMES = max(
    72,
    int(os.environ.get("LUMEN_C6_TINY_MIN_FRAMES", _default_tiny_min_frames)),
)

# Tiny-atlas pressure is a deterministic camera-partition schedule. Cornell's
# cardized scene contains multiple instances/faces (12+ cards in the current
# media asset); a single fixed view can touch only one or two pages. Each
# partition is a real camera view rendered for a block of frames, so request and
# eviction counters remain host evidence rather than synthetic values. With
# LUMEN_C6_PRESSURE_PHASES=1, the first 60 frames stay on partition 0; only
# after that residency window do partitions 1..11 get visited. This lets the
# allocator's UE-style minimum-residency window elapse before new demand.
TINY_PARTITION_COUNT = 12
TINY_PARTITION_FRAMES = max(1, (TINY_MIN_FRAMES + TINY_PARTITION_COUNT - 1) // TINY_PARTITION_COUNT)
PRESSURE_WARMUP_FRAMES = 60
PRESSURE_PHASE_FRAMES = max(
    1,
    (
        max(1, TINY_MIN_FRAMES - PRESSURE_WARMUP_FRAMES)
        + (TINY_PARTITION_COUNT - 2)
    )
    // (TINY_PARTITION_COUNT - 1),
)
TINY_CAMERA_PARTITIONS = (
    ((0.00, 0.28, 1.20), (0.00, 0.28, 0.00)),
    ((0.42, 0.30, 1.10), (0.00, 0.26, 0.00)),
    ((0.78, 0.32, 0.82), (0.08, 0.24, 0.00)),
    ((0.88, 0.32, 0.38), (0.12, 0.24, 0.00)),
    ((0.70, 0.30, 0.02), (0.10, 0.25, 0.00)),
    ((0.35, 0.31, -0.08), (0.00, 0.25, 0.00)),
    ((-0.35, 0.31, -0.08), (0.00, 0.25, 0.00)),
    ((-0.70, 0.30, 0.02), (-0.10, 0.25, 0.00)),
    ((-0.88, 0.32, 0.38), (-0.12, 0.24, 0.00)),
    ((-0.78, 0.32, 0.82), (-0.08, 0.24, 0.00)),
    ((-0.42, 0.30, 1.10), (0.00, 0.26, 0.00)),
    ((0.00, 0.62, 0.98), (0.00, 0.20, 0.00)),
)


def _parse_resolution(value):
    if not value:
        return DEFAULT_RESOLUTION
    try:
        width, height = (int(part) for part in value.lower().replace(" ", "").split("x", 1))
        if width > 0 and height > 0:
            return width, height
    except Exception:
        pass
    print("C6 WARNING invalid resolution; using", DEFAULT_RESOLUTION)
    return DEFAULT_RESOLUTION


def _parse_checkpoints(value):
    if not value:
        if TINY_ATLAS:
            return (1, 8, 32, 64, TINY_MIN_FRAMES, max(TINY_MIN_FRAMES, 96))
        return DEFAULT_CHECKPOINTS
    result = []
    for token in value.split(","):
        try:
            frame = int(token.strip())
            if frame > 0:
                result.append(frame)
        except Exception:
            print("C6 WARNING invalid checkpoint", repr(token))
    return tuple(sorted(set(result))) or DEFAULT_CHECKPOINTS


def _parse_cases(value):
    if not value:
        return DEFAULT_CASES
    allowed = set(DEFAULT_CASES)
    result = tuple(token.strip() for token in value.split(",") if token.strip() in allowed)
    return result or DEFAULT_CASES


RESOLUTION = _parse_resolution(os.environ.get("LUMEN_C6_RESOLUTION"))
CHECKPOINTS = _parse_checkpoints(os.environ.get("LUMEN_C6_CHECKPOINTS"))
CASES = _parse_cases(os.environ.get("LUMEN_C6_CASES"))

if FALCOR_AVAILABLE:
    FIXED_CAMERA_POSITION = float3(0, 0.28, 1.2)
    FIXED_CAMERA_TARGET = float3(0, 0.28, 0)
    FIXED_CAMERA_UP = float3(0, 1, 0)
else:
    # Keep --self-test dependency-free; Mogwai replaces these with float3.
    FIXED_CAMERA_POSITION = (0.0, 0.28, 1.2)
    FIXED_CAMERA_TARGET = (0.0, 0.28, 0.0)
    FIXED_CAMERA_UP = (0.0, 1.0, 0.0)
FIXED_CAMERA_FOCAL_LENGTH = 35.0

# Runtime-only dirty-pressure state.  The object contains only lightweight
# serializable records plus opaque Falcor handles while a case is executing.
# It is recreated after each scene reload so stale material/instance handles
# are never reused.
_DIRTY_PRESSURE_CONTEXT = None

OUTPUTS = ("diffuseGI", "resolvedDiffuseGI")
# Values match Falcor StandardMaterial.baseColor's public float4 setter.  The
# self-test never evaluates these objects because FALCOR_AVAILABLE is false;
# Mogwai supplies float4 at runtime.
MATERIAL_COLORS = (
    float4(0.725, 0.71, 0.68, 1.0) if FALCOR_AVAILABLE else (0.725, 0.71, 0.68, 1.0),
    float4(0.20, 0.30, 0.90, 1.0) if FALCOR_AVAILABLE else (0.20, 0.30, 0.90, 1.0),
)
STAT_KEYS = (
    "frameIndex",
    "useSurfaceCache",
    "useCacheLighting",
    "maxPagesPerFrame",
    "cards",
    "dirtyCards",
    "totalPages",
    "allocatedPages",
    "freePages",
    "coverage",
    "residentBytesMB",
    "allocations",
    "releases",
    "evictions",
    "invalidations",
    "schedCaptureCommands",
    "schedAllocations",
    "schedRecaptures",
    "schedAllocFailures",
    "schedStarvationFrames",
    "schedReleases",
    "schedLostPages",
    "schedCompletedCaptures",
    "pendingQueueDepth",
    "lastRequestedCards",
    "lastCaptureCommands",
    "lastNewPageAllocations",
    "lastRecaptureWithPage",
    "lastAllocFailures",
    "lastBudgetCappedCards",
    "lastInFlightCards",
    "lastPendingCards",
    "lastStarvationFrames",
    "lastReleasedPages",
    "lastLostPages",
    "lastTouchCalls",
    "cacheLightingActive",
    "cacheLightingPagesLit",
)


def _json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, np.generic):
        return _json_safe(value.item())
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    return str(value)


def _write_json(path, payload):
    path = os.path.abspath(path)
    out_dir = os.path.dirname(path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    temp_path = path + ".tmp"
    with open(temp_path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_safe(payload), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temp_path, path)


def _configure_for_case(label):
    """Return immutable LumenGI properties for one isolated case."""
    if label == "lookup_off":
        properties = {
            "useSurfaceCache": False,
            "useCacheLighting": False,
            "captureMaxPagesPerFrame": 64,
        }
    elif label == "low_budget":
        properties = {
            "useSurfaceCache": True,
            "useCacheLighting": True,
            "captureMaxPagesPerFrame": LOW_BUDGET,
        }
    else:
        # lookup_on and invalidate share the production cache configuration.
        properties = {
            "useSurfaceCache": True,
            "useCacheLighting": True,
            "captureMaxPagesPerFrame": 64,
        }
    if TINY_ATLAS and label != "lookup_off":
        properties["surfaceCacheAtlasSize"] = ATLAS_SIZE
        properties["captureMaxPagesPerFrame"] = min(
            int(properties.get("captureMaxPagesPerFrame", LOW_BUDGET)), LOW_BUDGET
        )
    if label != "lookup_off":
        properties["useCacheCardGrid"] = CACHE_CARD_GRID
    return properties


def _gbuffer_edges(graph):
    for source, destination in (
        ("vbuffer", "vbuffer"),
        ("linearZ", "linearZ"),
        ("mvec", "mvec"),
        ("mvecW", "mvecW"),
        ("normWRoughnessMaterialID", "normWRoughnessMaterialID"),
        ("viewW", "viewW"),
        ("diffuseOpacity", "diffuseOpacity"),
        ("emissive", "emissive"),
    ):
        graph.addEdge("GBufferRT." + source, "LumenGI." + destination)


def _make_graph(label, properties):
    graph = RenderGraph("LumenGIC6_%s" % label)
    graph.addPass(
        createPass(
            "GBufferRT",
            {"samplePattern": "Center", "sampleCount": 1, "useAlphaTest": True},
        ),
        "GBufferRT",
    )
    # Probes are enabled because this is the consumer of the Surface Cache
    # lookup define.  Filters stay off so this gate isolates cache lookup from
    # temporal/spatial convergence.
    lumen_properties = dict(properties)
    lumen_properties.update(
        {
            "enabled": True,
            "useScreenTrace": True,
            "useScreenProbes": True,
            "useTemporalFilter": False,
            "useSpatialFilter": False,
        }
    )
    graph.addPass(createPass("LumenGI", lumen_properties), "LumenGI")
    _gbuffer_edges(graph)
    for output_name in OUTPUTS + ("confidence",):
        graph.markOutput("LumenGI." + output_name)
    return graph


def _setup_scene():
    m.loadScene(SCENE)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0
    camera = m.scene.camera
    camera.position = FIXED_CAMERA_POSITION
    camera.target = FIXED_CAMERA_TARGET
    camera.up = FIXED_CAMERA_UP
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH
    _prepare_dirty_pressure()


def _iter_binding_container(container):
    """Best-effort iteration over Falcor's vector/map Python bindings."""
    if container is None:
        return []
    try:
        if hasattr(container, "values"):
            return list(container.values())
        return list(container)
    except Exception:
        try:
            count = len(container)
            return [container[index] for index in range(count)]
        except Exception:
            return []


def _material_candidates():
    """Return material handles whose public setter can drive MaterialsChanged.

    Merely finding a name is not enough evidence: the actual setter is attempted
    by _apply_dirty_pressure(), and a failed assignment records BLOCKED.  The
    common StandardMaterial baseColor path is preferred because it is a direct
    Material::UpdateFlags::DataChanged input to LumenCardScene.
    """
    candidates = []
    try:
        materials = getattr(m.scene, "materials")
    except Exception:
        return candidates
    for index, material in enumerate(_iter_binding_container(materials)):
        for property_name in ("baseColor", "emissiveFactor"):
            try:
                getattr(material, property_name)
            except Exception:
                continue
            candidates.append(
                {
                    "kind": "material",
                    "index": int(index),
                    "name": str(getattr(material, "name", "material_%d" % index)),
                    "property": property_name,
                    "handle": material,
                }
            )
            break
    return candidates


def _geometry_candidates():
    """Return concrete mutable instance transforms, if the binding exposes them."""
    candidates = []
    for container_name in ("instances", "geometryInstances", "sceneGraph"):
        try:
            container = getattr(m.scene, container_name)
        except Exception:
            continue
        for index, instance in enumerate(_iter_binding_container(container)):
            try:
                transform = getattr(instance, "transform")
            except Exception:
                continue
            # Do not treat an opaque float4x4 proxy as mutable.  A concrete
            # sequence can be copied and reassigned through the public setter.
            if not isinstance(transform, (list, tuple)) or len(transform) < 16:
                continue
            candidates.append(
                {
                    "kind": "geometry",
                    "index": int(index),
                    "name": str(getattr(instance, "name", "instance_%d" % index)),
                    "property": "transform",
                    "handle": instance,
                    "baseline": list(transform),
                }
            )
    return candidates


def _prepare_dirty_pressure():
    """Discover real Falcor mutation APIs for the current scene.

    This function never synthesizes card/page IDs.  If neither material setters
    nor concrete instance transforms are exposed, the runtime report explicitly
    remains BLOCKED and the normal camera schedule is not relabeled as dirty
    pressure.
    """
    global _DIRTY_PRESSURE_CONTEXT
    context = {
        "enabled": DIRTY_PRESSURE,
        "mode": DIRTY_MODE,
        "interval": DIRTY_MUTATION_INTERVAL,
        "batch": DIRTY_MUTATION_BATCH,
        "apiStatus": "NOT_APPLICABLE" if not DIRTY_PRESSURE else "PENDING",
        "apiReason": None,
        "candidates": [],
        "cursor": 0,
        "toggle": 0,
        "events": [],
    }
    if not DIRTY_PRESSURE:
        _DIRTY_PRESSURE_CONTEXT = context
        return context

    materials = _material_candidates()
    geometry = _geometry_candidates()
    if DIRTY_MODE in ("auto", "material") and materials:
        context["candidates"] = materials
        context["selectedKind"] = "material"
        context["apiStatus"] = "AVAILABLE"
        context["apiReason"] = "m.scene.materials + Material.%s" % materials[0]["property"]
    elif DIRTY_MODE in ("auto", "geometry") and geometry:
        context["candidates"] = geometry
        context["selectedKind"] = "geometry"
        context["apiStatus"] = "AVAILABLE"
        context["apiReason"] = "m.scene instance transform setter"
    else:
        context["apiStatus"] = "BLOCKED"
        if DIRTY_MODE == "material":
            context["apiReason"] = "Falcor binding exposes no mutable material baseColor/emissiveFactor"
        elif DIRTY_MODE == "geometry":
            context["apiReason"] = "Falcor binding exposes no concrete mutable instance transform"
        else:
            context["apiReason"] = "Falcor binding exposes neither mutable material nor geometry instance API"
    _DIRTY_PRESSURE_CONTEXT = context
    return context


def _apply_dirty_pressure(frame):
    """Apply one real mutation batch before rendering ``frame``."""
    context = _DIRTY_PRESSURE_CONTEXT
    if not context or not context.get("enabled"):
        return None
    if context.get("apiStatus") != "AVAILABLE":
        if frame % DIRTY_MUTATION_INTERVAL == 0:
            event = {
                "frame": int(frame),
                "status": "BLOCKED",
                "kind": context.get("selectedKind"),
                "reason": context.get("apiReason"),
                "expectedNextFrame": int(frame) + 1,
            }
            context["events"].append(event)
            return event
        return None
    if int(frame) % DIRTY_MUTATION_INTERVAL != 0:
        return None

    candidates = context.get("candidates", [])
    if not candidates:
        context["apiStatus"] = "BLOCKED"
        return _apply_dirty_pressure(frame)

    attempted = []
    applied = []
    start = int(context.get("cursor", 0))
    batch = min(DIRTY_MUTATION_BATCH, len(candidates))
    for offset in range(batch):
        candidate_index = (start + offset) % len(candidates)
        candidate = candidates[candidate_index]
        handle = candidate.get("handle")
        try:
            if candidate["kind"] == "material":
                property_name = candidate["property"]
                attempted.append(candidate_index)
                if property_name == "baseColor":
                    value = MATERIAL_COLORS[(int(context["toggle"]) + offset) % len(MATERIAL_COLORS)]
                    setattr(handle, property_name, value)
                    value_repr = [float(value.x), float(value.y), float(value.z), float(value.w)]
                else:
                    old = float(getattr(handle, property_name))
                    new = old * 1.5 if old > 0.0 else 0.5
                    setattr(handle, property_name, new)
                    value_repr = new
            else:
                attempted.append(candidate_index)
                updated = list(candidate["baseline"])
                # Falcor float4x4 bindings use the final column for translation.
                updated[12] = float(updated[12]) + (0.05 if context["toggle"] % 2 == 0 else -0.05)
                setattr(handle, candidate["property"], updated)
                value_repr = updated[12]
            applied.append(
                {
                    "index": candidate.get("index"),
                    "name": candidate.get("name"),
                    "property": candidate.get("property"),
                    "value": value_repr,
                }
            )
        except Exception as exc:
            candidate["lastError"] = str(exc)

    context["cursor"] = (start + batch) % len(candidates)
    context["toggle"] = int(context.get("toggle", 0)) + 1
    if not applied:
        context["apiStatus"] = "BLOCKED"
        context["apiReason"] = "Falcor setter rejected every dirty-pressure mutation"
    event = {
        "frame": int(frame),
        "status": "APPLIED" if len(applied) == len(attempted) else ("PARTIAL" if applied else "BLOCKED"),
        "kind": context.get("selectedKind"),
        "attemptedCount": len(attempted),
        "appliedCount": len(applied),
        "targets": applied,
        "expectedNextFrame": int(frame) + 1,
        "reason": context.get("apiReason"),
        "cardIdentityStatus": "BLOCKED",
        "cardIdentityReason": "host stats do not expose card IDs for material/geometry targets",
    }
    context["events"].append(event)
    return event


def _raw_page_value(raw, field_name):
    aliases = PAGE_TELEMETRY_ALIASES.get(field_name, ())
    value, _source, _reason = _numeric_stat(raw if isinstance(raw, dict) else {}, aliases)
    return value


def _raw_optional_value(raw, aliases):
    value, source, reason = _numeric_stat(raw if isinstance(raw, dict) else {}, aliases)
    return value, source, reason


def _page_identity_from_raw(raw):
    """Extract real allocator identity fields, filtering uint32 invalid sentinels."""
    result = {}
    for field_name in (
        "lastAllocatedPageID",
        "lastAllocatedGeneration",
        "lastAllocatedFrame",
        "lastEvictedPageID",
        "lastEvictedGeneration",
        "lastEvictedFrame",
        "lastTouchedPageID",
        "lastTouchedFrame",
    ):
        result[field_name] = _raw_page_value(raw, field_name)
    for field_name in ("lastAllocatedPageID", "lastEvictedPageID", "lastTouchedPageID"):
        value = result.get(field_name)
        if value is None or value >= 4294967295.0:
            result[field_name] = None
        elif value >= 0.0:
            result[field_name] = int(value)
    for field_name in (
        "lastAllocatedGeneration",
        "lastAllocatedFrame",
        "lastEvictedGeneration",
        "lastEvictedFrame",
        "lastTouchedFrame",
    ):
        value = result.get(field_name)
        if value is not None and value >= 0.0:
            result[field_name] = int(value)
    return result


def _collect_surface_cache_event_identity(samples):
    """Collect explicit card/page identities from the host event ledger.

    ``surfaceCacheStats`` intentionally remains a numeric aggregate, while the
    ``surfaceCacheEvents`` binding carries the authoritative per-request
    sequence/card/page/generation tuple.  Samples expose a cumulative ring, so
    de-duplicate by sequence before evaluating tiny-atlas identity evidence.
    Missing or malformed events remain BLOCKED; no identity is reconstructed
    from aggregate counters or images.
    """
    by_sequence = {}
    for sample in samples or []:
        for event in (sample.get("surfaceCacheEvents") or []) if isinstance(sample, dict) else []:
            if not isinstance(event, dict):
                continue
            sequence = event.get("sequence")
            try:
                sequence = int(float(sequence))
            except (TypeError, ValueError):
                continue
            if sequence <= 0:
                continue
            by_sequence[sequence] = event

    records = []
    card_ids = set()
    page_ids = set()
    generations = set()
    pairs = set()
    for sequence in sorted(by_sequence):
        event = by_sequence[sequence]
        def _uint(name):
            try:
                value = int(float(event.get(name)))
            except (TypeError, ValueError):
                return None
            return value if 0 <= value < 0xFFFFFFFF else None

        card_id = _uint("cardID")
        page_id = _uint("pageID")
        generation = _uint("generation")
        record = {
            "sequence": sequence,
            "sceneGeneration": event.get("sceneGeneration"),
            "cardID": card_id,
            "pageID": page_id,
            "generation": generation,
            "requestFrame": event.get("requestFrame"),
            "captureFrame": event.get("captureFrame"),
            "readyFrame": event.get("readyFrame"),
            "firstHitFrame": event.get("firstHitFrame"),
            "state": event.get("state"),
            "lookupHits": event.get("lookupHits"),
        }
        records.append(record)
        if card_id is not None:
            card_ids.add(card_id)
        if page_id is not None and page_id > 0:
            page_ids.add(page_id)
        if generation is not None and generation > 0:
            generations.add(generation)
        if card_id is not None and page_id is not None and page_id > 0:
            pairs.add((card_id, page_id, generation))

    status = "PASS_BOUNDED" if card_ids and page_ids and pairs else "BLOCKED"
    return {
        "status": status,
        "records": records,
        "recordCount": len(records),
        "distinctCardIDs": sorted(card_ids),
        "distinctPageIDs": sorted(page_ids),
        "distinctGenerations": sorted(generations),
        "distinctCardPagePairs": [list(pair) for pair in sorted(pairs)],
        "hostTelemetryOnly": True,
        "reason": None if status == "PASS_BOUNDED" else "surfaceCacheEvents missing usable card/page identity",
    }


def _dirty_pressure_annotation(frame, stats, page_telemetry):
    context = _DIRTY_PRESSURE_CONTEXT or {
        "enabled": False,
        "mode": DIRTY_MODE,
        "apiStatus": "NOT_APPLICABLE",
        "apiReason": None,
        "events": [],
    }
    events = context.get("events", [])
    event = next((item for item in events if item.get("frame") == int(frame)), None)
    previous = next((item for item in events if item.get("frame") == int(frame) - 1), None)
    evidence = None
    if previous is not None:
        observed_frame = _raw_page_value(stats, "lastTouchedFrame")
        frame_index = stats.get("frameIndex") if isinstance(stats, dict) else None
        expected_frame = int(previous.get("expectedNextFrame", int(frame)))
        metadata_pending = _raw_page_value(stats, "pageMetadataPending")
        metadata_ready = _raw_page_value(stats, "pageMetadataReady")
        evidence = {
            "status": "PASS_BOUNDED" if frame_index is not None and metadata_pending is not None and metadata_ready is not None else "BLOCKED",
            "mutationFrame": previous.get("frame"),
            "expectedFrame": expected_frame,
            "observedFrame": int(frame),
            "frameIndex": frame_index,
            "frameDelta": int(frame) - int(previous.get("frame", frame)),
            "pageMetadataPending": metadata_pending,
            "pageMetadataReady": metadata_ready,
            "lastTouchedFrame": observed_frame,
            "pageIdentity": _page_identity_from_raw(stats),
            "hostTelemetryOnly": True,
        }
    return {
        "enabled": bool(context.get("enabled")),
        "mode": context.get("mode"),
        "apiStatus": context.get("apiStatus"),
        "apiReason": context.get("apiReason"),
        "mutation": event,
        "nextFrameEvidence": evidence,
    }


def _update_pressure_camera(frame):
    """Move through deterministic card-facing camera partitions.

    The normal C6 smoke keeps a deterministic fixed camera. Tiny-atlas mode is
    explicitly a page-pressure test, so twelve explicit views are part of that
    contract; without them a Cornell view can legitimately fit in one page and
    produce no eviction even after a long run. The return value is a schedule
    index only, never a card/page identity or synthetic statistic.
    """
    if not TINY_ATLAS:
        return None
    partition = _camera_partition_for_frame(frame)
    position, target = TINY_CAMERA_PARTITIONS[partition]
    camera = m.scene.camera
    camera.position = float3(*position)
    camera.target = float3(*target)
    camera.up = FIXED_CAMERA_UP
    camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH
    return partition


def _camera_partition_for_frame(frame):
    """Return the deterministic tiny-mode partition selected for ``frame``.

    The optional phase schedule intentionally keeps partition 0 resident for
    60 frames, then walks partitions 1..11.  It does not synthesize request or
    eviction counters; those remain read-only host telemetry.
    """
    if PRESSURE_PHASES:
        if int(frame) <= PRESSURE_WARMUP_FRAMES:
            return 0
        return min(
            TINY_PARTITION_COUNT - 1,
            1 + (int(frame) - PRESSURE_WARMUP_FRAMES - 1) // PRESSURE_PHASE_FRAMES,
        )
    return min(
        TINY_PARTITION_COUNT - 1,
        max(0, (int(frame) - 1) // TINY_PARTITION_FRAMES),
    )


def _read_stats():
    try:
        raw = dict(m.activeGraph.getPass("LumenGI").surfaceCacheStats)
    except Exception as exc:
        return None, "surfaceCacheStats unavailable: %s" % str(exc)
    stats = {}
    for key in STAT_KEYS:
        if key in raw:
            try:
                stats[key] = float(raw[key])
            except Exception:
                stats[key] = None
    # Keep any newly exposed telemetry in the JSON without making the gate
    # depend on a future key list update.
    for key, value in raw.items():
        if key not in stats:
            try:
                stats[str(key)] = float(value)
            except Exception:
                stats[str(key)] = None
    return _flatten_c6_stats(stats), None


def _read_surface_cache_events():
    """Read card/page lifecycle events without reconstructing identity from counters."""
    try:
        raw = m.activeGraph.getPass("LumenGI").surfaceCacheEvents
    except Exception as exc:
        return None, "surfaceCacheEvents unavailable: %s" % str(exc)
    if raw is None:
        return None, "surfaceCacheEvents returned None"
    events = []
    try:
        for item in raw:
            if isinstance(item, dict):
                events.append(_json_safe(dict(item)))
            else:
                events.append(_json_safe(item))
    except Exception as exc:
        return None, "surfaceCacheEvents invalid: %s" % str(exc)
    return events, None


def _flatten_c6_stats(stats):
    """Add canonical frozen C6 fields while retaining every source key.

    This helper is deliberately pure host-telemetry normalization.  Keeping it
    separate from ``_read_stats`` makes the schema migration dependency-free to
    self-test outside Mogwai.
    """
    if not isinstance(stats, dict):
        return stats
    for field_name, aliases in C6_FROZEN_ALIASES.items():
        if field_name in stats and stats[field_name] is not None:
            continue
        value, _source, _reason = _numeric_stat(stats, aliases)
        if value is not None:
            stats[field_name] = value
    return stats


def _numeric_stat(raw, aliases):
    """Return (value, source_key, reason) without guessing missing telemetry."""
    for key in aliases:
        if key not in raw:
            continue
        try:
            value = float(raw[key])
        except Exception:
            return None, key, "non-numeric"
        if not math.isfinite(value):
            return None, key, "non-finite"
        return value, key, None
    return None, None, "missing"


def _collect_page_telemetry(raw):
    """Normalize the C6 host page-lifecycle contract into the report schema.

    This function intentionally does not inspect GI arrays.  If the host does not
    expose every required generation/state/owner/request/LRU/eviction field, the
    result is BLOCKED rather than a guessed PASS/FAIL.
    """
    raw = raw if isinstance(raw, dict) else {}
    fields = {}
    missing = []
    invalid = []
    for name, aliases in PAGE_TELEMETRY_ALIASES.items():
        value, source_key, reason = _numeric_stat(raw, aliases)
        fields[name] = {
            "value": value,
            "sourceKey": source_key,
        }
        if reason == "missing":
            missing.append(name)
        elif reason:
            invalid.append({"field": name, "sourceKey": source_key, "reason": reason})

    values = {
        name: item["value"]
        for name, item in fields.items()
        if item["value"] is not None
    }
    nonnegative_fields = (
        "pageGeneration",
        "sceneGeneration",
        "resetCount",
        "feedbackHits",
        "feedbackPages",
        "feedbackDedup",
        "feedbackStaleRejects",
        "requestRaw",
        "requestCards",
        "requestReasonUnmapped",
        "requestReasonStaleOwner",
        "requestReasonMetadataInvalid",
        "requestReasonVisibilityInvalid",
        "requestDedup",
        "requestStaleRejects",
        "requestCaptureCompleted",
        "pageMetadataAllocated",
        "pageMetadataTouched",
        "pageMetadataInvalid",
        "pageMetadataPending",
        "pageMetadataReady",
        "generationMismatchRejects",
        "stateMismatchRejects",
        "staleOwnerRejects",
        "requestDedup",
        "lastUsed",
        "evictions",
        "staleTexelSentinel",
    )
    negative = [name for name in nonnegative_fields if name in values and values[name] < 0.0]
    if negative:
        invalid.extend({"field": name, "sourceKey": fields[name]["sourceKey"], "reason": "negative"} for name in negative)

    invariants = {
        "stateCountsConsistent": None,
        "stateCountsNonnegative": not negative,
        "generationNonzeroWhenResident": None,
    }
    total_pages = raw.get("totalPages")
    if all(name in values for name in ("pageMetadataAllocated", "pageMetadataTouched", "pageMetadataInvalid")):
        state_sum = (
            values["pageMetadataAllocated"]
            + values["pageMetadataTouched"]
            + values["pageMetadataInvalid"]
        )
        try:
            total_pages_value = float(total_pages)
            invariants["stateCountsConsistent"] = (
                math.isfinite(total_pages_value)
                and abs(state_sum - total_pages_value) <= 1e-6
            )
        except Exception:
            invariants["stateCountsConsistent"] = False
    if "pageGeneration" in values:
        resident_count = values.get("pageMetadataAllocated", 0.0) + values.get("pageMetadataTouched", 0.0)
        invariants["generationNonzeroWhenResident"] = resident_count == 0.0 or values["pageGeneration"] > 0.0

    if invariants["stateCountsConsistent"] is False:
        invalid.append({"field": "pageMetadataStateCounts", "sourceKey": None, "reason": "totalPages mismatch"})
    if invariants["generationNonzeroWhenResident"] is False:
        invalid.append({"field": "pageGeneration", "sourceKey": fields["pageGeneration"]["sourceKey"], "reason": "resident pages with zero generation"})

    # C6.1 feedback activity is host telemetry only.  An explicit cache-hit
    # counter is preferred when available; otherwise feedbackHits is itself the
    # activity signal.  Never infer a hit from GI pixels or page counts.
    cache_hit_value, cache_hit_source, cache_hit_reason = _numeric_stat(
        raw,
        (
            "surfaceCacheHits",
            "surfaceCacheLookupHits",
            "cacheHits",
            "cacheLookupHits",
            "cacheLightingHits",
        ),
    )
    if cache_hit_reason == "non-numeric" or cache_hit_reason == "non-finite":
        invalid.append({"field": "cacheHits", "sourceKey": cache_hit_source, "reason": cache_hit_reason})
    if cache_hit_value is not None and cache_hit_value < 0.0:
        invalid.append({"field": "cacheHits", "sourceKey": cache_hit_source, "reason": "negative"})
    if cache_hit_value is None:
        cache_hit_value = values.get("feedbackHits")
        cache_hit_source = fields["feedbackHits"]["sourceKey"]
    feedback_hits = values.get("feedbackHits")
    feedback_pages = values.get("feedbackPages")
    feedback_active = cache_hit_value is not None and cache_hit_value > 0.0
    feedback_activity_consistent = (
        not feedback_active
        or (feedback_hits is not None and feedback_pages is not None and (feedback_hits > 0.0 or feedback_pages > 0.0))
    )
    if not feedback_activity_consistent:
        invalid.append(
            {
                "field": "feedbackActivity",
                "sourceKey": cache_hit_source,
                "reason": "cache hit without feedback hit/page evidence",
            }
        )

    request_raw = values.get("requestRaw")
    request_cards = values.get("requestCards")
    request_completed = values.get("requestCaptureCompleted")
    request_activity_consistent = (
        request_raw is None
        or request_raw <= 0.0
        or (request_cards is not None and request_completed is not None and request_completed <= request_cards)
    )
    if not request_activity_consistent:
        invalid.append(
            {
                "field": "requestActivity",
                "sourceKey": fields["requestRaw"]["sourceKey"],
                "reason": "capture completion exceeds queued request cards",
            }
        )

    status = "PASS" if not missing and not invalid else "BLOCKED"
    return {
        "schemaVersion": PAGE_TELEMETRY_SCHEMA_VERSION,
        "status": status,
        "fields": fields,
        "missing": missing,
        "invalid": invalid,
        "invariants": invariants,
        "feedbackActivity": {
            "cacheHits": cache_hit_value,
            "cacheHitsSourceKey": cache_hit_source,
            "active": feedback_active,
            "feedbackHits": feedback_hits,
            "feedbackPages": feedback_pages,
            "consistent": feedback_activity_consistent,
        },
        "requestActivity": {
            "requestRaw": request_raw,
            "requestCards": request_cards,
            "requestCaptureCompleted": request_completed,
            "consistent": request_activity_consistent,
        },
        "hostTelemetryOnly": True,
    }


def _page_telemetry_for_case(label, raw):
    """Apply the lifecycle gate only to cases that actually enable the cache."""
    if label == "lookup_off":
        return {
            "schemaVersion": PAGE_TELEMETRY_SCHEMA_VERSION,
            "status": "NOT_APPLICABLE",
            "required": False,
            "reason": "useSurfaceCache and useCacheLighting are disabled",
            "fields": {},
            "missing": [],
            "invalid": [],
            "invariants": {},
            "hostTelemetryOnly": True,
        }
    telemetry = _collect_page_telemetry(raw)
    telemetry["required"] = True
    return telemetry


def _page_telemetry_value(sample, field_name):
    telemetry = sample.get("pageTelemetry", {}) if isinstance(sample, dict) else {}
    fields = telemetry.get("fields", {}) if isinstance(telemetry, dict) else {}
    entry = fields.get(field_name, {}) if isinstance(fields, dict) else {}
    value = entry.get("value") if isinstance(entry, dict) else None
    return value if isinstance(value, (int, float)) and math.isfinite(float(value)) else None


def _page_telemetry_source(sample, field_name):
    telemetry = sample.get("pageTelemetry", {}) if isinstance(sample, dict) else {}
    fields = telemetry.get("fields", {}) if isinstance(telemetry, dict) else {}
    entry = fields.get(field_name, {}) if isinstance(fields, dict) else {}
    source = entry.get("sourceKey") if isinstance(entry, dict) else None
    return source if isinstance(source, str) and source else None


def _generation_transition_evidence(before, after):
    """Return host-telemetry evidence for an invalidate/reload transition."""
    evidence = []
    for field_name in ("pageGeneration", "sceneGeneration", "resetCount"):
        before_value = _page_telemetry_value(before, field_name)
        after_value = _page_telemetry_value(after, field_name)
        if before_value is not None and after_value is not None and before_value != after_value:
            evidence.append(
                {
                    "field": field_name,
                    "before": before_value,
                    "after": after_value,
                    "sourceBefore": _page_telemetry_source(before, field_name),
                    "sourceAfter": _page_telemetry_source(after, field_name),
                }
            )
    return evidence


def _summarize_array(array):
    arr = np.asarray(array, dtype=np.float32)
    rgb = arr[..., :3] if arr.ndim >= 3 and arr.shape[-1] >= 3 else arr
    finite = bool(np.isfinite(rgb).all())
    if rgb.size:
        min_value = float(np.min(rgb))
        max_value = float(np.max(rgb))
        mean_value = float(np.mean(rgb))
        nonnegative = bool(min_value >= 0.0)
        nonzero_fraction = float(np.count_nonzero(rgb > 0.0)) / float(rgb.size)
    else:
        min_value = max_value = mean_value = 0.0
        nonnegative = True
        nonzero_fraction = 0.0
    return {
        "shape": list(arr.shape),
        "min": min_value,
        "max": max_value,
        "mean": mean_value,
        "nonzeroFraction": nonzero_fraction,
        "finite": finite,
        "nonnegative": nonnegative,
    }


def _read_outputs():
    arrays = {}
    summaries = {}
    errors = []
    for output_name in OUTPUTS:
        try:
            array = np.asarray(m.activeGraph.get_output("LumenGI." + output_name).to_numpy(), dtype=np.float32)
            arrays[output_name] = array
            summaries[output_name] = _summarize_array(array)
        except Exception as exc:
            errors.append("%s: %s" % (output_name, str(exc)))
    return arrays, summaries, errors


def _sample(label, frame, phase, camera_partition=None):
    arrays, outputs, output_errors = _read_outputs()
    stats, stats_error = _read_stats()
    lifecycle_events, events_error = _read_surface_cache_events()
    page_telemetry = _page_telemetry_for_case(label, stats)
    dirty_pressure = _dirty_pressure_annotation(frame, stats, page_telemetry)
    frozen_fields = {
        field_name: stats.get(field_name)
        for field_name in C6_FROZEN_FIELDS
        if isinstance(stats, dict) and stats.get(field_name) is not None
    }
    sample = {
        "label": label,
        "phase": phase,
        "frame": int(frame),
        "cameraPartition": camera_partition,
        "outputs": outputs,
        "stats": stats,
        "surfaceCacheEvents": lifecycle_events,
        "surfaceCacheEventsError": events_error,
        # This is a sample-local, host-only view of the frozen gate fields.
        # Values are copied from canonical stats, never inferred from images.
        "c6Telemetry": {
            "schemaVersion": "C6-next-frame-validity-v1",
            "fields": frozen_fields,
            "source": "LumenGI.surfaceCacheStats",
            "hostTelemetryOnly": True,
        },
        "pageTelemetry": page_telemetry,
        "dirtyPressure": dirty_pressure,
        "outputErrors": output_errors,
        "statsError": stats_error,
        "_arrays": arrays,
    }
    print(
        "C6_SAMPLE",
        label,
        phase,
        "frame",
        frame,
        "diffuseMean",
        outputs.get("diffuseGI", {}).get("mean"),
        "resolvedMean",
        outputs.get("resolvedDiffuseGI", {}).get("mean"),
        "allocatedPages",
        stats.get("allocatedPages") if stats else None,
        "pageTelemetry",
        page_telemetry.get("status"),
        "dirtyPressure",
        dirty_pressure.get("mutation", {}).get("status") if dirty_pressure.get("mutation") else dirty_pressure.get("apiStatus"),
    )
    return sample


def _sample_schedule(label, phase="warmup", checkpoints=None):
    checkpoints = CHECKPOINTS if checkpoints is None else tuple(checkpoints)
    samples = []
    max_frame = max(checkpoints)
    # A strict request at the last configured checkpoint needs the full host sequence:
    # capture publication, page-ready promotion, and one or more delayed feedback readbacks.
    # Keep the tail explicit even for sparse sampling; otherwise the final request is silently
    # left in state=2 and the validator would have to infer completion beyond the captured series.
    schedule_end = max_frame + TAIL_FRAMES
    checkpoint_set = set(range(1, schedule_end + 1)) if SAMPLE_EVERY_FRAME else set(checkpoints)
    checkpoint_set.update(range(max_frame + 1, schedule_end + 1))
    # Add an explicit sample immediately after each scheduled mutation.  This
    # is the next-frame publication evidence; it is not inferred from a sparse
    # checkpoint or from GI pixels.
    if DIRTY_PRESSURE:
        for mutation_frame in range(DIRTY_MUTATION_INTERVAL, max_frame + 1, DIRTY_MUTATION_INTERVAL):
            if mutation_frame + 1 <= schedule_end:
                checkpoint_set.add(mutation_frame + 1)
    for frame in range(1, schedule_end + 1):
        m.clock.frame = frame
        if frame <= max_frame:
            camera_partition = _update_pressure_camera(frame)
            _apply_dirty_pressure(frame)
        else:
            # The explicit tail is a lifecycle-drain window.  Do not introduce
            # a new camera partition or dirty mutation after the last requested
            # checkpoint: doing so creates a fresh request in the tail and makes
            # a fixed N+1/N+2/N+3/N+4 budget non-terminating.  Keep the last
            # camera/scene state resident while host capture, ready promotion,
            # and delayed feedback readbacks finish.  The partition value is
            # annotation-only and does not mutate Falcor state.
            camera_partition = (
                _camera_partition_for_frame(max_frame) if TINY_ATLAS else None
            )
        m.renderFrame()
        if frame in checkpoint_set:
            sample = _sample(label, frame, phase, camera_partition)
            if frame > max_frame:
                sample["tailSample"] = True
            samples.append(sample)
    return samples


def _save_arrays(case_label, sample):
    if not SAVE_ARRAYS:
        return {}
    case_dir = os.path.splitext(os.path.abspath(OUT_JSON))[0] + "_arrays"
    os.makedirs(case_dir, exist_ok=True)
    paths = {}
    for output_name, array in sample.get("_arrays", {}).items():
        path = os.path.join(case_dir, "%s_%s_f%03d.npy" % (case_label, output_name, sample["frame"]))
        np.save(path, array)
        paths[output_name] = path
    return paths


def _strip_internal(sample):
    result = dict(sample)
    result.pop("_arrays", None)
    return result


def _collect_dirty_pressure_report(samples):
    """Summarize real mutations, allocator identities, and next-frame samples."""
    samples = list(samples or [])
    if not DIRTY_PRESSURE:
        return {
            "enabled": False,
            "status": "NOT_APPLICABLE",
            "mode": DIRTY_MODE,
            "apiStatus": "NOT_APPLICABLE",
            "mutations": [],
            "nextFrameEvidence": [],
            "pageIdentity": {"status": "NOT_APPLICABLE", "records": [], "distinctPageIDs": []},
            "distinctCardPageEvidence": {"status": "NOT_APPLICABLE", "reason": "dirty pressure disabled"},
        }

    annotations = [sample.get("dirtyPressure", {}) for sample in samples if isinstance(sample, dict)]
    mutations = [item.get("mutation") for item in annotations if item.get("mutation") is not None]
    evidence = [item.get("nextFrameEvidence") for item in annotations if item.get("nextFrameEvidence") is not None]
    page_records = []
    card_page_pairs = set()
    card_ids = set()
    page_ids = set()
    generations = set()
    for sample in samples:
        raw = sample.get("stats") if isinstance(sample, dict) else None
        if not isinstance(raw, dict):
            continue
        identity = _page_identity_from_raw(raw)
        if any(value is not None for value in identity.values()):
            record = {"frame": sample.get("frame"), **identity}
            card_id, card_source, _reason = _raw_optional_value(raw, CARD_ID_ALIASES)
            if card_id is not None and 0.0 <= card_id < 4294967295.0:
                card_id = int(card_id)
                record["cardID"] = card_id
                record["cardIDSourceKey"] = card_source
                card_ids.add(card_id)
            page_id = identity.get("lastAllocatedPageID")
            if page_id is not None:
                page_ids.add(page_id)
                generation = identity.get("lastAllocatedGeneration")
                if generation is not None:
                    generations.add(generation)
                if "cardID" in record:
                    card_page_pairs.add((record["cardID"], page_id, generation))
            page_records.append(record)

    next_frame_pass = [item for item in evidence if item.get("status") == "PASS_BOUNDED"]
    next_frame_blocked = [item for item in evidence if item.get("status") != "PASS_BOUNDED"]
    applied = [item for item in mutations if item.get("status") in ("APPLIED", "PARTIAL") and item.get("appliedCount", 0) > 0]
    blocked_mutations = [item for item in mutations if item.get("status") == "BLOCKED"]
    if not applied:
        api_status = "BLOCKED"
        reason = (
            blocked_mutations[0].get("reason")
            if blocked_mutations
            else "no real material/geometry mutation was applied in the sampled frame window"
        )
    else:
        api_status = "AVAILABLE"
        reason = None

    if applied and evidence and not next_frame_blocked and next_frame_pass:
        next_frame_status = "PASS_BOUNDED"
    elif applied:
        next_frame_status = "BLOCKED"
    else:
        next_frame_status = "BLOCKED"

    if page_ids:
        page_identity = {
            "status": "PASS_BOUNDED",
            "records": page_records,
            "distinctPageIDs": sorted(page_ids),
            "distinctAllocationGenerations": sorted(generations),
            "recordCount": len(page_records),
            "hostTelemetryOnly": True,
        }
    else:
        page_identity = {
            "status": "BLOCKED",
            "records": page_records,
            "distinctPageIDs": [],
            "distinctAllocationGenerations": sorted(generations),
            "recordCount": len(page_records),
            "reason": "allocator page identity fields were absent or still uint32 invalid sentinels",
            "hostTelemetryOnly": True,
        }

    if card_page_pairs:
        distinct_card_page = {
            "status": "PASS_BOUNDED",
            "distinctCardIDs": sorted(card_ids),
            "distinctPageIDs": sorted(page_ids),
            "distinctCardPagePairs": [list(pair) for pair in sorted(card_page_pairs)],
            "hostTelemetryOnly": True,
        }
    else:
        distinct_card_page = {
            "status": "BLOCKED",
            "distinctCardIDs": sorted(card_ids),
            "distinctPageIDs": sorted(page_ids),
            "distinctCardPagePairs": [],
            "reason": "current surfaceCacheStats exposes page IDs but no card IDs; material targets are not card identities",
            "hostTelemetryOnly": True,
        }

    return {
        "enabled": True,
        "status": "PASS_BOUNDED" if api_status == "AVAILABLE" and next_frame_status == "PASS_BOUNDED" else "BLOCKED",
        "mode": DIRTY_MODE,
        "apiStatus": api_status,
        "apiReason": reason,
        "mutationIntervalFrames": DIRTY_MUTATION_INTERVAL,
        "mutationBatch": DIRTY_MUTATION_BATCH,
        "mutations": mutations,
        "appliedMutationCount": len(applied),
        "blockedMutationCount": len(blocked_mutations),
        "nextFrameEvidence": evidence,
        "nextFrameStatus": next_frame_status,
        "nextFramePassCount": len(next_frame_pass),
        "pageIdentity": page_identity,
        "distinctCardPageEvidence": distinct_card_page,
        "cardIdentitySource": list(CARD_ID_ALIASES),
    }


def _run_case(label):
    properties = _configure_for_case(label)
    graph = None
    result = {
        "case": label,
        "properties": properties,
        "status": "in-progress",
        "samples": [],
        "dirtyPressure": None,
        "reload": None,
        "error": None,
    }
    try:
        graph = _make_graph(label, properties)
        m.addGraph(graph)
        m.setActiveGraph(graph)
        _setup_scene()

        if label == "invalidate":
            # Render the same frame-index schedule before the reload, then
            # reload and replay it from frame 1.  This makes the reset and
            # re-warm behavior directly comparable to lookup_on.
            pre_checkpoints = tuple(frame for frame in CHECKPOINTS if frame <= INVALIDATE_AT)
            if not pre_checkpoints:
                pre_checkpoints = (INVALIDATE_AT,)
            pre_samples = _sample_schedule(label, phase="before_reload", checkpoints=pre_checkpoints)
            before = pre_samples[-1] if pre_samples else None
            m.loadScene(SCENE)
            m.resizeFrameBuffer(*RESOLUTION)
            m.clock.frame = 0
            camera = m.scene.camera
            camera.position = FIXED_CAMERA_POSITION
            camera.target = FIXED_CAMERA_TARGET
            camera.up = FIXED_CAMERA_UP
            camera.focalLength = FIXED_CAMERA_FOCAL_LENGTH
            _prepare_dirty_pressure()
            post_samples = _sample_schedule(label, phase="after_reload")
            after = post_samples[-1] if post_samples else None
            after_first = post_samples[0] if post_samples else None
            result["samples"] = [_strip_internal(sample) for sample in pre_samples + post_samples]
            if before and after:
                before_generation = _page_telemetry_value(before, "pageGeneration")
                after_generation = _page_telemetry_value(after_first, "pageGeneration") if after_first else None
                before_scene_generation = _page_telemetry_value(before, "sceneGeneration")
                after_scene_generation = _page_telemetry_value(after_first, "sceneGeneration") if after_first else None
                before_reset_count = _page_telemetry_value(before, "resetCount")
                after_reset_count = _page_telemetry_value(after_first, "resetCount") if after_first else None
                before_stale_owner = _page_telemetry_value(before, "staleOwnerRejects")
                after_stale_owner = _page_telemetry_value(after_first, "staleOwnerRejects") if after_first else None
                transition_evidence = _generation_transition_evidence(before, after_first)
                result["reload"] = {
                    "invalidateAt": INVALIDATE_AT,
                    "beforeFrameIndex": before.get("stats", {}).get("frameIndex"),
                    "afterFrameIndex": after_first.get("stats", {}).get("frameIndex") if after_first else None,
                    "resetObserved": (
                        before.get("stats", {}).get("frameIndex") is not None
                        and after_first is not None
                        and after_first.get("stats", {}).get("frameIndex") is not None
                        and after_first["stats"]["frameIndex"] < before["stats"]["frameIndex"]
                    ),
                    "beforeAllocatedPages": before.get("stats", {}).get("allocatedPages"),
                    "afterAllocatedPages": after.get("stats", {}).get("allocatedPages"),
                    "beforePageGeneration": before_generation,
                    "afterPageGeneration": after_generation,
                    "beforeSceneGeneration": before_scene_generation,
                    "afterSceneGeneration": after_scene_generation,
                    "beforeResetCount": before_reset_count,
                    "afterResetCount": after_reset_count,
                    "generationTransitionObserved": bool(transition_evidence),
                    "generationTransitionEvidence": transition_evidence,
                    "beforeStaleOwnerRejects": before_stale_owner,
                    "afterStaleOwnerRejects": after_stale_owner,
                    "staleOwnerRejectDelta": (
                        after_stale_owner - before_stale_owner
                        if before_stale_owner is not None and after_stale_owner is not None
                        else None
                    ),
                    "afterStats": after.get("stats"),
                }
            final_sample = after
        else:
            samples = _sample_schedule(label)
            result["samples"] = [_strip_internal(sample) for sample in samples]
            final_sample = samples[-1] if samples else None

        if final_sample is not None:
            result["finalArrays"] = _save_arrays(label, final_sample)
            result["dirtyPressure"] = _collect_dirty_pressure_report(result.get("samples", []))
            # The event ledger is the authoritative source for card/page identity.
            # Keep it separate from numeric page telemetry so tiny-atlas gates can
            # prove distinct owners without treating aggregate requestCards as IDs.
            result["surfaceCacheEventIdentity"] = _collect_surface_cache_event_identity(
                result.get("samples", [])
            )
            final_outputs = final_sample.get("outputs", {})
            final_stats = final_sample.get("stats")
            required_stats = ("frameIndex", "maxPagesPerFrame", "allocatedPages", "coverage")
            stats_core_ok = bool(
                final_stats is not None
                and all(
                    key in final_stats
                    and final_stats[key] is not None
                    and math.isfinite(float(final_stats[key]))
                    for key in required_stats
                )
            )
            result["finalGate"] = {
                "outputsFinite": all(item.get("finite", False) for item in final_outputs.values())
                and set(final_outputs) == set(OUTPUTS),
                "outputsNonnegative": all(item.get("nonnegative", False) for item in final_outputs.values())
                and set(final_outputs) == set(OUTPUTS),
                "statsAvailable": stats_core_ok,
                "statsFinite": bool(
                    final_stats is not None
                    and all(value is None or math.isfinite(float(value)) for value in final_stats.values())
                ),
                "pageTelemetryStatus": final_sample.get("pageTelemetry", {}).get("status"),
                "pageTelemetryComplete": (
                    final_sample.get("pageTelemetry", {}).get("status") == "PASS"
                    if label != "lookup_off"
                    else final_sample.get("pageTelemetry", {}).get("status") == "NOT_APPLICABLE"
                ),
                "pageTelemetryRequired": label != "lookup_off",
                "pageTelemetry": final_sample.get("pageTelemetry", {}),
                "generationMismatchTelemetry": _page_telemetry_value(final_sample, "generationMismatchRejects") is not None,
                "stateMismatchTelemetry": _page_telemetry_value(final_sample, "stateMismatchRejects") is not None,
                "staleOwnerTelemetry": _page_telemetry_value(final_sample, "staleOwnerRejects") is not None,
                "lastUsedTelemetry": _page_telemetry_value(final_sample, "lastUsed") is not None,
                "evictionTelemetry": _page_telemetry_value(final_sample, "evictions") is not None,
                "feedbackHitsTelemetry": _page_telemetry_value(final_sample, "feedbackHits") is not None,
                "feedbackPagesTelemetry": _page_telemetry_value(final_sample, "feedbackPages") is not None,
                "feedbackDedupTelemetry": _page_telemetry_value(final_sample, "feedbackDedup") is not None,
                "feedbackStaleRejectsTelemetry": _page_telemetry_value(final_sample, "feedbackStaleRejects") is not None,
                "pageMetadataPendingTelemetry": _page_telemetry_value(final_sample, "pageMetadataPending") is not None,
                "pageMetadataReadyTelemetry": _page_telemetry_value(final_sample, "pageMetadataReady") is not None,
                "pageClearCommandsTelemetry": _page_telemetry_value(final_sample, "pageClearCommands") is not None,
                "pageClearTexelsTelemetry": _page_telemetry_value(final_sample, "pageClearTexels") is not None,
                "pageIdentityTelemetry": all(
                    _page_telemetry_value(final_sample, field_name) is not None
                    for field_name in (
                        "lastAllocatedPageID",
                        "lastAllocatedGeneration",
                        "lastAllocatedFrame",
                        "lastEvictedPageID",
                        "lastEvictedGeneration",
                        "lastEvictedFrame",
                        "lastTouchedPageID",
                        "lastTouchedFrame",
                    )
                ),
                "dirtyPressure": result.get("dirtyPressure"),
                "surfaceCacheEventIdentity": result.get("surfaceCacheEventIdentity"),
                "feedbackActivity": final_sample.get("pageTelemetry", {}).get("feedbackActivity", {}),
                "requestRawTelemetry": _page_telemetry_value(final_sample, "requestRaw") is not None,
                "requestCardsTelemetry": _page_telemetry_value(final_sample, "requestCards") is not None,
                "requestDedupTelemetry": _page_telemetry_value(final_sample, "requestDedup") is not None,
                "requestStaleRejectsTelemetry": _page_telemetry_value(final_sample, "requestStaleRejects") is not None,
                "requestCaptureCompletedTelemetry": _page_telemetry_value(final_sample, "requestCaptureCompleted") is not None,
                "requestActivity": final_sample.get("pageTelemetry", {}).get("requestActivity", {}),
            }
            tiny_required = TINY_ATLAS and label != "lookup_off"
            tiny_stats = final_stats or {}
            tiny_telemetry = final_sample.get("pageTelemetry", {})
            pressure_frame_count = max(CHECKPOINTS) if TINY_ATLAS else 0
            pressure_partitions = (
                sorted(
                    {
                        _camera_partition_for_frame(frame)
                        for frame in range(1, pressure_frame_count + 1)
                    }
                )
                if TINY_ATLAS
                else []
            )
            requested_card_count = _page_telemetry_value(final_sample, "requestCards")
            event_identity = result.get("surfaceCacheEventIdentity") or {}
            tiny_transition = bool(
                tiny_telemetry.get("generationTransitionObserved")
                or (_page_telemetry_value(final_sample, "generationMismatchRejects") or 0) > 0
                or (_page_telemetry_value(final_sample, "staleOwnerRejects") or 0) > 0
            )
            result["finalGate"]["tinyAtlasRequired"] = tiny_required
            result["finalGate"]["tinyAtlas"] = {
                "required": tiny_required,
                "atlasSize": ATLAS_SIZE if tiny_required else None,
                "minFrames": TINY_MIN_FRAMES if tiny_required else None,
                "pressurePhases": PRESSURE_PHASES if tiny_required else False,
                "pressureWarmupFrames": PRESSURE_WARMUP_FRAMES if tiny_required and PRESSURE_PHASES else None,
                "pressurePhaseFrames": PRESSURE_PHASE_FRAMES if tiny_required and PRESSURE_PHASES else None,
                # LumenGI frameIndex may reset on camera/scene updates. The capture scheduler
                # owns the page lifecycle, so use its monotonic frame index for this Gate.
                "minimumFrameReached": (
                    not tiny_required
                    or int(
                        tiny_stats.get(
                            "schedulerFrameIndex", tiny_stats.get("frameIndex", 0)
                        )
                        or 0
                    ) >= TINY_MIN_FRAMES
                ),
                "cardsAtLeast12": not tiny_required or int(tiny_stats.get("cards", 0) or 0) >= 12,
                "pressurePartitions": pressure_partitions,
                "pressurePartitionCount": len(pressure_partitions),
                "pressurePartitionsAtLeast12": not tiny_required or len(pressure_partitions) >= 12,
                "requestedCardCount": requested_card_count,
                "requestedCardCountAtLeast12": not tiny_required or (
                    requested_card_count is not None and requested_card_count >= 12.0
                ),
                "pageMetadataPendingTelemetry": not tiny_required or _page_telemetry_value(final_sample, "pageMetadataPending") is not None,
                "pageMetadataReadyTelemetry": not tiny_required or _page_telemetry_value(final_sample, "pageMetadataReady") is not None,
                "pageClearCommandsTelemetry": not tiny_required or _page_telemetry_value(final_sample, "pageClearCommands") is not None,
                "pageClearTexelsTelemetry": not tiny_required or _page_telemetry_value(final_sample, "pageClearTexels") is not None,
                "pageIdentityTelemetry": not tiny_required or all(
                    _page_telemetry_value(final_sample, field_name) is not None
                    for field_name in (
                        "lastAllocatedPageID",
                        "lastAllocatedGeneration",
                        "lastAllocatedFrame",
                        "lastEvictedPageID",
                        "lastEvictedGeneration",
                        "lastEvictedFrame",
                        "lastTouchedPageID",
                        "lastTouchedFrame",
                    )
                ),
                "dirtyPressureRequired": DIRTY_PRESSURE and label != "lookup_off",
                "dirtyPressureStatus": (
                    (result.get("dirtyPressure") or {}).get("status")
                    if DIRTY_PRESSURE and label != "lookup_off"
                    else "NOT_APPLICABLE"
                ),
                "uniqueCardPageEvidence": {
                    "status": event_identity.get("status", "BLOCKED")
                    if label != "lookup_off"
                    else "NOT_APPLICABLE",
                    "distinctCardIDs": event_identity.get("distinctCardIDs", []),
                    "distinctPageIDs": event_identity.get("distinctPageIDs", []),
                    "distinctCardPagePairs": event_identity.get("distinctCardPagePairs", []),
                    "recordCount": event_identity.get("recordCount", 0),
                    "reason": event_identity.get("reason"),
                    "source": "LumenGI.surfaceCacheEvents",
                    "hostTelemetryOnly": True,
                } if tiny_required else {
                    "status": "NOT_APPLICABLE",
                    "reason": "tiny atlas disabled",
                },
                "evictionObserved": not tiny_required or (_page_telemetry_value(final_sample, "evictions") or 0) > 0,
                "generationTransitionObserved": not tiny_required or tiny_transition,
                "staleOwnerTelemetry": _page_telemetry_value(final_sample, "staleOwnerRejects") is not None,
                # The host sentinel is an explicit page-clear fence invariant (1 means every
                # emitted page had all 256 texels cleared); do not infer this from GI pixels.
                "staleTexelSentinel": (
                    _page_telemetry_value(final_sample, "staleTexelSentinel") == 1.0
                    if _page_telemetry_value(final_sample, "staleTexelSentinel") is not None
                    else None
                ),
                "staleTexelSentinelObserved": any(
                    _page_telemetry_value(sample, "staleTexelSentinel") == 1.0
                    for sample in result.get("samples", [])
                ),
                "status": "NOT_APPLICABLE" if not tiny_required else "PENDING_RUNTIME_EVIDENCE",
            }
            # A pressure sequence may end on a quiet frame after the last page
            # clear.  The explicit host sentinel is still valid when observed in
            # any sampled frame of the current report; requiring it only on the
            # final sample would turn a telemetry timing detail into a false
            # BLOCKED result.
            if tiny_required:
                tiny_gate = result["finalGate"]["tinyAtlas"]
                tiny_gate["staleTexelSentinel"] = bool(tiny_gate["staleTexelSentinelObserved"])
                tiny_gate["uniqueCardPageEvidence"] = {
                    **tiny_gate["uniqueCardPageEvidence"],
                    "status": event_identity.get("status", "BLOCKED"),
                }
                tiny_evidence_keys = (
                    "minimumFrameReached",
                    "cardsAtLeast12",
                    "pressurePartitionsAtLeast12",
                    "requestedCardCountAtLeast12",
                    "pageMetadataPendingTelemetry",
                    "pageMetadataReadyTelemetry",
                    "pageClearCommandsTelemetry",
                    "pageClearTexelsTelemetry",
                    "pageIdentityTelemetry",
                    "evictionObserved",
                    "generationTransitionObserved",
                    "staleOwnerTelemetry",
                    "staleTexelSentinel",
                )
                tiny_gate["status"] = (
                    "PASS_BOUNDED"
                    if all(tiny_gate.get(key) is True for key in tiny_evidence_keys)
                    and tiny_gate["uniqueCardPageEvidence"].get("status") == "PASS_BOUNDED"
                    else "PENDING_RUNTIME_EVIDENCE"
                )
            if label == "invalidate":
                reload = result.get("reload") or {}
                result["finalGate"]["generationTransitionObserved"] = reload.get(
                    "generationTransitionObserved", False
                )
                result["finalGate"]["staleOwnerRejectFieldPresent"] = (
                    _page_telemetry_value(final_sample, "staleOwnerRejects") is not None
                )
            # Keep final arrays transiently for cross-case comparisons.
            result["_finalArrays"] = final_sample.get("_arrays", {})
        result["status"] = "complete"
    except Exception as exc:
        result["status"] = "error"
        result["error"] = str(exc)
        print("C6 WARNING case", label, "aborted:", str(exc))
    finally:
        if graph is not None:
            try:
                m.removeGraph(graph)
            except Exception as exc:
                print("C6 WARNING removeGraph", label, str(exc))
    return result


def _compare_arrays(reference, current):
    comparison = {}
    for output_name in OUTPUTS:
        left = reference.get(output_name)
        right = current.get(output_name)
        if left is None or right is None or left.shape != right.shape:
            comparison[output_name] = {"status": "SKIP", "reason": "array unavailable or shape mismatch"}
            continue
        delta = np.abs(right.astype(np.float32) - left.astype(np.float32))
        scale = np.maximum(np.abs(left.astype(np.float32)), 1e-6)
        comparison[output_name] = {
            "status": "PASS" if np.isfinite(delta).all() else "FAIL",
            "meanAbs": float(np.mean(delta)),
            "maxAbs": float(np.max(delta)),
            "meanRelative": float(np.mean(delta / scale)),
            "differentPixels": int(np.count_nonzero(delta > 1e-6)),
            "totalValues": int(delta.size),
        }
    return comparison


def _tiny_pressure_self_test():
    """Validate the deterministic pressure schedule without GPU/Falcor."""
    frame_count = (
        PRESSURE_WARMUP_FRAMES + (TINY_PARTITION_COUNT - 1) * PRESSURE_PHASE_FRAMES
        if PRESSURE_PHASES
        else TINY_PARTITION_COUNT * TINY_PARTITION_FRAMES
    )
    partitions = [
        _camera_partition_for_frame(frame)
        for frame in range(1, frame_count + 1)
    ]
    assert len(set(partitions)) == TINY_PARTITION_COUNT
    assert partitions[0] == 0
    assert partitions[-1] == TINY_PARTITION_COUNT - 1
    assert all(
        partitions[index] <= partitions[index + 1]
        for index in range(len(partitions) - 1)
    )
    if PRESSURE_PHASES:
        assert all(partition == 0 for partition in partitions[:PRESSURE_WARMUP_FRAMES])
        assert partitions[PRESSURE_WARMUP_FRAMES] == 1
        assert PRESSURE_PHASE_FRAMES >= 1
    # Validate the frozen N -> N+1 schema flattening against the source names
    # emitted by the current host binding.  The original keys stay intact.
    flattened_fixture = _flatten_c6_stats(
        {
            "surfaceCacheRequestRaw": 11.0,
            "surfaceCacheRequestCards": 3.0,
            "surfaceCacheRequestCaptureCompleted": 2.0,
            "pageMetadataPending": 1.0,
            "pageMetadataReady": 4.0,
            "cacheLookupHits": 0.0,
            "generationMismatchRejects": 0.0,
            "stateMismatchRejects": 0.0,
            "staleOwnerRejects": 0.0,
        }
    )
    assert flattened_fixture["surfaceCacheRequestRaw"] == 11.0
    assert flattened_fixture["requestRaw"] == 11.0
    assert flattened_fixture["requestCards"] == 3.0
    assert flattened_fixture["requestCaptureCompleted"] == 2.0
    assert flattened_fixture["pageMetadataPending"] == 1.0
    assert flattened_fixture["pageMetadataReady"] == 4.0
    assert all(name in flattened_fixture for name in C6_FROZEN_FIELDS)
    # Validate the page-metadata schema against a host-shaped fixture. This is
    # only a binding/normalization check; it deliberately does not assert
    # eviction or stale-texel behavior.
    fixture = {aliases[0]: 1.0 for aliases in PAGE_TELEMETRY_ALIASES.values()}
    fixture.update(
        {
            "totalPages": 4.0,
            "pageMetadataAllocated": 2.0,
            "pageMetadataTouched": 1.0,
            "pageMetadataInvalid": 1.0,
            "pageMetadataPending": 1.0,
            "pageMetadataReady": 2.0,
            "pageGeneration": 1.0,
            "requestRaw": 1.0,
            "requestCards": 1.0,
            "requestCaptureCompleted": 1.0,
        }
    )
    telemetry = _collect_page_telemetry(fixture)
    assert "pageMetadataPending" in PAGE_TELEMETRY_REQUIRED_FIELDS
    assert "pageMetadataReady" in PAGE_TELEMETRY_REQUIRED_FIELDS
    assert telemetry["status"] == "PASS"
    assert telemetry["fields"]["pageMetadataPending"]["value"] == 1.0
    assert telemetry["fields"]["pageMetadataReady"]["value"] == 2.0
    # The fixture intentionally stops at schedule evidence. Runtime request
    # counts, evictions, page ownership, and stale texel clearing remain
    # BLOCKED until host/GPU telemetry supplies them.
    print(
        "SURFACECACHE_EFFECT_FIXTURE PASS",
        "partitions=%d" % len(set(partitions)),
        "frames=%d" % frame_count,
        "phaseSchedule=%s" % ("on" if PRESSURE_PHASES else "off"),
        "pageMetadataPending=readable",
        "pageMetadataReady=readable",
    )
    return 0


def main():
    report = {
        "script": "run_surfacecache_effect.py",
        "stage": "C6",
        "status": "in-progress",
        "pageTelemetrySchema": {
            "version": PAGE_TELEMETRY_SCHEMA_VERSION,
            "requiredFields": list(PAGE_TELEMETRY_REQUIRED_FIELDS),
            "source": "LumenGI.surfaceCacheStats host telemetry only",
            "missingTelemetryVerdict": "BLOCKED",
            "imageInference": False,
        },
        "scene": SCENE,
        "resolution": list(RESOLUTION),
        "checkpoints": list(CHECKPOINTS),
        "sampleEveryFrame": SAMPLE_EVERY_FRAME,
        "c6FrozenTelemetry": {
            "schemaVersion": "C6-next-frame-validity-v1",
            "fields": list(C6_FROZEN_FIELDS),
            "canonicalLocation": "cases.*.samples.*.stats",
            "source": "LumenGI.surfaceCacheStats",
            "hostTelemetryOnly": True,
        },
        "seed_schedule": {
            "mode": "fixed frame-index sequence",
            "frames": list(CHECKPOINTS),
            "note": "LumenGI exposes no host seed property; every case replays the same frame indices.",
        },
        "camera": {
            "position": [0.0, 0.28, 1.2],
            "target": [0.0, 0.28, 0.0],
            "focalLength": FIXED_CAMERA_FOCAL_LENGTH,
        },
        "dirtyPressure": {
            "enabled": DIRTY_PRESSURE,
            "mode": DIRTY_MODE,
            "mutationIntervalFrames": DIRTY_MUTATION_INTERVAL,
            "mutationBatch": DIRTY_MUTATION_BATCH,
            "api": "Falcor material/geometry setter; unavailable => BLOCKED",
            "cameraOnlyPressureNotAccepted": True,
        },
        "cases": {},
        "comparisons": {},
        "verdicts": [],
    }

    case_results = {}
    for label in CASES:
        result = _run_case(label)
        case_results[label] = result
        report["cases"][label] = dict(result)
        report["cases"][label].pop("_finalArrays", None)
        _write_json(OUT_JSON, report)

    baseline = case_results.get("lookup_on")
    if baseline and baseline.get("_finalArrays"):
        for label, result in case_results.items():
            if label == "lookup_on" or not result.get("_finalArrays"):
                continue
            report["comparisons"][label] = _compare_arrays(baseline["_finalArrays"], result["_finalArrays"])

    for label, result in case_results.items():
        gate = result.get("finalGate", {})
        outputs_ok = gate.get("outputsFinite") is True and gate.get("outputsNonnegative") is True
        stats_ok = gate.get("statsAvailable") is True and gate.get("statsFinite") is True
        page_telemetry_ok = gate.get("pageTelemetryComplete") is True
        tiny_gate = gate.get("tinyAtlas", {})
        tiny_ok = not gate.get("tinyAtlasRequired", False) or all(
            tiny_gate.get(key) is True
            for key in (
                "minimumFrameReached",
                "cardsAtLeast12",
                "pressurePartitionsAtLeast12",
                "requestedCardCountAtLeast12",
                "pageMetadataPendingTelemetry",
                "pageMetadataReadyTelemetry",
                "pageClearCommandsTelemetry",
                "pageClearTexelsTelemetry",
                "pageIdentityTelemetry",
                "evictionObserved",
                "generationTransitionObserved",
                "staleOwnerTelemetry",
                "staleTexelSentinel",
            )
        )
        if tiny_ok and gate.get("tinyAtlasRequired"):
            tiny_ok = (
                gate.get("tinyAtlas", {})
                .get("uniqueCardPageEvidence", {})
                .get("status")
                == "PASS_BOUNDED"
            )
        if DIRTY_PRESSURE and gate.get("tinyAtlasRequired"):
            dirty_status = gate.get("dirtyPressureStatus")
            if dirty_status is None:
                dirty_status = (gate.get("dirtyPressure") or {}).get("status")
            tiny_ok = tiny_ok and dirty_status == "PASS_BOUNDED"
        comparison = report["comparisons"].get(label)
        comparison_ok = True
        if label != "lookup_on":
            comparison_ok = bool(
                comparison
                and all(
                    item.get("status") == "PASS"
                    and math.isfinite(float(item.get("meanAbs", 0.0)))
                    and math.isfinite(float(item.get("maxAbs", 0.0)))
                    and math.isfinite(float(item.get("meanRelative", 0.0)))
                    for item in comparison.values()
                )
            )
        if result.get("status") != "complete":
            verdict = "FAIL"
        elif not page_telemetry_ok:
            # Do not convert absent generation/state/owner/LRU telemetry into a
            # visual PASS or FAIL.  The C6 contract is explicitly BLOCKED until
            # the host exports every required field.
            verdict = "BLOCKED"
        elif outputs_ok and stats_ok and comparison_ok and tiny_ok:
            verdict = "PASS"
        elif gate.get("tinyAtlasRequired"):
            verdict = "BLOCKED"
        else:
            verdict = "FAIL"
        report["verdicts"].append(
            {"case": label, "status": verdict, "gate": gate, "comparisonFinite": comparison_ok}
        )
        print("C6 VERDICT", label, verdict)

    report["status"] = (
        "blocked"
        if any(item.get("status") == "BLOCKED" for item in report["verdicts"])
        else "complete"
    )
    report["low_budget"] = LOW_BUDGET
    report["invalidate_at"] = INVALIDATE_AT
    report["array_capture"] = SAVE_ARRAYS
    report["tinyAtlas"] = {
        "enabled": TINY_ATLAS,
        "atlasSize": ATLAS_SIZE if TINY_ATLAS else None,
        "minimumFrames": TINY_MIN_FRAMES if TINY_ATLAS else None,
        "cameraPartitionCount": TINY_PARTITION_COUNT if TINY_ATLAS else None,
        "cameraPartitionFrames": TINY_PARTITION_FRAMES if TINY_ATLAS else None,
        "pressurePhases": PRESSURE_PHASES if TINY_ATLAS else False,
        "pressureWarmupFrames": PRESSURE_WARMUP_FRAMES if TINY_ATLAS and PRESSURE_PHASES else None,
        "pressurePhaseFrames": PRESSURE_PHASE_FRAMES if TINY_ATLAS and PRESSURE_PHASES else None,
        "dirtyPressure": {
            "enabled": DIRTY_PRESSURE,
            "mode": DIRTY_MODE,
            "mutationIntervalFrames": DIRTY_MUTATION_INTERVAL,
            "mutationBatch": DIRTY_MUTATION_BATCH,
            "cameraOnlyPressureNotAccepted": True,
            "requiresRuntimeMutationEvidence": DIRTY_PRESSURE,
        },
        "uniqueCardPageTelemetry": {
            "status": (
                "PASS_BOUNDED"
                if TINY_ATLAS
                and any(
                    (case.get("finalGate", {})
                     .get("tinyAtlas", {})
                     .get("uniqueCardPageEvidence", {})
                     .get("status") == "PASS_BOUNDED")
                    for case in case_results.values()
                )
                else ("BLOCKED" if TINY_ATLAS else "NOT_APPLICABLE")
            ),
            "reason": (
                None
                if not TINY_ATLAS
                or any(
                    (case.get("finalGate", {})
                     .get("tinyAtlas", {})
                     .get("uniqueCardPageEvidence", {})
                     .get("status") == "PASS_BOUNDED")
                    for case in case_results.values()
                )
                else "surfaceCacheEvents did not expose usable card/page identity"
            ),
        },
        "missingEvidenceVerdict": "BLOCKED",
    }
    _write_json(OUT_JSON, report)
    print("C6 wrote", os.path.abspath(OUT_JSON))


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(_tiny_pressure_self_test())
    if not FALCOR_AVAILABLE:
        print("C6 BLOCKED: Mogwai falcor binding unavailable", FALCOR_IMPORT_ERROR)
        sys.exit(2)
    main()
    exit()
elif FALCOR_AVAILABLE:
    # Mogwai may embed this module under a launcher-specific __name__.
    main()
    exit()
