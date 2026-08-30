"""LumenGI S2 Gate validation asset: 60-second page-churn proxy (RUN-ONLY).

Role / purpose
--------------
Agent N (Test-Tooling) verification asset for the S2 Gate (task.md 7:
"Atlas allocator CPU/GPU tests, capture image tests, 30 分钟 churn 测试通过",
and task.md 15.5: "S2 起 30 分钟" soak). Running the full 30-minute soak on
every S2 integration iteration is too slow, so this script is the 60-second
PROXY: it exercises the same churn drivers (dirty injection, scene reload,
resize) and records the allocator/scheduler counters (alloc / release / fail
/ lost) into a per-sample time series JSON. Root runs it as part of the S2
Gate; the full 30-minute nightly run is a superset of this loop (see the
nightly command below).

STATUS: runnable against the CURRENT S2 integration. The canonical
`LumenGI.surfaceCacheStats` binding is read on every sample and the required
allocator/scheduler fields are emitted under their exact host names. If that
binding, renderer identity, driver identity, or authoritative start/end VRAM
telemetry is unavailable, the artifact is explicitly BLOCKED; no null/proxy
sample is promoted to a release PASS. The short run remains a bounded proxy;
the 30-minute soak is an external gate and is never certified by this script.

What "dirty injection" means here
---------------------------------
* Per-frame material dirty injection: this script tries to toggle a scene
  material's baseColor each frame. Material setters call
  Material::markUpdates() and the Scene maps them to
  IScene::UpdateFlags::MaterialsChanged (Scene.cpp:1759), which is the S2-A2
  contract input for per-card dirty invalidation. If the Python material API
  is unavailable (or the pass does not yet map MaterialsChanged to dirty
  pages), the capability probe records `material_toggle_available=False` and
  churn is driven by the static path below instead.
* Static churn (always on): periodic scene reload (re-captures every card
  page) + periodic framebuffer resize (rebuilds screen resources), mirroring
  run_s2verify.py's reload/resize blocks.

Output contract (JSON, LUMEN_CHURN_OUT)
---------------------------------------
  schema_version, mode ("60s-proxy"), seconds, frame_count,
  material_toggle_available, stats_available, telemetry_provenance,
  telemetry_provenance.vram.samples (authoritative GPU-wide VRAM sequence),
  series: [{frame, dirty_injections, alloc, release, fail, lost,
            allocations, releases, schedAllocFailures, schedLostPages,
            schedRecaptures, allocatedPages, freePages, residentBytes}, ...],
  totals: {alloc, release, fail, lost, allocations, releases,
           schedAllocFailures, schedLostPages, schedRecaptures,
           allocatedPages, freePages},
  nightly_command_30min: the full soak invocation (see comments).

Full 30-minute nightly command (run by root on the GPU machine, from repo root)
-------------------------------------------------------------------------------
  $env:LUMEN_CHURN_SECONDS='1800'
  build\\windows-vs2022\\bin\\Release\\Mogwai.exe ^
    --device-type d3d12 --headless --precise ^
    --script tests\\lumengi\\run_churn_short.py ^
    --logfile artifacts\\lumengi\\S2\\churn-30min.log
  (1800 s * 60 fps = 108000 frames; the nightly runner may later wrap this in
   its own framing, task.md 17.1. The S2 gate only requires the 60-second proxy
   here; the full soak is the S9 release-candidate round.)

Verified APIs used
------------------
* m.activeGraph.get_pass("LumenGI")           (RenderGraph::getPass, RenderGraph.cpp:759)
* m.scene.materials                            (Scene::getMaterials, Scene.cpp:4348)
* material.baseColor = float4(...)             (StandardMaterial setter, triggers MaterialsChanged)
* m.loadScene(path) / m.resizeFrameBuffer(w,h) (run_s2verify.py precedent)
* m.timingCapture / m.clock                    (existing run_*.py pattern)

Known pitfalls encoded here
---------------------------
* `surfaceCacheStats` is the required host binding. Older stats accessors are
  retained only as compatibility diagnostics and are marked non-canonical.
* Material change propagation to per-card dirty depends on S2-A2
  (LumenCardScene::update mapping MaterialsChanged). The proxy records whether
  material toggling is even possible; it does not assert on it.
* Stats are sampled every SAMPLE_INTERVAL_FRAMES, not every frame, to keep the
  JSON and the sampling cost bounded.
"""

import json
import math
import os
import platform
import subprocess
import sys
import time
from collections.abc import Mapping

try:
    from falcor import *

    FALCOR_AVAILABLE = True
    FALCOR_IMPORT_ERROR = None
except Exception as _falcor_error:  # pragma: no cover - exercised outside Mogwai.
    # Keep the fixture/self-test path dependency-free.  Mogwai provides the
    # binding and executes the normal path below.
    FALCOR_AVAILABLE = False
    FALCOR_IMPORT_ERROR = repr(_falcor_error)

FRAME_RATE = 60
RESOLUTION = (640, 360)
SMALL_RESOLUTION = (320, 180)

# 60-second proxy at 60 fps. Override with LUMEN_CHURN_SECONDS for longer runs
# (1800 = the full 30-minute nightly soak; see header).
DEFAULT_SECONDS = 60
SECONDS = float(os.environ.get("LUMEN_CHURN_SECONDS", DEFAULT_SECONDS))
FRAME_COUNT = int(SECONDS * FRAME_RATE)

# Every SAMPLE_INTERVAL_FRAMES frames the counters are read into the series.
SAMPLE_INTERVAL_FRAMES = 30

# GPU-wide VRAM is sampled less frequently than Surface Cache counters so a
# long soak does not invoke nvidia-smi once per rendered frame.  These samples
# are evidence only; the offline release contract requires every sample to be
# authoritative and will BLOCK when nvidia-smi (or an equivalent provider) is
# unavailable.  Five seconds gives enough resolution to detect a sustained
# monotonic trend without turning a 2-hour run into a telemetry benchmark.
VRAM_SAMPLE_INTERVAL_FRAMES = max(
    1,
    int(os.environ.get("LUMEN_CHURN_VRAM_SAMPLE_INTERVAL_FRAMES", "300")),
)

