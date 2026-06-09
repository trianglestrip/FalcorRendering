#include "NaniteAsset.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
    std::cout << "Meshes: " << asset.meshes.size() << '\n';
    std::cout << "Materials: " << asset.materials.size() << '\n';
    std::cout << "Clusters: " << asset.clusters.size() << '\n';
    std::cout << "Triangles: " << triangleCount(asset) << '\n';
    std::cout << "Vertices: " << asset.vertices.size() << '\n';
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
            << " tris=" << cluster.triangleCount
            << " verts=" << cluster.vertexCount
            << " vOffset=" << cluster.vertexOffset
            << " iOffset=" << cluster.indexOffset
            << " radius=" << cluster.sphereRadius
            << " error=" << cluster.geometricError
            << '\n';
    }
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

        std::cout << "Loaded: " << args.input << '\n';
        printAssetSummary(asset);
        std::cout << "Validation: OK\n";

        if (args.listMeshes) printMeshes(asset);
        if (args.listMaterials) printMaterials(asset);
        if (args.listClusters) printClusters(asset);

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
