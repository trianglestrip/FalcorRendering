add_library(NaniteToolCore STATIC)

target_sources(NaniteToolCore PRIVATE
    Common/NaniteAsset.cpp
    Common/NaniteAsset.h
    Common/NaniteBuild.cpp
    Common/NaniteBuild.h
    Common/NaniteObj.cpp
    Common/NaniteObj.h
)

target_compile_features(NaniteToolCore PUBLIC cxx_std_20)
target_include_directories(NaniteToolCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Common
    ${CMAKE_SOURCE_DIR}/third_party/taskflow
)
target_source_group(NaniteToolCore "Tools/Nanite")

add_falcor_executable(NaniteBuilder)

target_sources(NaniteBuilder PRIVATE
    NaniteBuilder/NaniteBuilder.cpp
)

target_compile_features(NaniteBuilder PRIVATE cxx_std_20)
target_link_libraries(NaniteBuilder PRIVATE NaniteToolCore)
target_source_group(NaniteBuilder "Tools/Nanite")

add_falcor_executable(NaniteLoader)

target_sources(NaniteLoader PRIVATE
    NaniteLoader/NaniteLoader.cpp
)

target_compile_features(NaniteLoader PRIVATE cxx_std_20)
target_link_libraries(NaniteLoader PRIVATE NaniteToolCore)
target_source_group(NaniteLoader "Tools/Nanite")
