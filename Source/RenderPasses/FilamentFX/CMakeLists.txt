# FilamentFX - Filament-style post processing, SSAO, shadow mapping library
# Builds as both a Falcor plugin AND a standalone static library

set(FILAMENT_FX_SHADER_FILES
    Utils/FilamentDepth.slangh
    Utils/FilamentGeometry.slangh
    Utils/FilamentNoise.slangh
    AO/FilamentAO.slangh
    AO/FilamentSSAOUtils.slangh
    AO/FilamentSAOImpl.slangh
    AO/FilamentGTAOImpl.slangh
    AO/FilamentAOBilateral.slangh
    AO/StructurePass.cs.slang
    AO/SSAO.cs.slang
    AO/DeferredSSAO.cs.slang
    AO/GTAO.cs.slang
    Shadow/FilamentShadow.slangh
    Shadow/ShadowMap.cs.slang
    Shadow/ShadowEVSM.cs.slang
    Lighting/FilamentPBR.slangh
    Lighting/FilamentIBL.slangh
    Lighting/FilamentIBLData.slang
    PostProcess/Bloom.cs.slang
    PostProcess/ColorGrading.cs.slang
    PostProcess/FXAA.cs.slang
    PostProcess/DoF.cs.slang
    PostProcess/Fog.cs.slang
    PostProcess/FSR.cs.slang
    PostProcess/TAA.cs.slang
    PostProcess/FilamentPostProcess.cs.slang
)

# Plugin build (for Falcor RenderGraph integration)
add_plugin(FilamentFX)

# Also build as standalone library for external consumers like PBRTOfflineRenderer
add_library(FilamentPostProcessLib STATIC
    FilamentPostProcess.cpp
    FilamentPostProcess.h
    FilamentIBL.cpp
    FilamentIBL.h
    ${FILAMENT_FX_SHADER_FILES}
)

target_copy_shaders(FilamentPostProcessLib RenderPasses/FilamentFX)

# Include path for FilamentPostProcess.h itself
target_include_directories(FilamentPostProcessLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Link against Falcor (provides Falcor.h, RenderGraph, etc.)
target_link_libraries(FilamentPostProcessLib PUBLIC Falcor)

target_sources(FilamentFX PRIVATE
    FilamentFX.cpp
    FilamentPostProcess.cpp
    FilamentPostProcess.h
    FilamentIBL.cpp
    FilamentIBL.h
    ${FILAMENT_FX_SHADER_FILES}
)

target_compile_definitions(FilamentFX PRIVATE BUILDING_FILAMENT_FX)

target_copy_shaders(FilamentFX RenderPasses/FilamentFX)
target_source_group(FilamentFX "RenderPasses")
