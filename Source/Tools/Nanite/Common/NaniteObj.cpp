#include "NaniteObj.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace FalcorRendering::NaniteTool
{
namespace
{
struct ObjRef
{
    int position = -1;
    int texCoord = -1;
    int normal = -1;
};

std::string trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;

    return value.substr(begin, end - begin);
}

int parseOptionalInt(const std::string& value)
{
    if (value.empty()) return 0;
    return std::stoi(value);
}

ObjRef parseObjRef(const std::string& token)
{
    ObjRef ref;

    const size_t firstSlash = token.find('/');
    if (firstSlash == std::string::npos)
    {
        ref.position = parseOptionalInt(token);
        return ref;
    }

    ref.position = parseOptionalInt(token.substr(0, firstSlash));
    const size_t secondSlash = token.find('/', firstSlash + 1);
    if (secondSlash == std::string::npos)
    {
        ref.texCoord = parseOptionalInt(token.substr(firstSlash + 1));
        return ref;
    }

    ref.texCoord = parseOptionalInt(token.substr(firstSlash + 1, secondSlash - firstSlash - 1));
    ref.normal = parseOptionalInt(token.substr(secondSlash + 1));
    return ref;
}

int resolveObjIndex(int index, size_t count)
{
    if (index > 0) return index - 1;
    if (index < 0) return static_cast<int>(count) + index;
    return -1;
}

Float3 getPosition(const std::vector<Float3>& positions, const ObjRef& ref, size_t lineNumber)
{
    const int index = resolveObjIndex(ref.position, positions.size());
    if (index < 0 || static_cast<size_t>(index) >= positions.size())
    {
        throw std::runtime_error("OBJ face references an invalid position at line " + std::to_string(lineNumber) + ".");
    }
    return positions[static_cast<size_t>(index)];
}

Float2 getTexCoord(const std::vector<Float2>& texCoords, const ObjRef& ref)
{
    const int index = resolveObjIndex(ref.texCoord, texCoords.size());
    if (index < 0 || static_cast<size_t>(index) >= texCoords.size()) return {};
    return texCoords[static_cast<size_t>(index)];
}

Float3 getNormal(const std::vector<Float3>& normals, const ObjRef& ref, Float3 fallback)
{
    const int index = resolveObjIndex(ref.normal, normals.size());
    if (index < 0 || static_cast<size_t>(index) >= normals.size()) return fallback;
    return normalize(normals[static_cast<size_t>(index)]);
}

uint32_t getMaterialIndex(InputScene& scene, const std::string& name)
{
    const std::string materialName = name.empty() ? "default" : name;
    for (uint32_t i = 0; i < scene.materialNames.size(); ++i)
    {
        if (scene.materialNames[i] == materialName) return i;
    }

    scene.materialNames.push_back(materialName);
    return static_cast<uint32_t>(scene.materialNames.size() - 1);
}

InputMesh& getMesh(InputScene& scene, std::unordered_map<std::string, size_t>& meshMap, const std::string& objectName, uint32_t materialIndex)
{
    const std::string meshName = objectName.empty() ? "mesh" : objectName;
    const std::string key = meshName + "\n" + std::to_string(materialIndex);
    auto it = meshMap.find(key);
    if (it != meshMap.end()) return scene.meshes[it->second];

    InputMesh mesh;
    mesh.name = meshName + ":" + scene.materialNames[materialIndex];
    mesh.materialIndex = materialIndex;
    mesh.bounds = emptyBounds();

    const size_t index = scene.meshes.size();
    scene.meshes.push_back(std::move(mesh));
    meshMap.emplace(key, index);
    return scene.meshes.back();
}

void appendTriangle(InputMesh& mesh, InputScene& scene, const Vertex& a, const Vertex& b, const Vertex& c)
{
    const uint32_t baseIndex = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(a);
    mesh.vertices.push_back(b);
    mesh.vertices.push_back(c);
    mesh.indices.push_back(baseIndex + 0);
    mesh.indices.push_back(baseIndex + 1);
    mesh.indices.push_back(baseIndex + 2);

    include(mesh.bounds, a.position);
    include(mesh.bounds, b.position);
    include(mesh.bounds, c.position);
    include(scene.bounds, a.position);
    include(scene.bounds, b.position);
    include(scene.bounds, c.position);
}

size_t hashFloat(float value)
{
    return std::hash<uint32_t>{}(std::bit_cast<uint32_t>(value));
}

struct VertexHasher
{
    size_t operator()(const Vertex& vertex) const
    {
        size_t hash = hashFloat(vertex.position.x);
        hash ^= hashFloat(vertex.position.y) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= hashFloat(vertex.position.z) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= hashFloat(vertex.normal.x) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= hashFloat(vertex.normal.y) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= hashFloat(vertex.normal.z) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= hashFloat(vertex.texCoord.x) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= hashFloat(vertex.texCoord.y) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct VertexEqual
{
    bool operator()(const Vertex& a, const Vertex& b) const
    {
        return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z
            && a.normal.x == b.normal.x && a.normal.y == b.normal.y && a.normal.z == b.normal.z
            && a.texCoord.x == b.texCoord.x && a.texCoord.y == b.texCoord.y;
    }
};
}

void deduplicateMeshVertices(InputMesh& mesh)
{
    if (mesh.vertices.size() <= 1) return;

    std::vector<Vertex> uniqueVertices;
    uniqueVertices.reserve(mesh.vertices.size());
    std::vector<uint32_t> remap(mesh.vertices.size());
    std::unordered_map<Vertex, uint32_t, VertexHasher, VertexEqual> vertexMap;

    for (size_t i = 0; i < mesh.vertices.size(); ++i)
    {
        const Vertex& vertex = mesh.vertices[i];
        auto it = vertexMap.find(vertex);
        if (it == vertexMap.end())
        {
            const uint32_t newIndex = static_cast<uint32_t>(uniqueVertices.size());
            vertexMap.emplace(vertex, newIndex);
            uniqueVertices.push_back(vertex);
            remap[i] = newIndex;
        }
        else
        {
            remap[i] = it->second;
        }
    }

    if (uniqueVertices.size() == mesh.vertices.size()) return;

    for (uint32_t& index : mesh.indices) index = remap[index];
    mesh.vertices = std::move(uniqueVertices);
}

InputScene loadObjScene(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Failed to open OBJ file: " + path.string());

    InputScene scene;
    scene.sourcePath = path;
    scene.bounds = emptyBounds();
    scene.materialNames.push_back("default");

    std::vector<Float3> positions;
    std::vector<Float2> texCoords;
    std::vector<Float3> normals;
    std::unordered_map<std::string, size_t> meshMap;

    std::string objectName = path.stem().string();
    uint32_t materialIndex = 0;
    std::string line;
    size_t lineNumber = 0;

    while (std::getline(stream, line))
    {
        ++lineNumber;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string tag;
        ss >> tag;

        if (tag == "v")
        {
            Float3 p;
            ss >> p.x >> p.y >> p.z;
            if (!ss) throw std::runtime_error("Invalid OBJ position at line " + std::to_string(lineNumber) + ".");
            positions.push_back(p);
        }
        else if (tag == "vt")
        {
            Float2 uv;
            ss >> uv.x >> uv.y;
            if (!ss) throw std::runtime_error("Invalid OBJ texcoord at line " + std::to_string(lineNumber) + ".");
            texCoords.push_back(uv);
        }
        else if (tag == "vn")
        {
            Float3 n;
            ss >> n.x >> n.y >> n.z;
            if (!ss) throw std::runtime_error("Invalid OBJ normal at line " + std::to_string(lineNumber) + ".");
            normals.push_back(normalize(n));
        }
        else if (tag == "usemtl")
        {
            std::string materialName;
            ss >> materialName;
            materialIndex = getMaterialIndex(scene, materialName);
        }
        else if (tag == "o" || tag == "g")
        {
            std::string name;
            ss >> name;
            if (!name.empty()) objectName = name;
        }
        else if (tag == "f")
        {
            std::vector<ObjRef> refs;
            std::string token;
            while (ss >> token) refs.push_back(parseObjRef(token));
            if (refs.size() < 3) continue;

            std::vector<Float3> polygonPositions;
            polygonPositions.reserve(refs.size());
            for (const ObjRef& ref : refs) polygonPositions.push_back(getPosition(positions, ref, lineNumber));

            const Float3 faceNormal = normalize(cross(polygonPositions[1] - polygonPositions[0], polygonPositions[2] - polygonPositions[0]));
            InputMesh& mesh = getMesh(scene, meshMap, objectName, materialIndex);

            for (size_t i = 1; i + 1 < refs.size(); ++i)
            {
                const ObjRef triRefs[3] = { refs[0], refs[i], refs[i + 1] };
                Vertex tri[3];
                for (size_t v = 0; v < 3; ++v)
                {
                    tri[v].position = getPosition(positions, triRefs[v], lineNumber);
                    tri[v].normal = getNormal(normals, triRefs[v], faceNormal);
                    tri[v].texCoord = getTexCoord(texCoords, triRefs[v]);
                }
                appendTriangle(mesh, scene, tri[0], tri[1], tri[2]);
            }
        }
    }

    std::erase_if(scene.meshes, [](const InputMesh& mesh) { return mesh.indices.empty(); });
    if (scene.meshes.empty()) throw std::runtime_error("OBJ file contains no triangle faces: " + path.string());

    return scene;
}
}
