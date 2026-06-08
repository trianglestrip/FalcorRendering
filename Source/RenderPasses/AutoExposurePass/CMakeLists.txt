add_plugin(AutoExposurePass)

target_sources(AutoExposurePass PRIVATE
    AutoExposurePass.cpp
    AutoExposurePass.h
    AutoExposure.cs.slang
)

target_copy_shaders(AutoExposurePass RenderPasses/AutoExposurePass)

target_source_group(AutoExposurePass "RenderPasses")

# Also build as static lib for PBRTOfflineRenderer direct linking
add_library(AutoExposurePassLib STATIC
    AutoExposurePass.cpp
    AutoExposurePass.h
    AutoExposure.cs.slang
)

target_compile_definitions(AutoExposurePassLib PRIVATE FALCOR_NO_PLUGIN_REGISTRATION)

target_include_directories(AutoExposurePassLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(AutoExposurePassLib PUBLIC Falcor)
target_copy_shaders(AutoExposurePassLib RenderPasses/AutoExposurePass)
target_source_group(AutoExposurePassLib "RenderPasses")