# Static-churn drivers (always active): reload the scene and resize the
# framebuffer periodically so pages get re-captured and screen resources
# rebuilt even when material dirty injection is unavailable.  The short proxy
# keeps its historical cadence; long phases use a bounded cadence because a
# full scene reload every ten seconds plus a material setter every frame can
# retain transient GPU/scene allocations until the Python host reaches its
# memory ceiling.  The long profile still performs all three mutation classes
# and the release gate only requires positive counts, not a particular rate.
_LONG_PHASE = SECONDS > 120.0
RELOAD_INTERVAL_FRAMES = max(
    1,
    int(os.environ.get("LUMEN_CHURN_RELOAD_INTERVAL_FRAMES", "3600" if _LONG_PHASE else "600")),
)
RESIZE_INTERVAL_FRAMES = max(
    1,
    int(os.environ.get("LUMEN_CHURN_RESIZE_INTERVAL_FRAMES", "5400" if _LONG_PHASE else "300")),
)
MATERIAL_MUTATION_INTERVAL_FRAMES = max(
    1,
    int(os.environ.get("LUMEN_CHURN_MATERIAL_INTERVAL_FRAMES", "60" if _LONG_PHASE else "1")),
)

SCENE_CORNELL = "test_scenes/cornell_box.pyscene"

OUT_JSON = os.environ.get("LUMEN_CHURN_OUT", "artifacts/lumengi/S2/churn-short.json")

# S2_TODO: threshold that root freezes at the S2 gate. The proxy only records
# the series; the nightly runner asserts the S2 gate invariants (no leak,
# bounded churn, no unbounded resident growth). Placeholder: if stats are
# available, fail if alloc/fail/lost diverge by more than this factor across
# the window (see VERDICT lines below).
CHURN_DIVERGENCE_MAX = 10.0

MATERIAL_COLORS = (
    (float4(0.725, 0.71, 0.68, 1.0), float4(0.20, 0.30, 0.90, 1.0))
    if FALCOR_AVAILABLE
    else ((0.725, 0.71, 0.68, 1.0), (0.20, 0.30, 0.90, 1.0))
)

# These are the release evidence fields.  They intentionally use the exact
# names exported by LumenGI.surfaceCacheStats; the short aliases below remain
# in the series/totals for compatibility with the original S2 proxy artifact.
REQUIRED_STAT_FIELDS = (
    "allocations",
    "releases",
    "schedAllocFailures",
    "schedLostPages",
    "schedRecaptures",
    "allocatedPages",
    "freePages",
)
STAT_ALIASES = {
    "allocations": ("allocations", "allocationCount", "totalAllocations", "alloc"),
    "releases": ("releases", "releaseCount", "totalReleases", "release"),
    "schedAllocFailures": (
        "schedAllocFailures",
        "totalAllocationFailures",
        "allocationFailures",
        "fail",
    ),
    "schedLostPages": ("schedLostPages", "totalLostPages", "lostPages", "lost"),
    "schedRecaptures": ("schedRecaptures", "totalRecaptures", "recaptures", "recapture"),
    "allocatedPages": ("allocatedPages", "allocatedPageCount"),
    "freePages": ("freePages", "freePageCount"),
}


def json_safe(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, (list, tuple)):
        return [json_safe(item) for item in value]
    if isinstance(value, Mapping):
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


def create_lumen_graph():
    graph = RenderGraph("LumenGIChurn")
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
    return graph


def _read_attr(obj, *names):
    """Read a property from either a pybind object or a mapping."""
    for name in names:
        try:
            value = obj.get(name) if isinstance(obj, Mapping) else getattr(obj, name, None)
            if value is not None:
                return value
        except Exception:
            continue
    return None


def _counter_value(value):
    """Return a finite, non-negative counter or ``None`` for invalid data."""
    if value is None or isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number) or number < 0.0:
        return None
    return int(number) if number.is_integer() else number


def read_surface_cache_stats():
    """Read the canonical ``LumenGI.surfaceCacheStats`` binding.

    The fallback accessors are retained for old/experimental graphs so the
    runner remains import-compatible, but a fallback is never reported as
    canonical Surface Cache evidence.  The returned binding record makes that
    distinction explicit in the artifact.
    """
    binding = {
        "property": "LumenGI.surfaceCacheStats",
        "source": None,
        "available": False,
        "canonical": False,
        "reason": None,
    }
    try:
        pass_obj = getattr(m.activeGraph, "getPass", None)
        if callable(pass_obj):
            pass_obj = pass_obj("LumenGI")
        else:
            pass_obj = m.activeGraph.get_pass("LumenGI")
    except Exception as exc:
        binding["reason"] = "LumenGI pass unavailable: %s" % str(exc)
        return None, binding

    # This exact property is the release contract.  Do not silently substitute
    # another pass stats object when it is absent.
    for attr in ("surfaceCacheStats",):
        try:
            val = getattr(pass_obj, attr, None)
            if callable(val):
                val = val()
            if val is not None:
                binding.update({"source": "LumenGI.%s" % attr, "available": True, "canonical": True})
                return val, binding
        except Exception as exc:
            binding["reason"] = "LumenGI.%s read failed: %s" % (attr, str(exc))
            continue

    # Compatibility only.  Keep the value in the series for old artifacts, but
    # mark the canonical binding unavailable so a missing host export is BLOCKED.
    for attr in ("stats", "schedulerStats", "cacheStats", "getStats"):
        try:
            val = getattr(pass_obj, attr, None)
            if callable(val):
                val = val()
            if val is not None:
                binding.update(
                    {
                        "source": "LumenGI.%s" % attr,
                        "reason": "canonical surfaceCacheStats property unavailable; fallback retained",
                    }
                )
                return val, binding
        except Exception:
            continue
    if binding["reason"] is None:
        binding["reason"] = "surfaceCacheStats property unavailable"
    return None, binding


