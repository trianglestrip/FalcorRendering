#include "NaniteToolAsset.h"
#include "NaniteBuild.h"
#include "NaniteGltf.h"
#include "NaniteObj.h"
#include "NanitePbrt.h"

#include <algorithm>
#include <cctype>
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
    WriteOptions writeOptions;
    bool writeDebugJson = false;
    bool combined = false;
    bool rebuildFromSource = false;
};

void printUsage()
{
    std::cout
        << "Usage:\n"
        << "  NaniteBuilder --input <model.obj|model.gltf|model.glb|scene.pbrt> [--output <asset.fnanite>] [options]\n\n"
        << "Options:\n"
        << "  -i, --input <path>             Input mesh file (.obj, .gltf, .glb, or .pbrt).\n"
        << "  -o, --output <path>            Output .fnanite file. Defaults to input path with .fnanite extension.\n"
        << "  --cluster-tris <count>         Target triangles per cluster. Default: 128.\n"
        << "  --max-cluster-verts <count>    Maximum local vertices per cluster. Default: 256.\n"
        << "  --workers <count>              Taskflow worker count. Default: hardware thread count.\n"
        << "  --dedup-verts                  Merge identical vertices within each source mesh section.\n"
        << "  --no-compress                  Write uncompressed vertices (debug).\n"
        << "  --debug-uncompressed           Alias for --no-compress.\n"
        << "  --group-clusters <count>       Clusters per cluster group. Default: 32.\n"
        << "  --combined                     Write one .fnanite for the whole scene (legacy).\n"
        << "  --rebuild, --rebuild-from-source  Re-cluster from embedded source in an existing .fnanite.\n"
        << "  --no-embed-source              Do not store pre-cluster source geometry in the output.\n"
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
        else if (option == "--workers")
        {
            args.build.workerCount = parseUInt(requireValue(option.c_str()), option.c_str());
        }
        else if (option == "--dedup-verts")
        {
            args.build.dedupVerts = true;
        }
        else if (option == "--no-compress" || option == "--debug-uncompressed")
        {
            args.writeOptions.compressVertices = false;
            args.writeOptions.debugUncompressed = true;
        }
        else if (option == "--group-clusters")
        {
            args.writeOptions.groupClusters = parseUInt(requireValue(option.c_str()), option.c_str());
        }
        else if (option == "--combined")
        {
            args.combined = true;
        }
        else if (option == "--rebuild" || option == "--rebuild-from-source")
        {
            args.rebuildFromSource = true;
        }
        else if (option == "--no-embed-source")
        {
            args.writeOptions.embedSource = false;
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

    if (args.input.empty()) throw std::runtime_error("Missing input mesh file.");
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

std::string sanitizeFileName(const std::string& name)
{
    std::string sanitized;
    sanitized.reserve(name.size());
    for (char ch : name)
    {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')
            sanitized += ch;
        else
            sanitized += '_';
    }
    if (sanitized.empty()) sanitized = "mesh";
    return sanitized;
}


std::filesystem::path perMeshOutputPath(const std::filesystem::path& baseOutput, const std::string& meshName)
{
    const std::string safeName = sanitizeFileName(meshName);
    std::filesystem::path dir;
    std::string basename;

    if (baseOutput.has_extension() && baseOutput.extension() == ".fnanite")
    {
        dir = baseOutput.parent_path();
        basename = baseOutput.stem().string();
    }
    else if (std::filesystem::exists(baseOutput) && std::filesystem::is_directory(baseOutput))
    {
        dir = baseOutput;
        basename = baseOutput.filename().string();
    }
    else
    {
        dir = baseOutput.parent_path();
        basename = baseOutput.filename().string();
    }

    if (basename.empty()) basename = "mesh";
    return dir / (basename + "_" + safeName + ".fnanite");
}

std::filesystem::path perMeshDebugJsonPath(const std::filesystem::path& outputPath)
{
    std::filesystem::path debugPath = outputPath;
    debugPath.replace_extension(".fnanite.json");
    return debugPath;
}

void validateBuiltAsset(const Asset& asset)
{
    const std::vector<std::string> validationErrors = validateAsset(asset);
    if (!validationErrors.empty())
    {
        for (const std::string& error : validationErrors) std::cerr << "Validation: " << error << '\n';
        throw std::runtime_error("Generated Nanite asset failed validation.");
    }

    const std::vector<std::string> sourceErrors = validateSourceGeometry(asset);
    if (!sourceErrors.empty())
    {
        for (const std::string& error : sourceErrors) std::cerr << "Source validation: " << error << '\n';
        throw std::runtime_error("Embedded source geometry failed validation.");
    }
}

void writeDebugJsonFile(const std::filesystem::path& path, const Asset& asset, const BuildOptions& options);

void writeBuiltAsset(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& debugJsonPath,
    const Asset& asset,
    const BuildOptions& buildOptions,
    const WriteOptions& writeOptions,
    bool emitDebugJson
)
{
    writeAsset(outputPath, asset, writeOptions);
    if (emitDebugJson) writeDebugJsonFile(debugJsonPath, asset, buildOptions);

    std::cout << "Wrote: " << outputPath << '\n';
    std::cout << "Meshes: " << asset.meshes.size()
        << ", Materials: " << asset.materials.size()
        << ", Clusters: " << asset.clusters.size()
        << ", Triangles: " << triangleCount(asset)
        << ", Vertices: " << asset.vertices.size();
    if (hasSourceGeometry(asset))
    {
        uint64_t sourceTriangles = 0;
        for (const SourceMeshSection& section : asset.sourceMeshes)
        {
            sourceTriangles += section.indices.size() / 3;
        }
        std::cout << ", Source sections: " << asset.sourceMeshes.size()
            << ", Source triangles: " << sourceTriangles;
    }
    std::cout << '\n';
    if (emitDebugJson) std::cout << "Debug JSON: " << debugJsonPath << '\n';
}

InputScene singleMeshScene(const InputScene& scene, size_t meshIndex)
{
    InputScene subScene;
    subScene.sourcePath = scene.sourcePath;
    subScene.materialNames = scene.materialNames;
    subScene.meshes.push_back(scene.meshes[meshIndex]);
    subScene.bounds = scene.meshes[meshIndex].bounds;
    return subScene;
}

void buildAndWriteScene(
    const InputScene& scene,
    const Arguments& args,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& debugJsonPath
)
{
    Asset asset = buildNaniteAsset(scene, args.build);
    if (args.writeOptions.embedSource) embedSourceGeometry(asset, scene);
    validateBuiltAsset(asset);
    writeBuiltAsset(outputPath, debugJsonPath, asset, args.build, args.writeOptions, args.writeDebugJson);
}

void writeBounds(std::ofstream& stream, const Bounds& bounds)
{
    stream
        << "{ \"min\": [" << bounds.min.x << ", " << bounds.min.y << ", " << bounds.min.z
        << "], \"max\": [" << bounds.max.x << ", " << bounds.max.y << ", " << bounds.max.z << "] }";
}

void writeFloat3(std::ofstream& stream, const Float3& value)
{
    stream << "[" << value.x << ", " << value.y << ", " << value.z << "]";
}

void writeTriangleRemap(std::ofstream& stream, const ClusterDebugInfo* debugInfo)
{
    stream << "[";
    if (debugInfo)
    {
        for (size_t i = 0; i < debugInfo->sourceTriangleIndices.size(); ++i)
        {
            if (i > 0) stream << ", ";
            stream << debugInfo->sourceTriangleIndices[i];
        }
    }
    stream << "]";
}

void writeDebugJsonFile(const std::filesystem::path& path, const Asset& asset, const BuildOptions& options)
{
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    if (!stream) throw std::runtime_error("Failed to open debug JSON path: " + path.string());

    stream << "{\n";
    stream << "  \"source\": \"" << jsonEscape(asset.sourcePath) << "\",\n";
    stream << "  \"clusterTriangleTarget\": " << options.clusterTriangleTarget << ",\n";
    stream << "  \"maxClusterVertices\": " << options.maxClusterVertices << ",\n";
    stream << "  \"workerCount\": " << options.workerCount << ",\n";
    stream << "  \"dedupVerts\": " << (options.dedupVerts ? "true" : "false") << ",\n";
    stream << "  \"meshCount\": " << asset.meshes.size() << ",\n";
    stream << "  \"materialCount\": " << asset.materials.size() << ",\n";
    stream << "  \"clusterCount\": " << asset.clusters.size() << ",\n";
    stream << "  \"vertexCount\": " << asset.vertices.size() << ",\n";
    stream << "  \"indexCount\": " << asset.indices.size() << ",\n";
    stream << "  \"triangleCount\": " << triangleCount(asset) << ",\n";
    stream << "  \"sourceTriangleCount\": " << asset.sourceTriangleCount << ",\n";
    stream << "  \"degenerateTriangleCount\": " << asset.degenerateTriangleCount << ",\n";
    stream << "  \"partitionStats\": { \"totalLocalVertices\": " << asset.partitionStats.totalLocalVertices
        << ", \"boundarySourceVertices\": " << asset.partitionStats.boundarySourceVertices
        << ", \"interClusterEdges\": " << asset.partitionStats.interClusterEdges << " },\n";
    stream << "  \"bounds\": ";
    writeBounds(stream, asset.bounds);
    stream << ",\n";
    stream << "  \"clusters\": [\n";
    for (size_t i = 0; i < asset.clusters.size(); ++i)
    {
        const Cluster& cluster = asset.clusters[i];
        const ClusterDebugInfo* debugInfo = i < asset.clusterDebugInfo.size() ? &asset.clusterDebugInfo[i] : nullptr;
        stream << "    { \"id\": " << i
            << ", \"mesh\": " << cluster.meshIndex
            << ", \"material\": " << cluster.materialIndex
            << ", \"triangles\": " << cluster.triangleCount
            << ", \"vertices\": " << cluster.vertexCount
            << ", \"boundaryVertices\": " << (debugInfo ? debugInfo->boundaryVertexCount : 0)
            << ", \"surfaceArea\": " << cluster.surfaceArea
            << ", \"coneAngle\": " << cluster.coneAngle
            << ", \"coneNormal\": ";
        writeFloat3(stream, cluster.coneNormal);
        stream << ", \"sourceMesh\": " << (debugInfo ? debugInfo->sourceMeshIndex : 0)
            << ", \"sourceMaterial\": " << (debugInfo ? debugInfo->sourceMaterialIndex : 0)
            << ", \"sourceTriangles\": ";
        writeTriangleRemap(stream, debugInfo);
        stream
            << ", \"bounds\": ";
        writeBounds(stream, cluster.bounds);
        stream << " }" << (i + 1 == asset.clusters.size() ? "\n" : ",\n");
    }
    stream << "  ]\n";
    stream << "}\n";
}
bool isGltfInput(const std::filesystem::path& path)
{
    const std::string extension = path.extension().string();
    std::string lower = extension;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower == ".gltf" || lower == ".glb";
}
}

int main(int argc, char** argv)
{
    try
    {
        Arguments args = parseArguments(argc, argv);

        if (args.rebuildFromSource)
        {
            if (args.input.extension() != ".fnanite")
            {
                throw std::runtime_error("--rebuild-from-source requires a .fnanite input file.");
            }

            std::cout << "Loading source from: " << args.input << '\n';
            Asset loaded = readAsset(args.input);
            InputScene scene = inputSceneFromSource(loaded);

            std::cout << "Rebuilding clusters from embedded source...\n";
            buildAndWriteScene(scene, args, args.output, args.debugJson);
            return 0;
        }

        const bool gltfInput = isGltfInput(args.input);
        const bool pbrtInput = args.input.extension() == ".pbrt";
        if (args.input.extension() != ".obj" && !gltfInput && !pbrtInput)
        {
            throw std::runtime_error("NaniteBuilder supports OBJ, glTF, GLB, and PBRT input.");
        }

        InputScene scene;
        if (gltfInput)
        {
            std::cout << "Loading glTF: " << args.input << '\n';
            scene = loadGltfScene(args.input);
        }
        else if (pbrtInput)
        {
            std::cout << "Loading PBRT: " << args.input << '\n';
            scene = loadPbrtScene(args.input, args.build.workerCount);
        }
        else
        {
            std::cout << "Loading OBJ: " << args.input << '\n';
            scene = loadObjScene(args.input);
        }

        size_t nonEmptyMeshCount = 0;
        for (const InputMesh& mesh : scene.meshes)
        {
            if (!mesh.indices.empty()) ++nonEmptyMeshCount;
        }

        const bool usePerMesh = !args.combined && nonEmptyMeshCount > 1;
        if (usePerMesh)
        {
            std::cout << "Building " << nonEmptyMeshCount << " per-mesh .fnanite assets...\n";
            size_t writtenCount = 0;
            for (size_t meshIndex = 0; meshIndex < scene.meshes.size(); ++meshIndex)
            {
                if (scene.meshes[meshIndex].indices.empty()) continue;

                const InputScene subScene = singleMeshScene(scene, meshIndex);
                const std::filesystem::path outputPath = perMeshOutputPath(args.output, subScene.meshes[0].name);
                const std::filesystem::path debugJsonPath = args.writeDebugJson ? perMeshDebugJsonPath(outputPath) : args.debugJson;
                buildAndWriteScene(subScene, args, outputPath, debugJsonPath);
                ++writtenCount;
            }

            std::cout << "Wrote " << writtenCount << " .fnanite files.\n";
            return 0;
        }

        std::cout << "Building clusters...\n";
        buildAndWriteScene(scene, args, args.output, args.debugJson);
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
