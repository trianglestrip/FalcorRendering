/*
 * Compatibility layer for the PBRT parser.
 * SPDX-License-Identifier: Apache-2.0 (parser derived from pbrt-v4)
 */

#pragma once

#include <pbrtio/PbrtMathTypes.h>

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <span>
#include <optional>
#include <variant>
#include <vector>

namespace pbrtio::pbrt {

using Float = float;

inline float4x4 identity4x4() { return float4x4(1.0f); }

#define PBRTIO_ASSERT(x) assert(x)
#define PBRTIO_UNREACHABLE() assert(false)
#define PBRTIO_UNIMPLEMENTED() assert(false)
#define PBRTIO_CHECK(cond, ...) assert(cond)

[[noreturn]] inline void pbrtioThrowMsg(const std::string& msg) {
    throw std::runtime_error(msg);
}

template<typename... Args>
[[noreturn]] inline void pbrtioThrow(const char* fmt, Args&&...) {
    pbrtioThrowMsg(fmt);
}

inline bool hasExtension(const std::filesystem::path& path, const char* ext) {
    const auto e = path.extension().string();
    const std::string dotExt = std::string(".") + ext;
    return e == ext || e == dotExt;
}

inline std::string decompressFile(const std::filesystem::path& path) {
    (void)path;
    throw std::runtime_error("gzip decompression is not supported in pbrtio");
}

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to read file: " + path.string());
    }
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

template<typename... Args>
inline void logInfo(std::format_string<Args...> fmt, Args&&... args) {
    std::fprintf(stderr, "[pbrtio] %s\n",
            std::format(fmt, std::forward<Args>(args)...).c_str());
}

} // namespace pbrtio::pbrt
