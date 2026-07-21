cmake_minimum_required(VERSION 3.15)
project(MiniObjViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Find MiniFalcorSDK ──────────────────────────────────────────
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../../MiniFalcorSDK")
include("${CMAKE_SOURCE_DIR}/../../MiniFalcorSDK/MiniFalcorConfig.cmake")

# ── Executable ───────────────────────────────────────────────────
add_executable(MiniObjViewer
    MiniObjViewer.cpp
    MiniObjViewer.h
)

target_link_libraries(MiniObjViewer PRIVATE MiniFalcor::Falcor)

# Python lib path for packman python
target_link_directories(MiniObjViewer PRIVATE
    "${CMAKE_SOURCE_DIR}/../../external/packman/python/libs"
)

target_compile_definitions(MiniObjViewer PRIVATE
    NOMINMAX UNICODE _USE_MATH_DEFINES
)

# ── Copy MiniFalcorSDK runtime to output ─────────────────────────
minifalcor_copy_runtime(MiniObjViewer)

# Copy media
add_custom_command(TARGET MiniObjViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/../media"
        "$<TARGET_FILE_DIR:MiniObjViewer>/media"
    COMMENT "Copying media files..."
)

# Copy shader
add_custom_command(TARGET MiniObjViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/MiniObjViewer.3d.slang"
        "$<TARGET_FILE_DIR:MiniObjViewer>/shaders/MiniObjViewer.3d.slang"
    COMMENT "Copying shader..."
)
