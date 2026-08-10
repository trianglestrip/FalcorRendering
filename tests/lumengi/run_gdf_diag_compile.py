"""Runtime compile gate for the bounded C4 E1/E2 diagnostic shaders.

This deliberately does not dispatch or touch the production LumenGI pass. It only
asks the real Mogwai device/compiler to reflect both diagnostic entry points so a
later host descriptor-bisection wave starts from a known shader contract.
"""

import json
import os
import gc

from falcor import ComputePass, Device, ProgramDesc


OUT = os.environ.get("LUMEN_GDF_DIAG_COMPILE_OUT", "artifacts/lumengi/C4/gdf-diag-compile.json")
SHADERS = [
    "RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiag.cs.slang",
    "RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiagAll.cs.slang",
]


def main():
    report = {"schema": "LumenGI.C4.DiagCompile.v1", "shaders": [], "status": "PASS"}
    # Prefer Mogwai's already-created device. Creating a second D3D12 device in a
    # headless app can make shutdown report DXGI_DEVICE_REMOVED even when shader
    # reflection itself succeeded.
    app = globals().get("m")
    device = getattr(app, "device", None) if app is not None else None
    if device is None:
        # Match the established runtime shader gates: initialize a real scene before
        # opening the dedicated debug-layer device, otherwise the empty Mogwai app can
        # tear down the transient heap while the second device is still compiling.
        if app is not None:
            app.loadScene("test_scenes/cornell_box.pyscene")
            app.resizeFrameBuffer(64, 64)
            app.ui = False
            app.clock.pause()
        try:
            device = Device(enable_debug_layer=True)
        except Exception:
            device = Device(enable_debug_layer=False)

    for shader in SHADERS:
        item = {"shader": shader, "status": "PASS"}
        try:
            desc = ProgramDesc()
            desc.add_shader_module().add_file(shader)
            desc.cs_entry("main")
            ComputePass(device, desc, {})
        except Exception as exc:  # pragma: no cover - exercised by Mogwai runtime.
            item["status"] = "FAIL"
            item["error"] = repr(exc)
            report["status"] = "FAIL"
        report["shaders"].append(item)

    # Release the temporary compiler device before Mogwai's own shutdown wait.
    device = None
    gc.collect()
    parent = os.path.dirname(OUT)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print("GDFDIAG", report["status"], OUT)


# Falcor's embedded Python executes scripts as `builtins`, so the main guard is
# not entered. Keep the same module-level convention as the other Mogwai gates.
main()
exit()
