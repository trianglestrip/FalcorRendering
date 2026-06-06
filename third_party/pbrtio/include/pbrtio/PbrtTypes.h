/*
 * PBRT v4 parser data types.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <pbrtio/PbrtCompat.h>
#include <pbrtio/PbrtSpectrum.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <variant>

namespace pbrtio::pbrt {

struct FileLoc {
    FileLoc() = default;
    explicit FileLoc(std::string_view filename) : filename(filename) {}
    std::string toString() const {
        return std::string(filename) + ":" + std::to_string(line) + ":" + std::to_string(column);
    }

    std::string_view filename;
    uint32_t line = 1;
    uint32_t column = 0;
};

struct RGBColorSpace {};

using Resolver = std::function<std::filesystem::path(const std::filesystem::path& path)>;

enum class SpectrumType {
    Illuminant,
    Albedo,
    Unbounded
};

} // namespace pbrtio::pbrt
