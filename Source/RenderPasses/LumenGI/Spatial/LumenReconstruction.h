/***************************************************************************
 # Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
 ***************************************************************************/
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

/** S5-A2: GI reconstruction host component (Agent Z8, S5-A2 module). Header-only
 *  PURE CPU: full / half / quarter GI resolution management, upscale parameter
 *  defaults and weight formulas, resource size computation and the half/quarter
 *  -> full coordinate mapping. It does NOT depend on a Device, a texture or a
 *  RenderContext (no Falcor.h), so it can be compiled and unit-tested directly.
 *
 *  It mirrors the S5-B2 GPU contract documented in LumenSpatialFilterData.slang
 *  (the constant-buffer mirror + static_assert live at the bottom) and the
 *  S4-B3 interpolate weight convention. The S5-A1 host uses this component to
 *  (a) pick the GI resolution from a quality preset, (b) size the GI / temporal
 *  buffers, (c) feed the spatial filter CB, and (d) drive the future half/quarter
 *  -> full bilateral upscale pass with the same depth / normal / material
 *  weights as the spatial filter.
 *
 *  ============================================================================
 *  RESOLUTION MODEL (frozen)
 *  ============================================================================
 *    scaleFor(Full)   = 1   GI renders at full frame res, no upscale.
 *    scaleFor(Half)   = 2   GI renders at ceil(frame / 2), upscaled 2x.
 *    scaleFor(Quarter)= 4   GI renders at ceil(frame / 4), upscaled 4x.
 *  GI dims are CEIL-rounded so the low-res grid always covers the whole frame:
 *        giW = ceil(frameW / scale), giH = ceil(frameH / scale).
 *  The last GI texel of each axis may therefore cover fewer than `scale` full-
 *  res pixels (e.g. a 1921-wide frame at half-res yields giW = 961, and texel
 *  960 covers pixels [1920, 1920] only).
 *
 *  ============================================================================
 *  UPSCALE COORDINATE MAPPING (half/quarter -> full, frozen)
 *  ============================================================================
 *  GI texel i spans the full-res pixel range [i*scale, (i+1)*scale - 1]. A
 *  full-res pixel center (px + 0.5, py + 0.5) maps to a low-res sampling
 *  coordinate (in low-res TEXEL units, texel i occupying [i, i+1)):
 *        src = (p + 0.5) / scale
 *  so px = 0,1 -> src in [0.25, 0.75] (inside texel 0's extent) and px = scale,
 *  ... -> texel 1's extent. The shader / host then performs a manual 4-tap
 *  bilinear gather around `src` (base = floor(src - 0.5), frac = src - base -
 *  0.5) exactly like the S5-B1 temporal gather, but with each tap additionally
 *  weighted by the bilateral upscale weights below; taps outside [0, giW-1] are
 *  clamped (edge duplication) exactly like the S4-B3 probe interpolation.
 *
 *  ============================================================================
 *  BILATERAL UPSCALE WEIGHTS (frozen; identical form to the S5-B2 spatial
 *  filter and the S4-B3 interpolate weight composition)
 *  ============================================================================
 *    perTap = bilinearW(dx,dy,frac)
 *           * depthWeight(pixelZ, tapZ, p)     // dead-zone + exp falloff
 *           * normalWeight(pixelN, tapN, p)    // pow(saturate(dot))
 *           * materialWeight(pixelM, tapM, p)  // 1 equal / unknown, w else
 *    output = sum(perTap * GI_tap) / sum(perTap)
 *  An optional / unsupported channel is disabled by passing the identity
 *  (tapNormal == pixelNormal, tapMaterialID == kMaterialIDNone), which makes
 *  the corresponding factor 1.
 *
 *  Thread safety: stateless; all methods are const/static and safe to call
 *  from any thread.
 */
