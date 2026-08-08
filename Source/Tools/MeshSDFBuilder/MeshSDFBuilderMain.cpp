// =====================================================================================
//  MeshSDFBuilderMain.cpp - command-line entry point for the S6-A1 Mesh SDF builder.
//  -------------------------------------------------------------------------------------
//  USAGE
//  -------------------------------------------------------------------------------------
//  MeshSDFBuilder.exe --input <mesh.obj> --output <out.msdf> [options]
//
//  Options:
//    --input <path>             Input Wavefront OBJ file (positions + faces; see NOTES)
//    --output <path>            Output .msdf volume (default: input with .msdf extension)
//    --report <path>            Optional JSON summary (default: <output>.json)
//    --resolution <N|Nx,Ny,Nz>  Voxel count along the longest grid axis (default 128);
//                               "Nx,Ny,Nz" fixes per-axis counts
//    --padding <units>          World-space margin around the mesh bbox (default: auto
//                               = 10% of the largest mesh extent; 0 selects auto)
//    --bbox <minX,minY,minZ,maxX,maxY,maxZ>  Override the grid bounds (world space)
//    --no-normalize             Keep mesh coordinates as-is (normalizationScale = 1)
//    --brute-force              Use the naive O(V*T) reference distance pass instead
//                               of the BVH (identical results; slower on large grids)
//    --seed <n>                 Deterministic seed for ray jitter (default 0)
//    --help                     Show this help and exit
//
//  Exit codes: 0 success (warnings may still be reported), 1 usage/input error,
//  2 I/O error, 3 internal error.
//
//  NOTES
//  -------------------------------------------------------------------------------------
//  * Sign convention: distance > 0 outside, < 0 inside (SignConvention::PositiveOutside).
//    Open/thin/non-manifold meshes are flagged via warnings and signReliable = 0 in
//    the output header - the tool never silently "fixes" or falls back.
//  * .msdf is a self-describing little-endian binary; see MeshSDFBuilder.h for the
//    exact layout and the meaning of every header field.
//  * OBJ parser subset: `v x y z` and `f` faces with v[/vt][/vn] indices (1-based or
//    negative relative). Quads and larger polygons are triangulated as fans. `vt`,
//    `vn`, `o`, `g`, `s`, `usemtl`, `mtllib`, `l`, `p` and comments are ignored.
//    Vertex normals are NOT used (recomputed from winding). Malformed lines,
//    out-of-range indices and non-finite coordinates are hard errors.
//  * This tool is standalone: standard library only, no Falcor, no CMake target yet.
//
//  BUILD (from this directory)
//  -------------------------------------------------------------------------------------
//  MSVC:   cl /nologo /std:c++17 /O2 /EHsc /W4 MeshSDFBuilder.cpp MeshSDFBuilderMain.cpp /Fe:MeshSDFBuilder.exe
//  GCC:    g++ -std=c++17 -O2 MeshSDFBuilder.cpp MeshSDFBuilderMain.cpp -o MeshSDFBuilder
//
//  EXAMPLES
//  -------------------------------------------------------------------------------------
//  MeshSDFBuilder.exe --input cube.obj --output cube.msdf --resolution 128 --padding 0.1
//  MeshSDFBuilder.exe --input sponza.obj --resolution 256 --bbox -50,-50,-50,50,50,50 --no-normalize
// =====================================================================================

#include "MeshSDFBuilder.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct Args
{
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path report;
    LumenGI::MeshSDF::SDFBuildParams params;
    bool help = false;
};

