# PBRT Viewer vs Filament Viewer Gap Analysis

Baseline date: 2026-06-07

Falcor entry point: `Source/Samples/PBRTOfflineRenderer`

Target executable: `build/windows-vs2022/bin/Release/pbrt_viewer.exe`

Filament references: `ViewerGui.cpp`, `Options.h`, `Renderer.cpp`, `PostProcessManager.cpp`

## Current Status

The Falcor PBRT viewer is now much closer to a Filament-style preview path than the original raster preview. It has:

- Filament-style UI groups matching the viewer panels: View, Bloom Options, TAA Options, SSAO Options, Screen-space reflections Options, Dynamic Resolution, Light, Fog, Scene, Camera, Debug Options, and Color Grading.
- IBL intensity exposed and bound as a preview-friendly `0..10` scale.
- Deferred GBuffer lighting in `PBRTIBLLighting.ps.slang`.
- Filament-style PBR direct sunlight through `surfaceShading()`.
- Deferred SSAO driven by depth plus GBuffer normals.
- Sun shadow map atlas, PCF/VSM resource binding, and screen-space shadow visibility sampling.
- Headless debug views for AO, shadow visibility, shadow map, cascade, shadow UV, shadow Z, and shadow delta.

## Verified Debug Images

Generated under `docs/development/debug_images/` using:

```powershell
build\windows-vs2022\bin\Release\pbrt_viewer.exe --headless --preview --single-frame --scene D:\models\pbrt-v4-scenes\barcelona-pavilion\pavilion-day.pbrt --enable-shadows --width 1280 --height 720
```

Key outputs:

- `pavilion_ao_reconstruct_fixed.png`: AO debug output.
- `pavilion_shadow_uv_reconstruct_fixed.png`: verifies deferred world-position reconstruction now varies correctly across the scene.
- `pavilion_shadow_compare_reconstruct_fixed.png`: verifies shadow visibility is no longer all white and receives tree/roof occlusion structure.
- `pavilion_final_shadow_compare_reconstruct_fixed.png`: final lit output after AO/material/shadow fixes.
- `pavilion_shadowmap_forward_fixed.png`: shadow atlas depth debug.
- `pavilion_final_5items_stable.png`: latest stable output after IBL/material/shadow/AO/color-grading updates.
- `pavilion_ao_5items_stable.png`: latest AO debug output.
- `pavilion_shadow_5items_stable.png`: latest shadow visibility output.
- `pavilion_shadowuv_5items_stable.png`: latest shadow UV reconstruction output.

The scene still logs a non-fatal PBRT/RGL measured material warning for `cm_white_spec.bsdf`; rendering and image output continue.

## Fixes Completed In This Pass

### UI

The UI is organized to mirror Filament viewer panels first, as requested. Some entries are currently UI-compatible rather than algorithm-complete. For example, Filament tone mapper names are exposed, but unsupported modes map to the closest current implementation; Filament shadow type names are exposed, while current rendering supports PCF Hard, PCF Low, and VSM.

### AO

The main preview path now uses deferred SSAO with GBuffer normals instead of the older depth-only pre-pass path. AO quality now responds to quality and low-pass settings, not only sample count. Remaining difference: Filament AO options such as bent normals, SSCT, dominant light shadows, and exact upsampling semantics are still incomplete.

### Material And Direct Light

Direct sun lighting no longer uses the older rough Blinn/Phong approximation. It now routes through the Filament-style BRDF helper via `surfaceShading()`. Remaining difference: indirect IBL is still SH/procedural fallback rather than full Filament prefiltered cubemap plus DFG LUT, so glossy material response still differs from Filament.

The GBuffer material mapping now uses a stronger PBRT-to-Filament heuristic: diffuse/specular albedo are converted into base color, perceptual roughness is clamped to the Filament minimum, f0 is saturated, and metallic-like PBRT materials can use colored specular as the base/f0 source.

### Shadows

The current deferred shadow path now works:

