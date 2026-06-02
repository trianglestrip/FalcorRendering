# Filament Strict Implementation Plan

## 1. Overview
The goal is to strictly replicate Filament's lighting, shadow, sky, post-processing, and UI layouts without simplified approximations. We will build a multi-pass architecture within Falcor that mirrors Filament's FrameGraph approach for post-processing.

## 2. UI & Parameter Synchronization
Target: Replicate Filament's `gltf_viewer` ImGui layout exactly.
- **View**: Anti-aliasing (FXAA, TAA), Dithering, Post-processing Enable toggle.
- **Light / IBL**: Show Skybox toggle, IBL Intensity, IBL Rotation. Sun intensity, color, direction.
- **Shadows**: Enable, Type (PCF/VSM), Cascades.
- **Post-processing Options**:
  - **SSAO**: Enable, Radius, Bias, Power.
  - **Bloom**: Enable, Strength, Levels (up to 7), Blend Mode.
  - **Depth of Field (DoF)**: Enable, Focal Distance, Aperture, Max CoC.
  - **Vignette**: Enable, Midpoint, Roundness, Feather, Color.
  - **Color Grading**: Tone mapping curve (ACES, Filmic, Display, Linear), Exposure, Contrast, Vibrance, Saturation.

## 3. Multi-Pass Post-Processing Architecture
Instead of a single "fake" compute pass, `FilamentPostProcess` will manage multiple `ComputePass` instances representing Filament's pipeline.

### Passes to Implement:
1. **Bloom**:
   - Create a mipmapped intermediate texture.
   - `BloomDownsample.cs.slang`: 13-tap downsample (Kawase-like, matching Filament). Iterate for N levels.
   - `BloomUpsample.cs.slang`: 3x3 tent filter upsample and accumulate.
2. **SSAO**:
   - `SSAOCompute.cs.slang`: Horizon-Based or Scalable Ambient Obscurance based on raw depth.
   - `SSAOBlur.cs.slang`: Edge-preserving separable blur (X and Y passes).
3. **Depth of Field (DoF)**:
   - `DoFCoC.cs.slang`: Calculate Circle of Confusion from depth.
   - `DoFBlur.cs.slang`: Gather-based scatter/gather blur using CoC.
4. **Color Grading & Tone Mapping**:
   - `ColorGrading.cs.slang`: Strictly apply Filament's math sequence:
     1. Exposure
     2. Tone Mapping (Filament's exact ACES/Filmic curves)
     3. Color Grading (Contrast, Vibrance, Saturation)
     4. Dithering

## 4. Execution Steps
1. Write this MD plan.
2. Overhaul `PBRTOfflineRenderer` UI to strictly match Filament's layout and parameters.
3. Refactor `FilamentPostProcess` C++ class to allocate multiple ComputePasses and intermediate mipmapped textures.
4. Write strict Slang shaders (`Bloom.cs.slang`, `ColorGrading.cs.slang`, etc.) using Filament's exact math.
5. Integrate the rendering loop and test.

## 5. Implemented Pipeline (Current)

| Stage | Pass | Status |
|-------|------|--------|
| 0 | Shadow depth raster (`ShadowDepth.3d.slang`) | Done |
| 0b | Shadow visibility PCF (`ShadowMap.cs.slang`) | Done |
| 1 | SSAO + bilateral blur (`SSAO.cs.slang`) | Done |
| 1b | Depth of Field (`DoF.cs.slang`) | Done |
| 2 | Bloom down/up (`Bloom.cs.slang`) | Done |
| 3 | Color grading + AO/Shadow/Bloom/Vignette/Tone map/Dither (`ColorGrading.cs.slang`) | Done |
| 4 | TAA (`TAA.cs.slang`) or FXAA (`FXAA.cs.slang`) | Done |

UI groups: View, Light (Sun), Environment Map (IBL), Shadows, Post-processing (SSAO, Bloom, DoF, Vignette, Color Grading).
