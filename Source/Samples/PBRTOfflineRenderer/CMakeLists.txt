add_falcor_executable(PBRTOfflineRenderer)

target_sources(PBRTOfflineRenderer PRIVATE
    PBRTOfflineRenderer.cpp
    PBRTOfflineRenderer.h
)

target_copy_shaders(PBRTOfflineRenderer Samples/PBRTOfflineRenderer)

target_source_group(PBRTOfflineRenderer "Samples")
