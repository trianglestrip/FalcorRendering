# FilamentFX - Filament-style post processing, SSAO, shadow mapping library
# Builds as both a Falcor plugin AND a standalone static library

set(FILAMENT_FX_SHADER_FILES
    Utils/FilamentDepth.slangh
    Utils/FilamentGeometry.slangh
    Utils/FilamentNoise.slangh
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

# AO shader files now live in FilamentAOPass/ (decoupled)
set(FILAMENT_AO_EXTERNAL_SHADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/FilamentAO.slangh
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/FilamentSSAOUtils.slangh
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/FilamentSAOImpl.slangh
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/FilamentGTAOImpl.slangh
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/FilamentAOBilateral.slangh
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/StructurePass.cs.slang
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/SSAO.cs.slang
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/DeferredSSAO.cs.slang
    ${CMAKE_CURRENT_SOURCE_DIR}/../FilamentAOPass/GTAO.cs.slang
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
    ${FILAMENT_AO_EXTERNAL_SHADERS}
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
    ${FILAMENT_AO_EXTERNAL_SHADERS}
)

target_compile_definitions(FilamentFX PRIVATE BUILDING_FILAMENT_FX)

target_copy_shaders(FilamentFX RenderPasses/FilamentFX)
# Use custom source_group to handle files from both FilamentFX and FilamentAOPass
get_target_property(_fx_sources FilamentFX SOURCES)
get_target_property(_fx_source_dir FilamentFX SOURCE_DIR)
set(_fx_common_root ${CMAKE_CURRENT_SOURCE_DIR}/..)
source_group(TREE ${_fx_common_root} FILES ${_fx_sources} PREFIX "")
set_target_properties(FilamentFX PROPERTIES FOLDER "RenderPasses")

get_target_property(_lib_sources FilamentPostProcessLib SOURCES)
get_target_property(_lib_source_dir FilamentPostProcessLib SOURCE_DIR)
source_group(TREE ${_fx_common_root} FILES ${_lib_sources} PREFIX "")
set_target_properties(FilamentPostProcessLib PROPERTIES FOLDER "RenderPasses")
