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
        static const PiecewiseLinearSpectrum kAlEta = PiecewiseLinearSpectrum::fromInterleaved(
            {
                298.757050f, 0.273375f, 302.400421f, 0.280000f, 306.133759f, 0.286813f, 309.960449f, 0.294000f,
                313.884003f, 0.301875f, 317.908142f, 0.310000f, 322.036835f, 0.317875f, 326.274139f, 0.326000f,
                330.624481f, 0.334750f, 335.092377f, 0.344000f, 339.682678f, 0.353813f, 344.400482f, 0.364000f,
                349.251221f, 0.374375f, 354.240509f, 0.385000f, 359.374420f, 0.395750f, 364.659332f, 0.407000f,
                370.102020f, 0.419125f, 375.709625f, 0.432000f, 381.489777f, 0.445688f, 387.450562f, 0.460000f,
                393.600555f, 0.474688f, 399.948975f, 0.490000f, 406.505493f, 0.506188f, 413.280579f, 0.523000f,
                420.285339f, 0.540063f, 427.531647f, 0.558000f, 435.032196f, 0.577313f, 442.800629f, 0.598000f,
                450.851562f, 0.620313f, 459.200653f, 0.644000f, 467.864838f, 0.668625f, 476.862213f, 0.695000f,
                486.212463f, 0.723750f, 495.936707f, 0.755000f, 506.057861f, 0.789000f, 516.600769f, 0.826000f,
                527.592224f, 0.867000f, 539.061646f, 0.912000f, 551.040771f, 0.963000f, 563.564453f, 1.020000f,
                576.670593f, 1.080000f, 590.400818f, 1.150000f, 604.800842f, 1.220000f, 619.920898f, 1.300000f,
                635.816284f, 1.390000f, 652.548279f, 1.490000f, 670.184753f, 1.600000f, 688.800964f, 1.740000f,
                708.481018f, 1.910000f, 729.318665f, 2.140000f, 751.419250f, 2.410000f, 774.901123f, 2.630000f,
                799.897949f, 2.800000f, 826.561157f, 2.740000f, 855.063293f, 2.580000f, 885.601257f, 2.240000f,
            },
            false);
        static const PiecewiseLinearSpectrum kAlK = PiecewiseLinearSpectrum::fromInterleaved(
            {
                298.757050f, 3.593750f, 302.400421f, 3.640000f, 306.133759f, 3.689375f, 309.960449f, 3.740000f,
                313.884003f, 3.789375f, 317.908142f, 3.840000f, 322.036835f, 3.894375f, 326.274139f, 3.950000f,
                330.624481f, 4.005000f, 335.092377f, 4.060000f, 339.682678f, 4.113750f, 344.400482f, 4.170000f,
                349.251221f, 4.233750f, 354.240509f, 4.300000f, 359.374420f, 4.365000f, 364.659332f, 4.430000f,
                370.102020f, 4.493750f, 375.709625f, 4.560000f, 381.489777f, 4.633750f, 387.450562f, 4.710000f,
                393.600555f, 4.784375f, 399.948975f, 4.860000f, 406.505493f, 4.938125f, 413.280579f, 5.020000f,
                420.285339f, 5.108750f, 427.531647f, 5.200000f, 435.032196f, 5.290000f, 442.800629f, 5.380000f,
                450.851562f, 5.480000f, 459.200653f, 5.580000f, 467.864838f, 5.690000f, 476.862213f, 5.800000f,
                486.212463f, 5.915000f, 495.936707f, 6.030000f, 506.057861f, 6.150000f, 516.600769f, 6.280000f,
                527.592224f, 6.420000f, 539.061646f, 6.550000f, 551.040771f, 6.700000f, 563.564453f, 6.850000f,
                576.670593f, 7.000000f, 590.400818f, 7.150000f, 604.800842f, 7.310000f, 619.920898f, 7.480000f,
                635.816284f, 7.650000f, 652.548279f, 7.820000f, 670.184753f, 8.010000f, 688.800964f, 8.210000f,
                708.481018f, 8.390000f, 729.318665f, 8.570000f, 751.419250f, 8.620000f, 774.901123f, 8.600000f,
                799.897949f, 8.450000f, 826.561157f, 8.310000f, 855.063293f, 8.210000f, 885.601257f, 8.210000f,
            },
            false);
        if (name == "metal-Ag-eta") return &kAgEta;
        if (name == "metal-Ag-k") return &kAgK;
        if (name == "metal-Al-eta") return &kAlEta;
        if (name == "metal-Al-k") return &kAlK;
        return nullptr;
    }
};

} // namespace pbrtio::pbrt
