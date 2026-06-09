add_plugin(NaniteRaster)

target_sources(NaniteRaster PRIVATE
    NaniteRaster.cpp
    NaniteRaster.h
    NaniteGBufferComposite.cpp
    NaniteGBufferComposite.h
)

target_copy_shaders(NaniteRaster RenderPasses/NaniteRaster)
target_source_group(NaniteRaster "RenderPasses")
