# ============================================================
# MiniFalcorConfig.cmake
# Minimal Falcor SDK import config for consumer projects.
#
# Usage in CMakeLists.txt:
#   list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../MiniFalcorSDK")
#   find_package(MiniFalcor REQUIRED)
#   target_link_libraries(MyApp PRIVATE MiniFalcor::Falcor)
# ============================================================

set(MiniFalcor_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

set(MiniFalcor_RUNTIME_DIR "${MiniFalcor_SDK_ROOT}/bin")
set(MiniFalcor_SHADER_DIR  "${MiniFalcor_RUNTIME_DIR}/shaders")
set(MiniFalcor_INCLUDE_DIR "${MiniFalcor_SDK_ROOT}/include")
set(MiniFalcor_LIB_DIR     "${MiniFalcor_SDK_ROOT}/lib")

# ── Imported target: MiniFalcor::Falcor ──────────────────────
if(NOT TARGET MiniFalcor::Falcor)
    add_library(MiniFalcor::Falcor SHARED IMPORTED)

    set_target_properties(MiniFalcor::Falcor PROPERTIES
        IMPORTED_LOCATION "${MiniFalcor_RUNTIME_DIR}/Falcor.dll"
        IMPORTED_IMPLIB   "${MiniFalcor_LIB_DIR}/Falcor.lib"
        INTERFACE_INCLUDE_DIRECTORIES
            "${MiniFalcor_INCLUDE_DIR};${MiniFalcor_INCLUDE_DIR}/Falcor;${MiniFalcor_INCLUDE_DIR}/external/fmt/include;${MiniFalcor_INCLUDE_DIR}/external/pybind11/include;${MiniFalcor_INCLUDE_DIR}/external/vulkan-headers/include;${MiniFalcor_INCLUDE_DIR}/external/imgui;${MiniFalcor_INCLUDE_DIR}/external/imgui_addons;${MiniFalcor_INCLUDE_DIR}/external/include;${MiniFalcor_INCLUDE_DIR}/external/mikktspace;${MiniFalcor_INCLUDE_DIR}/external/packman/deps/include;${MiniFalcor_INCLUDE_DIR}/external/packman/slang/include;${MiniFalcor_INCLUDE_DIR}/external/packman/rtxdi/include"
        INTERFACE_COMPILE_FEATURES cxx_std_20
        INTERFACE_COMPILE_DEFINITIONS
            "NOMINMAX;UNICODE;_USE_MATH_DEFINES;_SCL_SECURE_NO_WARNINGS;_CRT_SECURE_NO_WARNINGS;_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING;_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS;IMGUI_USER_CONFIG=\"Utils/UI/ImGuiConfig.h\""
    )
endif()

# ── Helper: copy SDK runtime to output dir ────────────────────
function(minifalcor_copy_runtime target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "minifalcor_copy_runtime: target '${target}' does not exist")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MiniFalcor_RUNTIME_DIR}"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying MiniFalcorSDK runtime files..."
    )
endfunction()

message(STATUS "MiniFalcorSDK: ${MiniFalcor_SDK_ROOT}")
