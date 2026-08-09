# MeshSDFBuilder - LumenGI S6-A1 standalone signed-distance-field builder.
# -----------------------------------------------------------------------------
# PURE C++17, standard library only: this tool must NOT link Falcor (its output,
# the ".msdf" volume, is consumed by the LumenGI render pass through the disk cache).
# Registered here so the tool builds with the rest of the solution; the CLI contract
# is documented in MeshSDFBuilderMain.cpp.
# -----------------------------------------------------------------------------
add_executable(MeshSDFBuilder
    MeshSDFBuilder.h
    MeshSDFBuilder.cpp
    MeshSDFBuilderMain.cpp
)

target_compile_features(MeshSDFBuilder PRIVATE cxx_std_17)
target_compile_options(MeshSDFBuilder PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

set_target_properties(MeshSDFBuilder PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${FALCOR_RUNTIME_OUTPUT_DIRECTORY}
    FOLDER "Tools"
)

if(MSVC)
    target_compile_options(MeshSDFBuilder PRIVATE /EHsc)
endif()