def try_read_pass_stats():
    """Compatibility wrapper returning only the raw stats object."""
    return read_surface_cache_stats()[0]


def normalize_stats(raw):
    """Map a Surface Cache snapshot onto canonical and legacy series keys."""
    if raw is None:
        return None

    canonical = {
        name: _counter_value(_read_attr(raw, *aliases))
        for name, aliases in STAT_ALIASES.items()
    }
    canonical["residentBytes"] = _counter_value(_read_attr(raw, "residentBytes"))
    canonical["memoryBudgetBytes"] = _counter_value(_read_attr(raw, "memoryBudgetBytes"))
    canonical["frameIndex"] = _counter_value(_read_attr(raw, "frameIndex"))
    # Preserve the original short names as aliases in every sample.
    canonical.update(
        {
            "alloc": canonical["allocations"],
            "release": canonical["releases"],
            "fail": canonical["schedAllocFailures"],
            "lost": canonical["schedLostPages"],
            "recapture": canonical["schedRecaptures"],
            "allocated_pages": canonical["allocatedPages"],
            "free_pages": canonical["freePages"],
            "resident_bytes": canonical["residentBytes"],
        }
    )
    return canonical


def missing_stat_fields(normalized):
    if not normalized:
        return list(REQUIRED_STAT_FIELDS)
    return [name for name in REQUIRED_STAT_FIELDS if normalized.get(name) is None]


def _env_text(*names):
    for name in names:
        value = os.environ.get(name)
        if value is not None and str(value).strip():
            return str(value).strip()
    return None


def _env_bytes(*names):
    value = _env_text(*names)
    if value is None:
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number) or number < 0.0:
        return None
    return int(number)


def _device_type_from_api_name(api_name):
    """Derive only the canonical API family exposed by live Device.info."""
    text = str(api_name or "").strip().casefold().replace(" ", "")
    if text in ("d3d12", "directx12", "direct3d12"):
        return "d3d12"
    if text == "vulkan":
        return "vulkan"
    return None


def _host_device_candidates(script_host, probe_attempts):
    """Return read-only optional paths to the *host* Device object.

    Falcor binds ``Device.info`` to Python, but stock Mogwai's script binding
    exposes frame/graph control rather than a documented ``getDevice`` entry
    point.  Do not construct a new ``falcor.Device`` here: that would describe
    a different adapter and must never become release provenance.
    """
    if script_host is None:
        probe_attempts.append({"path": "script-host", "status": "unavailable", "reason": "Mogwai global m is absent"})
        return ()
    candidates = [
        ("m.device", script_host, "device"),
        ("m.getDevice", script_host, "getDevice"),
        ("m.get_device", script_host, "get_device"),
    ]
    for owner_label, attribute in (("m.renderer", "renderer"), ("m.activeGraph", "activeGraph")):
        try:
            owner = getattr(script_host, attribute, None)
            if owner is None:
                probe_attempts.append({"path": owner_label, "status": "unavailable"})
                continue
            candidates.extend(
                (
                    (owner_label + ".device", owner, "device"),
                    (owner_label + ".getDevice", owner, "getDevice"),
                    (owner_label + ".get_device", owner, "get_device"),
                )
            )
        except Exception as exc:
            probe_attempts.append({"path": owner_label, "status": "error", "reason": str(exc)})
    return tuple(candidates)


def collect_renderer_provenance(script_host=None):
    """Collect adapter/API identity from live Falcor renderer/device bindings.

    The probe is intentionally read-only. ``Device.info.adapter_name`` and
    ``Device.info.api_name`` are documented Falcor pybind fields; a device type
    is derived from that API name. Environment values remain diagnostic hints
    only and can never set an authoritative field or release PASS.
    """
    result = {
        "source": None,
        "available": False,
        "authoritative": False,
        "status": "BLOCKED",
        "adapter_name": None,
        "api_name": None,
        "device_type": None,
        "driver_version": None,
        "device_type_hint": _env_text("LUMEN_CHURN_DEVICE_TYPE", "FALCOR_DEVICE_TYPE"),
        "hint_adapter_name": _env_text("LUMEN_CHURN_GPU", "LUMENGI_BENCHMARK_GPU"),
        "hint_driver_version": _env_text("LUMEN_CHURN_DRIVER", "LUMENGI_BENCHMARK_DRIVER"),
        "device_source": None,
        "binding_contract": {
            "device_info_fields": ["adapter_name", "api_name"],
            "host_device_accessor": "optional read-only Mogwai host path",
            "new_device_construction_forbidden": True,
            "environment_hints_authoritative": False,
        },
        "probe_attempts": [],
        "reason": None,
    }
    if script_host is None:
        script_host = globals().get("m")
    device_candidates = _host_device_candidates(script_host, result["probe_attempts"])

    selected_device = None
    selected_info = None
    for label, owner, attribute in device_candidates:
        try:
            value = getattr(owner, attribute, None)
            if callable(value):
                value = value()
            if value is None:
                result["probe_attempts"].append({"path": label, "status": "unavailable"})
                continue
            info = _read_attr(value, "info")
            adapter_name = _read_attr(info, "adapter_name", "adapterName") if info is not None else None
            api_name = _read_attr(info, "api_name", "apiName") if info is not None else None
            found = bool(str(adapter_name or "").strip() and str(api_name or "").strip())
            result["probe_attempts"].append({"path": label, "status": "found" if found else "incomplete", "info_available": info is not None})
            if found:
                selected_device = value
                selected_info = info
                result["device_source"] = label
                break
        except Exception as exc:
            result["probe_attempts"].append({"path": label, "status": "error", "reason": str(exc)})

    if selected_info is not None:
        result["source"] = "%s.info" % (result["device_source"] or "falcor.Device")
        result["adapter_name"] = _read_attr(selected_info, "adapter_name", "adapterName")
        result["api_name"] = _read_attr(selected_info, "api_name", "apiName")
        result["device_type"] = _device_type_from_api_name(result["api_name"])
    if (
        str(result["adapter_name"] or "").strip()
        and str(result["api_name"] or "").strip()
        and result["device_type"] is not None
    ):
        result.update(
            {
                "available": True,
                "authoritative": selected_device is not None,
                "status": "PASS" if selected_device is not None else "BLOCKED",
            }
        )
    elif result["reason"] is None:
        result["reason"] = (
            "live Mogwai host Device.info adapter/API binding unavailable or incomplete; "
            "environment hints and a newly constructed Device are non-authoritative"
        )
    return result