namespace Falcor
{

/** GI resolution quality (S5-A2; task §10 S5-A2). `Full` = full-res GI, `Half`
 *  = half-res GI upscaled 2x, `Quarter` = quarter-res GI upscaled 4x. */
enum class LumenGIResolutionQuality : uint32_t
{
    Quarter = 0, ///< 4x upscale; lowest cost, coarsest GI.
    Half = 1,    ///< 2x upscale; balanced (default for the Medium preset).
    Full = 2,    ///< No upscale; highest quality.
    Count,
};

///< User-facing quality preset (S8-C2). Maps to a GI resolution; the mapping is
///< the frozen default and S8 may retune it through a serialized configuration.
enum class LumenGIQualityPreset : uint32_t
{
    Low = 0,      ///< Quarter-res GI.
    Medium = 1,   ///< Half-res GI.
    High = 2,     ///< Full-res GI.
    Reference = 3, ///< Full-res GI (highest settings).
};

/** Pure-CPU math helper for the S5-A2 reconstruction component. */
class LumenReconstruction
{
public:
    ///< Upper bound on the upscale factor; keeps every size computation inside
    ///< uint32_t while covering full / half / quarter.
    static constexpr uint32_t kMaxScale = 4u;

    ///< GI resolution -> per-axis scale factor (1, 2 or 4).
    static uint32_t scaleFor(LumenGIResolutionQuality quality)
    {
        switch (quality)
        {
        case LumenGIResolutionQuality::Quarter: return 4u;
        case LumenGIResolutionQuality::Half: return 2u;
        case LumenGIResolutionQuality::Full:
        default: return 1u;
        }
    }

    ///< User preset -> GI resolution (frozen default mapping, S8 may retune).
    static LumenGIResolutionQuality qualityForPreset(LumenGIQualityPreset preset)
    {
        switch (preset)
        {
        case LumenGIQualityPreset::Low: return LumenGIResolutionQuality::Quarter;
        case LumenGIQualityPreset::Medium: return LumenGIResolutionQuality::Half;
        case LumenGIQualityPreset::High:
        case LumenGIQualityPreset::Reference:
        default: return LumenGIResolutionQuality::Full;
        }
    }

    ///< Frozen size contract for one frame at a GI quality.
    struct Dimensions
    {
        uint32_t frameW = 0;  ///< Full-res frame width (upscale output).
        uint32_t frameH = 0;  ///< Full-res frame height (upscale output).
        uint32_t giW = 0;     ///< GI render width (trace / temporal / spatial input).
        uint32_t giH = 0;     ///< GI render height.
        uint32_t scale = 1u;  ///< Per-axis scale (1, 2 or 4).
        LumenGIResolutionQuality quality = LumenGIResolutionQuality::Full;

        bool isValid() const
        {
            return frameW >= 1u && frameH >= 1u && giW >= 1u && giH >= 1u &&
                   giW <= frameW && giH <= frameH &&
                   scale >= 1u && scale <= LumenReconstruction::kMaxScale;
        }
    };

    ///< Build the frozen size contract: giW/H = ceil(frame / scale) so the GI
    ///< grid always covers the full frame. Invalid (isValid() == false) when
    ///< frameW/frameH == 0.
    static Dimensions makeDimensions(uint32_t frameW, uint32_t frameH, LumenGIResolutionQuality quality)
    {
        Dimensions d;
        if (frameW >= 1u && frameH >= 1u)
        {
            const uint32_t scale = scaleFor(quality);
            d.frameW = frameW;
            d.frameH = frameH;
            d.giW = (frameW + scale - 1u) / scale;
            d.giH = (frameH + scale - 1u) / scale;
            d.scale = scale;
            d.quality = quality;
        }
        return d;
    }

    ///< Total GI texel count (resource size for the GI buffer).
    static size_t giTexelCount(const Dimensions& d)
    {
        return static_cast<size_t>(d.giW) * d.giH;
    }

    ///< Byte size of the GI buffer for the frozen RGBA16F (8 bytes/texel)
    ///< temporal / spatial format.
    static size_t giByteSize16F(const Dimensions& d)
    {
        return giTexelCount(d) * 8u;
    }

