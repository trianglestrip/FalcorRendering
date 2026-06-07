/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "TriangleMesh.h"
#include "GlobalState.h"
#include "Core/Error.h"
#include "Core/Platform/OS.h"
#include "Utils/Logger.h"
#include "Utils/Scripting/ScriptBindings.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cmath>
#include <fstream>
#include <sstream>

namespace Falcor
{
    namespace
    {
        constexpr float kMaxImportedPositionMagnitude = 1e4f;

        bool isFinite(const float2& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y);
        }

        bool isFinite(const float3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        bool isUsablePosition(const float3& p)
        {
            return isFinite(p) && all(abs(p) < float3(kMaxImportedPositionMagnitude));
        }

        float3 safeNormalize(const float3& v, const float3& fallback)
        {
            if (!isFinite(v)) return fallback;
            const float len = length(v);
            return len > 1e-12f && std::isfinite(len) ? v / len : fallback;
        }

        struct PlyProperty
        {
            std::string type;
            std::string name;
        };

        size_t getPlyScalarSize(const std::string& type)
        {
            if (type == "float" || type == "float32" || type == "int" || type == "int32" || type == "uint" || type == "uint32")
                return 4;
            if (type == "double" || type == "float64")
                return 8;
            if (type == "char" || type == "int8" || type == "uchar" || type == "uint8")
                return 1;
            if (type == "short" || type == "int16" || type == "ushort" || type == "uint16")
                return 2;
            return 0;
        }

        bool readPlyFloat(std::istream& stream, const std::string& type, float& value)
        {
            if (type == "float" || type == "float32")
            {
                stream.read(reinterpret_cast<char*>(&value), sizeof(float));
                return bool(stream);
            }
            if (type == "double" || type == "float64")
            {
                double v = 0.0;
                stream.read(reinterpret_cast<char*>(&v), sizeof(double));
                value = static_cast<float>(v);
                return bool(stream);
            }

            const size_t size = getPlyScalarSize(type);
            if (size == 0) return false;
            char discard[8] = {};
            stream.read(discard, size);
            value = 0.f;
            return bool(stream);
        }

        bool readPlyIndex(std::istream& stream, const std::string& type, int32_t& value)
        {
            if (type == "int" || type == "int32")
            {
                stream.read(reinterpret_cast<char*>(&value), sizeof(int32_t));
                return bool(stream);
            }
            if (type == "uint" || type == "uint32")
            {
                uint32_t v = 0;
                stream.read(reinterpret_cast<char*>(&v), sizeof(uint32_t));
                value = static_cast<int32_t>(v);
                return bool(stream);
            }

            return false;
        }

        ref<TriangleMesh> tryCreateBinaryLittleEndianPly(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) return nullptr;

            std::string line;
            if (!std::getline(stream, line) || line != "ply") return nullptr;

            bool binaryLittleEndian = false;
            bool inVertex = false;
            bool hasFaceList = false;
            size_t vertexCount = 0;
            size_t faceCount = 0;
            std::string faceCountType;
            std::string faceIndexType;
            std::vector<PlyProperty> vertexProperties;

            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "end_header") break;

