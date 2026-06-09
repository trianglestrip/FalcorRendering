#include "NaniteToolAsset.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace FalcorRendering::NaniteTool;

namespace
{
struct Arguments
{
    std::filesystem::path input;
    bool listMeshes = false;
    bool listMaterials = false;
    bool listClusters = false;
    bool listGroups = false;
    bool listHierarchy = false;
    bool listPages = false;
    bool memoryStats = false;
};

void printUsage()
{
    std::cout
        << "Usage:\n"
        << "  NaniteLoader --input <asset.fnanite> [options]\n\n"
        << "Options:\n"
        << "  -i, --input <path>     Input .fnanite file.\n"
        << "  --list-meshes          Print mesh table.\n"
        << "  --list-materials       Print material table.\n"
        << "  --list-clusters        Print cluster table.\n"
        << "  --list-groups          Print cluster group table.\n"
        << "  --list-hierarchy       Print hierarchy node table.\n"
        << "  --list-pages           Print page table.\n"
        << "  --memory-stats         Print estimated GPU buffer sizes.\n"
        << "  -h, --help             Show this help.\n";
}

bool isOption(const char* value)
{
    return value && value[0] == '-';
}

Arguments parseArguments(int argc, char** argv)
{
    Arguments args;

    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        auto requireValue = [&](const char* name) -> std::string
        {
            if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value for ") + name + ".");
            return argv[++i];
        };

        if (option == "-h" || option == "--help")
        {
            printUsage();
            std::exit(0);
        }
        else if (option == "-i" || option == "--input")
        {
            args.input = requireValue(option.c_str());
        }
        else if (option == "--list-meshes")
        {
            args.listMeshes = true;
        }
        else if (option == "--list-materials")
        {
            args.listMaterials = true;
        }
        else if (option == "--list-clusters")
        {
            args.listClusters = true;
        }
        else if (option == "--list-groups")
        {
            args.listGroups = true;
        }
        else if (option == "--list-hierarchy")
        {
            args.listHierarchy = true;
        }
        else if (option == "--list-pages")
        {
            args.listPages = true;
        }
        else if (option == "--memory-stats")
        {
            args.memoryStats = true;
        }
        else if (args.input.empty() && !isOption(argv[i]))
        {
            args.input = option;
        }
        else
        {
            throw std::runtime_error("Unknown argument: " + option);
        }
    }

    if (args.input.empty()) throw std::runtime_error("Missing input .fnanite file.");
    return args;
}

void printBounds(const Bounds& bounds)
{
    std::cout
        << "min(" << bounds.min.x << ", " << bounds.min.y << ", " << bounds.min.z << ") "
        << "max(" << bounds.max.x << ", " << bounds.max.y << ", " << bounds.max.z << ")";
}

void printAssetSummary(const Asset& asset)
{
    std::cout << "Source: " << asset.sourcePath << '\n';
    std::cout << "Format version: " << asset.version << '\n';
    std::cout << "Meshes: " << asset.meshes.size() << '\n';
    std::cout << "Materials: " << asset.materials.size() << '\n';
    std::cout << "Clusters: " << asset.clusters.size() << '\n';
    std::cout << "Cluster groups: " << asset.clusterGroups.size() << '\n';
    std::cout << "Hierarchy nodes: " << asset.hierarchyNodes.size() << '\n';
    std::cout << "Pages: " << asset.pages.size() << '\n';
    std::cout << "Triangles: " << triangleCount(asset) << '\n';
    std::cout << "Vertices: " << asset.vertices.size() << '\n';
    std::cout << "Format version: " << asset.version << '\n';
    std::cout << "Compressed: " << ((asset.flags & kFlagCompressedVertices) ? "yes" : "no") << '\n';
    std::cout << "Indices: " << asset.indices.size() << '\n';
    std::cout << "Bounds: ";
    printBounds(asset.bounds);
    std::cout << '\n';
}

void printMeshes(const Asset& asset)
{
    std::cout << "\nMesh table:\n";
    for (size_t i = 0; i < asset.meshes.size(); ++i)
    {
        const Mesh& mesh = asset.meshes[i];
        std::cout << "  [" << i << "] " << mesh.name
            << " clusters=" << mesh.clusterCount
            << " firstCluster=" << mesh.firstCluster
            << " materialRange=[" << mesh.firstMaterial << ", " << mesh.firstMaterial + mesh.materialCount << ")"
            << " bounds=";
        printBounds(mesh.bounds);
        std::cout << '\n';
    }
}

void printMaterials(const Asset& asset)
{
    std::cout << "\nMaterial table:\n";
    for (size_t i = 0; i < asset.materials.size(); ++i)
    {
        std::cout << "  [" << i << "] " << asset.materials[i].name << '\n';
    }
}