    ///< Full-res texel count (upscale output size).
    static size_t frameTexelCount(const Dimensions& d)
    {
        return static_cast<size_t>(d.frameW) * d.frameH;
    }

    // ------------------------------------------------------------------------
    // Coordinate mapping (frozen; see the header contract).
    // ------------------------------------------------------------------------

    ///< Low-res sampling coordinate (in low-res texel units, texel i spans
    ///< [i, i+1)) for the full-res pixel center (px + 0.5, py + 0.5):
    ///< src = (p + 0.5) / scale.
    struct Float2
    {
        float x = 0.f;
        float y = 0.f;
    };

    ///< Same as Float2 but signed-integer (bilinear tap base may be -1 at the
    ///< frame edge; the caller clamps to [0, giDim - 1] before reading).
    struct Int2
    {
        int32_t x = 0;
        int32_t y = 0;
    };

    ///< Unsigned integer pair (clamped GI texel).
    struct UInt2
    {
        uint32_t x = 0;
        uint32_t y = 0;
    };

    ///< 3-vector (world normal) for the upscale weights.
    struct Float3
    {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
    };

    static Float2 sourceTexelCenter(uint32_t px, uint32_t py, const Dimensions& d)
    {
        Float2 c;
        c.x = (float(px) + 0.5f) / float(d.scale);
        c.y = (float(py) + 0.5f) / float(d.scale);
        return c;
    }

    ///< Normalized UV in the low-res texture for a full-res pixel.
    static Float2 sourceUV(uint32_t px, uint32_t py, const Dimensions& d)
    {
        const Float2 c = sourceTexelCenter(px, py, d);
        Float2 uv;
        uv.x = c.x / float(d.giW);
        uv.y = c.y / float(d.giH);
        return uv;
    }

    ///< Nearest low-res texel for a full-res pixel (nearest-neighbor mapping /
    ///< the center tap of the bilinear quad): floor(src) — src = (p + 0.5) /
    ///< scale is always inside [i, i+1) for the pixels covered by texel i, so
    ///< floor(src) selects exactly that texel. May be -1 at the frame edge;
    ///< clamp to [0, giDim - 1] before reading.
    static Int2 nearestTexel(uint32_t px, uint32_t py, const Dimensions& d)
    {
        const Float2 c = sourceTexelCenter(px, py, d);
        Int2 t;
        t.x = int32_t(std::floor(c.x));
        t.y = int32_t(std::floor(c.y));
        return t;
    }

    ///< Bilinear quad for a full-res pixel: base = lower-left tap of the 2x2
    ///< block containing `src`, frac = fractional position within that block
    ///< (in [0,1)^2). The four taps are base + (0,0), (1,0), (0,1), (1,1);
    ///< their bilinear weights come from bilinearWeight(dx, dy, frac). Taps
    ///< with base.x or base.y == -1 (or == giDim) are out of range and must be
    ///< clamped to the GI bounds before reading (edge duplication).
    static void bilinearQuad(uint32_t px, uint32_t py, const Dimensions& d, Int2& base, Float2& frac)
    {
        const Float2 c = sourceTexelCenter(px, py, d);
        base.x = int32_t(std::floor(c.x - 0.5f));
        base.y = int32_t(std::floor(c.y - 0.5f));
        frac.x = c.x - (float(base.x) + 0.5f);
        frac.y = c.y - (float(base.y) + 0.5f);
    }

    ///< Bilinear tap weight for tap (dx, dy) in {0,1}x{0,1} and frac (fx, fy).
    static float bilinearWeight(uint32_t dx, uint32_t dy, const Float2& frac)
    {
        const float wx = (dx == 0u) ? (1.f - frac.x) : frac.x;
        const float wy = (dy == 0u) ? (1.f - frac.y) : frac.y;
        return wx * wy;
    }

