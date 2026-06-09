add_library(NaniteCore STATIC)

target_sources(NaniteCore PRIVATE
    NaniteTypes.h
    NaniteAssetData.h
    NaniteGpuTypes.h
    NaniteRuntime.cpp
    NaniteCompress.h
    NaniteCompress.cpp
    NaniteFormat.h
    NaniteFormat.cpp
)

target_compile_features(NaniteCore PUBLIC cxx_std_20)
target_include_directories(NaniteCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_source_group(NaniteCore "Falcor/Scene/Nanite")