void printHelp(std::ostream& os)
{
    os << "MeshSDFBuilder (LumenGI S6-A1) - signed distance field volume from a triangle mesh\n"
          "\n"
          "USAGE\n"
          "  MeshSDFBuilder.exe --input <mesh.obj> --output <out.msdf> [options]\n"
          "\n"
          "OPTIONS\n"
          "  --input <path>             Input Wavefront OBJ file (positions + faces)\n"
          "  --output <path>            Output .msdf volume (default: input with .msdf extension)\n"
          "  --report <path>            Optional JSON summary (default: <output>.json)\n"
          "  --resolution <N|Nx,Ny,Nz>  Voxel count along the longest grid axis (default 128);\n"
          "                             \"Nx,Ny,Nz\" fixes per-axis voxel counts\n"
          "  --padding <units>          World-space margin around the mesh bbox (default: auto =\n"
          "                             10% of the largest mesh extent; 0 selects auto)\n"
          "  --bbox <minX,minY,minZ,maxX,maxY,maxZ>\n"
          "                             Override the grid bounds (world space)\n"
          "  --no-normalize             Keep mesh coordinates as-is (normalizationScale = 1)\n"
          "  --brute-force              Use the naive O(V*T) reference distance pass instead of\n"
          "                             the BVH (identical results; slower on large grids)\n"
          "  --seed <n>                 Deterministic seed for ray jitter (default 0)\n"
          "  --help                     Show this help and exit\n"
          "\n"
          "EXIT CODES\n"
          "  0 success (warnings may still be reported on stdout)\n"
          "  1 usage or input error   2 I/O error   3 internal error\n"
          "\n"
          "SIGN CONVENTION\n"
          "  distance > 0 => outside, distance < 0 => inside (recorded in the output header).\n"
          "  Open / thin / non-manifold meshes are FLAGGED (warnings + signReliable=0),\n"
          "  never silently fixed.\n"
          "\n"
          "OBJ SUBSET\n"
          "  v x y z ; f with v[/vt][/vn] (1-based or negative relative indices); polygons\n"
          "  with >= 3 vertices are fan-triangulated. vt/vn/o/g/s/usemtl/mtllib/l/p and\n"
          "  comments are ignored. Vertex normals are NOT used (recomputed from winding).\n"
          "  Malformed lines, out-of-range indices and non-finite coordinates are hard errors.\n"
          "\n"
          "OUTPUT FORMAT\n"
          "  .msdf: self-describing little-endian binary. Layout and semantics are documented\n"
          "  in the file header of MeshSDFBuilder.h (magic 'MSDF', version 1, 88-byte header,\n"
          "  warnings section, x-fastest float data, trailing FNV-1a64 checksum).\n"
          "\n"
          "EXAMPLES\n"
          "  MeshSDFBuilder.exe --input cube.obj --output cube.msdf --resolution 128 --padding 0.1\n"
          "  MeshSDFBuilder.exe --input sponza.obj --resolution 256 --bbox -50,-50,-50,50,50,50 --no-normalize\n"
          "\n"
          "BUILD\n"
          "  cl /nologo /std:c++17 /O2 /EHsc /W4 MeshSDFBuilder.cpp MeshSDFBuilderMain.cpp /Fe:MeshSDFBuilder.exe\n"
          "  g++ -std=c++17 -O2 MeshSDFBuilder.cpp MeshSDFBuilderMain.cpp -o MeshSDFBuilder\n";
}

