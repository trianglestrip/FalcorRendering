/*
 * PBRT helper utilities.
 */
#pragma once

#include <pbrtio/PbrtTypes.h>

#include <format>
#include <stdexcept>

namespace pbrtio::pbrt {

template<typename... Args>
[[noreturn]] inline void throwError(std::format_string<Args...> fmt, Args&&... args) {
    throw std::runtime_error(std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
[[noreturn]] inline void throwError(const FileLoc& loc, std::format_string<Args...> fmt, Args&&... args) {
    throw std::runtime_error(loc.toString() + ": " + std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
inline void logWarning(const FileLoc& loc, std::format_string<Args...> fmt, Args&&... args) {
    std::fprintf(stderr, "[pbrtio] warning %s: %s\n", loc.toString().c_str(),
            std::format(fmt, std::forward<Args>(args)...).c_str());
}

} // namespace pbrtio::pbrt
