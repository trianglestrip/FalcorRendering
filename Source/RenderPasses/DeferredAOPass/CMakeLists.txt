add_plugin(DeferredAOPass)

target_sources(DeferredAOPass PRIVATE
    DeferredAOPass.cpp
    DeferredAOPass.h
    DeferredAOPass.cs.slang
)

target_copy_shaders(DeferredAOPass RenderPasses/DeferredAOPass)

target_source_group(DeferredAOPass "RenderPasses")

# Also build as static lib for PBRTOfflineRenderer direct linking
add_library(DeferredAOPassLib STATIC
    DeferredAOPass.cpp
    DeferredAOPass.h
    DeferredAOPass.cs.slang
)

target_compile_definitions(DeferredAOPassLib PRIVATE FALCOR_NO_PLUGIN_REGISTRATION)

target_include_directories(DeferredAOPassLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(DeferredAOPassLib PUBLIC Falcor)
target_copy_shaders(DeferredAOPassLib RenderPasses/DeferredAOPass)
target_source_group(DeferredAOPassLib "RenderPasses")
