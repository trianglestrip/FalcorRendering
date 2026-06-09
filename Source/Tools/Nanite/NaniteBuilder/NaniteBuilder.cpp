#include "NaniteAsset.h"
#include "NaniteBuild.h"
#include "NaniteObj.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

using namespace FalcorRendering::NaniteTool;

namespace
{
struct Arguments
{
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path debugJson;
    BuildOptions build;
    bool writeDebugJson = false;
};

void printUsage()
{
    std::cout
        << "Usage:\n"
        << "  NaniteBuilder --input <model.obj> [--output <asset.fnanite>] [options]\n\n"
        << "Options:\n"
        << "  -i, --input <path>             Input OBJ file.\n"
        << "  -o, --output <path>            Output .fnanite file. Defaults to input path with .fnanite extension.\n"
        << "  --cluster-tris <count>         Target triangles per cluster. Default: 128.\n"
        << "  --max-cluster-verts <count>    Maximum local vertices per cluster. Default: 256.\n"
        << "  --debug-json [path]            Write a debug JSON summary. Default path is output.fnanite.json.\n"
        << "  -h, --help                     Show this help.\n";
}

uint32_t parseUInt(const std::string& value, const char* option)
{
    try
    {
        const unsigned long parsed = std::stoul(value);
        if (parsed > std::numeric_limits<uint32_t>::max()) throw std::out_of_range(option);
        return static_cast<uint32_t>(parsed);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error(std::string("Invalid numeric value for ") + option + ": " + value);
    }
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
        else if (option == "-o" || option == "--output")
        {
            args.output = requireValue(option.c_str());
        }
        else if (option == "--cluster-tris")
        {
            args.build.clusterTriangleTarget = parseUInt(requireValue(option.c_str()), option.c_str());
        }
        else if (option == "--max-cluster-verts")
        {
            args.build.maxClusterVertices = parseUInt(requireValue(option.c_str()), option.c_str());
        }
        else if (option == "--debug-json")
        {
            args.writeDebugJson = true;
            if (i + 1 < argc && !isOption(argv[i + 1]))
            {
                args.debugJson = argv[++i];
            }
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

    if (args.input.empty()) throw std::runtime_error("Missing input OBJ file.");
    if (args.output.empty())
    {
        args.output = args.input;
        args.output.replace_extension(".fnanite");
    }
    if (args.writeDebugJson && args.debugJson.empty())
    {
        args.debugJson = args.output;
        args.debugJson.replace_extension(".fnanite.json");
    }

    return args;
}

std::string jsonEscape(const std::string& value)
{
    std::string escaped;
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += ch; break;
        }
    }
    return escaped;
}

void writeBounds(std::ofstream& stream, const Bounds& bounds)
{
    stream
        << "{ \"min\": [" << bounds.min.x << ", " << bounds.min.y << ", " << bounds.min.z
        << "], \"max\": [" << bounds.max.x << ", " << bounds.max.y << ", " << bounds.max.z << "] }";
}

void writeDebugJson(const std::filesystem::path& path, const Asset& asset, const BuildOptions& options)
{
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    if (!stream) throw std::runtime_error("Failed to open debug JSON path: " + path.string());

    stream << "{\n";
    stream << "  \"source\": \"" << jsonEscape(asset.sourcePath) << "\",\n";
    stream << "  \"clusterTriangleTarget\": " << options.clusterTriangleTarget << ",\n";
    stream << "  \"maxClusterVertices\": " << options.maxClusterVertices << ",\n";
    stream << "  \"meshCount\": " << asset.meshes.size() << ",\n";
    stream << "  \"materialCount\": " << asset.materials.size() << ",\n";
    stream << "  \"clusterCount\": " << asset.clusters.size() << ",\n";
    stream << "  \"vertexCount\": " << asset.vertices.size() << ",\n";
    stream << "  \"indexCount\": " << asset.indices.size() << ",\n";
    stream << "  \"triangleCount\": " << triangleCount(asset) << ",\n";
    stream << "  \"bounds\": ";
    writeBounds(stream, asset.bounds);
    stream << ",\n";
    stream << "  \"clusters\": [\n";
    for (size_t i = 0; i < asset.clusters.size(); ++i)
    {
        const Cluster& cluster = asset.clusters[i];
        stream << "    { \"id\": " << i
            << ", \"mesh\": " << cluster.meshIndex
            << ", \"material\": " << cluster.materialIndex
            << ", \"triangles\": " << cluster.triangleCount
            << ", \"vertices\": " << cluster.vertexCount
            << ", \"bounds\": ";
        writeBounds(stream, cluster.bounds);
        stream << " }" << (i + 1 == asset.clusters.size() ? "\n" : ",\n");
    }
    stream << "  ]\n";
    stream << "}\n";
}
}

int main(int argc, char** argv)
{
    try
    {
        Arguments args = parseArguments(argc, argv);
        if (args.input.extension() != ".obj")
        {
            throw std::runtime_error("NaniteBuilder MVP currently supports OBJ input only.");
        }

        std::cout << "Loading OBJ: " << args.input << '\n';
        InputScene scene = loadObjScene(args.input);

        std::cout << "Building clusters...\n";
        Asset asset = buildNaniteAsset(scene, args.build);

        const std::vector<std::string> validationErrors = validateAsset(asset);
        if (!validationErrors.empty())
        {
            for (const std::string& error : validationErrors) std::cerr << "Validation: " << error << '\n';
            throw std::runtime_error("Generated Nanite asset failed validation.");
        }

        writeAsset(args.output, asset);
        if (args.writeDebugJson) writeDebugJson(args.debugJson, asset, args.build);

        std::cout << "Wrote: " << args.output << '\n';
        std::cout << "Meshes: " << asset.meshes.size()
            << ", Materials: " << asset.materials.size()
            << ", Clusters: " << asset.clusters.size()
            << ", Triangles: " << triangleCount(asset)
            << ", Vertices: " << asset.vertices.size() << '\n';
        if (args.writeDebugJson) std::cout << "Debug JSON: " << args.debugJson << '\n';

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "NaniteBuilder error: " << e.what() << '\n';
        std::cerr << '\n';
        printUsage();
        return 1;
    }
}