    ///< Full-res pixel range covered by GI texel (i, j): [x0, x1] x [y0, y1]
    ///< inclusive, clamped to the frame bounds (the last texel of an odd-sized
    ///< frame covers fewer pixels).
    static void coveredPixelRange(const UInt2& texel, const Dimensions& d, uint32_t& x0, uint32_t& x1, uint32_t& y0, uint32_t& y1)
    {
        x0 = texel.x * d.scale;
        y0 = texel.y * d.scale;
        x1 = std::min(x0 + d.scale - 1u, d.frameW - 1u);
        y1 = std::min(y0 + d.scale - 1u, d.frameH - 1u);
    }

    // ------------------------------------------------------------------------
    // Bilateral upscale weights (frozen; identical form to the S5-B2 spatial
    // filter and the S4-B3 interpolate weights).
    // ------------------------------------------------------------------------

    ///< Frozen upscale weight parameters. The S5-A1 host fills the spatial
    ///< filter CB's depth / normal / material fields from these, and the future
    ///< upscale pass re-uses the same values so filtering and upscaling are
    ///< consistent.
    struct UpscaleParams
    {
        float depthThreshold = 0.02f;          ///< Meters; |zP - zT| below keeps depthW = 1.
        float depthSigmaInv = 4.0f;            ///< 1/meters; depthW exponential falloff rate.
        float normalExponent = 8.0f;           ///< pow(saturate(dot(nP, nT)), exponent).
        float materialMismatchWeight = 0.05f;  ///< Weight of a material-ID mismatch.
        uint32_t materialIDNone = 0xFFFFFFFFu; ///< Sentinel for "unknown" (matches the shader).
    };

    ///< Frozen defaults per GI quality. Half / quarter use a slightly wider
    ///< depth dead zone because the GI texels cover 2x / 4x as many full-res
    ///< pixels, so the same physical surface spans a larger low-res depth step.
    static UpscaleParams defaultUpscaleParams(LumenGIResolutionQuality quality)
    {
        UpscaleParams p;
        const uint32_t scale = scaleFor(quality);
        p.depthThreshold = 0.02f * float(scale);
        p.depthSigmaInv = 4.0f;
        p.normalExponent = 8.0f;
        p.materialMismatchWeight = 0.05f;
        return p;
    }

    static UpscaleParams makeUpscaleParams(float depthThreshold, float depthSigmaInv, float normalExponent, float materialMismatchWeight)
    {
        UpscaleParams p;
        p.depthThreshold = depthThreshold;
        p.depthSigmaInv = depthSigmaInv;
        p.normalExponent = normalExponent;
        p.materialMismatchWeight = materialMismatchWeight;
        return p;
    }

    ///< Depth affinity factor: 1 within `threshold` meters, exponential
    ///< falloff beyond (same dead-zone + exp form as the shader and the S4-B3
    ///< interpolate).
    static float depthWeight(float pixelZ, float tapZ, float threshold, float sigmaInv)
    {
        const float gap = std::fabs(pixelZ - tapZ);
        if (gap <= threshold)
            return 1.f;
        return std::exp(-(gap - threshold) * std::max(sigmaInv, 0.f));
    }

    ///< Normal affinity factor: pow(saturate(dot(nP, nT)), exponent).
    static float normalWeight(const Float3& pixelNormal, const Float3& tapNormal, float exponent)
    {
        const float ndot = pixelNormal.x * tapNormal.x + pixelNormal.y * tapNormal.y + pixelNormal.z * tapNormal.z;
        const float s = ndot < 0.f ? 0.f : (ndot > 1.f ? 1.f : ndot);
        return std::pow(s, std::max(exponent, 0.f));
    }

    ///< Material affinity factor: 1 for equal IDs or an unknown ID on either
    ///< side, `mismatchWeight` else (a small residual, never 0).
    static float materialWeight(uint32_t pixelMaterialID, uint32_t tapMaterialID, float mismatchWeight, uint32_t materialIDNone)
    {
        if (pixelMaterialID == materialIDNone || tapMaterialID == materialIDNone)
            return 1.f;
        return (pixelMaterialID == tapMaterialID) ? 1.f : mismatchWeight;
    }

