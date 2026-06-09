include(CMakePackageConfigHelpers)

option(FALCOR_ENABLE_SDK_INSTALL "Install Falcor as a reusable SDK package" ON)

if(FALCOR_ENABLE_SDK_INSTALL)
    configure_package_config_file(
        ${CMAKE_SOURCE_DIR}/cmake/FalcorConfig.cmake.in
        ${CMAKE_BINARY_DIR}/FalcorConfig.cmake
        INSTALL_DESTINATION cmake
    )

    install(TARGETS Falcor
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/Source/Falcor/
        DESTINATION include/Falcor
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.slangh"
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/fmt/
        DESTINATION include/external/fmt
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.inl"
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/pybind11/
        DESTINATION include/external/pybind11
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/vulkan-headers/
        DESTINATION include/external/vulkan-headers
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/imgui/
        DESTINATION include/external/imgui
        FILES_MATCHING
            PATTERN "*.h"
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/imgui_addons/
        DESTINATION include/external/imgui_addons
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/include/
        DESTINATION include/external/include
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.inl"
    )

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/mikktspace/
        DESTINATION include/external/mikktspace
        FILES_MATCHING
            PATTERN "*.h"
    )

    if(EXISTS ${CMAKE_SOURCE_DIR}/external/packman/deps/include)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/packman/deps/include/
            DESTINATION include/external/packman/deps/include
        )
    endif()

    if(EXISTS ${CMAKE_SOURCE_DIR}/external/packman/slang/include)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/packman/slang/include/
            DESTINATION include/external/packman/slang/include
        )
    endif()

    if(EXISTS ${CMAKE_SOURCE_DIR}/external/packman/rtxdi/include)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/packman/rtxdi/include/
            DESTINATION include/external/packman/rtxdi/include
        )
    endif()

    install(FILES
        ${CMAKE_BINARY_DIR}/FalcorConfig.cmake
        DESTINATION cmake
    )
endif()
