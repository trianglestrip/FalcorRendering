add_plugin(PBRTImporter)

target_sources(PBRTImporter PRIVATE
    EnvMapConverter.cs.slang
    EnvMapConverter.h
    PBRTImporter.cpp
    PBRTImporter.h
)

target_link_libraries(PBRTImporter PRIVATE pbrtio)

target_copy_shaders(PBRTImporter plugins/importers/PBRTImporter)

target_source_group(PBRTImporter "Plugins/Importers")

validate_headers(PBRTImporter)
