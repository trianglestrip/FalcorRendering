add_falcor_executable(PBRTOfflineRenderer)

target_include_directories(PBRTOfflineRenderer PRIVATE ${CMAKE_SOURCE_DIR}/third_party/taskflow)
target_include_directories(PBRTOfflineRenderer PRIVATE ${CMAKE_SOURCE_DIR}/Source/RenderPasses/FilamentFX)

target_sources(PBRTOfflineRenderer PRIVATE
    PBRTOfflineRenderer.cpp
    PBRTOfflineRenderer.h
    PBRTOfflineRenderer.3d.slang
    ForwardFlat.3d.slang
    PBRTGBuffer.3d.slang
    PBRTIBLLighting.ps.slang
    ShadowDepth.3d.slang
)

# Link against FilamentPostProcessLib (standalone static library)
target_link_libraries(PBRTOfflineRenderer PRIVATE FilamentPostProcessLib)

target_copy_shaders(PBRTOfflineRenderer Samples/PBRTOfflineRenderer)
target_copy_shaders(PBRTOfflineRenderer RenderPasses/FilamentFX)

# Deploy Filament default IBL assets (data/ibl/) to bin/<Config>/data/ibl/.
add_dependencies(PBRTOfflineRenderer copy_data_folder)

