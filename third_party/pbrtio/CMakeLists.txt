cmake_minimum_required(VERSION 3.19)
project(pbrtio)

set(TARGET pbrtio)
set(PUBLIC_HDR_DIR include)
set(PBRTIO_THIRD_PARTY_DIR ${CMAKE_CURRENT_LIST_DIR}/.. CACHE PATH "Directory containing pbrtio third-party dependencies.")

if(EXISTS ${PBRTIO_THIRD_PARTY_DIR}/glm/glm.hpp)
    set(PBRTIO_GLM_INCLUDE_DIR ${PBRTIO_THIRD_PARTY_DIR})
elseif(EXISTS ${PBRTIO_THIRD_PARTY_DIR}/glm/glm/glm.hpp)
    set(PBRTIO_GLM_INCLUDE_DIR ${PBRTIO_THIRD_PARTY_DIR}/glm)
else()
    message(FATAL_ERROR "pbrtio requires GLM next to it in third_party/glm")
endif()

set(PUBLIC_HDRS
    include/pbrtio/PbrtCompat.h
    include/pbrtio/PbrtMathTypes.h
    include/pbrtio/PbrtMath.h
    include/pbrtio/PbrtSpectrum.h
    include/pbrtio/PbrtTypes.h
    include/pbrtio/Helpers.h
    include/pbrtio/Parser.h
    include/pbrtio/Parameters.h
    include/pbrtio/Builder.h
    include/pbrtio/LoopSubdivide.h
    include/pbrtio/Scene.h
)

set(SRCS
    src/Parser.cpp
    src/Parameters.cpp
    src/Builder.cpp
    src/LoopSubdivide.cpp
    src/Scene.cpp
)

include_directories(${PUBLIC_HDR_DIR})
add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})
target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR} ${PBRTIO_GLM_INCLUDE_DIR})
target_include_directories(${TARGET} PRIVATE ${PBRTIO_THIRD_PARTY_DIR}/taskflow)
target_compile_features(${TARGET} PUBLIC cxx_std_20)
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)
