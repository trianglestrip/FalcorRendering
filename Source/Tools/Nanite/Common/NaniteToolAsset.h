#pragma once

#include "Nanite/NaniteAssetData.h"
#include "Nanite/NaniteGpuTypes.h"
#include "Nanite/NaniteTypes.h"

namespace FalcorRendering::NaniteTool
{
using Falcor::Nanite::Asset;
using Falcor::Nanite::Bounds;
using Falcor::Nanite::ChunkType;
using Falcor::Nanite::Cluster;
using Falcor::Nanite::ClusterDebugInfo;
using Falcor::Nanite::ClusterGroup;
using Falcor::Nanite::Float2;
using Falcor::Nanite::Float3;
using Falcor::Nanite::HierarchyNode;
using Falcor::Nanite::Material;
using Falcor::Nanite::Mesh;
using Falcor::Nanite::PageDesc;
using Falcor::Nanite::PartitionStats;
using Falcor::Nanite::SourceMeshSection;
using Falcor::Nanite::Vertex;
using Falcor::Nanite::WriteOptions;

using Falcor::Nanite::center;
using Falcor::Nanite::cross;
using Falcor::Nanite::dot;
using Falcor::Nanite::emptyBounds;
using Falcor::Nanite::include;
using Falcor::Nanite::isEmpty;
using Falcor::Nanite::length;
using Falcor::Nanite::normalize;
using Falcor::Nanite::radius;
using Falcor::Nanite::triangleCount;

using Falcor::Nanite::kFlagCompressedVertices;
using Falcor::Nanite::kFlagDebugUncompressed;
using Falcor::Nanite::kFlagHasSourceGeometry;
using Falcor::Nanite::kNaniteMagic;
using Falcor::Nanite::kNaniteVersion;
using Falcor::Nanite::kNaniteVersionV1;
using Falcor::Nanite::kPageFlagResident;

using Falcor::Nanite::buildMetadataTables;
using Falcor::Nanite::computeGpuMemoryStats;
using Falcor::Nanite::hasSourceGeometry;
using Falcor::Nanite::readAsset;
using Falcor::Nanite::validateAsset;
using Falcor::Nanite::validateRuntimeTables;
using Falcor::Nanite::validateSourceGeometry;
using Falcor::Nanite::writeAsset;
using Falcor::Nanite::writeAssetV1;
using Falcor::Nanite::GpuMemoryStats;

void writeAssetUncompressed(const std::filesystem::path& path, const Asset& asset);
}
