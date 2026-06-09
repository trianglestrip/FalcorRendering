add_falcor_executable(NaniteViewer)

target_sources(NaniteViewer PRIVATE
    NaniteViewer.cpp
    NaniteViewer.h
    NaniteAABBDebug.3d.slang
)

target_copy_shaders(NaniteViewer Samples/NaniteViewer)

target_source_group(NaniteViewer "Samples")

add_dependencies(NaniteViewer NaniteRaster)