- Shadow resources are bound into `PBRTIBLLighting.ps.slang`.
- Deferred world-position reconstruction was fixed by using `mul(gInvViewProj, clip)` and D3D screen-to-NDC Y conversion.
- Light view-projection matrix composition was fixed for Falcor row-vector use.
- PCF compare and bias direction were adjusted for the current light projection depth convention.
- Cascade tile bounds now reject invalid `shadowUV`/Z before sampling.

Remaining difference: the result is still a basic CSM atlas implementation. Filament's full shadow system includes more stable fitting, additional soft shadow modes, VSM options, and more complete atlas management.

Stable shadow mode now snaps cascade orthographic bounds to shadow texels to reduce shimmer.

### IBL

The deferred preview path now uses Filament-style DFG/Fresnel energy terms and the `0..10` IBL intensity range. A true deferred cubemap sample path was tested but triggered `DXGI_ERROR_DEVICE_REMOVED` with the current procedural fallback cubemap on this D3D12 setup, so the stable deferred path intentionally uses SH/procedural radiance plus analytic DFG approximation. `FilamentIBL` still loads/binds the specular cubemap and DFG resources for the forward/shared IBL path and for a future isolated cubemap fix.

### Color Grading

Tone mapping now follows the Filament UI enum directly: LINEAR, ACES_LEGACY, ACES, FILMIC, AGX, GENERIC, PBR_NEUTRAL, GT7, and DISPLAY_RANGE. Several modes are approximations, but the UI no longer collapses unsupported modes into ACES.

## Remaining Visual Gaps

| Area | Current Falcor State | Remaining Gap vs Filament |
|---|---|---|
| AO | Normal-aware deferred SSAO is active and debugged. | Exact Filament AO upsampling, bent normals, SSCT, dominant light shadows, and tuning still differ. |
| Materials | Direct sun uses Filament-style BRDF helper. | PBRT material extraction is simplified; clear coat, sheen, anisotropy, transmission, cloth, subsurface, and measured material behavior are incomplete. |
| IBL | SH/procedural radiance with analytic DFG/Fresnel energy and `0..10` intensity. | True deferred prefiltered cubemap sampling is disabled until the device-removed issue is isolated. |
| Shadows | PCF/VSM atlas path is connected, visible, and stable mode snaps cascade bounds to texels. | Needs better split fitting, tuned normal/slope bias, DPCF/PCSS/PCFd, VSM high precision/MSAA/mips/aniso, and spot/point shadows. |
| Color grading | Filament tone-mapping enum is wired one-to-one with approximations for all listed modes. | Full Filament LUT generation, gamut mapping, temperature/tint, and tonal ranges are not exact. |
| TAA | Halton jitter and history path exist. | Velocity, rejection, resolve, transparent handling, and dynamic resolution interactions are not Filament-equivalent. |
| DoF | Single-pass approximation. | Filament multi-pass CoC/tile/median/dilate/combine path is not implemented. |
| SSR/Refraction | UI and structure-depth preparation only. | No visible Filament-equivalent SSR/refraction path yet. |
| Dynamic resolution | UI fields and RCAS sharpening exist. | No complete Filament dynamic-resolution/upscaler scheduling yet. |

## Priority Next Steps

1. Tune AO defaults per comparison scene: radius, power, intensity, bilateral threshold, and high-quality upsampling.
2. Improve PBRT material-to-GBuffer mapping: base color fallback, roughness/perceptual roughness consistency, metallic/f0 heuristics, and measured material fallback behavior.
3. Tune shadows after the visibility fix: bias, cascade split distances, cascade fitting, and PCF filter size.
4. Add full Filament IBL parity: DFG LUT plus prefiltered cubemap specular.
5. Replace UI-compatible placeholders with real implementations for SSR, dynamic resolution, VSM advanced options, and non-ACES tone mappers.

## Validation Policy

For each visual change, keep generating debug images, not only final beauty output:

- `--debug-view ao`
- `--debug-view shadow`
- `--debug-view shadowmap`
- `--debug-view shadow-uv`
- `--debug-view shadow-z`
- `--debug-view shadow-delta`

Use the same scene, camera, resolution, sun, IBL, exposure, and tone mapping when comparing to Filament. The most useful comparisons are AO, shadow visibility, HDR lighting before tone mapping, and final LDR output.
