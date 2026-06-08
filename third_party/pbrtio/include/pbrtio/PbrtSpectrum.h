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
        static const PiecewiseLinearSpectrum kCuZnEta = PiecewiseLinearSpectrum::fromInterleaved(
            {
                290.f, 1.358f, 300.f, 1.388f, 310.f, 1.419f, 320.f, 1.446f, 330.f, 1.473f, 340.f, 1.494f,
                350.f, 1.504f, 360.f, 1.503f, 370.f, 1.497f, 380.f, 1.487f, 390.f, 1.471f, 400.f, 1.445f,
                410.f, 1.405f, 420.f, 1.350f, 430.f, 1.278f, 440.f, 1.191f, 450.f, 1.094f, 460.f, 0.994f,
                470.f, 0.900f, 480.f, 0.816f, 490.f, 0.745f, 500.f, 0.686f, 510.f, 0.639f, 520.f, 0.602f,
                530.f, 0.573f, 540.f, 0.549f, 550.f, 0.527f, 560.f, 0.505f, 570.f, 0.484f, 580.f, 0.468f,
                590.f, 0.460f, 600.f, 0.450f, 610.f, 0.452f, 620.f, 0.449f, 630.f, 0.445f, 640.f, 0.444f,
                650.f, 0.444f, 660.f, 0.445f, 670.f, 0.444f, 680.f, 0.444f, 690.f, 0.445f, 700.f, 0.446f,
                710.f, 0.448f, 720.f, 0.450f, 730.f, 0.452f, 740.f, 0.455f, 750.f, 0.457f, 760.f, 0.458f,
                770.f, 0.460f, 780.f, 0.464f, 790.f, 0.469f, 800.f, 0.473f, 810.f, 0.478f, 820.f, 0.481f,
                830.f, 0.483f, 840.f, 0.486f, 850.f, 0.490f, 860.f, 0.494f, 870.f, 0.500f, 880.f, 0.507f,
                890.f, 0.515f,
            },
            false);
        static const PiecewiseLinearSpectrum kCuZnK = PiecewiseLinearSpectrum::fromInterleaved(
            {
                290.f, 1.688f, 300.f, 1.731f, 310.f, 1.764f, 320.f, 1.789f, 330.f, 1.807f, 340.f, 1.815f,
                350.f, 1.815f, 360.f, 1.815f, 370.f, 1.818f, 380.f, 1.818f, 390.f, 1.813f, 400.f, 1.805f,
                410.f, 1.794f, 420.f, 1.786f, 430.f, 1.784f, 440.f, 1.797f, 450.f, 1.829f, 460.f, 1.883f,
                470.f, 1.957f, 480.f, 2.046f, 490.f, 2.145f, 500.f, 2.250f, 510.f, 2.358f, 520.f, 2.464f,
                530.f, 2.568f, 540.f, 2.668f, 550.f, 2.765f, 560.f, 2.860f, 570.f, 2.958f, 580.f, 3.059f,
                590.f, 3.159f, 600.f, 3.253f, 610.f, 3.345f, 620.f, 3.434f, 630.f, 3.522f, 640.f, 3.609f,
                650.f, 3.695f, 660.f, 3.778f, 670.f, 3.860f, 680.f, 3.943f, 690.f, 4.025f, 700.f, 4.106f,
                710.f, 4.186f, 720.f, 4.266f, 730.f, 4.346f, 740.f, 4.424f, 750.f, 4.501f, 760.f, 4.579f,
                770.f, 4.657f, 780.f, 4.737f, 790.f, 4.814f, 800.f, 4.890f, 810.f, 4.965f, 820.f, 5.039f,
                830.f, 5.115f, 840.f, 5.192f, 850.f, 5.269f, 860.f, 5.346f, 870.f, 5.423f, 880.f, 5.500f,
                890.f, 5.575f,
            },
            false);
        if (name == "metal-Ag-eta") return &kAgEta;
        if (name == "metal-Ag-k") return &kAgK;
        if (name == "metal-Al-eta") return &kAlEta;
        if (name == "metal-Al-k") return &kAlK;
        if (name == "metal-CuZn-eta") return &kCuZnEta;
        if (name == "metal-CuZn-k") return &kCuZnK;
        return nullptr;
    }
};

} // namespace pbrtio::pbrt
