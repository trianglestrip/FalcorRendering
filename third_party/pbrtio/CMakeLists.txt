cmake_minimum_required(VERSION 3.19)
project(pbrtio)

set(TARGET pbrtio)
set(PUBLIC_HDR_DIR include)
set(PBRTIO_THIRD_PARTY_DIR ${CMAKE_CURRENT_LIST_DIR}/.. CACHE PATH "Directory containing pbrtio third-party dependencies.")
option(PBRTIO_STB_IMAGE_IMPLEMENTATION "Compile stb_image implementation into pbrtio." ON)

if(EXISTS ${PBRTIO_THIRD_PARTY_DIR}/glm/glm.hpp)
    set(PBRTIO_GLM_INCLUDE_DIR ${PBRTIO_THIRD_PARTY_DIR})
elseif(EXISTS ${PBRTIO_THIRD_PARTY_DIR}/glm/glm/glm.hpp)
    set(PBRTIO_GLM_INCLUDE_DIR ${PBRTIO_THIRD_PARTY_DIR}/glm)
else()
    message(FATAL_ERROR "pbrtio requires GLM next to it in third_party/glm")
endif()

set(PBRTIO_TASKFLOW_INCLUDE_DIR ${PBRTIO_THIRD_PARTY_DIR}/taskflow)
if(EXISTS ${PBRTIO_THIRD_PARTY_DIR}/stb/stb_image.h)
    set(PBRTIO_STB_INCLUDE_DIR ${PBRTIO_THIRD_PARTY_DIR}/stb)
elseif(EXISTS ${PBRTIO_THIRD_PARTY_DIR}/tinygltf/stb_image.h)
    set(PBRTIO_STB_INCLUDE_DIR ${PBRTIO_THIRD_PARTY_DIR}/tinygltf)
else()
    message(FATAL_ERROR "pbrtio requires stb_image.h in third_party/stb or third_party/tinygltf")
endif()

set(PUBLIC_HDRS
    include/pbrtio/PbrtCompat.h
    include/pbrtio/PbrtFormat.h
    include/pbrtio/PbrtMathTypes.h
    include/pbrtio/PbrtMath.h
    include/pbrtio/PbrtSpectrum.h
    include/pbrtio/PbrtTypes.h
    include/pbrtio/Helpers.h
    include/pbrtio/Parser.h
    include/pbrtio/Parameters.h
    include/pbrtio/Builder.h
    include/pbrtio/LoopSubdivide.h
    include/pbrtio/PbrtSceneLoader.h
)

set(SRCS
    src/Parser.cpp
    src/Parameters.cpp
    src/Builder.cpp
    src/LoopSubdivide.cpp
    src/PbrtSceneCollector.cpp
    src/PbrtSceneLoader.cpp
)

include_directories(${PUBLIC_HDR_DIR})
add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})
target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR} ${PBRTIO_GLM_INCLUDE_DIR})
target_include_directories(${TARGET} PRIVATE ${PBRTIO_STB_INCLUDE_DIR})
if(PBRTIO_STB_IMAGE_IMPLEMENTATION)
    target_compile_definitions(${TARGET} PRIVATE PBRTIO_STB_IMAGE_IMPLEMENTATION)
endif()
if(EXISTS ${PBRTIO_TASKFLOW_INCLUDE_DIR}/taskflow.hpp)
    target_include_directories(${TARGET} PRIVATE ${PBRTIO_TASKFLOW_INCLUDE_DIR})
endif()
target_compile_features(${TARGET} PUBLIC cxx_std_20)
if(TARGET taskflow)
    target_link_libraries(${TARGET} PRIVATE taskflow)
endif()
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)