bool parseU32(const std::string& s, uint32_t& out)
{
    if (s.empty())
        return false;
    for (char c : s)
    {
        if (c < '0' || c > '9')
            return false;
    }
    try
    {
        unsigned long long v = std::stoull(s);
        if (v > 0xFFFFFFFFULL)
            return false;
        out = uint32_t(v);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool parseFloat(const std::string& s, float& out)
{
    if (s.empty())
        return false;
    try
    {
        size_t pos = 0;
        out = std::stof(s, &pos);
        return pos == s.size();
    }
    catch (...)
    {
        return false;
    }
}

// Parses a comma-separated list of floats; expects exactly `count` values.
bool parseFloats(const std::string& s, size_t count, std::vector<float>& out)
{
    out.clear();
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        float v = 0.f;
        if (!parseFloat(item, v))
            return false;
        out.push_back(v);
    }
    return out.size() == count;
}

// --resolution accepts either a single number or "Nx,Ny,Nz".
bool parseResolution(const std::string& s, Args& args, std::string& err)
{
    if (s.find(',') == std::string::npos)
    {
        uint32_t n = 0;
        if (!parseU32(s, n))
        {
            err = "invalid --resolution value '" + s + "'";
            return false;
        }
        args.params.maxResolution = n;
    }
    else
    {
        std::vector<float> v;
        if (!parseFloats(s, 3, v))
        {
            err = "invalid --resolution value '" + s + "' (expected N or Nx,Ny,Nz)";
            return false;
        }
        for (int i = 0; i < 3; ++i)
        {
            args.params.fixedResolution[i] = uint32_t(v[i]);
            if (float(args.params.fixedResolution[i]) != v[i])
            {
                err = "invalid --resolution value '" + s + "' (must be integers)";
                return false;
            }
        }
    }
    return true;
}

bool parseArgs(int argc, char** argv, Args& args, std::string& err)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
            {
                err = std::string("missing value for ") + name;
                return nullptr;
            }
            ++i;
            return argv[i];
        };
        if (arg == "--help" || arg == "-h")
        {
            args.help = true;
        }
        else if (arg == "--input")
        {
            const char* v = needValue("--input");
            if (!v)
                return false;
            args.input = v;
        }
        else if (arg == "--output")
        {
            const char* v = needValue("--output");
            if (!v)
                return false;
            args.output = v;
        }
        else if (arg == "--report")
        {
            const char* v = needValue("--report");
            if (!v)
                return false;
            args.report = v;
        }
        else if (arg == "--resolution")
        {
            const char* v = needValue("--resolution");
            if (!v)
                return false;
            if (!parseResolution(v, args, err))
                return false;
        }
        else if (arg == "--padding")
        {
            const char* v = needValue("--padding");
            if (!v)
                return false;
            if (!parseFloat(v, args.params.paddingWorld))
            {
                err = "invalid --padding value '" + std::string(v) + "'";
                return false;
            }
        }
        else if (arg == "--bbox")
        {
            const char* v = needValue("--bbox");
            if (!v)
                return false;
            std::vector<float> b;
            if (!parseFloats(v, 6, b))
            {
                err = "invalid --bbox value '" + std::string(v) + "' (expected minX,minY,minZ,maxX,maxY,maxZ)";
                return false;
            }
            args.params.bboxOverride = std::array<float, 6>{b[0], b[1], b[2], b[3], b[4], b[5]};
        }
        else if (arg == "--no-normalize")
        {
            args.params.normalize = false;
        }
        else if (arg == "--brute-force")
        {
            args.params.useBruteForce = true;
        }
        else if (arg == "--seed")
        {
            const char* v = needValue("--seed");
            if (!v)
                return false;
            uint32_t n = 0;
            if (!parseU32(v, n))
            {
                err = "invalid --seed value '" + std::string(v) + "'";
                return false;
            }
            args.params.seed = n;
        }
        else
        {
            err = "unknown option '" + arg + "'";
            return false;
        }
    }

    if (args.input.empty())
    {
        err = "no input mesh specified (--input <file.obj>)";
        return false;
    }
    if (args.output.empty())
    {
        std::filesystem::path p = args.input;
        p.replace_extension(".msdf");
        args.output = p;
    }
    if (args.report.empty())
    {
        args.report = args.output;
        args.report += ".json";
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Args args;
    std::string err;
    if (!parseArgs(argc, argv, args, err))
    {
        std::cerr << "MeshSDFBuilder: " << err << "\n\n";
        printHelp(std::cerr);
        return 1;
    }
    if (args.help)
    {
        printHelp(std::cout);
        return 0;
    }

    namespace msdf = LumenGI::MeshSDF;

    // Load and parse the OBJ.
    std::ifstream in(args.input, std::ios::binary);
    if (!in)
    {
        std::cerr << "MeshSDFBuilder: cannot open input file '" << args.input.string() << "'\n";
        return 2;
    }
    msdf::Mesh mesh;
    if (!msdf::parseOBJ(in, mesh, err))
    {
        std::cerr << "MeshSDFBuilder: failed parsing '" << args.input.string() << "': " << err << "\n";
        return 1;
    }

    // Build.
    msdf::SDFBuildResult result;
    if (!msdf::buildMeshSDF(mesh, args.params, result, err))
    {
        std::cerr << "MeshSDFBuilder: build failed: " << err << "\n";
        return 1;
    }

    // Serialize.
    if (!msdf::writeMSDF(args.output, result, err))
    {
        std::cerr << "MeshSDFBuilder: " << err << "\n";
        return 2;
    }
    if (!msdf::writeJSONReport(args.report, result, err))
    {
        std::cerr << "MeshSDFBuilder: " << err << "\n";
        return 2;
    }

    // Summary to stdout.
    const msdf::SDFHeader& h = result.header;
    std::cout << "MeshSDFBuilder (LumenGI S6-A1)\n";
    std::cout << "input     : " << args.input.string() << " (" << result.analysis.vertexCount << " verts, "
              << result.analysis.triangleCount << " tris)\n";
    std::cout << "content   : hash 0x" << result.contentHashHex << "\n";
    std::cout << "watertight: " << (result.analysis.watertight ? "yes" : "NO") << "  (boundary edges: "
              << result.analysis.boundaryEdgeCount << ", non-manifold: "
              << result.analysis.nonManifoldEdgeCount << ", components: "
              << result.analysis.componentCount << ", degenerate: "
              << result.analysis.degenerateTriangleCount << ")\n";
    std::cout << "grid      : " << h.resolution[0] << "x" << h.resolution[1] << "x" << h.resolution[2]
              << " voxels, voxelSize " << h.voxelSize << ", normalizeScale " << h.normalizationScale
              << ", bbox [" << h.bboxMin[0] << ", " << h.bboxMin[1] << ", " << h.bboxMin[2] << "] .. ["
              << h.bboxMax[0] << ", " << h.bboxMax[1] << ", " << h.bboxMax[2] << "]\n";
    std::cout << "sign      : convention positive-outside/negative-inside, reliable="
              << (h.signReliable ? "yes" : "NO") << ", ambiguous voxels " << result.ambiguousVoxelCount << " ("
              << (result.ambiguousRatio * 100.f) << "%), vote disagreements " << result.voteDisagreementCount
              << ", thin voxels " << result.thinVoxelCount << "\n";
    for (const msdf::Warning& w : result.warnings)
        std::cout << "warning   : [" << msdf::warningCodeName(w.code) << "] " << w.message << "\n";
    std::cout << "output    : " << args.output.string() << "\n";
    std::cout << "report    : " << args.report.string() << "\n";
    std::cout << "timing    : analysis " << result.analysisMs << " ms, hash " << result.hashMs << " ms, distance "
              << result.distanceMs << " ms, total " << result.totalMs << " ms\n";
    return 0;
}