                std::istringstream ls(line);
                std::string token;
                ls >> token;
                if (token == "format")
                {
                    std::string format;
                    ls >> format;
                    binaryLittleEndian = format == "binary_little_endian";
                }
                else if (token == "element")
                {
                    std::string element;
                    size_t count = 0;
                    ls >> element >> count;
                    inVertex = element == "vertex";
                    if (element == "vertex") vertexCount = count;
                    else if (element == "face") faceCount = count;
                }
                else if (token == "property")
                {
                    std::string type;
                    ls >> type;
                    if (inVertex)
                    {
                        std::string name;
                        ls >> name;
                        if (getPlyScalarSize(type) == 0) return nullptr;
                        vertexProperties.push_back({type, name});
                    }
                    else if (type == "list")
                    {
                        std::string name;
                        ls >> faceCountType >> faceIndexType >> name;
                        hasFaceList = name == "vertex_indices" && faceCountType == "uint8" && (faceIndexType == "int" || faceIndexType == "int32");
                    }
                }
            }

            if (!binaryLittleEndian || vertexCount == 0 || faceCount == 0 || vertexProperties.empty() || !hasFaceList)
                return nullptr;

            TriangleMesh::VertexList vertices;
            vertices.reserve(vertexCount);

            for (size_t i = 0; i < vertexCount; ++i)
            {
                float3 position(0.f);
                float3 normal(0.f, 1.f, 0.f);
                float2 texCoord(0.f);

                for (const auto& property : vertexProperties)
                {
                    float value = 0.f;
                    if (!readPlyFloat(stream, property.type, value)) return nullptr;

                    if (property.name == "x") position.x = value;
                    else if (property.name == "y") position.y = value;
                    else if (property.name == "z") position.z = value;
                    else if (property.name == "nx") normal.x = value;
                    else if (property.name == "ny") normal.y = value;
                    else if (property.name == "nz") normal.z = value;
                    else if (property.name == "u" || property.name == "s") texCoord.x = value;
                    else if (property.name == "v" || property.name == "t") texCoord.y = value;
                }

                vertices.emplace_back(TriangleMesh::Vertex{
                    position,
                    safeNormalize(normal, float3(0.f, 1.f, 0.f)),
                    isFinite(texCoord) ? texCoord : float2(0.f)
                });
            }

            TriangleMesh::IndexList indices;
            indices.reserve(faceCount * 3);
            size_t skippedFaceCount = 0;
            size_t skippedInvalidFaceCount = 0;

            for (size_t faceIdx = 0; faceIdx < faceCount; ++faceIdx)
            {
                uint8_t count = 0;
                stream.read(reinterpret_cast<char*>(&count), sizeof(uint8_t));
                if (!stream) return nullptr;

                std::vector<int32_t> faceIndices(count);
                for (uint8_t i = 0; i < count; ++i)
                {
                    if (!readPlyIndex(stream, faceIndexType, faceIndices[i])) return nullptr;
                }

                if (count != 3)
                {
                    skippedFaceCount++;
                    continue;
                }

                bool validFace = true;
                for (uint8_t i = 0; i < 3; ++i)
                {
                    const int32_t index = faceIndices[i];
                    if (index < 0 || static_cast<size_t>(index) >= vertices.size() || !isUsablePosition(vertices[index].position))
                    {
                        validFace = false;
                        break;
                    }
                }
                if (!validFace)
                {
                    skippedInvalidFaceCount++;
                    continue;
                }

                for (uint8_t i = 0; i < 3; ++i)
                    indices.emplace_back(static_cast<uint32_t>(faceIndices[i]));
            }

            if (indices.empty())
                return nullptr;

            if (skippedFaceCount > 0)
                logDebug("Skipped {} non-triangle face(s) while loading binary PLY triangle mesh from '{}'.", skippedFaceCount, path);
            if (skippedInvalidFaceCount > 0)
                logDebug("Skipped {} face(s) with invalid vertex positions while loading binary PLY triangle mesh from '{}'.", skippedInvalidFaceCount, path);

            return TriangleMesh::create(vertices, indices);
        }
    }

    ref<TriangleMesh> TriangleMesh::create()
    {
        return ref<TriangleMesh>(new TriangleMesh());
    }

    ref<TriangleMesh> TriangleMesh::create(const VertexList& vertices, const IndexList& indices, bool frontFaceCW)
    {
        return ref<TriangleMesh>(new TriangleMesh(vertices, indices, frontFaceCW));
    }

    ref<TriangleMesh> TriangleMesh::createDummy()
    {
        VertexList vertices = {{{0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f}}};
        IndexList indices = {0, 0, 0};
        return create(vertices, indices);
    }

    ref<TriangleMesh> TriangleMesh::createQuad(float2 size)
    {
        float2 hsize = 0.5f * size;
        float3 normal{0.f, 1.f, 0.f};
        bool frontFaceCW = size.x * size.y < 0.f;

        VertexList vertices{
            {{ -hsize.x, 0.f, -hsize.y }, normal, { 0.f, 0.f }},
            {{  hsize.x, 0.f, -hsize.y }, normal, { 1.f, 0.f }},
            {{ -hsize.x, 0.f,  hsize.y }, normal, { 0.f, 1.f }},
            {{  hsize.x, 0.f,  hsize.y }, normal, { 1.f, 1.f }},
        };

        IndexList indices{
            2, 1, 0,
            1, 2, 3,
        };

        return create(vertices, indices, frontFaceCW);
    }

    ref<TriangleMesh> TriangleMesh::createDisk(float radius, uint32_t segments)
    {
        std::vector<Vertex> vertices(segments + 1);
        std::vector<uint32_t> indices(segments * 3);

        float3 normal = { 0.f, 1.f, 0.f };
        vertices[0] = { { 0.f, 0.f, 0.f }, normal, { 0.5f, 0.5f } };

        for (uint32_t i = 0; i < segments; ++i)
        {
            float phi = float(i) / float(segments) * 2.f * (float)M_PI;
            float c = std::cos(phi);
            float s = -std::sin(phi);
            vertices[i + 1] = { { c * radius, 0.f, s * radius }, normal, { 0.5f + c * 0.5f, 0.5f + s * 0.5f } };

            indices[i * 3] = 0;
            indices[i * 3 + 1] = i + 1;
            indices[i * 3 + 2] = ((i + 1) % segments) + 1;
        }

        return create(vertices, indices, false);
    }

    ref<TriangleMesh> TriangleMesh::createCube(float3 size)
    {
        const float3 positions[6][4] =
        {
            {{ -0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f,  0.5f }, { 0.5f, -0.5f,  0.5f }, { 0.5f, -0.5f, -0.5f }},
            {{ -0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f, -0.5f }, { 0.5f,  0.5f, -0.5f }, { 0.5f,  0.5f,  0.5f }},
            {{ -0.5f,  0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f }, { 0.5f,  0.5f, -0.5f }},
            {{  0.5f,  0.5f,  0.5f }, {  0.5f, -0.5f,  0.5f }, {-0.5f, -0.5f,  0.5f }, {-0.5f,  0.5f,  0.5f }},
            {{ -0.5f,  0.5f,  0.5f }, { -0.5f, -0.5f,  0.5f }, {-0.5f, -0.5f, -0.5f }, {-0.5f,  0.5f, -0.5f }},
            {{  0.5f,  0.5f, -0.5f }, {  0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f,  0.5f }, { 0.5f,  0.5f,  0.5f }},
        };

        const float3 normals[6] =
        {
            { 0.f, -1.f, 0.f },
            { 0.f, 1.f, 0.f },
            { 0.f, 0.f, -1.f },
            { 0.f, 0.f, 1.f },
            { -1.f, 0.f, 0.f },
            { 1.f, 0.f, 0.f },
        };

        const float2 texCoords[4] = {{ 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f }};

        VertexList vertices;
        IndexList indices;

        float3 sign = { size.x < 0.f ? -1.f : 1.f, size.y < 0.f ? -1.f : 1.f, size.z < 0.f ? -1.f : 1.f };
        bool frontFaceCW = size.x * size.y * size.z < 0.f;

        for (size_t i = 0; i < 6; ++i)
        {
            uint32_t idx = (uint32_t)vertices.size();
            indices.emplace_back(idx);
            indices.emplace_back(idx + 2);
            indices.emplace_back(idx + 1);
            indices.emplace_back(idx);
            indices.emplace_back(idx + 3);
            indices.emplace_back(idx + 2);

            for (size_t j = 0; j < 4; ++j)
            {
                vertices.emplace_back(Vertex{ positions[i][j] * size, normals[i] * sign, texCoords[j] });
            }
        }

        return create(vertices, indices, frontFaceCW);
    }

    ref<TriangleMesh> TriangleMesh::createSphere(float radius, uint32_t segmentsU, uint32_t segmentsV)
    {
        VertexList vertices;
        IndexList indices;

        // Create vertices.
        for (uint32_t v = 0; v <= segmentsV; ++v)
        {
            for (uint32_t u = 0; u <= segmentsU; ++u)
            {
                float2 uv = float2(u / float(segmentsU), v / float(segmentsV));
                float theta = uv.x * 2.f * (float)M_PI;
                float phi = uv.y * (float)M_PI;
                float3 dir = float3(
                    std::cos(theta) * std::sin(phi),
                    std::cos(phi),
                    std::sin(theta) * std::sin(phi)
                );
                vertices.emplace_back(Vertex{ dir * radius, dir, uv });
            }
        }

        // Create indices.
        for (uint32_t v = 0; v < segmentsV; ++v)
        {
            for (uint32_t u = 0; u < segmentsU; ++u)
            {
                uint32_t i0 = v * (segmentsU + 1) + u;
                uint32_t i1 = v * (segmentsU + 1) + (u + 1) % (segmentsU + 1);
                uint32_t i2 = (v + 1) * (segmentsU + 1) + u;
                uint32_t i3 = (v + 1) * (segmentsU + 1) + (u + 1) % (segmentsU + 1);

                indices.emplace_back(i0);
                indices.emplace_back(i1);
                indices.emplace_back(i2);

                indices.emplace_back(i2);
                indices.emplace_back(i1);
                indices.emplace_back(i3);
            }
        }

        return create(vertices, indices);
    }

    ref<TriangleMesh> TriangleMesh::createFromFile(const std::filesystem::path& path, ImportFlags importFlags)
    {
        if (!std::filesystem::exists(path))
        {
            logWarning("Failed to load triangle mesh from '{}': File not found", path);
            return nullptr;
        }

        Assimp::Importer importer;

        unsigned int flags =
            aiProcess_FlipUVs |
            aiProcess_Triangulate |
            aiProcess_PreTransformVertices;
        flags |= is_set(importFlags, ImportFlags::GenSmoothNormals) ? aiProcess_GenSmoothNormals : aiProcess_GenNormals;
        flags |= is_set(importFlags, ImportFlags::JoinIdenticalVertices) ? aiProcess_JoinIdenticalVertices : 0;

        const aiScene* scene = nullptr;

        if (hasExtension(path, "ply"))
        {
            if (auto pTriangleMesh = tryCreateBinaryLittleEndianPly(path))
                return pTriangleMesh;
        }

        if (hasExtension(path, "gz"))
        {
            auto decompressed = decompressFile(path);
            scene = importer.ReadFileFromMemory(decompressed.data(), decompressed.size(), flags);
        }
        else
        {
            scene = importer.ReadFile(path.string().c_str(), flags);
        }

        if (!scene)
        {
            logWarning("Failed to load triangle mesh from '{}': {}", path, importer.GetErrorString());
            return nullptr;
        }

        VertexList vertices;
        IndexList indices;

        size_t vertexCount = 0;
        size_t indexCount = 0;

        for (size_t meshIdx = 0; meshIdx < scene->mNumMeshes; ++meshIdx)
        {
            vertexCount += scene->mMeshes[meshIdx]->mNumVertices;
            indexCount += scene->mMeshes[meshIdx]->mNumFaces * 3;
        }

        vertices.reserve(vertexCount);
        indices.reserve(indexCount);

        size_t skippedFaceCount = 0;
        size_t skippedInvalidFaceCount = 0;
        for (size_t meshIdx = 0; meshIdx < scene->mNumMeshes; ++meshIdx)
        {
            size_t indexBase = vertices.size();
            auto mesh = scene->mMeshes[meshIdx];
            for (size_t vertexIdx = 0; vertexIdx < mesh->mNumVertices; ++vertexIdx)
            {
                const auto& vertex = mesh->mVertices[vertexIdx];
                const auto normal = mesh->mNormals ? mesh->mNormals[vertexIdx] : aiVector3D(0.f, 1.f, 0.f);
                const auto& texCoord = mesh->mTextureCoords[0] ? mesh->mTextureCoords[0][vertexIdx] : aiVector3D(0.f);
                const float3 position(vertex.x, vertex.y, vertex.z);
                const float3 sanitizedNormal = safeNormalize(float3(normal.x, normal.y, normal.z), float3(0.f, 1.f, 0.f));
                const float2 sanitizedTexCoord = isFinite(float2(texCoord.x, texCoord.y)) ? float2(texCoord.x, texCoord.y) : float2(0.f);
                vertices.emplace_back(Vertex{
                    position,
                    sanitizedNormal,
                    sanitizedTexCoord
                });
            }
            for (size_t faceIdx = 0; faceIdx < mesh->mNumFaces; ++faceIdx)
            {
                const auto& face = mesh->mFaces[faceIdx];
                if (face.mNumIndices != 3)
                {
                    skippedFaceCount++;
                    continue;
                }
                bool validFace = true;
                for (size_t i = 0; i < 3; ++i)
                {
                    if (face.mIndices[i] >= mesh->mNumVertices || !isUsablePosition(vertices[indexBase + face.mIndices[i]].position))
                    {
                        validFace = false;
                        break;
                    }
                }
                if (!validFace)
                {
                    skippedInvalidFaceCount++;
                    continue;
                }
                for (size_t i = 0; i < 3; ++i) indices.emplace_back((uint32_t)(indexBase + face.mIndices[i]));
            }
        }

        if (indices.empty())
        {
            if (skippedFaceCount > 0 || skippedInvalidFaceCount > 0)
                logDebug("Skipped triangle mesh from '{}': no valid triangle faces after filtering.", path);
            else
                logWarning("Failed to load triangle mesh from '{}': No valid triangle faces", path);
            return nullptr;
        }

        if (skippedFaceCount > 0)
            logDebug("Skipped {} non-triangle/broken face(s) while loading triangle mesh from '{}'.", skippedFaceCount, path);
        if (skippedInvalidFaceCount > 0)
            logDebug("Skipped {} face(s) with invalid vertex positions while loading triangle mesh from '{}'.", skippedInvalidFaceCount, path);

        return create(vertices, indices);
    }

    ref<TriangleMesh> TriangleMesh::createFromFile(const std::filesystem::path& path, bool smoothNormals)
    {
        ImportFlags flags = smoothNormals ? ImportFlags::GenSmoothNormals : ImportFlags::None;
        return createFromFile(path, flags);
    }

    uint32_t TriangleMesh::addVertex(float3 position, float3 normal, float2 texCoord)
    {
        mVertices.emplace_back(Vertex{position, normal, texCoord});
        FALCOR_ASSERT(mVertices.size() < std::numeric_limits<uint32_t>::max());
        return (uint32_t)(mVertices.size() - 1);
    }

    void TriangleMesh::addTriangle(uint32_t i0, uint32_t i1, uint32_t i2)
    {
        mIndices.emplace_back(i0);
        mIndices.emplace_back(i1);
        mIndices.emplace_back(i2);
    }

    void TriangleMesh::applyTransform(const Transform& transform)
    {
        applyTransform(transform.getMatrix());
    }

    void TriangleMesh::applyTransform(const float4x4& transform)
    {
        auto invTranspose = float3x3(transpose(inverse(transform)));

        for (auto& vertex : mVertices)
        {
            vertex.position = transformPoint(transform, vertex.position);
            vertex.normal = safeNormalize(transformVector(invTranspose, vertex.normal), float3(0.f, 1.f, 0.f));
        }

        // Check if triangle winding has flipped and adjust winding order accordingly.
        bool flippedWinding = determinant(float3x3(transform)) < 0.f;
        if (flippedWinding) mFrontFaceCW = !mFrontFaceCW;
    }

    TriangleMesh::TriangleMesh()
    {}

    TriangleMesh::TriangleMesh(const VertexList& vertices, const IndexList& indices, bool frontFaceCW)
        : mVertices(vertices)
        , mIndices(indices)
        , mFrontFaceCW(frontFaceCW)
    {}

    FALCOR_SCRIPT_BINDING(TriangleMesh)
    {
        using namespace pybind11::literals;

        pybind11::enum_<TriangleMesh::ImportFlags> flags(m, "TriangleMeshImportFlags");
        flags.value("Default", TriangleMesh::ImportFlags::Default);
        flags.value("GenSmoothNormals", TriangleMesh::ImportFlags::GenSmoothNormals);
        flags.value("JoinIdenticalVertices", TriangleMesh::ImportFlags::JoinIdenticalVertices);
        ScriptBindings::addEnumBinaryOperators(flags);

        pybind11::class_<TriangleMesh, ref<TriangleMesh>> triangleMesh(m, "TriangleMesh");

        pybind11::class_<TriangleMesh::Vertex> vertex(triangleMesh, "Vertex");
        vertex.def_readwrite("position", &TriangleMesh::Vertex::position);
        vertex.def_readwrite("normal", &TriangleMesh::Vertex::normal);
        vertex.def_readwrite("texCoord", &TriangleMesh::Vertex::texCoord);

        triangleMesh.def_property("name", &TriangleMesh::getName, &TriangleMesh::setName);
        triangleMesh.def_property("frontFaceCW", &TriangleMesh::getFrontFaceCW, &TriangleMesh::setFrontFaceCW);
        triangleMesh.def_property_readonly("vertices", &TriangleMesh::getVertices);
        triangleMesh.def_property_readonly("indices", &TriangleMesh::getIndices);
        triangleMesh.def(pybind11::init(pybind11::overload_cast<>(&TriangleMesh::create)));
        triangleMesh.def("addVertex", &TriangleMesh::addVertex, "position"_a, "normal"_a, "texCoord"_a);
        triangleMesh.def("addTriangle", &TriangleMesh::addTriangle, "i0"_a, "i1"_a, "i2"_a);
        triangleMesh.def_static("createQuad", &TriangleMesh::createQuad, "size"_a = float2(1.f));
        triangleMesh.def_static("createDisk", &TriangleMesh::createDisk, "radius"_a = 1.f, "segments"_a = 32);
        triangleMesh.def_static("createCube", &TriangleMesh::createCube, "size"_a = float3(1.f));
        triangleMesh.def_static("createSphere", &TriangleMesh::createSphere, "radius"_a = 1.f, "segmentsU"_a = 32, "segmentsV"_a = 32);
        triangleMesh.def_static("createFromFile",
            [](const std::filesystem::path& path, bool smoothNormals)
            { return TriangleMesh::createFromFile(getActiveAssetResolver().resolvePath(path), smoothNormals); },
            "path"_a, "smoothNormals"_a = false
        ); // PYTHONDEPRECATED
        triangleMesh.def_static("createFromFile",
            [](const std::filesystem::path& path, TriangleMesh::ImportFlags importFlags)
            { return TriangleMesh::createFromFile(getActiveAssetResolver().resolvePath(path), importFlags); },
            "path"_a, "importFlags"_a
        ); // PYTHONDEPRECATED
    }
}
