#pragma once

#include "NaniteObj.h"

#include <filesystem>

namespace FalcorRendering::NaniteTool
{
InputScene loadGltfScene(const std::filesystem::path& path);
}
