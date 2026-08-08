IMAGE_TEST = {
    "device_types": ["d3d12"],
}

"""LumenGI lighting-component toggle test (Arcade).

Purpose
-------
Verify that each indirect-light component (analytic, emissive, environment)
can be switched independently and disappears when disabled, and that
intensity steps and a camera translation produce valid frames. Uses the
LumenGI graph with a fixed-exposure ToneMapper (see graphs/LumenGILighting.py).

Usage
-----
Standard harness invocation:

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGILighting" --parallel 1

Run-only mode (no golden comparison until goldens are frozen):

    tests\\run_image_tests.bat --config windows-vs2022-Release --filter "LumenGILighting" --run-only --parallel 1

Determinism contract
--------------------
* Fixed resolution 640x360, fixed frame rate 60 fps.
* Fixed exposure: ToneMapper in the graph has autoExposure=False and
  exposureCompensation=0.0.
* Fixed seed: LumenGI PRNG is seeded per pixel and frame index (no host seed
  property; Agent B tracks seed handling). The PathTracer component used
  elsewhere for references fixes its own seed.
* Arcade provides two named analytic lights, the emissive Cabinet material
  and an environment map, so the toggles are exercised with real assets.

Inputs / outputs
----------------
* Scene: media/Arcade/Arcade.pyscene
* Captures: components-*.exr, analytic-intensity.exr, emissive-intensity.exr,
  environment-intensity.exr, camera-translation.exr
* Reference goldens: harness reference directory (root freezes with
  --gen-refs; never overwritten here).
* Threshold placeholder: MSE tolerance 1e-5 suggested for freeze (S1 Gate).

Status
------
RUN-ONLY: renders and captures only; golden comparison is the harness's job.
The scene renderSettings toggles follow Scene::setRenderSettings() semantics
(reassign the full settings object so the binding observes the change).
"""

import sys

sys.path.append("..")
from helpers import render_frames
from graphs.LumenGILighting import LumenGILighting as g
from falcor import *


def set_lighting_components(analytic, emissive, environment):
    # Reassign the settings object so Scene::setRenderSettings() observes the
    # complete change even if a Python binding returns a value copy.
    settings = m.scene.renderSettings
    settings.useAnalyticLights = analytic
    settings.useEmissiveLights = emissive
    settings.useEnvLight = environment
    m.scene.renderSettings = settings


m.addGraph(g)

# Arcade contains two named analytic lights, the emissive Cabinet material,
# and an environment map. These assets and bindings are defined by
# media/Arcade/Arcade.pyscene, so no runtime light creation is required here.
m.loadScene("Arcade/Arcade.pyscene")

# Component switches. Capture at a fixed frame count after each state change.
set_lighting_components(True, True, True)
render_frames(m, "components-all", frames=[8], resolution=[640, 360])

set_lighting_components(True, False, False)
render_frames(m, "components-analytic", frames=[8], resolution=[640, 360])

set_lighting_components(False, True, False)
render_frames(m, "components-emissive", frames=[8], resolution=[640, 360])

set_lighting_components(False, False, True)
render_frames(m, "components-environment", frames=[8], resolution=[640, 360])

set_lighting_components(False, False, False)
render_frames(m, "components-none", frames=[8], resolution=[640, 360])

# Restore all components before exercising granular update flags.
set_lighting_components(True, True, True)

point_light = m.scene.getLight("Point light")
point_light_intensity = point_light.intensity
point_light.intensity = point_light_intensity * 2.0
render_frames(m, "analytic-intensity", frames=[8], resolution=[640, 360])
point_light.intensity = point_light_intensity

cabinet = m.scene.get_material("Cabinet")
cabinet_emissive_factor = cabinet.emissiveFactor
cabinet.emissiveFactor = cabinet_emissive_factor * 2.0
render_frames(m, "emissive-intensity", frames=[8], resolution=[640, 360])
cabinet.emissiveFactor = cabinet_emissive_factor

env_map_intensity = m.scene.envMap.intensity
m.scene.envMap.intensity = env_map_intensity * 2.0
render_frames(m, "environment-intensity", frames=[8], resolution=[640, 360])
m.scene.envMap.intensity = env_map_intensity

# Camera translation validates temporal history handling while lighting is on.
camera = m.scene.camera
camera.position = camera.position + float3(0.15, 0.0, 0.0)
camera.target = camera.target + float3(0.15, 0.0, 0.0)
render_frames(m, "camera-translation", frames=[8], resolution=[640, 360])

exit()
