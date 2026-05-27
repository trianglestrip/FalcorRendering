add_falcor_executable(PBRTOfflineRenderer)

target_include_directories(PBRTOfflineRenderer PRIVATE ${CMAKE_SOURCE_DIR}/third_party/taskflow)
target_include_directories(PBRTOfflineRenderer PRIVATE ${CMAKE_SOURCE_DIR}/Source/RenderPasses/FilamentFX)

target_sources(PBRTOfflineRenderer PRIVATE
    PBRTOfflineRenderer.cpp
    PBRTOfflineRenderer.h
)

# Link against FilamentPostProcessLib (standalone static library)
target_link_libraries(PBRTOfflineRenderer PRIVATE FilamentPostProcessLib)

# Copy shader includes to shader dir
# Copy shader includes to shader dir
target_copy_shaders(PBRTOfflineRenderer Samples/PBRTOfflineRenderer)
target_copy_shaders(PBRTOfflineRenderer RenderPasses/FilamentFX)

