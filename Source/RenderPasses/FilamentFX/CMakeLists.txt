# FilamentFX - Filament-style post processing, SSAO, shadow mapping library
# Builds as both a Falcor plugin AND a standalone static library

# Plugin build (for Falcor RenderGraph integration)
add_plugin(FilamentFX)

# Also build as standalone library for external consumers like PBRTOfflineRenderer
add_library(FilamentPostProcessLib STATIC
    FilamentPostProcess.cpp
    FilamentPostProcess.h
)

# Include path for FilamentPostProcess.h itself
target_include_directories(FilamentPostProcessLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Link against Falcor (provides Falcor.h, RenderGraph, etc.)
target_link_libraries(FilamentPostProcessLib PUBLIC Falcor)

target_sources(FilamentFX PRIVATE
    FilamentFX.cpp
    FilamentPostProcess.cpp
    FilamentPostProcess.h
    FilamentPostProcess.cs.slang
    FilamentPBR.slangh
    Bloom.cs.slang
    ColorGrading.cs.slang
    FXAA.cs.slang
    SSAO.cs.slang
    ShadowMap.cs.slang
)

target_compile_definitions(FilamentFX PRIVATE BUILDING_FILAMENT_FX)

target_copy_shaders(FilamentFX RenderPasses/FilamentFX)
target_source_group(FilamentFX "RenderPasses")