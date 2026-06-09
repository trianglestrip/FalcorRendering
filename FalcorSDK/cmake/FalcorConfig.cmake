
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was FalcorConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

set(FALCOR_SDK_ROOT "${PACKAGE_PREFIX_DIR}")
set(FALCOR_RUNTIME_DIR "${FALCOR_SDK_ROOT}/bin")
set(FALCOR_SHADER_DIR "${FALCOR_RUNTIME_DIR}/shaders")
set(FALCOR_PLUGIN_DIR "${FALCOR_RUNTIME_DIR}/plugins")
set(FALCOR_DATA_DIR "${FALCOR_RUNTIME_DIR}/data")
set(FALCOR_SCRIPT_DIR "${FALCOR_RUNTIME_DIR}/scripts")

if(NOT TARGET Falcor::Falcor)
    add_library(Falcor::Falcor SHARED IMPORTED)

    set_target_properties(Falcor::Falcor PROPERTIES
        IMPORTED_LOCATION "${FALCOR_RUNTIME_DIR}/Falcor.dll"
        IMPORTED_IMPLIB "${FALCOR_SDK_ROOT}/lib/Falcor.lib"
        INTERFACE_INCLUDE_DIRECTORIES
            "${FALCOR_SDK_ROOT}/include;${FALCOR_SDK_ROOT}/include/Falcor;${FALCOR_SDK_ROOT}/include/external/fmt/include;${FALCOR_SDK_ROOT}/include/external/pybind11/include;${FALCOR_SDK_ROOT}/include/external/vulkan-headers/include;${FALCOR_SDK_ROOT}/include/external/imgui;${FALCOR_SDK_ROOT}/include/external/imgui_addons;${FALCOR_SDK_ROOT}/include/external/include;${FALCOR_SDK_ROOT}/include/external/mikktspace;${FALCOR_SDK_ROOT}/include/external/packman/deps/include;${FALCOR_SDK_ROOT}/include/external/packman/slang/include;${FALCOR_SDK_ROOT}/include/external/packman/rtxdi/include"
        INTERFACE_COMPILE_FEATURES cxx_std_17
        INTERFACE_COMPILE_DEFINITIONS
            "NOMINMAX;UNICODE;_USE_MATH_DEFINES;_SCL_SECURE_NO_WARNINGS;_CRT_SECURE_NO_WARNINGS;_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING;_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS;FALCOR_ENABLE_ASSERTS=0;FALCOR_ENABLE_PROFILER=1;FALCOR_HAS_D3D12=1;FALCOR_HAS_VULKAN=1;FALCOR_HAS_AFTERMATH=0;FALCOR_HAS_NVAPI=0;FALCOR_HAS_CUDA=ON;FALCOR_HAS_D3D12_AGILITY_SDK=ON;FALCOR_HAS_RTXDI=1;IMGUI_USER_CONFIG=\"Utils/UI/ImGuiConfig.h\""
    )
endif()

function(falcor_copy_runtime target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "falcor_copy_runtime target '${target}' does not exist")
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FALCOR_RUNTIME_DIR}"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying Falcor SDK runtime files"
    )
endfunction()
