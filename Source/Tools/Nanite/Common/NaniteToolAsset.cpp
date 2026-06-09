#include "NaniteToolAsset.h"

namespace FalcorRendering::NaniteTool
{
void writeAssetUncompressed(const std::filesystem::path& path, const Asset& asset)
{
    WriteOptions options{};
    options.compressVertices = false;
    options.debugUncompressed = true;
    Falcor::Nanite::writeAsset(path, asset, options);
}
}
