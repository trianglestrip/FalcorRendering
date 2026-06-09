#pragma once

#include "NaniteToolAsset.h"

#include <filesystem>
#include <string>
#include <vector>

namespace FalcorRendering::NaniteTool
{
struct InputMesh
{
    std::string name;
    uint32_t materialIndex = 0;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Bounds bounds;
};

struct InputScene
{
    std::filesystem::path sourcePath;
    std::vector<std::string> materialNames;
    std::vector<InputMesh> meshes;
    Bounds bounds;
};

InputScene loadObjScene(const std::filesystem::path& path);

/// Merges vertices with matching position, normal, and texcoord within one mesh section.
void deduplicateMeshVertices(InputMesh& mesh);
}
