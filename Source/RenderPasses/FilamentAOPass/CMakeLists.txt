set(FILAMENT_AO_SHADER_FILES
    FilamentAO.slangh
    FilamentSSAOUtils.slangh
    FilamentSAOImpl.slangh
    FilamentGTAOImpl.slangh
    FilamentAOBilateral.slangh
    StructurePass.cs.slang
    SSAO.cs.slang
    DeferredSSAO.cs.slang
    GTAO.cs.slang
)

add_plugin(FilamentAOPass)

target_sources(FilamentAOPass PRIVATE
    FilamentAOPass.cpp
    FilamentAOPass.h
)

target_link_libraries(FilamentAOPass PRIVATE FilamentPostProcessLib)

target_copy_shaders(FilamentAOPass RenderPasses/FilamentAOPass)

target_source_group(FilamentAOPass "RenderPasses")