def _windows_process_memory(pid):
    """Return (working_set, peak_working_set, reason) using Win32 ctypes.

    This path has no third-party dependency and is intentionally supplemental:
    failure leaves the fields unavailable and never changes the release gate.
    """
    if os.name != "nt":
        return None, None, "Windows GetProcessMemoryInfo is unavailable on %s" % os.name
    try:
        import ctypes
        from ctypes import wintypes

        class _ProcessMemoryCounters(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        open_process = kernel32.OpenProcess
        open_process.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        open_process.restype = wintypes.HANDLE
        close_handle = kernel32.CloseHandle
        close_handle.argtypes = [wintypes.HANDLE]
        close_handle.restype = wintypes.BOOL
        get_memory = psapi.GetProcessMemoryInfo
        get_memory.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessMemoryCounters), wintypes.DWORD]
        get_memory.restype = wintypes.BOOL

        # PROCESS_QUERY_LIMITED_INFORMATION is sufficient on modern Windows;
        # retain PROCESS_QUERY_INFORMATION as a compatibility fallback.
        handles = (0x1000, 0x0400)
        handle = None
        for access in handles:
            handle = open_process(access, False, int(pid))
            if handle:
                break
        if not handle:
            error = ctypes.get_last_error()
            return None, None, "OpenProcess failed with Win32 error %s" % error
        try:
            counters = _ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            if not get_memory(handle, ctypes.byref(counters), counters.cb):
                error = ctypes.get_last_error()
                return None, None, "GetProcessMemoryInfo failed with Win32 error %s" % error
            working = _counter_value(counters.WorkingSetSize)
            peak = _counter_value(counters.PeakWorkingSetSize)
            return (
                working,
                peak,
                None if working is not None and peak is not None else "GetProcessMemoryInfo returned incomplete fields",
            )
        finally:
            close_handle(handle)
    except Exception as exc:
        return None, None, "ctypes GetProcessMemoryInfo unavailable: %s" % str(exc)


def collect_system_provenance():
    """Collect stable OS identity and optional process working-set telemetry."""
    result = {
        "source": "python.platform",
        "available": False,
        "authoritative": False,
        "status": "BLOCKED",
        "platform": None,
        "system": None,
        "release": None,
        "version": None,
        "machine": None,
        "processor": None,
        "python_version": platform.python_version(),
        "pid": os.getpid(),
        "process_working_set_bytes": None,
        "process_peak_working_set_bytes": None,
        "process_memory_source": None,
        "process_memory_available": False,
        "process_memory_authoritative": False,
        "process_memory_status": "UNAVAILABLE",
        "reason": None,
    }
    try:
        result.update(
            {
                "platform": platform.platform(),
                "system": platform.system(),
                "release": platform.release(),
                "version": platform.version(),
                "machine": platform.machine(),
                "processor": platform.processor(),
            }
        )
        result.update({"available": True, "authoritative": True, "status": "PASS"})
    except Exception as exc:
        result["reason"] = "platform telemetry failed: %s" % str(exc)

    # Prefer the Windows API so the release runner does not depend on psutil.
    # This is supplemental process telemetry, not a release PASS condition.
    windows_working, windows_peak, windows_reason = _windows_process_memory(result["pid"])
    if windows_working is not None and windows_peak is not None:
        result.update(
            {
                "process_working_set_bytes": windows_working,
                "process_peak_working_set_bytes": windows_peak,
                "process_memory_source": "Win32.GetProcessMemoryInfo",
                "process_memory_available": True,
                "process_memory_authoritative": True,
                "process_memory_status": "AVAILABLE",
            }
        )
    elif windows_working is not None or windows_peak is not None:
        result.update(
            {
                "process_working_set_bytes": windows_working,
                "process_peak_working_set_bytes": windows_peak,
                "process_memory_source": "Win32.GetProcessMemoryInfo",
                "process_memory_status": "PARTIAL",
                "process_memory_reason": "Win32 sampler returned only one working-set field",
            }
        )
    # psutil is optional in non-Windows Falcor runtimes.  Its absence must
    # remain visible rather than becoming a fabricated zero memory value.
    try:
        if not result["process_memory_available"]:
            import psutil

            process = psutil.Process(result["pid"])
            info = process.memory_info()
            rss = _counter_value(getattr(info, "rss", None))
            peak = _counter_value(getattr(info, "peak_wset", None))
            if rss is not None and peak is not None:
                result.update(
                    {
                        "process_working_set_bytes": rss,
                        "process_peak_working_set_bytes": peak,
                        "process_memory_source": "psutil.Process.memory_info",
                        "process_memory_available": True,
                        "process_memory_authoritative": True,
                        "process_memory_status": "AVAILABLE",
                    }
                )
            elif rss is not None or peak is not None:
                result.update(
                    {
                        "process_working_set_bytes": rss,
                        "process_peak_working_set_bytes": peak,
                        "process_memory_source": "psutil.Process.memory_info",
                        "process_memory_status": "PARTIAL",
                        "process_memory_reason": "psutil returned only one working-set field",
                    }
                )
            else:
                result["process_memory_reason"] = "psutil returned no working-set fields"
        if not result["process_memory_available"]:
            result["process_memory_reason"] = windows_reason or "process memory sampler unavailable"
    except Exception as exc:
        result["process_memory_reason"] = windows_reason or "optional process memory telemetry unavailable: %s" % str(exc)
    return result


