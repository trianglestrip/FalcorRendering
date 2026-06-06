/*
 * Taskflow-accelerated PBRT scene loading: parallel texture decode and mesh verification.
 */

#include <pbrtio/PbrtSceneLoader.h>

#ifdef PBRTIO_STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include <stb_image.h>

#include <taskflow.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pbrtio {

using Clock = std::chrono::steady_clock;

static void releaseDecodedPixels(PbrtDecodedImage& image) {
    if (image.pixels) {
        stbi_image_free(image.pixels);
        image.pixels = nullptr;
    }
    image.byteSize = 0;
    image.valid = false;
}

PbrtDecodedImage::PbrtDecodedImage(PbrtDecodedImage&& other) noexcept {
    *this = std::move(other);
}

PbrtDecodedImage& PbrtDecodedImage::operator=(PbrtDecodedImage&& other) noexcept {
    if (this != &other) {
        releaseDecodedPixels(*this);
        pixels = other.pixels;
        byteSize = other.byteSize;
        width = other.width;
        height = other.height;
        sRGB = other.sRGB;
        valid = other.valid;
        other.pixels = nullptr;
        other.byteSize = 0;
        other.valid = false;
    }
    return *this;
}

PbrtDecodedImage::~PbrtDecodedImage() {
    releaseDecodedPixels(*this);
}

static double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

#ifdef _WIN32
class MappedFile {
public:
    bool open(const std::filesystem::path& path) {
        mFile = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mFile == INVALID_HANDLE_VALUE) {
            return false;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(mFile, &fileSize) || fileSize.QuadPart <= 0) {
            close();
            return false;
        }

        mSize = static_cast<size_t>(fileSize.QuadPart);
        mMapping = CreateFileMappingW(mFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mMapping) {
            close();
            return false;
        }

        mView = MapViewOfFile(mMapping, FILE_MAP_READ, 0, 0, 0);
        if (!mView) {
            close();
            return false;
        }
        return true;
    }

    const uint8_t* data() const {
        return static_cast<const uint8_t*>(mView);
    }

    size_t size() const {
        return mSize;
    }

    ~MappedFile() {
        close();
    }

private:
    void close() {
        if (mView) {
            UnmapViewOfFile(mView);
            mView = nullptr;
        }
        if (mMapping) {
            CloseHandle(mMapping);
            mMapping = nullptr;
        }
        if (mFile != INVALID_HANDLE_VALUE) {
            CloseHandle(mFile);
            mFile = INVALID_HANDLE_VALUE;
        }
        mSize = 0;
    }

    HANDLE mFile = INVALID_HANDLE_VALUE;
    HANDLE mMapping = nullptr;
    void* mView = nullptr;
    size_t mSize = 0;
};
#endif

struct TextureJob {
    std::string key;
    std::filesystem::path path;
    bool sRGB = true;
};

static PbrtDecodedImage decodeImageFile(const std::filesystem::path& path, bool sRGB) {
    PbrtDecodedImage out;
    out.sRGB = sRGB;
    if (!std::filesystem::exists(path)) {
        return out;
    }

    const std::filesystem::path absPath = std::filesystem::absolute(path);
    int w = 0, h = 0, n = 0;
    constexpr int kChannels = 4;
    uint8_t* data = nullptr;

#ifdef _WIN32
    MappedFile mapped;
    if (mapped.open(absPath)) {
        const int fileSize = static_cast<int>(mapped.size());
        data = stbi_load_from_memory(mapped.data(), fileSize, &w, &h, &n, kChannels);
    }
#endif
    if (!data) {
        const std::string absPathString = absPath.string();
        data = stbi_load(absPathString.c_str(), &w, &h, &n, kChannels);
    }
    if (!data) {
        std::cerr << "Failed to decode texture: " << path << std::endl;
        return out;
    }

    out.pixels = data;
    out.width = w;
    out.height = h;
    out.byteSize = static_cast<size_t>(w) * h * kChannels;
    out.valid = true;
    return out;
}

static std::vector<TextureJob> collectTextureJobs(const PbrtLoadedScene& scene,
        bool loadEnvironmentTexture) {
    std::vector<TextureJob> jobs;
    std::set<std::string> seen;
    for (const auto& mesh : scene.meshes) {
        if (mesh.baseColorTexturePath.empty()) {
            continue;
        }
        const std::string key = mesh.baseColorTexturePath.string();
        if (seen.insert(key).second) {
            jobs.push_back({ key, mesh.baseColorTexturePath, mesh.baseColorTextureSRGB });
        }
    }
    if (loadEnvironmentTexture && scene.environment.valid && !scene.environment.mapPath.empty()) {
        const std::string key = scene.environment.mapPath.string();
        if (seen.insert(key).second) {
            jobs.push_back({ key, scene.environment.mapPath, false });
        }
    }
    return jobs;
}

bool loadPbrtSceneResources(const std::filesystem::path& pbrtPath,
        PbrtSceneResources& out, bool loadEnvironmentTexture) {
    const auto totalStart = Clock::now();
    out.textures = {};
    out.timings = {};

    const auto parseStart = Clock::now();
    if (!loadPbrtScene(pbrtPath, out.scene)) {
        return false;
    }
    out.timings.parseMs = elapsedMs(parseStart);

    const auto collectStart = Clock::now();
    const std::vector<TextureJob> textureJobs = collectTextureJobs(out.scene,
            loadEnvironmentTexture);
    out.timings.collectMs = elapsedMs(collectStart);

    std::vector<PbrtDecodedImage> decodedImages(textureJobs.size());
    std::vector<bool> meshExists(out.scene.meshes.size(), true);

    tf::Taskflow taskflow;
    taskflow.emplace([&](tf::Subflow& subflow) {
        const auto decodeStart = Clock::now();
        for (size_t i = 0; i < textureJobs.size(); ++i) {
            subflow.emplace([&, i] {
                decodedImages[i] = decodeImageFile(textureJobs[i].path, textureJobs[i].sRGB);
            });
        }
        subflow.join();
        out.timings.decodeTexturesMs = elapsedMs(decodeStart);
    }).name("decode_textures");

    taskflow.emplace([&](tf::Subflow& subflow) {
        const auto verifyStart = Clock::now();
        for (size_t i = 0; i < out.scene.meshes.size(); ++i) {
            subflow.emplace([&, i] {
                meshExists[i] = std::filesystem::exists(out.scene.meshes[i].plyPath);
            });
        }
        subflow.join();
        out.timings.verifyMeshesMs = elapsedMs(verifyStart);
    }).name("verify_meshes");

    tf::Executor executor;
    executor.run(taskflow).wait();

    for (size_t i = 0; i < textureJobs.size(); ++i) {
        out.textures.decoded.emplace(textureJobs[i].key, std::move(decodedImages[i]));
    }
    for (size_t i = 0; i < out.scene.meshes.size(); ++i) {
        out.scene.meshes[i].plyFileExists = meshExists[i];
    }

    out.timings.totalMs = elapsedMs(totalStart);
    return true;
}

void printPbrtLoadTimings(const PbrtLoadTimings& timings) {
    std::cout << "PBRT load timings (ms):"
              << " parse=" << timings.parseMs
              << " collect=" << timings.collectMs
              << " decode_textures=" << timings.decodeTexturesMs
              << " verify_meshes=" << timings.verifyMeshesMs
              << " upload_textures=" << timings.uploadTexturesMs
              << " total=" << timings.totalMs
              << std::endl;
}

} // namespace pbrtio
