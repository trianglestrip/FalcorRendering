/*
 * Minimal spectrum types for PBRT parameter parsing.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <pbrtio/PbrtCompat.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pbrtio::pbrt {

struct PiecewiseLinearSpectrum {
    std::vector<Float> wavelengths;
    std::vector<Float> values;

    PiecewiseLinearSpectrum() = default;
    PiecewiseLinearSpectrum(std::vector<Float> w, std::vector<Float> v)
        : wavelengths(std::move(w)), values(std::move(v)) {}

    static PiecewiseLinearSpectrum fromInterleaved(std::initializer_list<Float> interleaved, bool) {
        const size_t count = interleaved.size() / 2;
        std::vector<Float> w(count), v(count);
        size_t i = 0;
        for (Float f : interleaved) {
            if (i % 2 == 0) {
                w[i / 2] = f;
            } else {
                v[i / 2] = f;
            }
            ++i;
        }
        return { std::move(w), std::move(v) };
    }

    static std::optional<PiecewiseLinearSpectrum> fromFile(const std::filesystem::path&) {
        return std::nullopt;
    }
};

struct BlackbodySpectrum {
    Float temperature = 6500.f;
    explicit BlackbodySpectrum(Float t) : temperature(t) {}
};

using Spectrum = std::variant<float3, PiecewiseLinearSpectrum, BlackbodySpectrum>;

inline float3 spectrumToRGB(const Spectrum& spectrum) {
    if (auto p = std::get_if<float3>(&spectrum)) {
        return *p;
    }
    if (auto p = std::get_if<BlackbodySpectrum>(&spectrum)) {
        const Float t = p->temperature;
        return float3(1.f, 0.95f - t * 1e-5f, 0.9f - t * 2e-5f);
    }
    return float3(0.8f);
}

class Spectra {
public:
    static const PiecewiseLinearSpectrum* getNamedSpectrum(const std::string& name) {
        static const PiecewiseLinearSpectrum kAgEta = PiecewiseLinearSpectrum::fromInterleaved(
            { 400.f, 0.05f, 700.f, 0.15f }, false);
        static const PiecewiseLinearSpectrum kAgK = PiecewiseLinearSpectrum::fromInterleaved(
            { 400.f, 1.5f, 700.f, 3.5f }, false);
        if (name == "metal-Ag-eta") return &kAgEta;
        if (name == "metal-Ag-k") return &kAgK;
        return nullptr;
    }
};

} // namespace pbrtio::pbrt
