cmake_minimum_required(VERSION 3.15)
project(FalcorObjViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Find Falcor SDK ──────────────────────────────────────────────
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../FalcorSDK")
find_package(Falcor CONFIG REQUIRED)

# ── Executable ───────────────────────────────────────────────────
add_executable(FalcorObjViewer
    TestApp.cpp
    TestApp.h
)

target_link_libraries(FalcorObjViewer PRIVATE Falcor::Falcor)

# Falcor links against Python; add its lib path
target_link_directories(FalcorObjViewer PRIVATE "${CMAKE_SOURCE_DIR}/../external/packman/python/libs")

# Falcor public compile definitions needed
target_compile_definitions(FalcorObjViewer PRIVATE
    NOMINMAX
    UNICODE
    _USE_MATH_DEFINES
)

# ── Copy runtime DLLs and resources to output dir ────────────────
# Use local FalcorSDK directory for runtime files
if(EXISTS "${CMAKE_SOURCE_DIR}/../FalcorSDK/bin")
    add_custom_command(TARGET FalcorObjViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/../FalcorSDK/bin"
            "$<TARGET_FILE_DIR:FalcorObjViewer>"
        COMMENT "Copying Falcor runtime files..."
    )
else()
    add_custom_command(TARGET FalcorObjViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "SDK bin dir not found, skipping runtime copy"
    )
endif()

# Also copy the media directory for the example OBJ
add_custom_command(TARGET FalcorObjViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/media"
        "$<TARGET_FILE_DIR:FalcorObjViewer>/media"
    COMMENT "Copying example media files..."
)

# Copy the shader into the shaders/ directory (where Falcor's shader compiler looks)
add_custom_command(TARGET FalcorObjViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/TestApp.3d.slang"
        "$<TARGET_FILE_DIR:FalcorObjViewer>/shaders/TestApp.3d.slang"
    COMMENT "Copying shader to shaders/..."
)


source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${CMAKE_SOURCE_DIR}/TestApp.3d.slang)