void printClusters(const Asset& asset)
{
    std::cout << "\nCluster table:\n";
    for (size_t i = 0; i < asset.clusters.size(); ++i)
    {
        const Cluster& cluster = asset.clusters[i];
        std::cout << "  [" << i << "]"
            << " mesh=" << cluster.meshIndex
            << " material=" << cluster.materialIndex
            << " page=" << cluster.pageIndex
            << " group=" << cluster.groupIndex
            << " tris=" << cluster.triangleCount
            << " verts=" << cluster.vertexCount
            << " vOffset=" << cluster.vertexOffset
            << " iOffset=" << cluster.indexOffset
            << " radius=" << cluster.sphereRadius
            << " error=" << cluster.geometricError
            << " area=" << cluster.surfaceArea
            << '\n';
    }
}

void printClusterGroups(const Asset& asset)
{
    std::cout << "\nCluster group table:\n";
    for (size_t i = 0; i < asset.clusterGroups.size(); ++i)
    {
        const ClusterGroup& group = asset.clusterGroups[i];
        std::cout << "  [" << i << "]"
            << " clusters=[" << group.firstCluster << ", " << group.firstCluster + group.clusterCount << ")"
            << " parent=" << (group.parentGroup == std::numeric_limits<uint32_t>::max() ? "none" : std::to_string(group.parentGroup))
            << " lod=" << group.lodLevel
            << " error=" << group.geometricError
            << '\n';
    }
}

void printHierarchy(const Asset& asset)
{
    std::cout << "\nHierarchy node table:\n";
    for (size_t i = 0; i < asset.hierarchyNodes.size(); ++i)
    {
        const HierarchyNode& node = asset.hierarchyNodes[i];
        std::cout << "  [" << i << "]"
            << " clusters=[" << node.clusterOffset << ", " << node.clusterOffset + node.clusterCount << ")"
            << " group=" << (node.clusterGroupIndex == std::numeric_limits<uint32_t>::max() ? "none" : std::to_string(node.clusterGroupIndex))
            << " childOffset=" << node.childNodeOffset
            << " children=" << node.childNodeCount
            << " error=[" << node.minError << ", " << node.maxError << "]"
            << " radius=" << node.sphereRadius
            << '\n';
    }
}

void printPages(const Asset& asset)
{
    std::cout << "\nPage table:\n";
    for (size_t i = 0; i < asset.pages.size(); ++i)
    {
        const PageDesc& page = asset.pages[i];
        std::cout << "  [" << i << "]"
            << " clusters=[" << page.firstCluster << ", " << page.firstCluster + page.clusterCount << ")"
            << " byteSize=" << page.byteSize
            << " flags=0x" << std::hex << page.flags << std::dec;
        if (page.flags & kPageFlagResident) std::cout << " resident";
        std::cout << '\n';
    }
}

void printMemoryStats(const Asset& asset)
{
    const GpuMemoryStats stats = computeGpuMemoryStats(asset);
    std::cout << "\nEstimated GPU buffer sizes:\n";
    std::cout << "  clusters:   " << stats.clusterBytes << " bytes (" << stats.clusterCount << " entries)\n";
    std::cout << "  hierarchy:  " << stats.hierarchyBytes << " bytes\n";
    std::cout << "  pages:      " << stats.pageBytes << " bytes (" << stats.pageCount << " entries)\n";
    std::cout << "  vertices:   " << stats.vertexBytes << " bytes\n";
    std::cout << "  indices:    " << stats.indexBytes << " bytes\n";
    std::cout << "  materials:  " << stats.materialBytes << " bytes (" << stats.materialCount << " entries)\n";
    std::cout << "  residency:  " << stats.residencyBytes << " bytes\n";
    std::cout << "  total:      " << stats.totalGpuBytes << " bytes\n";
}
}

int main(int argc, char** argv)
{
    try
    {
        const Arguments args = parseArguments(argc, argv);
        Asset asset = readAsset(args.input);

        const std::vector<std::string> errors = validateAsset(asset);
        if (!errors.empty())
        {
            for (const std::string& error : errors) std::cerr << "Validation: " << error << '\n';
            return 2;
        }

        try
        {
            validateRuntimeTables(asset);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Runtime validation: " << e.what() << '\n';
            return 2;
        }

        std::cout << "Loaded: " << args.input << '\n';
        printAssetSummary(asset);
        std::cout << "Validation: OK\n";

        if (args.memoryStats) printMemoryStats(asset);

        if (args.listMeshes) printMeshes(asset);
        if (args.listMaterials) printMaterials(asset);
        if (args.listClusters) printClusters(asset);
        if (args.listGroups) printClusterGroups(asset);
        if (args.listHierarchy) printHierarchy(asset);
        if (args.listPages) printPages(asset);

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "NaniteLoader error: " << e.what() << '\n';
        std::cerr << '\n';
        printUsage();
        return 1;
    }
}
