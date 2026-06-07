add_plugin(PBRTImporter)

target_sources(PBRTImporter PRIVATE
    PBRTImporter.cpp
    PBRTImporter.h
)

target_link_libraries(PBRTImporter PRIVATE pbrtio)
target_include_directories(PBRTImporter PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/taskflow
)

target_copy_shaders(PBRTImporter plugins/importers/PBRTImporter)

target_source_group(PBRTImporter "Plugins/Importers")

validate_headers(PBRTImporter)
