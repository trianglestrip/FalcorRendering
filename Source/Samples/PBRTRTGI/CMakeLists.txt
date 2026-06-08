add_falcor_executable(PBRTRTGI)
set_target_properties(PBRTRTGI PROPERTIES OUTPUT_NAME pbrt_rtgi)

target_sources(PBRTRTGI PRIVATE
    PBRTRTGI.cpp
    PBRTRTGI.h
    PBRTRTGI.rt.slang
)

target_copy_shaders(PBRTRTGI Samples/PBRTRTGI)

target_source_group(PBRTRTGI "Samples")

