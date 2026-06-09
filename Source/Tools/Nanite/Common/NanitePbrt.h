#pragma once

#include "NaniteObj.h"

#include <cstdint>
#include <filesystem>

namespace FalcorRendering::NaniteTool
{
InputScene loadPbrtScene(const std::filesystem::path& path, uint32_t workerCount = 0);
}