    ///< Combined per-tap upscale weight: bilinearW * depthW * normalW *
    ///< materialW (the factor each of the 4 bilinear taps contributes before
    ///< the renormalized blend `output = sum(perTap * GI_tap) / sum(perTap)`).
    ///< An optional channel is disabled by passing the identity
    ///< (tapNormal == pixelNormal, tapMaterialID == materialIDNone), which
    ///< makes its factor 1.
    static float upscaleTapWeight(
        float bilinearW,
        float pixelZ, float tapZ,
        const Float3& pixelNormal, const Float3& tapNormal,
        uint32_t pixelMaterialID, uint32_t tapMaterialID,
        const UpscaleParams& p
    )
    {
        float w = bilinearW;
        w *= depthWeight(pixelZ, tapZ, p.depthThreshold, p.depthSigmaInv);
        w *= normalWeight(pixelNormal, tapNormal, p.normalExponent);
        w *= materialWeight(pixelMaterialID, tapMaterialID, p.materialMismatchWeight, p.materialIDNone);
        return w;
    }

    ///< Expected dispatch group count for a pass over `dim` pixels with the
    ///< [numthreads(8,8,1)] convention used by the S4/S5 compute passes.
    static uint32_t groupCount(uint32_t dim)
    {
        return (dim + 8u - 1u) / 8u;
    }

    ///< CPU mirror of the frozen LumenSpatialFilterCB (LumenSpatialFilterData.slang).
    ///< Kept in sync field-for-field; a static_assert checks the total size. The
    ///< host fills it by name through the ShaderVar binding.
    struct SpatialFilterConstantBuffer
    {
        uint32_t frameDim[2] = { 0u, 0u };       // +0
        uint32_t filterEnabled = 1u;             // +8
        uint32_t fireflyClamp = 1u;              // +12
        float fireflyMaxRadiance = 10000.f;      // +16
        float fireflyStdDevFactor = 4.0f;        // +20
        float varianceThresholdLow = 0.01f;      // +24
        float varianceThresholdHigh = 0.25f;     // +28
        float radiusMin = 0.0f;                  // +32
        float radiusMax = 3.0f;                  // +36
        float spatialSigmaScale = 0.5f;          // +40
        float depthThreshold = 0.05f;            // +44
        float depthSigmaInv = 8.0f;              // +48
        float normalExponent = 8.0f;             // +52
        float materialMismatchWeight = 0.05f;    // +56
        float temporalVarianceWeight = 1.0f;     // +60
        float maxVarianceClamp = 1e4f;           // +64
        float varianceEpsilon = 1e-6f;           // +68
        float invFrameDim[2] = { 0.f, 0.f };     // +72
        uint32_t neighborhoodRadius = 1u;        // +80
        uint32_t pad[3] = { 0u, 0u, 0u };        // +84
    };  // 96 bytes.
    static_assert(sizeof(SpatialFilterConstantBuffer) == 96,
        "LumenReconstruction::SpatialFilterConstantBuffer is 96 bytes (matches LumenSpatialFilterCB)");

    ///< CPU helper that fills the CB mirror from a frame + quality (sizes,
    ///< inv-frame-dims) plus the frozen spatial-filter defaults. The host then
    ///< overrides the radius / threshold / variance fields from its config.
    static SpatialFilterConstantBuffer makeSpatialFilterCB(const Dimensions& d)
    {
        SpatialFilterConstantBuffer cb;
        cb.frameDim[0] = d.giW;
        cb.frameDim[1] = d.giH;
        cb.invFrameDim[0] = 1.f / float(d.giW);
        cb.invFrameDim[1] = 1.f / float(d.giH);
        return cb;
    }
};

} // namespace Falcor