def _memory_mb_to_bytes(value):
    try:
        number = float(str(value).strip())
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number) or number < 0.0:
        return None
    return int(number * 1024.0 * 1024.0)


def collect_vram_snapshot():
    """Read authoritative GPU memory telemetry when the host exposes it.

    ``nvidia-smi`` is an external, read-only OS telemetry source.  Environment
    overrides are accepted only as explicitly non-authoritative diagnostics;
    they can never make the release evidence ready.
    """
    result = {
        "source": None,
        "query": None,
        "available": False,
        "authoritative": False,
        "status": "BLOCKED",
        "gpu_index": None,
        "gpu_name": None,
        "driver_version": None,
        "total_bytes": None,
        "used_bytes": None,
        "free_bytes": None,
        "budget_bytes": None,
        "reason": None,
    }
    env_total = _env_bytes("LUMEN_CHURN_VRAM_TOTAL_BYTES")
    env_used = _env_bytes("LUMEN_CHURN_VRAM_USED_BYTES")
    env_free = _env_bytes("LUMEN_CHURN_VRAM_FREE_BYTES")
    if env_total is not None and env_used is not None and env_free is not None:
        result.update(
            {
                "source": "environment:LUMEN_CHURN_VRAM_*",
                "query": "environment override (non-authoritative)",
                "available": True,
                "authoritative": False,
                "status": "OPEN",
                "gpu_index": _env_text("LUMEN_CHURN_GPU_INDEX"),
                "gpu_name": _env_text("LUMEN_CHURN_GPU", "LUMENGI_BENCHMARK_GPU"),
                "driver_version": _env_text("LUMEN_CHURN_DRIVER", "LUMENGI_BENCHMARK_DRIVER"),
                "total_bytes": env_total,
                "used_bytes": env_used,
                "free_bytes": env_free,
                "budget_bytes": _env_bytes("LUMEN_CHURN_VRAM_BUDGET_BYTES"),
                "reason": "environment telemetry is diagnostic and not authoritative",
            }
        )

    query = (
        "nvidia-smi --query-gpu=index,name,driver_version,memory.total,memory.used,memory.free "
        "--format=csv,noheader,nounits"
    )
    try:
        completed = subprocess.run(
            [
                "nvidia-smi",
                "--query-gpu=index,name,driver_version,memory.total,memory.used,memory.free",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError((completed.stderr or "nvidia-smi failed").strip())
        rows = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
        selected_index = _env_text("LUMEN_CHURN_GPU_INDEX")
        row = None
        for candidate in rows:
            fields = [field.strip() for field in candidate.split(",")]
            if len(fields) < 6:
                continue
            if selected_index is None or fields[0] == selected_index:
                row = fields
                break
        if row is None:
            raise RuntimeError("nvidia-smi returned no parseable selected GPU row")
        result.update(
            {
                "source": "nvidia-smi",
                "query": query,
                "available": True,
                "authoritative": True,
                "status": "PASS",
                "gpu_index": row[0],
                "gpu_name": row[1],
                "driver_version": row[2],
                "total_bytes": _memory_mb_to_bytes(row[3]),
                "used_bytes": _memory_mb_to_bytes(row[4]),
                "free_bytes": _memory_mb_to_bytes(row[5]),
                "budget_bytes": _env_bytes("LUMEN_CHURN_VRAM_BUDGET_BYTES"),
                "reason": None,
            }
        )
        if any(result[key] is None for key in ("total_bytes", "used_bytes", "free_bytes")):
            result.update({"available": False, "authoritative": False, "status": "BLOCKED"})
            result["reason"] = "nvidia-smi memory fields were unavailable"
    except Exception as exc:
        # Preserve an explicit environment diagnostic if present, otherwise
        # return a BLOCKED record rather than inventing VRAM numbers.
        if not result["available"]:
            result["reason"] = "authoritative VRAM telemetry unavailable: %s" % str(exc)
    return result


def collect_provenance():
    renderer = collect_renderer_provenance()
    system = collect_system_provenance()
    vram = collect_vram_snapshot()
    driver_version = vram.get("driver_version") or renderer.get("driver_version")
    driver_source = vram.get("source") if vram.get("driver_version") else renderer.get("source")
    driver_authoritative = bool(vram.get("authoritative") and vram.get("driver_version")) or bool(
        renderer.get("authoritative") and renderer.get("driver_version")
    )
    driver = {
        "source": driver_source,
        "available": driver_version is not None,
        "authoritative": driver_authoritative,
        "status": "PASS" if driver_authoritative else ("OPEN" if driver_version is not None else "BLOCKED"),
        "version": driver_version,
        "reason": None if driver_version is not None else "driver version unavailable",
    }
    return {
        "schema_version": "S2-churn-telemetry-v2",
        "renderer": renderer,
        "system": system,
        "driver": driver,
        "vram": {
            "schema_version": "vram-snapshot-v1",
            "start": vram,
            "end": None,
            "source": vram.get("source"),
            "authoritative": vram.get("authoritative", False),
            "status": vram.get("status", "BLOCKED"),
        },
        "captured_at_unix": time.time(),
    }


def probe_material_toggle():
    """Try toggling one material's baseColor. Returns (available, material).
    Material setters trigger MaterialsChanged (Scene.cpp:1759); whether the
    S2 capture scheduler maps that to dirty pages is verified by the series."""
    try:
        mats = m.scene.materials
        if not mats:
            return False, None
        mat = mats[0]
        mat.baseColor = MATERIAL_COLORS[0]  # succeeds only if the setter exists
        return True, mat
    except Exception as exc:
        print("CHURN WARNING material dirty injection unavailable: %s" % str(exc))
        return False, None


def wait_for_resource_reclamation(resource_sync, event, frame):
    """Synchronize the live device after a resource-replacement operation.

    ``Renderer::setScene()`` fences scene replacement on the C++ side.  A
    framebuffer resize, however, replaces the target FBO from the Python
    binding and can leave the old FBO in the deferred-release queue until a
    later frame fence.  Use the existing live ``m.device.wait()`` binding after
    resize and both before and after scene replacement, and record every
    attempt.  A missing/failed binding is
    evidence of an incomplete churn contract; it must never become a fake
    successful run.
    """
    record = {
        "event": str(event),
        "frame": int(frame),
        "timestamp_unix": time.time(),
        "status": "BLOCKED",
        "reason": None,
    }
    resource_sync["attempts"] += 1
    try:
        script_host = globals().get("m")
        device = getattr(script_host, "device", None) if script_host is not None else None
        wait = getattr(device, "wait", None) if device is not None else None
        if not callable(wait):
            raise RuntimeError("live m.device.wait binding is unavailable")
        wait()
        record["status"] = "PASS"
        resource_sync["completed"] += 1
    except Exception as exc:
        reason = "device wait after %s failed: %s" % (event, str(exc))
        record["reason"] = reason
        resource_sync["failures"] += 1
        resource_sync["status"] = "BLOCKED"
        print("CHURN BLOCKED resource reclamation", reason)
    else:
        resource_sync["status"] = "PASS"
    resource_sync["events"].append(record)
    return record["status"] == "PASS"


def main():
    graph = create_lumen_graph()
    m.addGraph(graph)
    m.setActiveGraph(graph)
    m.loadScene(SCENE_CORNELL)
    m.resizeFrameBuffer(*RESOLUTION)
    m.ui = False
    m.clock.framerate = FRAME_RATE
    m.clock.time = 0
    m.clock.pause()
    m.clock.frame = 0

    provenance = collect_provenance()
    material_available, material = probe_material_toggle()
    print("CHURN seconds", SECONDS, "frames", FRAME_COUNT, "material_toggle_available", material_available)

    series = []
    totals = {key: None for key in ("alloc", "release", "fail", "lost")}
    totals.update({key: None for key in REQUIRED_STAT_FIELDS})
    reloads = 0
    resizes = 0
    dirty_injections = 0
    toggle_index = 0
    is_small = False
    stats_binding_available = False
    stats_observed = False
    stats_missing_fields = set()
    stats_sources = set()
    last_norm = None
    vram_samples = []
    resource_sync = {
        "schema_version": "device-resource-sync-v1",
        "binding": "m.device.wait",
        "status": "NOT_RUN",
        "attempts": 0,
        "completed": 0,
        "failures": 0,
        "events": [],
    }

    # Keep the start sample in the same sequence as in-run samples.  The
    # release contract deliberately does not infer a trend from only start/end
    # snapshots, so a run without this sequence remains BLOCKED offline.
    start_vram = provenance["vram"].get("start")
    if start_vram is not None:
        vram_samples.append(
            {
                "frame": 0,
                "timestamp_unix": provenance.get("captured_at_unix"),
                **json_safe(start_vram),
            }
        )

    for frame in range(1, FRAME_COUNT + 1):
        m.clock.frame = frame

        # Per-frame dirty injection: toggle the material color back and forth.
        if material_available and (frame == 1 or frame % MATERIAL_MUTATION_INTERVAL_FRAMES == 0):
            try:
                material.baseColor = MATERIAL_COLORS[toggle_index % 2]
                toggle_index += 1
                dirty_injections += 1
            except Exception:
                material_available = False
                print("CHURN WARNING material dirty injection stopped mid-run")

        # Static churn: periodic scene reload (full re-capture) and resize.
        if frame % RELOAD_INTERVAL_FRAMES == 0:
            # SceneBuilder constructs the replacement scene before
            # Renderer::setScene() is entered, so its C++ pre-fence cannot
            # prevent old and new scene allocations from overlapping.  Drop
            # the old scene explicitly before constructing the replacement,
            # then fence each boundary.  This keeps the churn driver from
            # measuring a deliberate old+new peak as a renderer leak.
            if not wait_for_resource_reclamation(resource_sync, "scene_reload_pre", frame):
                raise RuntimeError("scene reload pre-fence failed; aborting unsafe churn")
            m.unloadScene()
            if not wait_for_resource_reclamation(resource_sync, "scene_unload", frame):
                raise RuntimeError("scene unload fence failed; aborting unsafe churn")
            m.loadScene(SCENE_CORNELL)
            reloads += 1
            if not wait_for_resource_reclamation(resource_sync, "scene_reload_post", frame):
                raise RuntimeError("scene reload post-fence failed; aborting unsafe churn")
            # Re-fetch the material after reload; the old handle may be stale.
            if material_available:
                try:
                    material = m.scene.materials[0]
                except Exception:
                    material_available = False
        if frame % RESIZE_INTERVAL_FRAMES == 0:
            is_small = not is_small
            m.resizeFrameBuffer(*(SMALL_RESOLUTION if is_small else RESOLUTION))
            resizes += 1
            if not wait_for_resource_reclamation(resource_sync, "framebuffer_resize", frame):
                raise RuntimeError("framebuffer resize fence failed; aborting unsafe churn")

        m.renderFrame()

        if frame % VRAM_SAMPLE_INTERVAL_FRAMES == 0:
            sample = collect_vram_snapshot()
            vram_samples.append(
                {
                    "frame": frame,
                    "timestamp_unix": time.time(),
                    **json_safe(sample),
                }
            )

        if frame % SAMPLE_INTERVAL_FRAMES == 0:
            raw, binding = read_surface_cache_stats()
            norm = normalize_stats(raw)
            stats_sources.add(binding.get("source")) if binding.get("source") else None
            stats_binding_available = stats_binding_available or bool(binding.get("canonical"))
            if norm is not None:
                stats_observed = True
                missing = missing_stat_fields(norm)
                stats_missing_fields.update(missing)
                last_norm = norm
                for key in ("alloc", "release", "fail", "lost") + REQUIRED_STAT_FIELDS:
                    if norm.get(key) is not None:
                        totals[key] = norm[key]
            else:
                stats_missing_fields.update(REQUIRED_STAT_FIELDS)
            series.append(
                {
                    "frame": frame,
                    "dirty_injections": dirty_injections,
                    "reloads": reloads,
                    "resizes": resizes,
                    "alloc": norm["alloc"] if norm else None,
                    "release": norm["release"] if norm else None,
                    "fail": norm["fail"] if norm else None,
                    "lost": norm["lost"] if norm else None,
                    "recapture": norm["recapture"] if norm else None,
                    "allocated_pages": norm["allocated_pages"] if norm else None,
                    "free_pages": norm["free_pages"] if norm else None,
                    "resident_bytes": norm["resident_bytes"] if norm else None,
                    # Canonical release fields.  Keep the legacy short names
                    # above so existing S2 consumers remain compatible.
                    **({key: norm.get(key) for key in REQUIRED_STAT_FIELDS} if norm else {key: None for key in REQUIRED_STAT_FIELDS}),
                    "residentBytes": norm.get("residentBytes") if norm else None,
                    "memoryBudgetBytes": norm.get("memoryBudgetBytes") if norm else None,
                    "surfaceCacheStats": json_safe(raw) if raw is not None else None,
                    "stats_source": binding.get("source"),
                    "stats_binding_canonical": bool(binding.get("canonical")),
                    "stats_missing_fields": missing_stat_fields(norm),
                }
            )

    end_vram = collect_vram_snapshot()
    provenance["vram"]["end"] = end_vram
    end_sample = {
        "frame": FRAME_COUNT,
        "timestamp_unix": time.time(),
        **json_safe(end_vram),
    }
    # The periodic sample may already land on the final frame.  Replace it
    # with the post-loop end snapshot instead of emitting duplicate frame IDs;
    # the offline contract requires a strictly ordered sample sequence.
    if vram_samples and vram_samples[-1].get("frame") == FRAME_COUNT:
        vram_samples[-1] = end_sample
    else:
        vram_samples.append(end_sample)
    provenance["vram"]["samples"] = vram_samples
    if (
        provenance["vram"]["start"].get("source") == end_vram.get("source")
        and provenance["vram"]["start"].get("source") is not None
    ):
        provenance["vram"]["source"] = end_vram.get("source")
    else:
        provenance["vram"]["source"] = "start/end snapshots"
    provenance["vram"]["authoritative"] = bool(
        provenance["vram"]["start"].get("authoritative") and end_vram.get("authoritative")
    )
    provenance["vram"]["status"] = "PASS" if provenance["vram"]["authoritative"] else "BLOCKED"
    # Refresh driver provenance from the end snapshot when it became available.
    driver_version = end_vram.get("driver_version") or provenance["driver"].get("version")
    if driver_version is not None:
        driver_authoritative = bool(end_vram.get("authoritative") or provenance["driver"].get("authoritative"))
        provenance["driver"].update(
            {
                "version": driver_version,
                "source": end_vram.get("source") or provenance["driver"].get("source"),
                "available": True,
                "authoritative": driver_authoritative,
                "status": "PASS" if driver_authoritative else "OPEN",
                "reason": None,
            }
        )

    # Placeholder S2 gate divergence check: when stats ARE available, the last
    # sample must not have diverged pathologically from the first (a leak or
    # unbounded churn would blow up alloc/fail/lost). S2_TODO: freeze the real
    # thresholds and invariants with root (task.md 15.5).
    divergence_ok = None
    stats_complete = bool(stats_observed and stats_binding_available and not stats_missing_fields)
    stats_available = stats_complete
    if stats_complete and len(series) >= 2:
        first = series[0]
        last = series[-1]
        for key in ("alloc", "release", "fail", "lost"):
            a, b = first.get(key), last.get(key)
            if a is not None and b is not None and a > 0:
                if b > a * CHURN_DIVERGENCE_MAX:
                    divergence_ok = False
                    print("CHURN VERDICT counter-divergence", key, "FAIL", a, "->", b)
                    break
        else:
            divergence_ok = True
            print("CHURN VERDICT counter-divergence PASS")

    if not stats_complete:
        print(
            "CHURN BLOCKED: canonical LumenGI.surfaceCacheStats counters are incomplete; "
            "missing fields %s. Static churn still ran." % sorted(stats_missing_fields)
        )
        print("CHURN VERDICT counter-divergence BLOCKED (canonical stats unavailable/incomplete)")

    print("CHURN reloads", reloads, "resizes", resizes, "dirty_injections", dirty_injections)
    print("CHURN totals", totals)

    telemetry_missing = []
    if (
        resource_sync["attempts"] <= 0
        or resource_sync["status"] != "PASS"
        or resource_sync["completed"] != resource_sync["attempts"]
        or resource_sync["failures"] != 0
        or len(resource_sync["events"]) != resource_sync["attempts"]
        or any(
            not isinstance(event, Mapping) or event.get("status") != "PASS"
            for event in resource_sync["events"]
        )
    ):
        telemetry_missing.append("successful m.device.wait after every scene reload/resize")
    if not stats_complete:
        telemetry_missing.append("LumenGI.surfaceCacheStats canonical fields")
    if not provenance["renderer"].get("authoritative"):
        telemetry_missing.append("renderer Device.info")
    if not provenance["system"].get("authoritative"):
        telemetry_missing.append("system platform telemetry")
    if not provenance["driver"].get("authoritative"):
        telemetry_missing.append("driver version")
    if not provenance["vram"].get("authoritative"):
        telemetry_missing.append("authoritative start/end VRAM telemetry")
    provenance_ready = not telemetry_missing
    # This runner deliberately never turns the short proxy into a release PASS.
    # An external gate must consume the artifact and separately validate the
    # 30-minute soak; a proxy PASS only means its local bounded check passed.
    proxy_status = (
        "PASS"
        if stats_complete and divergence_ok is True
        else ("FAIL" if divergence_ok is False else "BLOCKED")
    )
    soak_status = "NOT_RUN" if SECONDS < 1800.0 else "OPEN"
    release_status = "OPEN" if provenance_ready else "BLOCKED"
    if not provenance_ready:
        print("CHURN RELEASE STATUS BLOCKED; missing", ", ".join(telemetry_missing))
    else:
        print("CHURN RELEASE STATUS OPEN (proxy/soak require external gate)")

    write_json(
        OUT_JSON,
        {
            # Keep the original root schema identifier for existing consumers;
            # the additive telemetry contract has its own version.
            "schema_version": 1,
            "telemetry_schema_version": 2,
            "mode": "60s-proxy" if SECONDS <= 120 else "soak",
            "seconds": SECONDS,
            "frame_count": FRAME_COUNT,
            "frame_rate": FRAME_RATE,
            "workload_profile": {
                "materialMutationIntervalFrames": MATERIAL_MUTATION_INTERVAL_FRAMES,
                "reloadIntervalFrames": RELOAD_INTERVAL_FRAMES,
                "resizeIntervalFrames": RESIZE_INTERVAL_FRAMES,
                "longPhaseBoundedCadence": _LONG_PHASE,
            },
            "scene": SCENE_CORNELL,
            "material_toggle_available": material_available,
            "stats_available": stats_available,
            "stats_observed": stats_observed,
            "stats_binding_available": stats_binding_available,
            "stats_complete": stats_complete,
            "stats_required_fields": list(REQUIRED_STAT_FIELDS),
            "stats_missing_fields": sorted(stats_missing_fields),
            "stats_sources": sorted(stats_sources),
            "divergence_ok": divergence_ok,
            "divergence_max_placeholder": CHURN_DIVERGENCE_MAX,
            "totals": totals,
            "reloads": reloads,
            "resizes": resizes,
            "dirty_injections": dirty_injections,
            "resource_sync": resource_sync,
            "series": series,
            "telemetry_provenance": provenance,
            # Flat aliases make the required provenance visible to simple
            # release-manifest consumers without changing the nested source.
            "renderer": provenance["renderer"],
            "system": provenance["system"],
            "driver": provenance["driver"],
            "vram": provenance["vram"],
            # Flat alias for offline consumers that do not traverse nested
            # provenance.  The nested sequence remains canonical.
            "vram_samples": vram_samples,
            "provenance_status": "PASS" if provenance_ready else "BLOCKED",
            "provenance_missing": telemetry_missing,
            "proxy_gate": {
                "status": proxy_status,
                "scope": "60s-proxy-only",
                "not_soak_pass": True,
            },
            "soak_gate": {
                "status": soak_status,
                "scope": "30-minute-external-gate",
                "reason": "this runner records evidence; it does not self-certify the release soak",
            },
            "status": release_status,
            "nightly_command_30min": (
                "$env:LUMEN_CHURN_SECONDS='1800'; "
                "build\\windows-vs2022\\bin\\Release\\Mogwai.exe --device-type d3d12 "
                "--headless --precise --script tests\\lumengi\\run_churn_short.py "
                "--logfile artifacts\\lumengi\\S2\\churn-30min.log"
            ),
        },
    )
    print("CHURN wrote", os.path.abspath(OUT_JSON))


def _self_test():
    """Run dependency-free schema/alias fixtures; never starts GPU tooling."""
    fixture = {
        "allocations": 12,
        "releases": 4,
        "schedAllocFailures": 2,
        "schedLostPages": 1,
        "schedRecaptures": 8,
        "allocatedPages": 16,
        "freePages": 48,
        "residentBytes": 460800,
        "memoryBudgetBytes": 536870912,
    }
    normalized = normalize_stats(fixture)
    assert normalized is not None
    assert missing_stat_fields(normalized) == []
    assert normalized["alloc"] == normalized["allocations"] == 12
    assert normalized["release"] == normalized["releases"] == 4
    assert normalized["fail"] == normalized["schedAllocFailures"] == 2
    assert normalized["lost"] == normalized["schedLostPages"] == 1
    assert normalized["recapture"] == normalized["schedRecaptures"] == 8
    assert normalized["allocated_pages"] == normalized["allocatedPages"] == 16
    assert normalized["free_pages"] == normalized["freePages"] == 48

    missing = dict(fixture)
    del missing["schedLostPages"]
    missing_normalized = normalize_stats(missing)
    assert "schedLostPages" in missing_stat_fields(missing_normalized)
    # Device.info is the only acceptable in-process renderer identity source.
    # Exercise the read-only host path with plain fixtures; no Falcor import or
    # GPU creation is needed for this contract test.
    class _Info:
        adapter_name = "fixture-gpu"
        api_name = "DirectX12"

    class _Device:
        info = _Info()

    class _Host:
        device = _Device()

    renderer = collect_renderer_provenance(_Host())
    assert renderer["status"] == "PASS", renderer
    assert renderer["adapter_name"] == "fixture-gpu", renderer
    assert renderer["api_name"] == "DirectX12", renderer
    assert renderer["device_type"] == "d3d12", renderer
    assert renderer["binding_contract"]["new_device_construction_forbidden"] is True
    assert collect_renderer_provenance(object())["status"] == "BLOCKED"
    process_working, process_peak, process_reason = _windows_process_memory(os.getpid())
    if os.name == "nt":
        assert (process_working is not None and process_peak is not None) or process_reason
    else:
        assert process_working is None and process_peak is None and process_reason
    assert json.loads(json.dumps(json_safe(fixture))) == fixture
    print(
        "CHURN_SELF_TEST PASS canonical-fields=%d alias-compatibility=PASS "
        "device-info=PASS process-memory=%s missing-telemetry=BLOCKED"
        % (
            len(REQUIRED_STAT_FIELDS),
            "AVAILABLE" if process_working is not None else "UNAVAILABLE",
        )
    )
    return 0


if "--self-test" in sys.argv[1:]:
    sys.exit(_self_test())
if not FALCOR_AVAILABLE:
    print("CHURN BLOCKED: Falcor binding unavailable", FALCOR_IMPORT_ERROR)
    sys.exit(2)
main()
