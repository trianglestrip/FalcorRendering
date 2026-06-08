add_falcor_executable(PBRTRealtimeGI)
set_target_properties(PBRTRealtimeGI PROPERTIES OUTPUT_NAME pbrt_realtime_gi)

target_include_directories(PBRTRealtimeGI PRIVATE ${CMAKE_SOURCE_DIR}/third_party/taskflow)
target_include_directories(PBRTRealtimeGI PRIVATE ${CMAKE_SOURCE_DIR}/Source/Samples/PBRTOfflineRenderer)
target_include_directories(PBRTRealtimeGI PRIVATE ${CMAKE_SOURCE_DIR}/Source/RenderPasses/FilamentFX)
target_include_directories(PBRTRealtimeGI PRIVATE ${CMAKE_SOURCE_DIR}/Source/RenderPasses/DeferredAOPass)
target_include_directories(PBRTRealtimeGI PRIVATE ${CMAKE_SOURCE_DIR}/Source/RenderPasses/AutoExposurePass)

target_compile_definitions(PBRTRealtimeGI PRIVATE PBRT_OFFLINE_RENDERER_LIBRARY)

target_sources(PBRTRealtimeGI PRIVATE
    PBRTRealtimeGI.cpp
    ../PBRTOfflineRenderer/PBRTOfflineRenderer.cpp
    ../PBRTOfflineRenderer/PBRTOfflineRenderer.h
    ../PBRTOfflineRenderer/PBRTOfflineRenderer.3d.slang
    ../PBRTOfflineRenderer/ForwardFlat.3d.slang
    ../PBRTOfflineRenderer/PBRTGBuffer.3d.slang
    ../PBRTOfflineRenderer/PBRTGBufferFast.3d.slang
    ../PBRTOfflineRenderer/PBRTIBLLighting.ps.slang
    ../PBRTOfflineRenderer/ShadowDepth.3d.slang
)

target_link_libraries(PBRTRealtimeGI PRIVATE FilamentPostProcessLib DeferredAOPassLib AutoExposurePassLib)

target_copy_shaders(PBRTRealtimeGI Samples/PBRTOfflineRenderer)
target_copy_shaders(PBRTRealtimeGI RenderPasses/FilamentFX)

add_dependencies(PBRTRealtimeGI copy_data_folder)

