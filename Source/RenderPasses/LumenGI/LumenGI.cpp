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
 # PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/

#include "LumenGI.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"
#include "Utils/Math/Float16.h" // float32ToFloat16 (fine-atlas upload).

#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace
{
// NOTE: no `using namespace LumenGI::MeshSDF...` here. The S6 headers declare a global
// `namespace LumenGI` that collides with the plugin class name in the enclosing scope; a
// using-directive inside the anonymous namespace would leak `Scene`/`Cache` into file scope
// and make the existing `Falcor::Scene` references ambiguous. The S6 helpers below therefore
// fully qualify the Mesh SDF types.
namespace s6ns = LumenGI::MeshSDF;       ///< Alias for the Mesh SDF host component namespace.
namespace s6scene = LumenGI::MeshSDF::Scene; ///< Alias for the Mesh SDF scene pipeline namespace.
const char kShaderFile[] = "RenderPasses/LumenGI/LumenGIDebug.cs.slang";
const char kTraceShaderFile[] = "RenderPasses/LumenGI/Tracing/LumenHardwareTrace.rt.slang";
const char kCaptureShaderFile[] = "RenderPasses/LumenGI/Capture/LumenCardCapture.3d.slang";
const char kCacheLightingShaderFile[] = "RenderPasses/LumenGI/Lighting/LumenSurfaceCacheLighting.cs.slang";
const char kHZBBuildShaderFile[] = "RenderPasses/LumenGI/ScreenTrace/LumenHZBBuild.cs.slang";
const char kScreenTraceShaderFile[] = "RenderPasses/LumenGI/ScreenTrace/LumenScreenTrace.cs.slang";
const char kScreenProbeShaderFile[] = "RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeTrace.cs.slang";
const char kScreenProbeIntegrateShaderFile[] = "RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeIntegrate.cs.slang";
const char kScreenProbeInterpolateShaderFile[] = "RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeInterpolate.cs.slang";
const char kTemporalFilterShaderFile[] = "RenderPasses/LumenGI/Temporal/LumenTemporalFilter.cs.slang";
const char kSpatialFilterShaderFile[] = "RenderPasses/LumenGI/Spatial/LumenSpatialFilter.cs.slang";

///< S5-B1 LumenTemporalFilterCB fields that are not exposed as tunable members; defaults frozen
///< with Z5's LumenTemporalFilterData.slang comments (all but the last three are inert while the
///< corresponding optional validation inputs are unbound).
constexpr float kTemporalClampBoxMargin = 0.0f;      ///< gClampBoxMargin (fraction of the neighborhood range).
constexpr float kTemporalNormalCosMin = 0.8f;        ///< gNormalCosMin (no normal input in the MVP).
constexpr float kTemporalNormalExponent = 8.0f;      ///< gNormalExponent (no normal input in the MVP).
constexpr float kTemporalMaterialMismatchWeight = 0.05f; ///< gMaterialMismatchWeight (no material input in the MVP).
constexpr float kTemporalHitDistanceThreshold = 0.5f; ///< gHitDistanceThreshold (m; no hit-distance input in the MVP).
///< gConfidenceWeight (confidence gating strength in wConf). DEFAULT 0 in the S5 MVP: the S4.3
///< interpolate confidence is currently dominated by the probe miss penalty (uniformly ~0.03 on
///< Cornell, see the S5 report) and a weight of 1.0 would make wConf ~0.03 for every pixel, which
///< pins alpha near gMaxRejectAlpha and disables the temporal accumulation entirely. With gating
///< off, history trust is driven by the working depth + motion validation; re-enable (1.0) once a
///< meaningful per-pixel confidence channel lands (S4-B3 fix / S5-B2).
constexpr float kTemporalConfidenceWeight = 0.0f;
///< gFireflyMaxRadiance; mirrors the kLumenGIMaxRadiance default in LumenGIData.slang / the
///< LUMEN_GI_MAX_RADIANCE fallback in LumenTemporalFilterData.slang.
constexpr float kTemporalFireflyMaxRadiance = 10000.f;

///< Host mirror of cbuffer LumenTemporalFilterCB in LumenTemporalFilterData.slang (frozen 96-byte
///< layout). The host sets every field by name each dispatch; this struct only documents and
///< static-asserts the GPU layout so a future root-side copy cannot drift from the shader contract.
struct LumenTemporalFilterCB
{
    uint2 gFrameDim;                // +0  Frame dims in pixels; guards the dispatch.
    uint32_t gFrameIndex;           // +8  Frame index (diagnostics / reserved).
    uint32_t gClampHistory;         // +12 != 0: AABB-clamp history RGB to the current 3x3 neighborhood.
    float gHistoryAlpha;            // +16 Base EMA weight toward the current frame.
    float gHistoryLengthCap;        // +20 Cap for the output history length.
    float gClampBoxMargin;          // +24 AABB clamp margin as a fraction of the neighborhood range.
    float gDepthThreshold;          // +28 Meters; depthW dead zone.
    float gDepthSigmaInv;           // +32 1/meters; depthW falloff rate.
    float gDepthRelativeThreshold;  // +36 Hard reject on relative depth jump.
    float gNormalCosMin;            // +40 Hard reject on normal flip.
    float gNormalExponent;          // +44 pow on the normal dot.
    float gMaterialMismatchWeight;  // +48 Material-ID mismatch weight.
    float gHitDistanceThreshold;    // +52 Meters; hard reject on hit-distance jump.
    float gConfidenceWeight;        // +56 Confidence gating strength in wConf.
    float gMaxRejectAlpha;          // +60 Blend alpha on disocclusion / soft reject.
    float gMotionVectorScale;       // +64 Multiplies the motion vector before reprojection.
    float gMotionLengthThreshold;   // +68 Hard reject when |mvec| exceeds this (normalized).
    float gFireflyMaxRadiance;      // +72 Firefly ceiling on the output RGB.
    float2 gInvFrameDim;            // +76 1 / frame dims.
    uint32_t gPad0;                 // +84
    uint32_t gPad1;                 // +88
    uint32_t gPad2;                 // +92
}; // 96 bytes.
static_assert(sizeof(LumenTemporalFilterCB) == 96, "LumenTemporalFilterCB mirror is 96 bytes (16B-aligned, no padding)");
static_assert(offsetof(LumenTemporalFilterCB, gFrameDim) == 0, "gFrameDim offset");
static_assert(offsetof(LumenTemporalFilterCB, gFrameIndex) == 8, "gFrameIndex offset");
static_assert(offsetof(LumenTemporalFilterCB, gClampHistory) == 12, "gClampHistory offset");
static_assert(offsetof(LumenTemporalFilterCB, gHistoryAlpha) == 16, "gHistoryAlpha offset");
static_assert(offsetof(LumenTemporalFilterCB, gHistoryLengthCap) == 20, "gHistoryLengthCap offset");
static_assert(offsetof(LumenTemporalFilterCB, gClampBoxMargin) == 24, "gClampBoxMargin offset");
static_assert(offsetof(LumenTemporalFilterCB, gDepthThreshold) == 28, "gDepthThreshold offset");
static_assert(offsetof(LumenTemporalFilterCB, gDepthSigmaInv) == 32, "gDepthSigmaInv offset");
static_assert(offsetof(LumenTemporalFilterCB, gDepthRelativeThreshold) == 36, "gDepthRelativeThreshold offset");
static_assert(offsetof(LumenTemporalFilterCB, gNormalCosMin) == 40, "gNormalCosMin offset");
static_assert(offsetof(LumenTemporalFilterCB, gNormalExponent) == 44, "gNormalExponent offset");
static_assert(offsetof(LumenTemporalFilterCB, gMaterialMismatchWeight) == 48, "gMaterialMismatchWeight offset");
static_assert(offsetof(LumenTemporalFilterCB, gHitDistanceThreshold) == 52, "gHitDistanceThreshold offset");
static_assert(offsetof(LumenTemporalFilterCB, gConfidenceWeight) == 56, "gConfidenceWeight offset");
static_assert(offsetof(LumenTemporalFilterCB, gMaxRejectAlpha) == 60, "gMaxRejectAlpha offset");
static_assert(offsetof(LumenTemporalFilterCB, gMotionVectorScale) == 64, "gMotionVectorScale offset");
static_assert(offsetof(LumenTemporalFilterCB, gMotionLengthThreshold) == 68, "gMotionLengthThreshold offset");
static_assert(offsetof(LumenTemporalFilterCB, gFireflyMaxRadiance) == 72, "gFireflyMaxRadiance offset");
static_assert(offsetof(LumenTemporalFilterCB, gInvFrameDim) == 76, "gInvFrameDim offset");

///< S4-A1 screen-trace direction input (frozen shader contract: gRayDirection is a view-space
///< direction texture, sampled per pixel and normalized; forward rays must have d.z < 0). The
///< S4 MVP uses the task-endorsed "fixed direction" option: one constant view-space direction
///< for every pixel, filled once per resize on the CPU. S4.2 probe direction sampling replaces
///< this with per-pixel directions (world normals / cosine hemisphere).
const float3 kScreenTraceRayDirection = float3(0.5f, 0.35f, -1.0f);

///< Frozen S4-B1 screen-trace algorithm defaults, mirrors of the kLumenScreenTrace*Default
///< constants in LumenScreenTraceData.slang (the shader lets the host override every one
///< through the CB; keep these in sync with the shader).
constexpr uint32_t kLumenScreenTraceMaxStepsHost = 64u;
constexpr float kLumenScreenTraceMinThicknessHost = 0.001f; ///< Meters.
constexpr float kLumenScreenTraceThicknessScaleHost = 2.0f;
constexpr float kLumenScreenTraceStepEpsilonHost = 1e-4f;   ///< Texel-space forward bias.
constexpr uint32_t kLumenScreenTraceMaxMipHost = 12u;

const uint32_t kTracePayloadSizeBytes = 24u;
const uint32_t kTraceRecursionDepth = 1u;

///< Fixed base seed for the cache-lighting RNG (task rule 5: every stochastic process must
///< support a fixed seed). The shader rotates it per frame with kLumenGICacheSeedFrameStride,
///< so adjacent frames sample different points while the sequence stays reproducible.
const uint32_t kCacheLightingSeed = 0x51B8DC0Du;

const char kEnabled[] = "enabled";
const char kTraceMode[] = "traceMode";
const char kQualityPreset[] = "qualityPreset";
const char kDebugMode[] = "debugMode";
const char kUseSurfaceCache[] = "useSurfaceCache";
const char kUseCacheLighting[] = "useCacheLighting";
const char kCacheLightingFeedback[] = "cacheLightingFeedback";
const char kCacheLightingFeedbackStrength[] = "cacheLightingFeedbackStrength";
const char kCacheLightingFeedbackMaxBounces[] = "cacheLightingFeedbackMaxBounces";
const char kUseScreenTrace[] = "useScreenTrace";
const char kUseScreenProbes[] = "useScreenProbes";
const char kProbeDirectionsPerProbe[] = "probeDirectionsPerProbe";
const char kProbeMaxProbesPerFrame[] = "probeMaxProbesPerFrame";
const char kUseTemporalFilter[] = "useTemporalFilter";
const char kUseSpatialFilter[] = "useSpatialFilter";
const char kSpatialRadiusMin[] = "spatialRadiusMin";
const char kSpatialRadiusMax[] = "spatialRadiusMax";
const char kSpatialVarianceThresholdLow[] = "spatialVarianceThresholdLow";
const char kSpatialVarianceThresholdHigh[] = "spatialVarianceThresholdHigh";
const char kSpatialFireflyClamp[] = "spatialFireflyClamp";
const char kSpatialNeighborhoodRadius[] = "spatialNeighborhoodRadius";
const char kSpatialTemporalVarianceWeight[] = "spatialTemporalVarianceWeight";
const char kSpatialDepthThreshold[] = "spatialDepthThreshold";
const char kSpatialDepthSigmaInv[] = "spatialDepthSigmaInv";
const char kSpatialNormalExponent[] = "spatialNormalExponent";
const char kSpatialMaterialMismatchWeight[] = "spatialMaterialMismatchWeight";
const char kUseRadianceCache[] = "useRadianceCache";
const char kSurfaceCacheAtlasSize[] = "surfaceCacheAtlasSize";
const char kCaptureMaxPagesPerFrame[] = "captureMaxPagesPerFrame";

// S6: Mesh SDF / Global Distance Field properties.
const char kUseGDF[] = "useGDF";
const char kMeshSDFBuilderPath[] = "meshSDFBuilderPath";
const char kMeshSDFCacheDir[] = "meshSDFCacheDir";
const char kMeshSDFResolution[] = "meshSDFResolution";
const char kMeshSDFQuality[] = "meshSDFQuality";
const char kMeshSDFPadding[] = "meshSDFPadding";
const char kMeshSDFBudgetBytes[] = "meshSDFBudgetBytes";
const char kGDFLevelCount[] = "gdfLevelCount";
const char kGDFResolution[] = "gdfResolution";
const char kGDFBaseExtent[] = "gdfBaseExtent";
const char kGDFTraceMaxSteps[] = "gdfTraceMaxSteps";
const char kGDFTraceMaxDistance[] = "gdfTraceMaxDistance";
const char kGDFEmptyDistanceScale[] = "gdfEmptyDistanceScale";

// S6 shader files.
const char kGDFComposeShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFCompose.cs.slang";
const char kGDFTraceShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFTrace.cs.slang";

///< Camera margin in front of the captured card face, in meters. Mirrors
///< kLumenCardCaptureDefaultNearMargin in LumenCardCaptureData.slang (frozen with Agent C).
const float kCaptureNearMargin = 0.01f;

///< Byte size of one indirect draw argument slot. DrawIndexedArguments is 5 x uint32
///< (20 B) and DrawArguments is 4 x uint32 (16 B, IndirectCommands.h); a uniform 20-byte
///< stride is used for both so the GPU-side offset is always commandIndex * 20.
constexpr uint32_t kCaptureDrawArgBytes = 20u;

///< Host mirror of cbuffer LumenSurfaceCacheLightingCB in
///< LumenSurfaceCacheLightingData.slang (frozen 48-byte layout). The host binds every field by
///< name each dispatch; this struct only documents and static-asserts the GPU layout so a
///< future root-side copy cannot drift from the shader contract. S3-B2: the S3-B1 gReserved[2]
///< padding slot was replaced by the three feedback fields; the total stays 48 bytes
///< (16-byte aligned, no padding), so the pre-S3-B2 fields keep their offsets.
struct LumenSurfaceCacheLightingCB
{
    uint32_t gPagesPerSide;    // +0  Tiles per atlas side (must equal the allocator grid).
    uint32_t gRenderPageCount; // +4  Pages lit this dispatch == dispatch X size.
    uint32_t gFrameIndex;      // +8  Frame counter, rotates the per-frame RNG seed.
    uint32_t gSeed;            // +12 Fixed base seed.
    uint32_t gSamplesPerTexel; // +16 1/2/4/8 (LumenCacheLightingQuality).
    uint32_t gDebugMode;       // +20 kLumenDebug* from LumenGIData.slang; inert without gDebugTexture.
    uint32_t gAtlasSize[2];    // +24 Atlas size in texels.
    float gNearMargin;         // +32 Card camera near margin (meters); <= 0 -> shader default.
    uint32_t gCacheLightingFeedbackEnabled;    // +36 S3-B2 feedback enable (0/1).
    float gCacheLightingFeedbackStrength;      // +40 S3-B2 feedback strength multiplier.
    uint32_t gCacheLightingFeedbackMaxBounces; // +44 S3-B2 bounce cap (uint).
};
static_assert(sizeof(LumenSurfaceCacheLightingCB) == 48, "LumenSurfaceCacheLightingCB mirror is 48 bytes (16B-aligned, no padding)");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gPagesPerSide) == 0, "gPagesPerSide offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gRenderPageCount) == 4, "gRenderPageCount offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gFrameIndex) == 8, "gFrameIndex offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gSeed) == 12, "gSeed offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gSamplesPerTexel) == 16, "gSamplesPerTexel offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gDebugMode) == 20, "gDebugMode offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gAtlasSize) == 24, "gAtlasSize offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gNearMargin) == 32, "gNearMargin offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gCacheLightingFeedbackEnabled) == 36, "gCacheLightingFeedbackEnabled offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gCacheLightingFeedbackStrength) == 40, "gCacheLightingFeedbackStrength offset");
static_assert(offsetof(LumenSurfaceCacheLightingCB, gCacheLightingFeedbackMaxBounces) == 44, "gCacheLightingFeedbackMaxBounces offset");

const ChannelList kInputChannels = {
    // clang-format off
    { "vbuffer",                       "gVBuffer",                       "Packed visibility buffer" },
    { "linearZ",                       "gLinearZ",                       "Linear depth and slope", false, ResourceFormat::RG32Float },
    { "mvec",                          "gMotionVector",                  "Screen-space motion vector", false, ResourceFormat::RG32Float },
    { "mvecW",                         "gMotionVectorW",                 "World-space motion vector", true, ResourceFormat::RGBA16Float },
    { "normWRoughnessMaterialID",      "gNormalRoughnessMaterialID",     "Packed world normal, roughness, and material ID", false, ResourceFormat::RGB10A2Unorm },
    { "viewW",                         "gViewW",                         "World-space view direction", false, ResourceFormat::RGBA32Float },
    { "diffuseOpacity",                "gDiffuseOpacity",                "Diffuse reflectance and opacity", true, ResourceFormat::RGBA32Float },
    { "emissive",                      "gEmissive",                      "Emissive radiance", true, ResourceFormat::RGBA32Float },
    { "directLighting",                "gDirectLighting",                "Optional externally evaluated direct illumination", true },
    // clang-format on
};

const ChannelList kOutputChannels = {
    // clang-format off
    { "diffuseGI",                     "gDiffuseGI",                     "Final modulated linear diffuse indirect radiance", false, ResourceFormat::RGBA16Float },
    { "diffuseRadianceHitDist",        "gDiffuseRadianceHitDist",        "Unmodulated NRD-compatible diffuse radiance and hit distance", false, ResourceFormat::RGBA16Float },
    { "confidence",                    "gConfidence",                    "GI confidence", false, ResourceFormat::R16Float },
    { "bentNormal",                    "gBentNormal",                    "Optional world-space bent normal", true, ResourceFormat::RGBA16Float },
    { "debugOutput",                   "gDebugOutput",                   "Selected LumenGI diagnostic output", false, ResourceFormat::RGBA16Float },
    { "cardCoverage",                  "gCardCoverage",                  "Surface cache page coverage (allocated / total pages)", true, ResourceFormat::R32Float },
    { "screenTrace",                   "gScreenTraceResult",             "S4 screen-space trace: RGB=hitUV/distance, A=confidence or -(reason+1)", true, ResourceFormat::RGBA16Float },
    { "probeRadiance",                 "gProbeRadiance",                 "S4.2 screen probe grid: RGB=avg radiance at the probe, A=hit fraction (sparse)", true, ResourceFormat::RGBA16Float },
    { "probeInterpolated",             "gGIOutput",                      "S4.3 probe interpolate: RGB=incident irradiance E, A=confidence in [0,1]. S5-B1 temporal-filter input.", true, ResourceFormat::RGBA16Float },
    { "temporalFiltered",              "gTemporalOutput",                "S5-B1 temporal filter: RGB=temporally filtered incident irradiance, A=NEW history length (capped). S5 main output.", true, ResourceFormat::RGBA16Float },
    { "temporalAlpha",                 "gTemporalAlpha",                 "S5-B1 effective EMA alpha (1 = full reject / reset). Accept/reject cross-check.", true, ResourceFormat::R32Float },
    { "temporalConfidence",            "gTemporalConfidence",            "S5-B1 updated confidence; input to the S5-B2 spatial filter.", true, ResourceFormat::R32Float },
    { "spatialFiltered",               "gSpatialOutput",                 "S5-B2 spatial filter: RGB=variance-guided filtered incident irradiance, A=filtered confidence. Consumes temporalFiltered + temporalConfidence.", true, ResourceFormat::RGBA16Float },
    { "gdfTrace",                      "gGDFTraceOutput",                "S6-B4 GDF sphere trace: RGB=(t/tMax, |SDF| at surface/voxel, t), A=hit. Scriptable S6 gate channel.", true, ResourceFormat::RGBA16Float },
    // clang-format on
};

// -------------------------------------------------------------------------------------
// S6: Mesh SDF / Global Distance Field host helpers. All pure host code (no CPU-component
// or shader edits): the mesh->SDF volume generation (built-in analytic box SDF or an
// external MeshSDFBuilder.exe), the mesh content hash, the box-OBJ writer and the GDF
// dirty-region / clipmap host mirrors.
// -------------------------------------------------------------------------------------

///< Sphere-trace counters layout (mirror of LumenGDFTrace.cs.slang kGDFTraceStat*).
constexpr uint32_t kGDFTraceStatCount = 5u;
constexpr uint32_t kGDFTraceStatTraced = 0;
constexpr uint32_t kGDFTraceStatHit = 1;
constexpr uint32_t kGDFTraceStatMiss = 2;
constexpr uint32_t kGDFTraceStatMaxSteps = 3;
constexpr uint32_t kGDFTraceStatNoGrid = 4;

///< GDF shader resource-array length (mirror of kLumenGDFMaxLevels in LumenGDFData.slang).
///< Every slot of the gGDFLevels[16] arrays must be bound to a VALID descriptor; the host fills
///< them all (repeating the last level) because D3D12 aborts the dispatch on an invalid entry.
constexpr uint32_t kLumenGDFMaxLevelsHost = 16;

/// FNV-1a 64-bit (algorithm-identical to the S6 cache / builder / atlas hashes; distinct
/// name avoids ODR collisions with the symbols those headers define).
uint64_t s6FNV1a64(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;
    constexpr uint64_t kPrime = 0x100000001b3ULL;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= kPrime;
    }
    return h;
}

/// Analytic signed distance to an axis-aligned box [bmin, bmax] at (px,py,pz), math
/// convention (positive outside). The box is the mesh's padded object-space AABB, so the
/// produced field is a faithful proxy for closed box-like meshes (Cornell walls / boxes).
float s6BoxSDF(
    float px,
    float py,
    float pz,
    const std::array<float, 3>& bmin,
    const std::array<float, 3>& bmax
)
{
    const float qx = std::fabs(px - 0.5f * (bmin[0] + bmax[0])) - 0.5f * (bmax[0] - bmin[0]);
    const float qy = std::fabs(py - 0.5f * (bmin[1] + bmax[1])) - 0.5f * (bmax[1] - bmin[1]);
    const float qz = std::fabs(pz - 0.5f * (bmin[2] + bmax[2])) - 0.5f * (bmax[2] - bmin[2]);
    const float outside = std::sqrt(std::max(qx, 0.f) * std::max(qx, 0.f) +
                                    std::max(qy, 0.f) * std::max(qy, 0.f) +
                                    std::max(qz, 0.f) * std::max(qz, 0.f));
    const float inside = std::min(std::max(qx, std::max(qy, qz)), 0.f);
    return outside + inside;
}

/// Write a minimal OBJ for the axis-aligned box [bmin, bmax] (feeds MeshSDFBuilder.exe).
bool s6WriteBoxOBJ(
    const std::filesystem::path& path,
    const std::array<float, 3>& bmin,
    const std::array<float, 3>& bmax,
    std::string& err
)
{
    std::ofstream out(path);
    if (!out)
    {
        err = "cannot open box obj: " + path.string();
        return false;
    }
    const float x[2] = {bmin[0], bmax[0]};
    const float y[2] = {bmin[1], bmax[1]};
    const float z[2] = {bmin[2], bmax[2]};
    for (int i = 0; i < 8; ++i)
    {
        const int ix = (i & 1) ? 1 : 0;
        const int iy = (i & 2) ? 1 : 0;
        const int iz = (i & 4) ? 1 : 0;
        out << "v " << x[ix] << " " << y[iy] << " " << z[iz] << "\n";
    }
    // 6 faces x 2 triangles (1-based indices). Winding only affects the normal-vote
    // diagnostics, not the unsigned distance; the parity sign is authoritative.
    const int faces[6][4] = {
        {0, 2, 6, 4}, // -z
        {1, 5, 7, 3}, // +z
        {0, 1, 3, 2}, // -y
        {4, 6, 7, 5}, // +y
        {0, 4, 5, 1}, // -x
        {2, 3, 7, 6}, // +x
    };
    for (int f = 0; f < 6; ++f)
    {
        out << "f " << (faces[f][0] + 1) << " " << (faces[f][1] + 1) << " " << (faces[f][2] + 1) << "\n";
        out << "f " << (faces[f][0] + 1) << " " << (faces[f][2] + 1) << " " << (faces[f][3] + 1) << "\n";
    }
    if (!out)
    {
        err = "failed writing box obj: " + path.string();
        return false;
    }
    return true;
}

/// Built-in volume builder: analytic box SDF over the mesh's padded object AABB
/// (cacheParams.gridBounds, which the host always populates). This is the "placeholder /
/// built-in generation" fallback used when MeshSDFBuilder.exe is not configured/present.
bool s6BoxSDFBuilder(
    const s6scene::LumenMeshSDFSceneMeshDesc& mesh,
    const std::filesystem::path& targetPath,
    std::string& err
)
{
    const std::array<uint32_t, 3>& res = mesh.cacheParams.resolution;
    const std::array<float, 6>& g = mesh.cacheParams.gridBounds;
    if (res[0] < 2 || res[1] < 2 || res[2] < 2)
    {
        err = "box SDF builder requires resolution >= 2 per axis";
        return false;
    }
    if (!(g[0] < g[3] && g[1] < g[4] && g[2] < g[5]))
    {
        err = "box SDF builder requires valid grid bounds";
        return false;
    }

    s6ns::MSDFHeader h;
    h.formatVersion = s6ns::kMSDFFormatVersion;
    h.resolution = res;
    h.bboxMin = {g[0], g[1], g[2]};
    h.bboxMax = {g[3], g[4], g[5]};
    const float maxExt = std::max(g[3] - g[0], std::max(g[4] - g[1], g[5] - g[2]));
    const uint32_t maxRes = std::max(res[0], std::max(res[1], res[2]));
    h.voxelSize = maxExt / (float(maxRes) - 1.f); // voxel-center convention.
    h.normalizationScale = 1.f;                   // output == object space (identity instance map).
    h.paddingWorld = 0.f;
    h.signConvention = s6ns::kLumenMeshSDFSignConventionPositiveOutside;
    h.signReliable = 1; // a closed box has a well-defined sign.
    h.dataCount = uint64_t(res[0]) * uint64_t(res[1]) * uint64_t(res[2]);

    const std::array<float, 3> bmin = {g[0], g[1], g[2]};
    const std::array<float, 3> bmax = {g[3], g[4], g[5]};
    std::vector<float> d(size_t(h.dataCount));
    size_t i = 0;
    for (uint32_t z = 0; z < res[2]; ++z)
        for (uint32_t y = 0; y < res[1]; ++y)
            for (uint32_t x = 0; x < res[0]; ++x, ++i)
            {
                const float px = g[0] + (float(x) + 0.5f) * h.voxelSize;
                const float py = g[1] + (float(y) + 0.5f) * h.voxelSize;
                const float pz = g[2] + (float(z) + 0.5f) * h.voxelSize;
                d[i] = s6BoxSDF(px, py, pz, bmin, bmax);
            }

    std::vector<uint8_t> bytes;
    std::string serr;
    if (!s6ns::Cache::serializeMSDFBytes(h, d, {}, bytes, serr))
    {
        err = "box SDF serialize failed: " + serr;
        return false;
    }
    if (!s6ns::Cache::store(targetPath, bytes, err))
        return false;
    return true;
}

/// External builder wrapper: invokes MeshSDFBuilder.exe on a box OBJ derived from the mesh's
/// grid bounds (or on mesh.sourcePath when provided). The scene validates the produced
/// ".msdf" before storing it, so a bad exit code or a corrupt output never reaches the cache.
bool s6ExternalMeshSDFBuilder(
    const std::filesystem::path& exe,
    const s6scene::LumenMeshSDFSceneMeshDesc& mesh,
    const std::filesystem::path& targetPath,
    std::string& err
)
{
    const std::array<uint32_t, 3>& res = mesh.cacheParams.resolution;
    if (res[0] < 2 || res[1] < 2 || res[2] < 2)
    {
        err = "external builder requires resolution >= 2 per axis";
        return false;
    }

    std::filesystem::path inputPath = mesh.sourcePath;
    std::string bboxArg;
    if (inputPath.empty())
    {
        const std::array<float, 6>& g = mesh.cacheParams.gridBounds;
        if (!(g[0] < g[3] && g[1] < g[4] && g[2] < g[5]))
        {
            err = "external builder requires valid grid bounds";
            return false;
        }
        inputPath = targetPath.parent_path() / (targetPath.filename().string() + ".box.obj");
        if (!s6WriteBoxOBJ(inputPath, {g[0], g[1], g[2]}, {g[3], g[4], g[5]}, err))
            return false;
        std::stringstream ss;
        ss << " --bbox " << g[0] << "," << g[1] << "," << g[2] << "," << g[3] << "," << g[4] << "," << g[5];
        bboxArg = ss.str();
    }

    std::stringstream cmd;
    cmd << "\"" << exe.string() << "\" --input \"" << inputPath.string()
        << "\" --output \"" << targetPath.string()
        << "\" --resolution " << res[0] << "," << res[1] << "," << res[2]
        << " --no-normalize --padding 0" << bboxArg;
    const int rc = std::system(cmd.str().c_str());
    if (rc != 0)
    {
        err = "MeshSDFBuilder.exe exited with code " + std::to_string(rc);
        return false;
    }
    return true;
}

/// Padded output-space grid bounds for a mesh's object AABB (S6-A volume grid input).
/// Every axis gets at least a 2-voxel pad so degenerate (flat) meshes still produce a
/// valid volume grid.
std::array<float, 6> s6PaddedGridBounds(const Falcor::AABB& oabb, uint32_t resolution, float padFraction)
{
    const Falcor::float3 extent = oabb.extent();
    const float maxExt = std::max(extent.x, std::max(extent.y, extent.z));
    const float voxel = maxExt / (float(std::max<uint32_t>(resolution, 2u)) - 1.f);
    const float pad = std::max(padFraction * maxExt, 2.f * voxel);
    std::array<float, 6> g;
    g[0] = oabb.minPoint.x - pad;
    g[3] = oabb.maxPoint.x + pad;
    g[1] = oabb.minPoint.y - pad;
    g[4] = oabb.maxPoint.y + pad;
    g[2] = oabb.minPoint.z - pad;
    g[5] = oabb.maxPoint.z + pad;
    return g;
}

/// Mesh geometry-identity hash (S6-A2 cache key input). Stable per scene; a geometry or
/// bounds change invalidates the entry.
uint64_t s6MeshContentHash(uint32_t meshID, const Falcor::AABB& oabb, uint32_t vertexCount, uint32_t indexCount)
{
    struct Key
    {
        uint32_t meshID;
        uint32_t vertexCount;
        uint32_t indexCount;
        std::array<float, 6> bounds;
    };
    Key k;
    k.meshID = meshID;
    k.vertexCount = vertexCount;
    k.indexCount = indexCount;
    k.bounds = {
        oabb.minPoint.x, oabb.minPoint.y, oabb.minPoint.z,
        oabb.maxPoint.x, oabb.maxPoint.y, oabb.maxPoint.z,
    };
    return s6FNV1a64(&k, sizeof(k));
}

// -------------------------------------------------------------------------------------
// S6 GPU layout mirrors (host -> GPU upload structs; keep in sync with LumenGDFData.slang /
// LumenMeshSDFAtlas.slang). The GDF instance entry is the scene's LumenMeshSDFGDFInstance
// (40 bytes, already a byte-exact mirror of LumenGDFInstance).
// -------------------------------------------------------------------------------------

/// Host mirror of `LumenGDFLevelParams` in LumenGDFData.slang (32 bytes).
struct LumenGDFLevelParamsHost
{
    uint32_t resolution = 0;   // +0  voxels per side.
    uint32_t format = 0;       // +4  kLumenMeshSDFFormatR16Float (0) / R8Snorm (1).
    float worldExtent = 0.f;   // +8  level extent (meters).
    float voxelSize = 0.f;     // +12 extent / resolution.
    float quantRange = 0.f;    // +16 R8Snorm scale R of this level.
    float emptyDistance = 0.f; // +20 empty-voxel distance (> 0).
    uint32_t flags = 0;        // +24 reserved.
    uint32_t pad = 0;          // +28
};
static_assert(sizeof(LumenGDFLevelParamsHost) == 32, "LumenGDFLevelParams host mirror is 32 bytes");

/// Host mirror of `LumenGDFDirtyRegion` in LumenGDFData.slang (40 bytes; the shader's "36 bytes"
/// comment is a doc slip - level+axis+int3+int3+flags+pad = 40).
struct LumenGDFDirtyRegionHost
{
    uint32_t level = 0;        // +0
    uint32_t axis = 0;         // +4 (informational; 0xFFFFFFFF for a full-level region).
    int32_t min[3] = {0, 0, 0}; // +8 inclusive lower corner.
    int32_t max[3] = {0, 0, 0}; // +20 inclusive upper corner.
    uint32_t flags = 0;        // +32 kLumenGDFDirtyFlagAdded (1) / kLumenGDFDirtyFlagRemoved (2).
    uint32_t pad = 0;          // +36
};
static_assert(sizeof(LumenGDFDirtyRegionHost) == 40, "LumenGDFDirtyRegion host mirror is 40 bytes");

/// Host mirror of the LumenGDFDirtyFlag* bits (LumenGDFData.slang).
constexpr uint32_t kLumenGDFDirtyFlagAdded = 1u << 0;
constexpr uint32_t kLumenGDFDirtyFlagRemoved = 1u << 1;

/// Tile one mip's float data (x-fastest, dims^3) into a flat atlas image (GPU layout:
/// page slot s -> brick origin (s % P, (s / P) % P, s / (P*P)) * pageSize, texel index
/// (tz * T + ty) * T + tx with T = P * pageSize). Mirrors the fixed atlas sampler in
/// LumenMeshSDFAtlas.slang (this host image is what the GPU atlas textures are uploaded from).
void s6TileMipIntoAtlasImage(
    const std::vector<float>& data,
    const std::array<uint32_t, 3>& res,
    uint32_t mip,
    uint32_t baseSlot,
    uint32_t pagesPerSide,
    std::vector<float>& image
)
{
    const std::array<uint32_t, 3> dims = s6ns::atlasMipDims(res, mip);
    const uint32_t bricks[3] = {
        (dims[0] + s6ns::kLumenMeshSDFAtlasPageSize - 1u) / s6ns::kLumenMeshSDFAtlasPageSize,
        (dims[1] + s6ns::kLumenMeshSDFAtlasPageSize - 1u) / s6ns::kLumenMeshSDFAtlasPageSize,
        (dims[2] + s6ns::kLumenMeshSDFAtlasPageSize - 1u) / s6ns::kLumenMeshSDFAtlasPageSize,
    };
    const uint32_t T = pagesPerSide * s6ns::kLumenMeshSDFAtlasPageSize;
    for (uint32_t bz = 0; bz < bricks[2]; ++bz)
        for (uint32_t by = 0; by < bricks[1]; ++by)
            for (uint32_t bx = 0; bx < bricks[0]; ++bx)
            {
                const uint32_t brickIdx = bz * (bricks[0] * bricks[1]) + by * bricks[0] + bx;
                const uint32_t slot = baseSlot + brickIdx;
                const uint32_t ox = (slot % pagesPerSide) * s6ns::kLumenMeshSDFAtlasPageSize;
                const uint32_t oy = ((slot / pagesPerSide) % pagesPerSide) * s6ns::kLumenMeshSDFAtlasPageSize;
                const uint32_t oz = (slot / (pagesPerSide * pagesPerSide)) * s6ns::kLumenMeshSDFAtlasPageSize;
                for (uint32_t z = 0; z < s6ns::kLumenMeshSDFAtlasPageSize; ++z)
                    for (uint32_t y = 0; y < s6ns::kLumenMeshSDFAtlasPageSize; ++y)
                        for (uint32_t x = 0; x < s6ns::kLumenMeshSDFAtlasPageSize; ++x)
                        {
                            const uint32_t vx = bx * s6ns::kLumenMeshSDFAtlasPageSize + x;
                            const uint32_t vy = by * s6ns::kLumenMeshSDFAtlasPageSize + y;
                            const uint32_t vz = bz * s6ns::kLumenMeshSDFAtlasPageSize + z;
                            if (vx >= dims[0] || vy >= dims[1] || vz >= dims[2])
                                continue;
                            const size_t src = (static_cast<size_t>(vz) * dims[1] + vy) * dims[0] + vx;
                            const size_t dst = (static_cast<size_t>(oz + z) * T + (oy + y)) * T + (ox + x);
                            image[dst] = data[src];
                        }
            }
}

/// Convert a float atlas image in [-1, 1] into int8 R8_SNORM codes (the coarse-atlas upload
/// representation; matches the GPU R8_SNORM float<->code/127 conversion).
void s6CoarseImageToCodes(const std::vector<float>& image, std::vector<int8_t>& out)
{
    out.resize(image.size());
    for (size_t i = 0; i < image.size(); ++i)
    {
        const float v = std::max(-1.f, std::min(1.f, image[i]));
        out[i] = static_cast<int8_t>(std::lround(v * 127.f));
    }
}

void registerBindings(pybind11::module& m)
{
    pybind11::class_<LumenGIPass, RenderPass, ref<LumenGIPass>> pass(m, "LumenGI");
    // Scriptable S2 gate snapshot: read as m.activeGraph.getPass("LumenGI").surfaceCacheStats.
    // std::map<std::string, double> converts to a Python dict losslessly.
    pass.def_property_readonly("surfaceCacheStats", &LumenGIPass::getSurfaceCacheStats);
    // Scriptable S6 gate snapshot: read as m.activeGraph.getPass("LumenGI").gdfStats.
    pass.def_property_readonly("gdfStats", &LumenGIPass::getGDFStats);
    // Compatibility alias for the Z15 S6-C2 atlas test skeleton (run_sdf_atlas.py) which probes
    // a `meshSDFSceneStats` dict binding; the scene-pipeline counters live in gdfStats.
    pass.def_property_readonly("meshSDFSceneStats", &LumenGIPass::getGDFStats);
}
} // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, LumenGIPass>();
    ScriptBindings::registerBinding(registerBindings);
}

LumenGIPass::LumenGIPass(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
    , mPageCache(
          kLumenSurfaceCacheDefaultAtlasSize,
          kLumenSurfaceCacheDefaultMemoryBudgetBytes,
          kLumenMinResidencyFrames
      )
    , mCaptureScheduler(nullptr, &mPageCache, kLumenCaptureDefaultMaxPagesPerFrame, kLumenCaptureDefaultInFlightTimeoutFrames)
{
    if (!mpDevice->isShaderModelSupported(ShaderModel::SM6_5))
        FALCOR_THROW("LumenGI requires Shader Model 6.5 support.");
    if (!mpDevice->isFeatureSupported(Device::SupportedFeatures::RaytracingTier1_1))
        FALCOR_THROW("LumenGI requires Raytracing Tier 1.1 support.");

    parseProperties(props);

    // Normalize the configured atlas size to whole tiles and rebuild the CPU components with
    // it. The scheduler re-points at mpCardScene and mPageCache on setScene().
    const uint32_t tileCount = std::max<uint32_t>(1u, mAtlasSizeTexels / kLumenSurfaceCacheTileSize);
    mAtlasSizeTexels = tileCount * kLumenSurfaceCacheTileSize;
    mCapturePagesPerSide = tileCount;
    mPageCache = LumenSurfaceCache(mAtlasSizeTexels, kLumenSurfaceCacheDefaultMemoryBudgetBytes, kLumenMinResidencyFrames);
    mCaptureScheduler = LumenCaptureSchedulerForScene(
        mpCardScene.get(), &mPageCache, mCaptureMaxPagesPerFrame, kLumenCaptureDefaultInFlightTimeoutFrames
    );

    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_DEFAULT);
    FALCOR_ASSERT(mpSampleGenerator);
    createDebugPass();
}

void LumenGIPass::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kEnabled)
            mEnabled = value;
        else if (key == kTraceMode)
            mTraceMode = value;
        else if (key == kQualityPreset)
            mQualityPreset = value;
        else if (key == kDebugMode)
            mDebugMode = value;
        else if (key == kUseSurfaceCache)
            mUseSurfaceCache = value;
        else if (key == kUseCacheLighting)
            mUseCacheLighting = value;
        else if (key == kCacheLightingFeedback)
            mCacheLightingFeedbackEnabled = value;
        else if (key == kCacheLightingFeedbackStrength)
            mCacheLightingFeedbackStrength = value;
        else if (key == kCacheLightingFeedbackMaxBounces)
            mCacheLightingFeedbackMaxBounces = value;
        else if (key == kUseScreenTrace)
            mUseScreenTrace = value;
        else if (key == kUseScreenProbes)
            mUseScreenProbes = value;
        else if (key == kProbeDirectionsPerProbe)
            mProbeDirectionsPerProbe = std::clamp<uint32_t>(value, 1u, LumenScreenProbe::kMaxDirectionsPerProbe);
        else if (key == kProbeMaxProbesPerFrame)
            mProbeMaxProbesPerFrame = value;
        else if (key == kUseTemporalFilter)
            mUseTemporalFilter = value;
        else if (key == kUseSpatialFilter)
            mUseSpatialFilter = value;
        else if (key == kSpatialRadiusMin)
            mSpatialRadiusMin = value;
        else if (key == kSpatialRadiusMax)
            mSpatialRadiusMax = value;
        else if (key == kSpatialVarianceThresholdLow)
            mSpatialVarianceThresholdLow = value;
        else if (key == kSpatialVarianceThresholdHigh)
            mSpatialVarianceThresholdHigh = value;
        else if (key == kSpatialFireflyClamp)
            mSpatialFireflyClamp = value;
        else if (key == kSpatialNeighborhoodRadius)
            mSpatialNeighborhoodRadius = std::clamp<uint32_t>(value, 1u, 2u);
        else if (key == kSpatialTemporalVarianceWeight)
            mSpatialTemporalVarianceWeight = value;
        else if (key == kSpatialDepthThreshold)
            mSpatialDepthThreshold = value;
        else if (key == kSpatialDepthSigmaInv)
            mSpatialDepthSigmaInv = value;
        else if (key == kSpatialNormalExponent)
            mSpatialNormalExponent = value;
        else if (key == kSpatialMaterialMismatchWeight)
            mSpatialMaterialMismatchWeight = value;
        else if (key == kUseRadianceCache)
            mUseRadianceCache = value;
        else if (key == kSurfaceCacheAtlasSize)
            mAtlasSizeTexels = value;
        else if (key == kCaptureMaxPagesPerFrame)
            mCaptureMaxPagesPerFrame = value;
        else if (key == kUseGDF)
            mUseGDF = value;
        else if (key == kMeshSDFBuilderPath)
        {
            const std::string s = value;
            mMeshSDFBuilderPath = std::filesystem::path(s);
        }
        else if (key == kMeshSDFCacheDir)
        {
            const std::string s = value;
            mMeshSDFCacheDir = std::filesystem::path(s);
        }
        else if (key == kMeshSDFResolution)
        {
            const uint32_t v = value;
            mMeshSDFResolution = std::max<uint32_t>(v, 8u);
        }
        else if (key == kMeshSDFQuality)
            mMeshSDFQuality = value ? 1u : 0u;
        else if (key == kMeshSDFPadding)
        {
            const float v = value;
            mMeshSDFPadding = std::max(v, 0.f);
        }
        else if (key == kMeshSDFBudgetBytes)
            mMeshSDFBudgetBytes = value;
        else if (key == kGDFLevelCount)
        {
            const uint32_t v = value;
            mGDFLevelCount = std::clamp<uint32_t>(v, 1u, LumenGI::GlobalDistanceField::kMaxGDFLevels);
        }
        else if (key == kGDFResolution)
        {
            const uint32_t v = value;
            mGDFResolution = std::max<uint32_t>(v, 16u);
        }
        else if (key == kGDFBaseExtent)
        {
            const float v = value;
            mGDFBaseExtent = std::max(v, 0.1f);
        }
        else if (key == kGDFTraceMaxSteps)
            mGDFTraceMaxSteps = value;
        else if (key == kGDFTraceMaxDistance)
        {
            const float v = value;
            mGDFTraceMaxDistance = std::max(v, 0.f);
        }
        else if (key == kGDFEmptyDistanceScale)
        {
            const float v = value;
            mGDFEmptyDistanceScale = std::max(v, 1.f);
        }
        else
            logWarning("Unknown property '{}' in LumenGI properties.", key);
    }
}

Properties LumenGIPass::getProperties() const
{
    Properties props;
    props[kEnabled] = mEnabled;
    props[kTraceMode] = mTraceMode;
    props[kQualityPreset] = mQualityPreset;
    props[kDebugMode] = mDebugMode;
    props[kUseSurfaceCache] = mUseSurfaceCache;
    props[kUseCacheLighting] = mUseCacheLighting;
    props[kCacheLightingFeedback] = mCacheLightingFeedbackEnabled;
    props[kCacheLightingFeedbackStrength] = mCacheLightingFeedbackStrength;
    props[kCacheLightingFeedbackMaxBounces] = mCacheLightingFeedbackMaxBounces;
    props[kUseScreenTrace] = mUseScreenTrace;
    props[kUseScreenProbes] = mUseScreenProbes;
    props[kProbeDirectionsPerProbe] = mProbeDirectionsPerProbe;
    props[kProbeMaxProbesPerFrame] = mProbeMaxProbesPerFrame;
    props[kUseTemporalFilter] = mUseTemporalFilter;
    props[kUseSpatialFilter] = mUseSpatialFilter;
    props[kSpatialRadiusMin] = mSpatialRadiusMin;
    props[kSpatialRadiusMax] = mSpatialRadiusMax;
    props[kSpatialVarianceThresholdLow] = mSpatialVarianceThresholdLow;
    props[kSpatialVarianceThresholdHigh] = mSpatialVarianceThresholdHigh;
    props[kSpatialFireflyClamp] = mSpatialFireflyClamp;
    props[kSpatialNeighborhoodRadius] = mSpatialNeighborhoodRadius;
    props[kSpatialTemporalVarianceWeight] = mSpatialTemporalVarianceWeight;
    props[kSpatialDepthThreshold] = mSpatialDepthThreshold;
    props[kSpatialDepthSigmaInv] = mSpatialDepthSigmaInv;
    props[kSpatialNormalExponent] = mSpatialNormalExponent;
    props[kSpatialMaterialMismatchWeight] = mSpatialMaterialMismatchWeight;
    props[kUseRadianceCache] = mUseRadianceCache;
    props[kSurfaceCacheAtlasSize] = mAtlasSizeTexels;
    props[kCaptureMaxPagesPerFrame] = mCaptureMaxPagesPerFrame;
    props[kUseGDF] = mUseGDF;
    props[kMeshSDFBuilderPath] = mMeshSDFBuilderPath.string();
    props[kMeshSDFCacheDir] = mMeshSDFCacheDir.string();
    props[kMeshSDFResolution] = mMeshSDFResolution;
    props[kMeshSDFQuality] = mMeshSDFQuality;
    props[kMeshSDFPadding] = mMeshSDFPadding;
    props[kMeshSDFBudgetBytes] = mMeshSDFBudgetBytes;
    props[kGDFLevelCount] = mGDFLevelCount;
    props[kGDFResolution] = mGDFResolution;
    props[kGDFBaseExtent] = mGDFBaseExtent;
    props[kGDFTraceMaxSteps] = mGDFTraceMaxSteps;
    props[kGDFTraceMaxDistance] = mGDFTraceMaxDistance;
    props[kGDFEmptyDistanceScale] = mGDFEmptyDistanceScale;
    return props;
}

RenderPassReflection LumenGIPass::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(
        reflector,
        kOutputChannels,
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
        compileData.defaultTexDims
    );

    // Scriptable S3 gate channel: exposes the internal radiance atlas (RGB = direct, linear)
    // at atlas resolution for tests/lumengi/run_cachelighting.py (Agent N) and the S3 gate.
    // Optional so it is only allocated when the graph marks it as an output; the host blits
    // the atlas into it each frame (exportCacheDirectRadiance). RenderTarget flag is required
    // for the blit's destination RTV.
    auto& cacheDirect = reflector.addOutput(kCacheDirectRadiance, "Surface cache direct radiance atlas (RGB, linear)");
    cacheDirect.texture2D(mAtlasSizeTexels, mAtlasSizeTexels);
    cacheDirect.format(ResourceFormat::RGBA16Float);
    cacheDirect.bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget);
    cacheDirect.flags(RenderPassReflection::Field::Flags::Optional);
    return reflector;
}

void LumenGIPass::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    if (any(mFrameDim != compileData.defaultTexDims))
    {
        mFrameDim = compileData.defaultTexDims;
        resetHistory();
    }
}

void LumenGIPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[kRenderPassRefreshFlags] = flags | RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }

    const auto& pDiffuseGI = renderData.getTexture("diffuseGI");
    FALCOR_ASSERT(pDiffuseGI);
    const uint2 frameDim = {pDiffuseGI->getWidth(), pDiffuseGI->getHeight()};
    if (any(frameDim != mFrameDim))
    {
        mFrameDim = frameDim;
        resetHistory();
    }

    // Snapshot the accumulated scene updates before they are consumed below: both the
    // existing trace path and the S2 capture path need the same flag value.
    const IScene::UpdateFlags sceneUpdates = mSceneUpdates;
    if (mSceneUpdates != IScene::UpdateFlags::None)
    {
        if (is_set(mSceneUpdates, IScene::UpdateFlags::RecompileNeeded) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::GeometryChanged))
        {
            createTraceProgram();
        }
        // S5-A1 reset policy: a camera MOVEMENT alone does NOT reset the temporal history -- the
        // S5-B1 filter reprojects it along the GBufferRT motion vector and self-resets on
        // disocclusion (a camera cut produces motion vectors above gMotionLengthThreshold and a
        // stale prev depth, both of which the filter rejects). Everything else (geometry /
        // meshes / materials / lights / env map / scene graph / camera switch or property change)
        // is a hard invalidation and clears the prev history/depth double buffer so every pixel
        // takes the disocclusion path for one frame.
        const IScene::UpdateFlags hardInvalidation = mSceneUpdates & ~IScene::UpdateFlags::CameraMoved;
        if (hardInvalidation != IScene::UpdateFlags::None)
            resetHistory();
        mSceneUpdates = IScene::UpdateFlags::None;
    }

    // S5-A1 camera-cut detector: a LARGE camera jump between frames invalidates the temporal
    // history even though the scene update is CameraMoved-only (which the filter would otherwise
    // reproject and, for coplanar surfaces, legitimately re-use). Smooth pan/orbit deltas stay
    // below mCameraCutDistance and keep the history, relying on the motion-vector reprojection.
    if (mpScene)
    {
        const float3 camPos = mpScene->getCamera()->getPosition();
        if (length(camPos - mPrevCameraPosition) > mCameraCutDistance)
            resetHistory(); // hard reset: clear the prev history/depth double buffer on the cut frame.
        mPrevCameraPosition = camPos;
    }

    clearOutputs(pRenderContext, renderData);
    if (!mEnabled || !mpScene)
        return;

    if (!mTracer.pProgram)
        createTraceProgram();

    // Falcor builds the light collection lazily. useEmissiveLights() only
    // reports true once it exists, so ensure it is built before evaluating
    // the emissive-lighting define (otherwise Cornell-style scenes lose all
    // emissive contributions).
    if (mpScene && !mLightCollectionInitialized)
    {
        mpScene->getLightCollection(pRenderContext);
        mLightCollectionInitialized = true;
    }

    readbackCounters(pRenderContext);
    ensureTraceResources();

    // Track program-define changes so the vars are recreated whenever the program
    // recompiles. Without this the per-frame bindings below (notably the emissive
    // sampler) can target a reflection that no longer matches the freshly
    // recompiled program, which crashes the bind ("No member named 'emissiveSampler'
    // found", or an access violation in bindShaderData) on scene/light toggles.
    bool programChanged = false;
    programChanged |= mTracer.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    programChanged |= mTracer.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    programChanged |= mTracer.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    programChanged |= mTracer.pProgram->addDefine("is_valid_gViewW", renderData.getTexture("viewW") ? "1" : "0");
    programChanged |= mTracer.pProgram->addDefine("is_valid_gLumenGICounters", mpLumenGICounters ? "1" : "0");
    programChanged |= mTracer.pProgram->addDefine("is_valid_gLightingComponents", mpLightingComponents ? "1" : "0");
    programChanged |= mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());

    // Secondary-hit emissive next-event estimation: create and bind the LightBVH
    // sampler when the scene has active emissive lights. The sampler owns a BVH
    // over the emissive triangles and is scene-scoped (rebuilt on setScene).
    if (mpScene->useEmissiveLights())
    {
        if (!mpEmissiveLightSampler)
            mpEmissiveLightSampler = std::make_unique<LightBVHSampler>(pRenderContext, mpScene->getILightCollection(pRenderContext));
        mpEmissiveLightSampler->update(pRenderContext, mpScene->getILightCollection(pRenderContext));
        programChanged |= mTracer.pProgram->addDefines(mpEmissiveLightSampler->getDefines());
        programChanged |= mTracer.pProgram->addDefine("LUMEN_GI_HAS_EMISSIVE_SAMPLER", "1");
    }
    else
    {
        // No emissive light sampler this frame. Falcor's Program::addDefine never removes a
        // define, so pin the shader-side _EMISSIVE_LIGHT_SAMPLER_TYPE back to the Null sampler
        // (EmissiveLightSamplerType::Null = 0xff); otherwise the typedef keeps resolving to the
        // previously-bound LightBVH type across scene/light toggles and default-initializing a
        // resource-typed sampler fails in dxc.
        programChanged |= mTracer.pProgram->addDefine("LUMEN_GI_HAS_EMISSIVE_SAMPLER", "0");
        // EmissiveLightSamplerType::Null == 0xff (see EmissiveLightSamplerType.slangh).
        programChanged |= mTracer.pProgram->addDefine("_EMISSIVE_LIGHT_SAMPLER_TYPE", "255");
    }

    // Recreate the vars when any program define changed so the reflection the
    // bindings below are resolved against matches the compiled program.
    if (programChanged)
        mTracer.pVars = nullptr;
    if (!mTracer.pVars)
        prepareTraceVars();

    auto traceVar = mTracer.pVars->getRootVar();
    traceVar["CB"]["gFrameDim"] = mFrameDim;
    traceVar["CB"]["gFrameIndex"] = mFrameIndex;
    traceVar["CB"]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;
    traceVar["gVBuffer"] = renderData.getTexture("vbuffer");
    traceVar["gViewW"] = renderData.getTexture("viewW");
    traceVar["gDiffuseRadianceHitDist"] = renderData.getTexture("diffuseRadianceHitDist");
    traceVar["gDiffuseGI"] = renderData.getTexture("diffuseGI");
    traceVar["gConfidence"] = renderData.getTexture("confidence");
    traceVar["gLumenGICounters"] = mpLumenGICounters;
    traceVar["gLightingComponents"] = mpLightingComponents;
    // Bind the emissive sampler only when the emissive-light defines are active this frame
    // (LUMEN_GI_HAS_EMISSIVE_SAMPLER = 1): binding it while the define is 0 targets a parameter
    // that no longer exists in the shader and crashes the bind.
    if (mpEmissiveLightSampler && mpScene->useEmissiveLights())
        mpEmissiveLightSampler->bindShaderData(traceVar["emissiveSampler"]);

    // Clear the per-pixel lighting components before the trace so pixels
    // without a primary hit (sky, out-of-bounds) are not read as stale data.
    if (mpLightingComponents)
        pRenderContext->clearUAV(mpLightingComponents->getUAV().get(), float4(0.f));

    // S6: TraceMode::MeshSDF makes the GDF sphere trace the PRIMARY path (the S6-B4
    // pass writes the S1 outputs below). The HWRT raytrace is skipped so the pass output
    // is genuinely software-traced (no DXR in the hot path); the tracer program/vars are
    // still maintained so toggling back to HardwareRT stays instant. In every other mode
    // the S1 raytrace runs exactly as before.
    const bool sdfPrimaryPath = (mTraceMode == TraceMode::MeshSDF);
    if (!sdfPrimaryPath)
    {
        // The counters must be cleared before each dispatch; they accumulate
        // across the frame and are copied to the readback buffer afterwards.
        if (mpLumenGICounters)
            pRenderContext->clearUAV(mpLumenGICounters->getUAV().get(), uint4(0));

        mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(mFrameDim, 1));

        if (mpLumenGICounters && mpLumenGICountersReadback)
        {
            pRenderContext->copyResource(mpLumenGICountersReadback.get(), mpLumenGICounters.get());
            mCounterReadbackPending = true;
        }
    }

    // S2: Surface Cache / Cards capture. Pure additive work gated behind mUseSurfaceCache;
    // when disabled the pass behaves exactly like the S1 baseline.
    if (mUseSurfaceCache)
        runSurfaceCacheCapture(pRenderContext, sceneUpdates);

    // S3: Surface Cache direct lighting (S3-B1). Runs AFTER the capture pass so this frame's
    // freshly captured pages are lit immediately. Data dependency: the radiance atlas is only
    // consumed by later stages (S3-B2 feedback, S4 cache queries) in FUTURE frames; the screen
    // trace neither writes nor reads it, so placement after the trace/capture is safe either way.
    // Requires useSurfaceCache (the atlases and card->page mirror only exist then).
    if (mUseCacheLighting && mUseSurfaceCache)
        runCacheLighting(pRenderContext);

    // Scriptable S3 gate channel: copy the internal radiance atlas into the optional graph
    // output (atlas-sized) so tests/lumengi can read the cache-direct radiance directly.
    exportCacheDirectRadiance(pRenderContext, renderData);

    // S4: hierarchical screen-space trace (S4-A1). Builds the HZB chain from GBufferRT.linearZ
    // and dispatches the screen trace into the optional "screenTrace" output. Reads only
    // renderData["linearZ"] (a GBuffer input), so it is independent of the trace/cache paths
    // and can run any time after the GBuffer pass; placing it here keeps S1/S2/S3 untouched.
    // S4.2 probes consume the HZB chain (and optionally the screenTrace output as a prefilter),
    // so the screen trace also runs when only the probes are enabled.
    if (mUseScreenTrace || mUseScreenProbes)
        runScreenTrace(pRenderContext, renderData);

    // S4.2: screen probe gather (S4-A2/B2). Consumes the HZB chain built above (and, when the
    // graph allocates them, the screenTrace output as a prefilter) plus the S1 per-pixel
    // diffuseRadianceHitDist for the screen-radiance reuse; writes the probe hit records and
    // the optional "probeRadiance" grid. Runs after the screen trace every frame.
    if (mUseScreenProbes)
        runScreenProbeTrace(pRenderContext, renderData);

    // S5: temporal filter (S5-A1 history host + S5-B1 pass). Consumes the S4.3 interpolated GI
    // (probeInterpolated), the GBufferRT linearZ/motion and the S5-A1 prev history/depth double
    // buffer; writes the "temporalFiltered" graph channel (and optionally temporalAlpha /
    // temporalConfidence). Runs AFTER the interpolate pass (inside runScreenProbeTrace) and
    // BEFORE the debug pass. The allocation gates (probeInterpolated / temporalFiltered graph
    // channels) live inside runTemporalFilter; when the filter is off or the channel is absent
    // the pass is a no-op and the output stays cleared.
    if (mUseTemporalFilter)
        runTemporalFilter(pRenderContext, renderData);

    // S5-B2: spatial / variance-guided filter (S5-B2 pass + S5-A2 reconstruction CB). Consumes
    // the S5-B1 temporalFiltered output (RGB) + the temporalConfidence channel (the chosen
    // confidence source -- temporalFiltered.a carries the HISTORY LENGTH, not a confidence), the
    // GBufferRT linearZ / normal / material, and writes the "spatialFiltered" graph channel.
    // Runs AFTER the temporal filter and BEFORE the debug pass, gated on mUseSpatialFilter plus
    // the graph allocating both temporalFiltered and spatialFiltered (no-ops otherwise).
    if (mUseSpatialFilter)
        runSpatialFilter(pRenderContext, renderData);

    // S6: Mesh SDF / Global Distance Field pipeline. Active ONLY when explicitly enabled
    // (mUseGDF). The TraceMode::MeshSDF/Hybrid values remain a UI/API placeholder that falls
    // back to the HWRT path (todo.md: the software path is not implemented yet) so toggling
    // them never runs the half-integrated GDF compose pass (which currently lacks the atlas
    // resource bindings and aborts at dispatch with E_INVALIDARG). Every frame when enabled:
    //   * the camera anchor is pushed into the CPU clipmap (GDF scroll bookkeeping),
    //   * resident Mesh SDF instances are composed into the GDF clipmap textures
    //     (LumenGDFCompose.cs.slang) over the dirty regions only,
    //   * the GDF sphere trace (LumenGDFTrace.cs.slang) runs over the screen: in MeshSDF mode it
    //     REPLACES the S1 outputs (diffuseGI / diffuseRadianceHitDist / confidence), in Hybrid /
    //     HardwareRT-with-GDF mode it writes the optional "gdfTrace" diagnostic channel only.
    if (mUseGDF)
    {
        // S6-in-progress guard: the compose pass reads the mesh-SDF atlas resources that the
        // host wiring does not create yet; dispatching with them unbound aborts E_INVALIDARG.
        // Skip the whole S6 path until the full data pipeline is wired.
        if (mSDF.pFineAtlas && mSDF.pCoarseAtlas && mSDF.pPageTable && mSDF.pVolumes && mSDF.pAtlasInstances)
        {
            if (!mSDF.pScene)
                ensureMeshSDFScene();
            if (mSDF.pScene)
            {
                ensureGDFResources(pRenderContext);
                runGDFCompose(pRenderContext);
                runGDFSphereTrace(pRenderContext, renderData);
                readbackGDFTraceStats(pRenderContext);
                mSDF.pScene->endFrame();
            }
        }
        else
        {
            logWarning("LumenGI: GDF pipeline skipped (mesh-SDF atlas resources not wired yet; S6 integration pending).");
        }
    }

    if (!mpDebugPass)
        createDebugPass();

    DefineList defines = getValidResourceDefines(kInputChannels, renderData);
    defines.add("is_valid_gLinearZ", "1");
    defines.add("is_valid_gMotionVector", "1");
    defines.add("is_valid_gNormalRoughnessMaterialID", "1");
    defines.add("is_valid_gConfidence", "1");
    defines.add("is_valid_gDiffuseRadianceHitDist", "1");
    defines.add("is_valid_gLightingComponents", mpLightingComponents ? "1" : "0");
    defines.add("is_valid_gCards", (mUseSurfaceCache && mCapture.pCards) ? "1" : "0");
    defines.add("is_valid_gCardCoverage", renderData.getTexture("cardCoverage") ? "1" : "0");
    if (mpDebugPass->getProgram()->addDefines(defines))
        mpDebugPass->setVars(nullptr);

    auto var = mpDebugPass->getRootVar();
    var["CB"]["gFrameDim"] = mFrameDim;
    var["CB"]["gFrameIndex"] = mFrameIndex;
    var["CB"]["gDebugMode"] = static_cast<uint32_t>(mDebugMode);
    // Cards overlay projection uses the same camera matrices as the GBuffer pass.
    var["CB"]["gViewProj"] = mpScene->getCamera()->getViewProjMatrixNoJitter();
    var["CB"]["gCardCount"] = (mUseSurfaceCache && mpCardScene) ? mpCardScene->getCardCount() : 0u;
    // Page coverage scalars written by the debug pass into the cardCoverage channel.
    // R = captured-card coverage (clean / total cards), the S2-gate metric for
    // card placement completeness. G/B/A = atlas allocated/total/free pages.
    const LumenSurfaceCacheStats cacheStats = mPageCache.getStats();
    float coverage = 0.f;
    if (mUseSurfaceCache && mpCardScene)
    {
        const uint32_t totalCards = mpCardScene->getCardCount();
        const uint32_t dirtyCards = mpCardScene->getDirtyCardCount();
        coverage = totalCards > 0 ? (float)(totalCards - dirtyCards) / (float)totalCards : 0.f;
    }
    var["CB"]["gCardCoverageValue"] = coverage;
    var["CB"]["gAllocatedPages"] = (float)cacheStats.allocatedPageCount;
    var["CB"]["gTotalPages"] = (float)cacheStats.pageCount;
    var["CB"]["gFreePages"] = (float)cacheStats.freePageCount;
    var["gLinearZ"] = renderData.getTexture("linearZ");
    var["gMotionVector"] = renderData.getTexture("mvec");
    var["gMotionVectorW"] = renderData.getTexture("mvecW");
    var["gNormalRoughnessMaterialID"] = renderData.getTexture("normWRoughnessMaterialID");
    var["gConfidence"] = renderData.getTexture("confidence");
    var["gDiffuseRadianceHitDist"] = renderData.getTexture("diffuseRadianceHitDist");
    var["gLightingComponents"] = mpLightingComponents;
    if (mUseSurfaceCache && mCapture.pCards)
        var["gCards"] = mCapture.pCards;
    var["gDebugOutput"] = renderData.getTexture("debugOutput");
    if (renderData.getTexture("cardCoverage"))
        var["gCardCoverage"] = renderData.getTexture("cardCoverage");
    mpDebugPass->execute(pRenderContext, uint3(mFrameDim, 1));

    // Periodic capture telemetry for scripted churn/soak gates (every 60 frames).
    // Uses a dedicated counter: resetHistory() (material/scene changes) resets
    // mFrameIndex, which would otherwise print this every frame during churn.
    if (mUseSurfaceCache && mpCardScene && (++mCaptureStatsLogCounter % 60 == 0))
    {
        const LumenCaptureSchedulerStats stats = mCaptureScheduler.getStats();
        logInfo(
            "LumenGI capture stats: frame={} cards={} dirty={} alloc={} release={} fail={} lost={} recapture={} "
            "completed={} pending={} residentBytes={}",
            mFrameIndex, mpCardScene->getCardCount(), mpCardScene->getDirtyCardCount(), stats.totalAllocations,
            stats.totalReleases, stats.totalAllocationFailures, stats.totalLostPages, stats.totalRecaptures,
            stats.completedCaptures, stats.pendingQueueDepth, mPageCache.getResidentBytes()
        );
    }

    ++mFrameIndex;
}

void LumenGIPass::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;
    dirty |= widget.checkbox("Enabled", mEnabled);
    dirty |= widget.dropdown("Trace mode", mTraceMode);
    dirty |= widget.dropdown("Quality preset", mQualityPreset);
    dirty |= widget.dropdown("Debug output", mDebugMode);

    if (auto group = widget.group("Features", true))
    {
        dirty |= group.checkbox("Surface cache", mUseSurfaceCache);
        dirty |= group.checkbox("Cache lighting", mUseCacheLighting);
        dirty |= group.checkbox("Screen trace", mUseScreenTrace);
        dirty |= group.checkbox("Screen probes", mUseScreenProbes);
        dirty |= group.checkbox("Temporal filter", mUseTemporalFilter);
        dirty |= group.checkbox("Spatial filter", mUseSpatialFilter);
        dirty |= group.checkbox("Radiance cache", mUseRadianceCache);
    }

    widget.text("Frame index: " + std::to_string(mFrameIndex));
    widget.text("Resolution: " + std::to_string(mFrameDim.x) + " x " + std::to_string(mFrameDim.y));
    widget.text(
        "Counters (NaN/Inf, firefly, negative, traced): " + std::to_string(mCounters.nanInfSamples) + ", " +
        std::to_string(mCounters.fireflySamples) + ", " + std::to_string(mCounters.negativeSamples) + ", " +
        std::to_string(mCounters.tracedSamples)
    );

    if (auto group = widget.group("Surface cache capture", true))
    {
        if (mpCardScene)
        {
            group.text(
                "Cards: " + std::to_string(mpCardScene->getCardCount()) +
                " (dirty: " + std::to_string(mpCardScene->getDirtyCardCount()) + ")"
            );
            group.text(
                "Instances (supported / unsupported): " + std::to_string(mpCardScene->getSupportedInstanceCount()) +
                " / " + std::to_string(mpCardScene->getUnsupportedInstanceCount())
            );
        }

        const LumenSurfaceCacheStats cacheStats = mPageCache.getStats();
        group.text(
            "Pages: " + std::to_string(cacheStats.allocatedPageCount) + " / " + std::to_string(cacheStats.pageCount) +
            " (free " + std::to_string(cacheStats.freePageCount) + ", evict-pending " +
            std::to_string(cacheStats.evictedPendingCount) + ")"
        );
        group.text(
            "Resident: " + std::to_string(cacheStats.residentBytes >> 20) + " MiB / budget " +
            std::to_string(cacheStats.memoryBudgetBytes >> 20) + " MiB (min residency " +
            std::to_string(cacheStats.minResidencyFrames) + " frames)"
        );

        const LumenCaptureSchedulerStats schedulerStats = mCaptureScheduler.getStats();
        group.text(
            "Last frame: commands " + std::to_string(mLastCaptureFrameStats.captureCommands) +
            " (new pages " + std::to_string(mLastCaptureFrameStats.newPageAllocations) +
            ", recaptures " + std::to_string(mLastCaptureFrameStats.recaptureWithPage) +
            ", alloc failures " + std::to_string(mLastCaptureFrameStats.allocationFailures) +
            ", lost " + std::to_string(mLastCaptureFrameStats.lostPages) +
            ", released " + std::to_string(mLastCaptureFrameStats.releasedPages) +
            ", pending " + std::to_string(mLastCaptureFrameStats.pendingCards) + ")"
        );
        group.text(
            "Totals: alloc " + std::to_string(schedulerStats.totalAllocations) +
            ", recapture " + std::to_string(schedulerStats.totalRecaptures) +
            ", fail " + std::to_string(schedulerStats.totalAllocationFailures) +
            ", lost " + std::to_string(schedulerStats.totalLostPages) +
            ", release " + std::to_string(schedulerStats.totalReleases) +
            ", complete " + std::to_string(schedulerStats.completedCaptures) +
            ", rebuilds " + std::to_string(schedulerStats.structuralRebuildCount)
        );
        group.text(
            "Atlas: " + std::to_string(mAtlasSizeTexels) + " texels (" + std::to_string(mCapturePagesPerSide) + " x " +
            std::to_string(mCapturePagesPerSide) + " tiles of " + std::to_string(kLumenSurfaceCacheTileSize) + " texels)"
        );
        group.slider("Capture pages / frame", mCaptureMaxPagesPerFrame, 1u, 256u);
        mCaptureScheduler.setMaxPagesPerFrame(mCaptureMaxPagesPerFrame);
    }

    if (auto group = widget.group("Cache lighting", true))
    {
        group.text(
            "Pages lit (last dispatch): " + std::to_string(mLastCacheLightingPageCount) +
            " (samples/texel: " + std::to_string(cacheLightingSamplesPerTexel()) + ")"
        );
        group.text(
            "Counters (NaN/Inf, firefly, negative, traced): " +
            std::to_string(mCacheLightingCounters.nanInfSamples) + ", " +
            std::to_string(mCacheLightingCounters.fireflySamples) + ", " +
            std::to_string(mCacheLightingCounters.negativeSamples) + ", " +
            std::to_string(mCacheLightingCounters.tracedSamples)
        );
        group.text(
            "Env sampler: " + std::string(mpEnvMapSampler ? "created" : "none") +
            "; emissive sampler: " + std::string(mpEmissiveLightSampler ? "created" : "none")
        );

        // S3-B2 multi-bounce feedback controls. The toggles take effect
        // immediately (like the capture budget slider above): when feedback is
        // turned off the shader self-cleans the double-buffer and the bounce
        // counter, so a later enable restarts from the single bounce.
        group.checkbox("Multi-bounce feedback", mCacheLightingFeedbackEnabled);
        group.slider("Feedback strength", mCacheLightingFeedbackStrength, 0.f, 2.f, false);
        group.slider("Feedback max bounces", mCacheLightingFeedbackMaxBounces, 1u, 32u);
    }

    if (auto group = widget.group("Screen probes", mUseScreenProbes))
    {
        const LumenScreenProbe::Stats& st = mScreenProbeStats;
        group.text("Probes: " + std::to_string(st.probeCount));
        group.slider("Directions / probe", mProbeDirectionsPerProbe, 1u, LumenScreenProbe::kMaxDirectionsPerProbe);
        group.slider("Max probes / frame (0 = all)", mProbeMaxProbesPerFrame, 0u, 65536u);
        const uint32_t interval = LumenScreenProbe::updateInterval(st.probeCount, mProbeMaxProbesPerFrame);
        group.text("Update interval: " + std::to_string(interval) + " frames (expected ~" +
                   std::to_string(LumenScreenProbe::expectedProbesPerFrame(st.probeCount, interval)) + "/frame)");
        group.text(
            "Last frame: screenHits " + std::to_string(st.screenHits) +
            ", fallback (hit/miss/unavail) " + std::to_string(st.fallbackHits) + "/" +
            std::to_string(st.fallbackMisses) + "/" + std::to_string(st.fallbackUnavailable) +
            ", traced " + std::to_string(st.directionsTraced) +
            " (screen hit rate " + std::to_string(st.screenHitRate()) + ")"
        );
        group.text(
            "Inactive " + std::to_string(st.inactiveProbes) + ", budget-skipped " +
            std::to_string(st.budgetSkipped)
        );
    }

    if (auto group = widget.group("Spatial filter", mUseSpatialFilter))
    {
        group.checkbox("Firefly clamp", mSpatialFireflyClamp);
        group.slider("Radius min (px)", mSpatialRadiusMin, 0.f, 4.f, false);
        group.slider("Radius max (px)", mSpatialRadiusMax, 0.f, 4.f, false);
        group.slider("Variance threshold low", mSpatialVarianceThresholdLow, 0.f, 0.5f, false);
        group.slider("Variance threshold high", mSpatialVarianceThresholdHigh, 0.f, 1.f, false);
        group.slider("Neighborhood radius (1 = 3x3, 2 = 5x5)", mSpatialNeighborhoodRadius, 1u, 2u);
        group.slider("Temporal variance weight", mSpatialTemporalVarianceWeight, 0.f, 2.f, false);
    }

    if (dirty)
    {
        mOptionsChanged = true;
        resetHistory();
    }
}

void LumenGIPass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mUpdateFlagsConnection = {};
    mSceneUpdates = IScene::UpdateFlags::None;
    mpScene = pScene;
    mTracer.pProgram = nullptr;
    mTracer.pBindingTable = nullptr;
    mTracer.pVars = nullptr;
    mLightCollectionInitialized = false;
    mpEmissiveLightSampler = nullptr;
    mpEnvMapSampler = nullptr;
    mCacheLightingCounters.reset();
    mCacheLightingCounterReadbackPending = false;
    mLastCacheLightingPageCount = 0;
    // S3-B2: reset the feedback history on scene change. The indirect double buffer and the
    // bounce counter are atlas-lifetime; clearing them here (plus the capture pass zeroing the
    // radiance atlas A channel on every re-capture) guarantees a fresh single-bounce start.
    mCacheLighting.indirectCurrIndex = 0;
    // S4.2: the probe passes carry the scene defines/type conformances (scene-mode raytracing)
    // and must be recreated on scene/geometry rebuilds; the probe buffers are frame-scoped and
    // deliberately kept (their contents are rebuilt every frame).
    mScreenProbes.pUpdate = nullptr;
    mScreenProbes.pTrace = nullptr;
    mScreenProbes.pFinalize = nullptr;
    mScreenProbes.pIntegrate = nullptr;
    mScreenProbes.pInterpolate = nullptr;
    // S6: the Mesh SDF scene + GDF are scene-scoped; drop everything and re-materialize lazily
    // from the disk cache on the next execute (a cache HIT after reload, never a rebuild).
    invalidateMeshSDF();
    if (pRenderContext)
    {
        for (const ref<Texture>& pIndirect : mCacheLighting.pIndirect)
        {
            if (pIndirect)
                pRenderContext->clearUAV(pIndirect->getUAV().get(), float4(0.f));
        }
        if (mCacheLighting.pBounceCount)
            pRenderContext->clearUAV(mCacheLighting.pBounceCount->getUAV().get(), uint4(0));
    }
    resetHistory();

    // S2: rebuild the card scene and reset the CPU components. invalidateCaptureResources()
    // drops every GPU resource that references the old scene's mesh buffers (atlases persist:
    // they are fixed-size and their contents are re-captured). The scheduler releases all
    // pages it holds; the cache reset afterwards is a no-op for those pages, so both orders
    // are safe (the scheduler contract allows either).
    invalidateCaptureResources();
    mpCardScene = pScene ? std::make_unique<LumenCardScene>(pScene) : nullptr;
    mPageCache.reset();
    mCaptureScheduler = LumenCaptureSchedulerForScene(
        mpCardScene.get(), &mPageCache, mCaptureMaxPagesPerFrame, kLumenCaptureDefaultInFlightTimeoutFrames
    );

    if (mpScene)
    {
        mUpdateFlagsConnection = mpScene->getUpdateFlagsSignal().connect([&](IScene::UpdateFlags flags) { mSceneUpdates |= flags; });
        createTraceProgram();
    }
}

void LumenGIPass::onHotReload(HotReloadFlags reloaded)
{
    if (is_set(reloaded, HotReloadFlags::Program))
    {
        mpDebugPass = nullptr;
        mTracer.pProgram = nullptr;
        mTracer.pBindingTable = nullptr;
        mTracer.pVars = nullptr;
        invalidateCaptureResources();
        mScreenTrace.pHZBBuild = nullptr;
        mScreenTrace.pTrace = nullptr;
        mScreenProbes.pUpdate = nullptr;
        mScreenProbes.pTrace = nullptr;
        mScreenProbes.pFinalize = nullptr;
    mScreenProbes.pIntegrate = nullptr;
    mScreenProbes.pInterpolate = nullptr;
    mTemporalFilter.pFilter = nullptr; // pure compute, no scene deps; recreated lazily.
    mSpatialFilter.pFilter = nullptr;  // pure compute, no scene deps; recreated lazily.
    mSDF.pCompose = nullptr;           // S6: pure compute, no scene deps; recreated lazily.
    mSDF.pTrace = nullptr;
    resetHistory();
    }
}

void LumenGIPass::resetHistory()
{
    mFrameIndex = 0;
    // S5-A1: mark the prev history/depth double buffer for a hard clear (camera cut / resize /
    // scene change). The actual clear is emitted inside runTemporalFilter (it needs a
    // RenderContext, and setScene/onHotReload call this before the buffers exist). Clearing the
    // prev buffers makes every pixel take the disocclusion path for one frame (prev depth 0 =>
    // validation weight 0), which is the "history immediately invalid after a cut" gate.
    mTemporalFilter.historyResetPending = true;
}

void LumenGIPass::clearOutputs(RenderContext* pRenderContext, const RenderData& renderData) const
{
    for (const auto& channel : kOutputChannels)
    {
        if (const auto& pTexture = renderData.getTexture(channel.name))
            pRenderContext->clearUAV(pTexture->getUAV().get(), float4(0.f));
    }
}

void LumenGIPass::createDebugPass(const DefineList& defines)
{
    mpDebugPass = ComputePass::create(mpDevice, kShaderFile, "main", defines);
}

void LumenGIPass::createTraceProgram()
{
    mTracer.pProgram = nullptr;
    mTracer.pBindingTable = nullptr;
    mTracer.pVars = nullptr;
    if (!mpScene)
        return;

    ProgramDesc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kTraceShaderFile);
    desc.setMaxPayloadSize(kTracePayloadSizeBytes);
    desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(kTraceRecursionDepth);

    mTracer.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
    auto& sbt = mTracer.pBindingTable;
    sbt->setRayGen(desc.addRayGen("lumenGIRayGen"));
    sbt->setMiss(0, desc.addMiss("lumenIndirectMiss"));

    if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh),
            desc.addHitGroup("lumenTriangleClosestHit", "lumenTriangleAnyHit")
        );
    }
    if (mpScene->hasGeometryType(Scene::GeometryType::DisplacedTriangleMesh))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
            desc.addHitGroup("lumenDisplacedTriangleClosestHit", "lumenDisplacedTriangleAnyHit", "lumenDisplacedTriangleIntersection")
        );
    }
    if (mpScene->hasGeometryType(Scene::GeometryType::Curve))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::Curve),
            desc.addHitGroup("lumenCurveClosestHit", "", "lumenCurveIntersection")
        );
    }
    if (mpScene->hasGeometryType(Scene::GeometryType::SDFGrid))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid),
            desc.addHitGroup("lumenSdfGridClosestHit", "", "lumenSdfGridIntersection")
        );
    }

    mTracer.pProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
}

void LumenGIPass::prepareTraceVars()
{
    FALCOR_ASSERT(mpScene && mTracer.pProgram && mTracer.pBindingTable);
    mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());
    mTracer.pVars = RtProgramVars::create(mpDevice, mTracer.pProgram, mTracer.pBindingTable);
    mpSampleGenerator->bindShaderData(mTracer.pVars->getRootVar());
}

void LumenGIPass::ensureTraceResources()
{
    // One uint4 per LumenGICounterIndex entry (NaN/Inf, firefly, negative,
    // traced rays). The shader defines the same layout in LumenGIData.slang.
    const uint32_t kCounterCount = 4u;
    if (!mpLumenGICounters)
    {
        mpLumenGICounters = mpDevice->createStructuredBuffer(
            sizeof(uint4), kCounterCount, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpLumenGICounters->setName("LumenGIPass::Counters");
    }
    if (!mpLumenGICountersReadback)
    {
        mpLumenGICountersReadback = mpDevice->createStructuredBuffer(
            sizeof(uint4), kCounterCount, ResourceBindFlags::None, MemoryType::ReadBack
        );
        mpLumenGICountersReadback->setName("LumenGIPass::CountersReadback");
    }

    if (!mpLightingComponents && any(mFrameDim > 0u))
    {
        mpLightingComponents = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpLightingComponents->setName("LumenGIPass::LightingComponents");
    }
}

void LumenGIPass::readbackCounters(RenderContext* pRenderContext)
{
    if (!mCounterReadbackPending || !mpLumenGICountersReadback)
        return;

    const uint4* pCounters = static_cast<const uint4*>(mpLumenGICountersReadback->map());
    if (pCounters)
    {
        mCounters.nanInfSamples = pCounters[0].x;
        mCounters.fireflySamples = pCounters[1].x;
        mCounters.negativeSamples = pCounters[2].x;
        mCounters.tracedSamples = pCounters[3].x;
        mpLumenGICountersReadback->unmap();
    }
    mCounterReadbackPending = false;
}

// ------------------------------------------------------------------------------------------
// S2: Surface Cache / Cards capture host
// ------------------------------------------------------------------------------------------

std::map<std::string, double> LumenGIPass::getSurfaceCacheStats() const
{
    std::map<std::string, double> stats;
    stats["frameIndex"] = (double)mFrameIndex;
    stats["useSurfaceCache"] = mUseSurfaceCache ? 1.0 : 0.0;
    stats["maxPagesPerFrame"] = (double)mCaptureMaxPagesPerFrame;
    stats["atlasSizeTexels"] = (double)mAtlasSizeTexels;
    stats["pagesPerSide"] = (double)mCapturePagesPerSide;

    if (mpCardScene)
    {
        stats["cards"] = (double)mpCardScene->getCardCount();
        stats["dirtyCards"] = (double)mpCardScene->getDirtyCardCount();
        stats["supportedInstances"] = (double)mpCardScene->getSupportedInstanceCount();
        stats["unsupportedInstances"] = (double)mpCardScene->getUnsupportedInstanceCount();
    }

    const LumenSurfaceCacheStats cacheStats = mPageCache.getStats();
    stats["totalPages"] = (double)cacheStats.pageCount;
    stats["allocatedPages"] = (double)cacheStats.allocatedPageCount;
    stats["freePages"] = (double)cacheStats.freePageCount;
    stats["evictedPendingPages"] = (double)cacheStats.evictedPendingCount;
    stats["coverage"] = cacheStats.pageCount > 0 ? (double)cacheStats.allocatedPageCount / (double)cacheStats.pageCount : 0.0;
    stats["residentBytesMB"] = (double)(cacheStats.residentBytes >> 20);
    stats["allocations"] = (double)cacheStats.allocationCount;
    stats["releases"] = (double)cacheStats.releaseCount;
    stats["evictions"] = (double)cacheStats.evictionCount;
    stats["invalidations"] = (double)cacheStats.invalidationCount;

    const LumenCaptureSchedulerStats schedulerStats = mCaptureScheduler.getStats();
    stats["schedulerFrameIndex"] = (double)schedulerStats.frameIndex;
    stats["schedCaptureCommands"] = (double)schedulerStats.totalCaptureCommands;
    stats["schedAllocations"] = (double)schedulerStats.totalAllocations;
    stats["schedRecaptures"] = (double)schedulerStats.totalRecaptures;
    stats["schedAllocFailures"] = (double)schedulerStats.totalAllocationFailures;
    stats["schedStarvationFrames"] = (double)schedulerStats.totalStarvationFrames;
    stats["schedReleases"] = (double)schedulerStats.totalReleases;
    stats["schedLostPages"] = (double)schedulerStats.totalLostPages;
    stats["schedTouches"] = (double)schedulerStats.totalTouches;
    stats["schedCompletedCaptures"] = (double)schedulerStats.completedCaptures;
    stats["avgQueuedFrames"] = schedulerStats.averageQueuedFrames;
    stats["maxQueuedFrames"] = (double)schedulerStats.maxQueuedFrames;
    stats["pendingQueueDepth"] = (double)schedulerStats.pendingQueueDepth;
    stats["maxPendingDepth"] = (double)schedulerStats.maxPendingDepth;
    stats["schedStructuralRebuilds"] = (double)schedulerStats.structuralRebuildCount;

    const LumenCaptureFrameStats& last = mLastCaptureFrameStats;
    stats["lastRequestedCards"] = (double)last.requestedCards;
    stats["lastCaptureCommands"] = (double)last.captureCommands;
    stats["lastNewPageAllocations"] = (double)last.newPageAllocations;
    stats["lastRecaptureWithPage"] = (double)last.recaptureWithPage;
    stats["lastAllocFailures"] = (double)last.allocationFailures;
    stats["lastBudgetCappedCards"] = (double)last.budgetCappedCards;
    stats["lastInFlightCards"] = (double)last.inFlightCards;
    stats["lastPendingCards"] = (double)last.pendingCards;
    stats["lastStarvationFrames"] = (double)last.starvationFrames;
    stats["lastReleasedPages"] = (double)last.releasedPages;
    stats["lastLostPages"] = (double)last.lostPages;
    stats["lastTouchCalls"] = (double)last.touchCalls;

    // S3: Surface Cache lighting state and counters. cacheLightingPagesLit is the dispatch
    // size of the last run; the counters are the last completed dispatch's readback.
    stats["useCacheLighting"] = mUseCacheLighting ? 1.0 : 0.0;
    stats["cacheLightingActive"] = (mUseCacheLighting && mUseSurfaceCache) ? 1.0 : 0.0;
    stats["cacheLightingPagesLit"] = (double)mLastCacheLightingPageCount;
    stats["cacheLightingSamplesPerTexel"] = (double)cacheLightingSamplesPerTexel();
    stats["cacheLightingCounterNanInf"] = (double)mCacheLightingCounters.nanInfSamples;
    stats["cacheLightingCounterFirefly"] = (double)mCacheLightingCounters.fireflySamples;
    stats["cacheLightingCounterNegative"] = (double)mCacheLightingCounters.negativeSamples;
    stats["cacheLightingCounterTraced"] = (double)mCacheLightingCounters.tracedSamples;

    return stats;
}

void LumenGIPass::invalidateCaptureResources()
{
    // Drop every capture resource that is scene-scoped or program-scoped. The atlas textures
    // are deliberately kept: they are fixed-size and their pages are re-captured on demand.
    mCapture.pProgram = nullptr;
    mCapture.pVars = nullptr;
    mCapture.pState = nullptr;
    mCapture.pVao32 = nullptr;
    mCapture.pVao16 = nullptr;
    mCapture.pInstanceIDs = nullptr;
    mCapture.pCards = nullptr;
    mCapture.pPageTable = nullptr;
    mCapture.pDrawArgs = nullptr;
    mCardPageTable.clear();
    mCardPageGeneration.clear();
    // S3: the cache lighting program carries the scene defines/type conformances and must be
    // recreated on scene/geometry rebuilds. The pageToCard/renderList buffers and the visibility
    // atlas are atlas-lifetime (fixed size) and are deliberately kept; their contents are rebuilt
    // every frame.
    mCacheLighting.pPass = nullptr;
}

void LumenGIPass::ensureCaptureResources(RenderContext* pRenderContext)
{
    if (!mpScene || !mpCardScene)
        return;

    // Atlas textures. Fixed size (mAtlasSizeTexels per side), UAV + SRV, cleared once at
    // creation: material/radiance zero (opacity 0 = "not captured", radiance 0), metadata
    // zero (flags 0 = invalid). Captured pages are overwritten by the shader.
    if (!mCapture.pMaterialAtlas)
    {
        mCapture.pMaterialAtlas = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::RGBA8Unorm, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCapture.pMaterialAtlas->setName("LumenGIPass::Capture::MaterialAtlas"); // RGBA8, 4 B/texel, atlas lifetime.
        pRenderContext->clearUAV(mCapture.pMaterialAtlas->getUAV().get(), float4(0.f));
    }
    if (!mCapture.pRadianceAtlas)
    {
        mCapture.pRadianceAtlas = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCapture.pRadianceAtlas->setName("LumenGIPass::Capture::RadianceAtlas"); // RGBA16F, 8 B/texel, atlas lifetime.
        pRenderContext->clearUAV(mCapture.pRadianceAtlas->getUAV().get(), float4(0.f));
    }
    if (!mCapture.pMetadataAtlas)
    {
        mCapture.pMetadataAtlas = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCapture.pMetadataAtlas->setName("LumenGIPass::Capture::MetadataAtlas"); // RGBA16F, 8 B/texel, atlas lifetime.
        pRenderContext->clearUAV(mCapture.pMetadataAtlas->getUAV().get(), float4(0.f));
    }

    const uint32_t cardCount = mpCardScene->getCardCount();

    // gCards: StructuredBuffer<LumenCard>, 96 B/card, full upload every capture frame. The
    // buffer (and the host page-table mirror) is recreated when the card count changes
    // (geometry rebuild), which also resets the page table to invalid.
    if (cardCount > 0 && (!mCapture.pCards || mCapture.pCards->getElementCount() != cardCount))
    {
        mCapture.pCards = mpDevice->createStructuredBuffer(sizeof(LumenCard), cardCount, ResourceBindFlags::ShaderResource);
        mCapture.pCards->setName("LumenGIPass::Capture::Cards"); // StructuredBuffer<LumenCard>, 96 B stride, scene-scoped.
        mCardPageTable.assign(cardCount, kLumenCardInvalidID);
        mCardPageGeneration.assign(cardCount, 0u);
        mCapture.pPageTable = mpDevice->createStructuredBuffer(sizeof(uint32_t), cardCount, ResourceBindFlags::ShaderResource);
        mCapture.pPageTable->setName("LumenGIPass::Capture::PageTable"); // cardIndex -> pageID, uint32, scene-scoped.
    }

    // Indirect draw argument blob: one 20-byte argument per command. Capacity = the per-frame
    // capture budget (the scheduler never emits more commands than the budget); recreated
    // when the budget grows past the current capacity (the UI slider can raise it at runtime).
    const uint32_t maxCommands = std::max<uint32_t>(1u, mCaptureMaxPagesPerFrame);
    if (!mCapture.pDrawArgs || mCapture.pDrawArgs->getSize() < (size_t)maxCommands * kCaptureDrawArgBytes)
    {
        mCapture.pDrawArgs = mpDevice->createBuffer(
            (size_t)maxCommands * kCaptureDrawArgBytes, ResourceBindFlags::IndirectArg, MemoryType::DeviceLocal, nullptr
        );
        mCapture.pDrawArgs->setName("LumenGIPass::Capture::DrawArgs"); // DrawIndexedArguments/DrawArguments blob, scene-scoped.
    }

    if (!mCapture.pProgram)
        createCaptureProgram();
}

void LumenGIPass::runSurfaceCacheCapture(RenderContext* pRenderContext, IScene::UpdateFlags updateFlags)
{
    if (!mpScene || !mpCardScene)
        return;

    // Keep the scheduler budget in sync with the property every frame so the
    // captureMaxPagesPerFrame property hot-switch works in headless runs
    // (the UI slider path only runs when the GUI is active). Idempotent.
    mCaptureScheduler.setMaxPagesPerFrame(mCaptureMaxPagesPerFrame);

    // Structural scene changes invalidate the capture program/vars/VAOs and the card buffer
    // (scene defines, mesh buffers and card indices may all have changed).
    if (is_set(updateFlags, IScene::UpdateFlags::GeometryChanged) ||
        is_set(updateFlags, IScene::UpdateFlags::MeshesChanged) ||
        is_set(updateFlags, IScene::UpdateFlags::RecompileNeeded))
    {
        invalidateCaptureResources();
    }

    // Per-frame contract with the scheduler (Agent H): update the card scene with the same
    // flags that scheduleFrame() receives, run the emitted commands through the capture pass,
    // then complete them. endFrame() of the page cache happens inside scheduleFrame().
    mpCardScene->update(pRenderContext, updateFlags);
    ensureCaptureResources(pRenderContext);
    if (!mCapture.pProgram)
        return;

    const LumenCaptureFrame frame = mCaptureScheduler.scheduleFrame(updateFlags);
    mLastCaptureFrameStats = frame.stats;

    // Full cards upload: the capture shader indexes gCards by gCardIndex, so every card must
    // be current. (A dirty-range upload is future work; 96 B x card count is small.)
    if (mCapture.pCards && mpCardScene->getCardCount() > 0)
    {
        const uint32_t count = mpCardScene->getCardCount();
        std::vector<LumenCard> cards;
        cards.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            cards.push_back(mpCardScene->getCard(i));
        }
        mCapture.pCards->setBlob(cards.data(), 0, cards.size() * sizeof(LumenCard));
    }

    // Page table upload: refresh the entries touched by this frame's commands. Entries go
    // stale only transiently (an evicted page is re-allocated and re-emitted by the
    // scheduler, which rewrites the entry; S3 consumers must check generation, future work).
    // mCardPageGeneration records the page generation each card received at its last command,
    // so S3 can resolve the current page owner when it rebuilds the page->card table.
    if (!frame.commands.empty())
    {
        for (const LumenCaptureCommand& cmd : frame.commands)
        {
            if (cmd.cardIndex < mCardPageTable.size())
            {
                mCardPageTable[cmd.cardIndex] = cmd.pageID;
                if (cmd.cardIndex < mCardPageGeneration.size())
                    mCardPageGeneration[cmd.cardIndex] = cmd.generation;
            }
        }
    }
    if (mCapture.pPageTable && !mCardPageTable.empty())
    {
        mCapture.pPageTable->setBlob(mCardPageTable.data(), 0, mCardPageTable.size() * sizeof(uint32_t));
    }

    if (!frame.commands.empty())
        runCapturePass(pRenderContext, frame);

    mCaptureScheduler.completeCaptures(frame.commands);
}

void LumenGIPass::runCapturePass(RenderContext* pRenderContext, const LumenCaptureFrame& frame)
{
    FALCOR_ASSERT(mCapture.pState && mCapture.pVars && mCapture.pCards && mCapture.pDrawArgs);
    const uint32_t commandCount = (uint32_t)frame.commands.size();
    if (commandCount == 0)
        return;

    // Per-command indirect draw arguments, one 20-byte slot per command (see
    // kCaptureDrawArgBytes). Indexed meshes use DrawIndexedArguments (IndexCount,
    // InstanceCount, StartIndex, BaseVertex, StartInstance); non-indexed meshes use
    // DrawArguments (VertexCount, InstanceCount, StartVertex, StartInstance) - the 5th word
    // of the slot is ignored by drawIndirect.
    std::vector<uint32_t> args(commandCount * 5u);
    for (uint32_t i = 0; i < commandCount; ++i)
    {
        const LumenCaptureCommand& cmd = frame.commands[i];
        const LumenCard& card = mpCardScene->getCard(cmd.cardIndex);
        const MeshDesc& mesh = mpScene->getMesh(MeshID::fromSlang(card.meshID));
        const bool use16Bit = mesh.use16BitIndices();
        const bool indexed = mesh.indexCount > 0;
        args[i * 5u + 0u] = indexed ? mesh.indexCount : mesh.vertexCount;
        args[i * 5u + 1u] = 1u; // InstanceCount.
        args[i * 5u + 2u] = indexed ? (mesh.ibOffset * (use16Bit ? 2u : 1u)) : mesh.vbOffset;
        args[i * 5u + 3u] = indexed ? mesh.vbOffset : 0u;
        args[i * 5u + 4u] = card.instanceID; // StartInstanceLocation; the per-instance buffer is the identity map.
    }
    mCapture.pDrawArgs->setBlob(args.data(), 0, args.size() * sizeof(uint32_t));

    auto var = mCapture.pVars->getRootVar();
    var["gCards"] = mCapture.pCards;
    var["gMaterialAtlas"] = mCapture.pMaterialAtlas;
    var["gRadianceAtlas"] = mCapture.pRadianceAtlas;
    var["gMetadataAtlas"] = mCapture.pMetadataAtlas;

    for (uint32_t i = 0; i < commandCount; ++i)
    {
        const LumenCaptureCommand& cmd = frame.commands[i];
        const LumenCard& card = mpCardScene->getCard(cmd.cardIndex);
        const MeshDesc& mesh = mpScene->getMesh(MeshID::fromSlang(card.meshID));
        const bool use16Bit = mesh.use16BitIndices();

        // Viewport (and scissor) = the page texel region of the atlases, so SV_Position.xy
        // inside the pixel shader is directly the atlas coordinate (Agent C contract).
        const LumenSurfaceCacheCoord coord = mPageCache.getPageAtlasCoord(cmd.pageID);
        const uint32_t originX = coord.atlasX * kLumenSurfaceCacheTileSize;
        const uint32_t originY = coord.atlasY * kLumenSurfaceCacheTileSize;
        mCapture.pState->setViewport(
            0,
            GraphicsState::Viewport(
                (float)originX, (float)originY, (float)kLumenSurfaceCacheTileSize,
                (float)kLumenSurfaceCacheTileSize, 0.f, 1.f
            ),
            true
        );
        mCapture.pState->setVao(use16Bit ? mCapture.pVao16 : mCapture.pVao32);

        // Per-draw capture parameters (LumenCardCaptureCB, field layout in
        // LumenCardCaptureData.slang). gPageTexelOrigin mirrors the Agent B formula and is
        // used by the shader for debug writes only.
        var["LumenCardCaptureCB"]["gCardIndex"] = cmd.cardIndex;
        var["LumenCardCaptureCB"]["gPageID"] = cmd.pageID;
        var["LumenCardCaptureCB"]["gPagesPerSide"] = mCapturePagesPerSide;
        var["LumenCardCaptureCB"]["gPageTexelOrigin"] = uint2(originX, originY);
        var["LumenCardCaptureCB"]["gNearMargin"] = kCaptureNearMargin;

        const uint64_t argOffset = (uint64_t)i * kCaptureDrawArgBytes;
        if (mesh.indexCount > 0)
            pRenderContext->drawIndexedIndirect(mCapture.pState.get(), mCapture.pVars.get(), 1, mCapture.pDrawArgs.get(), argOffset, nullptr, 0);
        else
            pRenderContext->drawIndirect(mCapture.pState.get(), mCapture.pVars.get(), 1, mCapture.pDrawArgs.get(), argOffset, nullptr, 0);
    }
}

void LumenGIPass::createCaptureProgram()
{
    FALCOR_ASSERT(mpScene && mpCardScene);

    // Raster program: material system modules + the capture shader. The scene defines and
    // type conformances come from the scene; they may change on geometry rebuilds, which is
    // why this is recreated through invalidateCaptureResources() in that case.
    ProgramDesc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kCaptureShaderFile).vsEntry("vsMain").psEntry("psMain");
    desc.addTypeConformances(mpScene->getTypeConformances());
    mCapture.pProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
    // Optional resource defines (the shader defaults the rest: gCards/gMaterialAtlas/
    // gMetadataAtlas are required and default to enabled). The params/debug atlases are not
    // created in the MVP.
    mCapture.pProgram->addDefine("is_valid_gRadianceAtlas", "1");
    mCapture.pProgram->addDefine("is_valid_gMaterialParamsAtlas", "0");
    mCapture.pProgram->addDefine("is_valid_gDebugTexture", "0");

    mCapture.pVars = ProgramVars::create(mpDevice, mCapture.pProgram.get());
    mpScene->bindShaderData(mCapture.pVars->getRootVar()["gScene"]);

    // FBO-less raster state: all capture outputs are UAVs (RasterPass precedent). Backface
    // rejection is done in the pixel shader (single-sided materials), so no culling here.
    mCapture.pState = GraphicsState::create(mpDevice);
    mCapture.pState->setProgram(mCapture.pProgram);
    mCapture.pState->setRasterizerState(
        RasterizerState::create(RasterizerState::Desc().setCullMode(RasterizerState::CullMode::None))
    );

    // Mesh VAOs. The scene's own VAO cannot be reused directly: its per-instance buffer
    // stores draw-list indices, which only equal the scene geometry instance ID for scenes
    // without displaced meshes/curves. Instead we bind a per-instance identity buffer
    // (element i == i) and draw with StartInstanceLocation = scene instance ID, so the
    // shader's DRAW_ID semantic (= gScene instance index, Agent C contract) is always the
    // true geometry instance ID. The vertex layout mirrors the scene's (PackedStaticVertexData
    // in buffer 0, per-instance ID in buffer 1), and the shared global index buffer supports
    // both 16- and 32-bit index formats via two VAOs.
    const ref<Vao>& pSceneVao = mpScene->getMeshVao();
    if (pSceneVao && pSceneVao->getVertexBuffersCount() >= 2u)
    {
        const uint32_t instanceCount = std::max<uint32_t>(1u, mpScene->getGeometryInstanceCount());
        std::vector<uint32_t> identity(instanceCount);
        for (uint32_t i = 0; i < instanceCount; ++i)
        {
            identity[i] = i;
        }
        mCapture.pInstanceIDs = mpDevice->createBuffer(
            instanceCount * sizeof(uint32_t), ResourceBindFlags::Vertex, MemoryType::DeviceLocal, identity.data()
        );
        mCapture.pInstanceIDs->setName("LumenGIPass::Capture::InstanceIDs"); // R32Uint identity map, scene-scoped.

        ref<VertexLayout> pLayout = VertexLayout::create();
        ref<VertexBufferLayout> pStaticLayout = VertexBufferLayout::create();
        pStaticLayout->addElement(
            VERTEX_POSITION_NAME, offsetof(PackedStaticVertexData, position), ResourceFormat::RGB32Float, 1, VERTEX_POSITION_LOC
        );
        pStaticLayout->addElement(
            VERTEX_PACKED_NORMAL_TANGENT_CURVE_RADIUS_NAME,
            offsetof(PackedStaticVertexData, packedNormalTangentCurveRadius),
            ResourceFormat::RGB32Float, 1, VERTEX_PACKED_NORMAL_TANGENT_CURVE_RADIUS_LOC
        );
        pStaticLayout->addElement(
            VERTEX_TEXCOORD_NAME, offsetof(PackedStaticVertexData, texCrd), ResourceFormat::RG32Float, 1, VERTEX_TEXCOORD_LOC
        );
        pLayout->addBufferLayout(0, pStaticLayout);
        ref<VertexBufferLayout> pInstanceLayout = VertexBufferLayout::create();
        pInstanceLayout->addElement(INSTANCE_DRAW_ID_NAME, 0, ResourceFormat::R32Uint, 1, INSTANCE_DRAW_ID_LOC);
        pInstanceLayout->setInputClass(VertexBufferLayout::InputClass::PerInstanceData, 1);
        pLayout->addBufferLayout(1, pInstanceLayout);

        Vao::BufferVec pVBs(2);
        pVBs[0] = pSceneVao->getVertexBuffer(0);
        pVBs[1] = mCapture.pInstanceIDs;
        const ref<Buffer>& pSceneIB = pSceneVao->getIndexBuffer();
        mCapture.pVao32 = Vao::create(Vao::Topology::TriangleList, pLayout, pVBs, pSceneIB, ResourceFormat::R32Uint);
        mCapture.pVao16 = Vao::create(Vao::Topology::TriangleList, pLayout, pVBs, pSceneIB, ResourceFormat::R16Uint);
        // Scene mesh buffers (shared static VB + global IB), 32/16-bit index formats, scene-scoped.
    }
    else
    {
        logWarning("LumenGI: scene mesh VAO unavailable; card capture will be skipped.");
    }
}

// ------------------------------------------------------------------------------------------
// S3: Surface Cache direct lighting host (S3-B1)
// ------------------------------------------------------------------------------------------

uint32_t LumenGIPass::cacheLightingSamplesPerTexel() const
{
    // LumenCacheLightingQuality mapping (LumenSurfaceCacheLightingData.slang):
    // Low/Medium/High/Reference -> 1/2/4/8 NEE draws per texel per technique.
    switch (mQualityPreset)
    {
    case QualityPreset::Low:
        return 1u;
    case QualityPreset::Medium:
        return 2u;
    case QualityPreset::High:
        return 4u;
    case QualityPreset::Reference:
        return 8u;
    default:
        return 4u;
    }
}

uint32_t LumenGIPass::buildCacheLightingRenderData()
{
    // Rebuild pageID -> cardIndex from the host card->page mirror (maintained from the
    // scheduler command stream in runSurfaceCacheCapture) plus the page cache residency and
    // the per-card page generation. A page maps to a card only when that card's recorded page
    // generation equals the page cache's CURRENT generation for the page: after an eviction
    // and reallocation the new owner's generation matches while every stale card->page entry
    // has a mismatched (older) generation and is skipped, so gLumenPageToCard never points at
    // a card that does not own the page. The render list is then every resident page with a
    // valid owner (render-list mode; full-atlas mode would dispatch pagesPerSide^2
    // threadgroups and light empty slots -- S3-A1's prioritized scheduling can trim this).
    const uint32_t pageCount = mPageCache.getPageCount();
    if (mPageToCardData.size() != pageCount + 1)
        mPageToCardData.assign(pageCount + 1, kLumenCardInvalidID);
    std::fill(mPageToCardData.begin(), mPageToCardData.end(), kLumenCardInvalidID);

    const uint32_t cardCount = mpCardScene ? mpCardScene->getCardCount() : 0u;
    if (mCardPageTable.size() != cardCount || mCardPageGeneration.size() != cardCount)
        return 0u; // capture resources not initialized yet (no cards captured this frame).

    for (uint32_t card = 0; card < cardCount; ++card)
    {
        const uint32_t pageID = mCardPageTable[card];
        if (pageID == kInvalidPageID || pageID > pageCount)
            continue;
        if (!mPageCache.isPageAllocated(pageID))
            continue;
        if (mCardPageGeneration[card] != mPageCache.getGeneration(pageID))
            continue;
        mPageToCardData[pageID] = card;
    }

    mRenderListData.clear();
    mRenderListData.reserve(pageCount);
    for (uint32_t pageID = 1; pageID <= pageCount; ++pageID)
    {
        if (mPageToCardData[pageID] != kLumenCardInvalidID)
            mRenderListData.push_back(pageID);
    }
    // D3D12 dispatch dimension cap: the X dimension of Dispatch is limited to 65535 thread
    // groups. Only reachable when the whole 65536-page atlas becomes resident (~256 MiB),
    // which the default per-frame budget cannot produce; clamp defensively.
    constexpr uint32_t kMaxDispatchX = 65535u;
    if (mRenderListData.size() > kMaxDispatchX)
    {
        logWarning("LumenGI: cache lighting render list clamped from {} to {} pages.", mRenderListData.size(), kMaxDispatchX);
        mRenderListData.resize(kMaxDispatchX);
    }
    return (uint32_t)mRenderListData.size();
}

void LumenGIPass::createCacheLightingProgram()
{
    FALCOR_ASSERT(mpScene);

    // Compute program: the lighting shader imports Scene.Scene / Scene.RaytracingInline for the
    // scene block and inline shadow ray queries, but never evaluates materials, so it needs the
    // scene's type conformances and defines (not the capture's VAO machinery). Mirrors the
    // GBufferRTInline compute-pass pattern.
    ProgramDesc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kCacheLightingShaderFile).csEntry("main");
    desc.addTypeConformances(mpScene->getTypeConformances());
    DefineList defines;
    defines.add(mpScene->getSceneDefines());
    defines.add(mpSampleGenerator->getDefines()); // SAMPLE_GENERATOR_TYPE selects the shader RNG type.
    mCacheLighting.pPass = ComputePass::create(mpDevice, desc, defines, /*createVars=*/true);
}

void LumenGIPass::ensureCacheLightingResources(RenderContext* pRenderContext)
{
    // Visibility/confidence atlas: R16F, atlas-lifetime (kept across scene changes like the
    // capture atlases; contents are re-written every dispatch).
    if (!mCacheLighting.pVisibilityAtlas)
    {
        mCacheLighting.pVisibilityAtlas = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::R16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCacheLighting.pVisibilityAtlas->setName("LumenGIPass::CacheLighting::VisibilityAtlas"); // R16F, atlas lifetime.
        pRenderContext->clearUAV(mCacheLighting.pVisibilityAtlas->getUAV().get(), float4(0.f));
    }

    // gLumenPageToCard: one uint per page ID (index 0 unused; the shader guards pageID == 0).
    // gLumenRenderList: capacity == page count; only the first renderPageCount entries are used.
    const uint32_t pageCount = mPageCache.getPageCount();
    if (!mCacheLighting.pPageToCard || mCacheLighting.pPageToCard->getElementCount() != pageCount + 1)
    {
        mCacheLighting.pPageToCard = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), pageCount + 1, ResourceBindFlags::ShaderResource
        );
        mCacheLighting.pPageToCard->setName("LumenGIPass::CacheLighting::PageToCard"); // gLumenPageToCard, uint32, atlas lifetime.
    }
    if (!mCacheLighting.pRenderList || mCacheLighting.pRenderList->getElementCount() != pageCount)
    {
        mCacheLighting.pRenderList = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), pageCount, ResourceBindFlags::ShaderResource
        );
        mCacheLighting.pRenderList->setName("LumenGIPass::CacheLighting::RenderList"); // gLumenRenderList, uint32, atlas lifetime.
    }

    // Independent cache-lighting counters (see the header comment: separate from the trace
    // counters so the S1/S2 counter statistics stay unchanged). LumenGICounterIndex layout.
    constexpr uint32_t kCounterCount = 4u;
    if (!mpCacheLightingCounters)
    {
        mpCacheLightingCounters = mpDevice->createStructuredBuffer(
            sizeof(uint4), kCounterCount, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpCacheLightingCounters->setName("LumenGIPass::CacheLighting::Counters");
    }
    if (!mpCacheLightingCountersReadback)
    {
        mpCacheLightingCountersReadback = mpDevice->createStructuredBuffer(
            sizeof(uint4), kCounterCount, ResourceBindFlags::None, MemoryType::ReadBack
        );
        mpCacheLightingCountersReadback->setName("LumenGIPass::CacheLighting::CountersReadback");
    }

    // S3-B2 multi-bounce feedback resources: two indirect atlases (RGBA16F, same format as the
    // radiance atlas; RGB = indirect radiance) double-buffered by the host, plus one per-texel
    // bounce-cap counter (R32Uint). All cleared once at creation; the shader rewrites every
    // texel it lights every dispatch (or clears the indirect when the feedback is off), and the
    // capture pass's radiance-atlas A-channel zeroing is the re-capture reset signal.
    for (uint32_t i = 0; i < 2; ++i)
    {
        if (!mCacheLighting.pIndirect[i])
        {
            mCacheLighting.pIndirect[i] = mpDevice->createTexture2D(
                mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::RGBA16Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mCacheLighting.pIndirect[i]->setName(
                std::string("LumenGIPass::CacheLighting::Indirect") + (i == 0 ? "A" : "B")
            ); // RGBA16F, atlas lifetime, S3-B2 double buffer.
            pRenderContext->clearUAV(mCacheLighting.pIndirect[i]->getUAV().get(), float4(0.f));
        }
    }
    if (!mCacheLighting.pBounceCount)
    {
        mCacheLighting.pBounceCount = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::R32Uint, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCacheLighting.pBounceCount->setName("LumenGIPass::CacheLighting::BounceCount"); // R32Uint, atlas lifetime, S3-B2.
        pRenderContext->clearUAV(mCacheLighting.pBounceCount->getUAV().get(), uint4(0));
    }

    if (!mCacheLighting.pPass)
        createCacheLightingProgram();
}

void LumenGIPass::runCacheLighting(RenderContext* pRenderContext)
{
    // Read back the previous dispatch's counters (one-frame lag, same as the trace path).
    if (mCacheLightingCounterReadbackPending && mpCacheLightingCountersReadback)
    {
        const uint4* pCounters = static_cast<const uint4*>(mpCacheLightingCountersReadback->map());
        if (pCounters)
        {
            mCacheLightingCounters.nanInfSamples = pCounters[0].x;
            mCacheLightingCounters.fireflySamples = pCounters[1].x;
            mCacheLightingCounters.negativeSamples = pCounters[2].x;
            mCacheLightingCounters.tracedSamples = pCounters[3].x;
            mpCacheLightingCountersReadback->unmap();
        }
        mCacheLightingCounterReadbackPending = false;
    }

    // EnvMapSampler lifecycle (scene-scoped, mirrors PathTracer.cpp): create when the scene has
    // an env map, drop otherwise. Construction builds the importance map internally.
    if (mpScene->useEnvLight())
    {
        if (!mpEnvMapSampler)
            mpEnvMapSampler = std::make_unique<EnvMapSampler>(mpDevice, mpScene->getEnvMap());
    }
    else
    {
        mpEnvMapSampler = nullptr;
    }

    if (!mpCardScene || !mCapture.pCards || !mCapture.pMaterialAtlas || !mCapture.pRadianceAtlas ||
        !mCapture.pMetadataAtlas)
    {
        return; // capture never ran; nothing to light.
    }

    ensureCacheLightingResources(pRenderContext);
    if (!mCacheLighting.pPass || !mCacheLighting.pVisibilityAtlas || !mCacheLighting.pPageToCard ||
        !mCacheLighting.pRenderList)
    {
        return;
    }

    const uint32_t renderPageCount = buildCacheLightingRenderData();
    if (renderPageCount == 0)
    {
        mLastCacheLightingPageCount = 0;
        return;
    }

    // Upload the two tables. The render list is re-uploaded in full every frame (small).
    mCacheLighting.pPageToCard->setBlob(mPageToCardData.data(), 0, mPageToCardData.size() * sizeof(uint32_t));
    mCacheLighting.pRenderList->setBlob(mRenderListData.data(), 0, renderPageCount * sizeof(uint32_t));

    // Per-frame program specialization. Program::addDefine returns true only when a value
    // changed; setVars(nullptr) recreates the vars (and thus the gScene binding) on change.
    ref<Program> pProgram = mCacheLighting.pPass->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    programChanged |= pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    programChanged |= pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    programChanged |= pProgram->addDefine("LUMEN_GI_HAS_ENVIRONMENT_SAMPLER", mpEnvMapSampler ? "1" : "0");
    programChanged |= pProgram->addDefine("LUMEN_GI_HAS_EMISSIVE_SAMPLER", mpEmissiveLightSampler ? "1" : "0");
    if (mpEmissiveLightSampler)
        programChanged |= pProgram->addDefines(mpEmissiveLightSampler->getDefines());
    else
        programChanged |= pProgram->addDefine("_EMISSIVE_LIGHT_SAMPLER_TYPE", "255"); // EmissiveLightSamplerType::Null, pin across toggles.
    // LUMEN_GI_ANALYTIC_LIGHT_MIS stays 0 (single-technique NEE; only meaningful together with
    // the S3-B2 BSDF-scatter feedback). LUMEN_GI_CACHE_LIGHTING_SHADOWS stays at the default 1
    // (DXR 1.1 inline ray queries; the scene data is bound below).
    programChanged |= pProgram->addDefine("is_valid_gMaterialParamsAtlas", "0");
    programChanged |= pProgram->addDefine("is_valid_gLumenVisibilityAtlas", "1");
    programChanged |= pProgram->addDefine("is_valid_gLumenGICounters", mpCacheLightingCounters ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gDebugTexture", "0");
    // S3-B2 feedback resources: gIndirectPrev is only bound (and its is_valid define set) when
    // the feedback is enabled; the shader guard turns feedback off without it. gIndirectCurr
    // and the bounce counter stay bound even when disabled so the shader self-cleans them.
    const bool feedbackOn = mCacheLightingFeedbackEnabled && mUseCacheLighting && mUseSurfaceCache;
    programChanged |= pProgram->addDefine("is_valid_gIndirectPrev", feedbackOn ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gIndirectCurr", "1");
    programChanged |= pProgram->addDefine("is_valid_gBounceCountAtlas", "1");
    if (programChanged)
        mCacheLighting.pPass->setVars(nullptr);

    // Bind the scene block (gScene.envMap fallback, light list) and the ray tracing data
    // (TLAS for the inline shadow ray queries). Both are per-frame so a vars recreation is
    // self-healing and the TLAS is rebuilt lazily only when the scene requires it.
    ShaderVar cacheVar = mCacheLighting.pPass->getRootVar();
    mpScene->bindShaderData(cacheVar["gScene"]);
    mpScene->bindShaderDataForRaytracing(pRenderContext, cacheVar["gScene"], 0u);

    cacheVar["gCards"] = mCapture.pCards;
    cacheVar["gMaterialAtlas"] = mCapture.pMaterialAtlas;
    cacheVar["gMetadataAtlas"] = mCapture.pMetadataAtlas;
    cacheVar["gRadianceAtlas"] = mCapture.pRadianceAtlas;
    cacheVar["gLumenVisibilityAtlas"] = mCacheLighting.pVisibilityAtlas;
    cacheVar["gLumenPageToCard"] = mCacheLighting.pPageToCard;
    cacheVar["gLumenRenderList"] = mCacheLighting.pRenderList;
    cacheVar["gLumenGICounters"] = mpCacheLightingCounters;
    // S3-B2 feedback double buffer. When the feedback is off, gIndirectPrev is left unbound
    // (is_valid_gIndirectPrev = 0 -> shader guard turns the feedback off) and the shader clears
    // gIndirectCurr to zero, so a later enable starts from a clean single bounce.
    if (feedbackOn)
        cacheVar["gIndirectPrev"] = mCacheLighting.pIndirect[1 - mCacheLighting.indirectCurrIndex];
    cacheVar["gIndirectCurr"] = mCacheLighting.pIndirect[mCacheLighting.indirectCurrIndex];
    cacheVar["gBounceCountAtlas"] = mCacheLighting.pBounceCount;
    if (mpEnvMapSampler)
        mpEnvMapSampler->bindShaderData(cacheVar["envMapSampler"]);
    if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->bindShaderData(cacheVar["emissiveSampler"]);

    // Constant buffer (LumenSurfaceCacheLightingCB; every field filled every dispatch).
    ShaderVar cb = cacheVar["LumenSurfaceCacheLightingCB"];
    cb["gPagesPerSide"] = mCapturePagesPerSide;
    cb["gRenderPageCount"] = renderPageCount;
    cb["gFrameIndex"] = mFrameIndex;
    cb["gSeed"] = kCacheLightingSeed;
    cb["gSamplesPerTexel"] = cacheLightingSamplesPerTexel();
    cb["gDebugMode"] = static_cast<uint32_t>(mDebugMode);
    cb["gAtlasSize"] = uint2(mAtlasSizeTexels, mAtlasSizeTexels);
    cb["gNearMargin"] = kCaptureNearMargin;
    cb["gCacheLightingFeedbackEnabled"] = feedbackOn ? 1u : 0u;
    cb["gCacheLightingFeedbackStrength"] = mCacheLightingFeedbackStrength;
    cb["gCacheLightingFeedbackMaxBounces"] = std::max<uint32_t>(1u, mCacheLightingFeedbackMaxBounces);

    // The counters must be cleared before each dispatch; copied to the readback buffer after.
    if (mpCacheLightingCounters)
        pRenderContext->clearUAV(mpCacheLightingCounters->getUAV().get(), uint4(0));

    // One 16x16 threadgroup per page: ComputePass::execute takes THREAD counts, so the group
    // count is nThreads / threadGroupSize. Passing (renderPageCount, 1, 1) threads would give
    // ceil(renderPageCount/16) groups (wrong); multiply the X axis by the tile size so the
    // dispatch issues exactly renderPageCount threadgroups (SV_GroupID.x == page index).
    mCacheLighting.pPass->execute(
        pRenderContext, uint3(renderPageCount * kLumenSurfaceCacheTileSize, kLumenSurfaceCacheTileSize, 1)
    );

    // Flip the S3-B2 indirect double buffer: the buffer just written becomes the previous
    // frame's input on the next dispatch (feedback on or off).
    mCacheLighting.indirectCurrIndex ^= 1u;

    if (mpCacheLightingCounters && mpCacheLightingCountersReadback)
    {
        pRenderContext->copyResource(mpCacheLightingCountersReadback.get(), mpCacheLightingCounters.get());
        mCacheLightingCounterReadbackPending = true;
    }
    mLastCacheLightingPageCount = renderPageCount;
}

void LumenGIPass::exportCacheDirectRadiance(RenderContext* pRenderContext, const RenderData& renderData)
{
    const ref<Texture> pCacheDirect = renderData.getTexture(kCacheDirectRadiance);
    if (!pCacheDirect)
        return; // optional channel not allocated (graph does not mark it as an output).
    if (mCapture.pRadianceAtlas)
    {
        // Whole-texture blit (point filter, no scaling): identical size/format (RGBA16F).
        pRenderContext->blit(mCapture.pRadianceAtlas->getSRV(), pCacheDirect->getRTV());
    }
    else
    {
        // No atlas (surface cache off): keep the channel well-defined (zero) instead of stale.
        pRenderContext->clearRtv(pCacheDirect->getRTV().get(), float4(0.f));
    }
}

// ------------------------------------------------------------------------------------------
// S4: hierarchical screen-space trace host (S4-A1)
// ------------------------------------------------------------------------------------------

void LumenGIPass::createHZBBuildProgram()
{
    mScreenTrace.pHZBBuild = ComputePass::create(mpDevice, kHZBBuildShaderFile, "main");
}

void LumenGIPass::createScreenTraceProgram()
{
    // The screen trace shader gates its inputs through the is_valid_g* defines (default 0);
    // all three resources are bound every dispatch, so all three are forced on.
    DefineList defines;
    defines.add("is_valid_gHZBMips", "1");
    defines.add("is_valid_gLinearZ", "1");
    defines.add("is_valid_gRayDirection", "1");
    mScreenTrace.pTrace = ComputePass::create(mpDevice, kScreenTraceShaderFile, "main", defines);
}

void LumenGIPass::ensureScreenTraceResources(RenderContext* pRenderContext)
{
    if (any(mFrameDim == uint2(0u, 0u)))
        return;

    const bool sizeChanged = any(mScreenTrace.resourceDim != mFrameDim);
    if (!mScreenTrace.pHZBBuild)
        createHZBBuildProgram();
    if (!mScreenTrace.pTrace)
        createScreenTraceProgram();

    // HZB levels (frozen contract in LumenHZB.h / LumenHZBBuild.cs.slang): one independent
    // R32Float texture per level (ceil-halving dims), mip 0 = full-res linear depth, level m+1
    // = 2x2 max of level m. Independent textures are required: D3D12 native mip chains are
    // floor-sized and cannot hold a ceil-halving chain. Each level is SRV + UAV bound.
    if (mScreenTrace.pHZBMips.empty() || sizeChanged)
    {
        mScreenTrace.pHZBMips.clear();
        const LumenHZB::CreateParams createParams = LumenHZB::makeCreateParams(mFrameDim.x, mFrameDim.y);
        FALCOR_ASSERT(createParams.isValid());
        for (uint32_t mip = 0; mip < createParams.mipCount; ++mip)
        {
            ref<Texture> pLevel = mpDevice->createTexture2D(
                LumenHZB::mipDimension(mFrameDim.x, mip), LumenHZB::mipDimension(mFrameDim.y, mip),
                ResourceFormat::R32Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            pLevel->setName("LumenGIPass::ScreenTrace::HZB::" + std::to_string(mip)); // R32F level, SRV + UAV.
            mScreenTrace.pHZBMips.push_back(pLevel);
        }
    }

    // S4-A1 direction input: the shader samples gRayDirection per pixel (there is no CB
    // direction field), so a direction texture is required. MVP = one constant view-space
    // direction per pixel (kScreenTraceRayDirection), built once per resize on the CPU. The
    // doc contract says RGBA16F; RGBA32F is used here so the CPU fill is lossless and format-
    // independent -- the shader's Texture2D<float4> binds either. S4.2 probe direction
    // sampling replaces this with per-pixel (normal / cosine-hemisphere) directions.
    if (!mScreenTrace.pRayDirection || sizeChanged)
    {
        const float3 dir = normalize(kScreenTraceRayDirection);
        const std::vector<float4> data((size_t)mFrameDim.x * mFrameDim.y, float4(dir, 1.f));
        mScreenTrace.pRayDirection = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA32Float, 1, 1, data.data(),
            ResourceBindFlags::ShaderResource
        );
        mScreenTrace.pRayDirection->setName("LumenGIPass::ScreenTrace::RayDirection"); // RGBA32F view-space dir, per-resize.
    }

    mScreenTrace.resourceDim = mFrameDim;
}

void LumenGIPass::runScreenTrace(RenderContext* pRenderContext, const RenderData& renderData)
{
    const ref<Texture> pLinearZ = renderData.getTexture("linearZ");
    const ref<Texture> pResult = renderData.getTexture("screenTrace");
    if (!pLinearZ || !pResult || !mpScene)
        return; // linearZ is a required input; the screenTrace output is optional per the graph.

    ensureScreenTraceResources(pRenderContext);
    if (!mScreenTrace.pHZBBuild || !mScreenTrace.pTrace || mScreenTrace.pHZBMips.empty() || !mScreenTrace.pRayDirection)
        return;

    // ---- 1. HZB build: one dispatch per level, ascending order (LumenHZB::makeAllDispatchParams).
    // Level 0 reads GBufferRT.linearZ (RG32F, .x = linear depth); level m > 0 reads level m-1
    // (2x2 max). Every dispatch rebinds the CB + source SRV / target UAV views, exactly as
    // documented in LumenHZBBuild.cs.slang and LumenHZB.h.
    const LumenHZB::CreateParams createParams = LumenHZB::makeCreateParams(mFrameDim.x, mFrameDim.y);
    FALCOR_ASSERT(createParams.isValid());
    const std::vector<LumenHZB::DispatchParams> dispatches = LumenHZB::makeAllDispatchParams(createParams);
    ShaderVar hzbVar = mScreenTrace.pHZBBuild->getRootVar();
    ShaderVar hzbCb = hzbVar["LumenHZBBuildCB"];
    for (const LumenHZB::DispatchParams& d : dispatches)
    {
        hzbCb["gSourceMipSize"] = uint2(d.sourceWidth, d.sourceHeight);
        hzbCb["gTargetMipSize"] = uint2(d.targetWidth, d.targetHeight);
        hzbCb["gSourceIsLinearZ"] = d.sourceIsLinearZ ? 1u : 0u;
        hzbCb["gPad"] = 0u;
        if (d.sourceIsLinearZ)
            hzbVar["gLinearZSource"] = pLinearZ;
        else
            hzbVar["gHZBSource"].setSrv(mScreenTrace.pHZBMips[d.mip - 1]->getSRV(0, 1));
        hzbVar["gHZBTarget"].setUav(mScreenTrace.pHZBMips[d.mip]->getUAV(0, 1));
        // ComputePass::execute takes THREAD counts; 16x16 threads per group -> exactly the
        // groupsX x groupsY thread groups the frozen LumenHZB dispatch contract requires.
        mScreenTrace.pHZBBuild->execute(
            pRenderContext, d.groupsX * LumenHZB::kBuildThreads, d.groupsY * LumenHZB::kBuildThreads, 1
        );
    }

    // ---- 2. Screen trace. gCameraFocalPx / gPrincipalPoint are pixel-space (texel coords),
    // matching the shader's view-space origin (p.x + 0.5 - principal) / focal * z0. Falcor's
    // camera has no principal-point offset, so the principal point is the frame center.
    const ref<Camera>& pCamera = mpScene->getCamera();
    const float focalLengthPx = pCamera->getFocalLength() * (float)mFrameDim.y / pCamera->getFrameHeight();

    ShaderVar var = mScreenTrace.pTrace->getRootVar();
    ShaderVar cb = var["LumenScreenTraceCB"];
    cb["gFrameDim"] = mFrameDim;
    cb["gMaxSteps"] = kLumenScreenTraceMaxStepsHost;
    cb["gStartMip"] = 0u;
    cb["gMinThickness"] = kLumenScreenTraceMinThicknessHost;
    cb["gThicknessScale"] = kLumenScreenTraceThicknessScaleHost;
    cb["gStepEpsilon"] = kLumenScreenTraceStepEpsilonHost;
    cb["gCameraFocalPx"] = focalLengthPx;
    cb["gPrincipalPoint"] = float2(0.5f * (float)mFrameDim.x, 0.5f * (float)mFrameDim.y);
    cb["gInvFrameDim"] = float2(1.f / (float)mFrameDim.x, 1.f / (float)mFrameDim.y);
    cb["gMaxMip"] = std::min<uint32_t>(kLumenScreenTraceMaxMipHost, createParams.mipCount - 1u);
    cb["gPad"] = 0u;
    var["gLinearZ"] = pLinearZ;
    // Bind the HZB level array (16-slot array; only mipCount levels are valid).
    for (uint32_t mip = 0; mip < (uint32_t)mScreenTrace.pHZBMips.size(); ++mip)
        var["gHZBMips"][mip] = mScreenTrace.pHZBMips[mip];
    var["gRayDirection"] = mScreenTrace.pRayDirection;
    var["gScreenTraceResult"] = pResult;
    mScreenTrace.pTrace->execute(pRenderContext, mFrameDim.x, mFrameDim.y, 1);

    // Miss-reason statistics: deliberately NO separate stats texture/buffer. The per-pixel miss
    // reason is already encoded in gScreenTraceResult.a (A = -(reason + 1) on miss, confidence
    // in (0,1] on hit), so the "miss-reason total == rays launched" gate (task.md S4 gate #2) is
    // aggregated host-side from the optional screenTrace output by tests/lumengi/run_screentrace.py
    // (G2: hits + misses == W*H). A GPU histogram would need a new shader (out of scope here).
}

// ------------------------------------------------------------------------------------------
// S4.2: screen probe gather host (S4-A2/B2)
// ------------------------------------------------------------------------------------------

void LumenGIPass::createScreenProbePrograms()
{
    FALCOR_ASSERT(mpScene);

    // Compute programs over LumenScreenProbeTrace.cs.slang. The three entry points share the
    // frozen data module (LumenScreenProbeData.slang); each gets its own ComputePass. The
    // shader compiles in the scene mode (LUMEN_GI_PROBE_SCENE_TRACE = 1) so the HWRT fallback
    // uses SceneRayQuery<0> against gScene (bound below exactly like the cache-lighting pass);
    // the scene defines + type conformances are required for the scene module imports.
    const auto createPass = [&](const char* entry)
    {
        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kScreenProbeShaderFile).csEntry(entry);
        desc.addTypeConformances(mpScene->getTypeConformances());
        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("LUMEN_GI_PROBE_SCENE_TRACE", "1");
        return ComputePass::create(mpDevice, desc, defines, /*createVars=*/true);
    };

    mScreenProbes.pUpdate = createPass("updateMain");
    mScreenProbes.pTrace = createPass("traceMain");
    mScreenProbes.pFinalize = createPass("finalizeMain");

    // S4.3 integrate + interpolate: pure compute over the frozen data contract
    // (LumenScreenProbeData.slang). They import no scene modules and trace no rays, so they
    // are created WITHOUT the scene shader modules / type conformances / gScene binding.
    // LUMEN_GI_PROBE_SCENE_TRACE=1 skips the data module's #if !LUMEN_GI_PROBE_SCENE_TRACE
    // gProbeTLAS declaration (no TLAS is ever bound to these passes).
    const auto createProbeCompute = [&](const char* shaderFile, const char* entry)
    {
        DefineList defines;
        defines.add("LUMEN_GI_PROBE_SCENE_TRACE", "1");
        return ComputePass::create(mpDevice, shaderFile, entry, defines);
    };
    mScreenProbes.pIntegrate = createProbeCompute(kScreenProbeIntegrateShaderFile, "main");
    mScreenProbes.pInterpolate = createProbeCompute(kScreenProbeInterpolateShaderFile, "main");
}

void LumenGIPass::ensureScreenProbeResources(RenderContext* pRenderContext)
{
    if (any(mFrameDim == uint2(0u, 0u)))
        return;

    const uint32_t probeCount = LumenScreenProbe::probeCount(mFrameDim);
    if (!mScreenProbes.pTrace || !mScreenProbes.pUpdate || !mScreenProbes.pFinalize ||
        !mScreenProbes.pIntegrate || !mScreenProbes.pInterpolate)
        createScreenProbePrograms();
    if (!mScreenProbes.pTrace)
        return; // scene not ready.

    if (!mScreenProbes.pMetadata || !mScreenProbes.pHitRecords || mScreenProbes.probeCount != probeCount)
    {
        // gProbeMeta: LumenScreenProbe::Meta (64 B) per probe. gProbeHitRecords:
        // LumenScreenProbe::Hit (32 B) per (probe, direction), fixed stride
        // kMaxDirectionsPerProbe so the record indexing is independent of the runtime
        // directions-per-probe. Both UAV + SRV (the trace reads and writes them).
        mScreenProbes.pMetadata = mpDevice->createStructuredBuffer(
            sizeof(LumenScreenProbe::Meta), probeCount,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pMetadata->setName("LumenGIPass::ScreenProbe::Metadata"); // LumenScreenProbe::Meta, 64 B, frame-scoped.
        mScreenProbes.pHitRecords = mpDevice->createStructuredBuffer(
            sizeof(LumenScreenProbe::Hit), (size_t)probeCount * LumenScreenProbe::kMaxDirectionsPerProbe,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pHitRecords->setName("LumenGIPass::ScreenProbe::HitRecords"); // LumenScreenProbe::Hit, 32 B, frame-scoped.
        mScreenProbes.pCounters = mpDevice->createStructuredBuffer(
            sizeof(LumenScreenProbe::Counters), 1u,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pCounters->setName("LumenGIPass::ScreenProbe::Counters");
        mScreenProbes.pCountersReadback = mpDevice->createStructuredBuffer(
            sizeof(LumenScreenProbe::Counters), 1u, ResourceBindFlags::None, MemoryType::ReadBack
        );
        mScreenProbes.pCountersReadback->setName("LumenGIPass::ScreenProbe::CountersReadback");

        // Native floor-halved R32F mip chain for the probe march (gHZBMips): a real D3D12 mip
        // chain is floor-sized, and the probe shader indexes it with explicit mip levels
        // (Load(int3(cell, mip))). Built every frame by the HZB build pass with floor dims.
        const uint32_t hzbMipCount = LumenHZB::makeCreateParams(mFrameDim.x, mFrameDim.y).mipCount;
        mScreenProbes.pHZBNative = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::R32Float, 1, hzbMipCount, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pHZBNative->setName("LumenGIPass::ScreenProbe::HZBNative"); // R32F native mip chain, frame-scoped.

        // S4.3 internal integrated-probe radiance (gProbeRadiance for the integrate/interpolate
        // passes). Full-res RGBA16F, sparse writes at the probe tile-center texel: RGB = incident
        // irradiance E, A = confidence. DISTINCT from the graph "probeRadiance" output (Z1's
        // finalize naive average); it is the integrate -> interpolate intermediate.
        mScreenProbes.pRadiance = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pRadiance->setName("LumenGIPass::ScreenProbe::RadianceInternal"); // RGBA16F, frame-scoped.

        // Prefill the static probe positions (tile centers). Everything else is zero
        // (the update pass resamples active/depth/normal/worldPos every frame).
        std::vector<LumenScreenProbe::Meta> metas(probeCount);
        const uint2 gridDims = LumenScreenProbe::probeGridDims(mFrameDim);
        for (uint32_t i = 0; i < probeCount; ++i)
        {
            const uint2 gridPos = uint2(i % gridDims.x, i / gridDims.x);
            metas[i].screenPos = LumenScreenProbe::probeScreenPos(gridPos);
        }
        mScreenProbes.pMetadata->setBlob(metas.data(), 0, metas.size() * sizeof(LumenScreenProbe::Meta));
        pRenderContext->clearUAV(mScreenProbes.pHitRecords->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mScreenProbes.pCounters->getUAV().get(), uint4(0));

        mScreenProbes.probeCount = probeCount;
        mScreenProbes.resourceDim = mFrameDim;
    }
}

void LumenGIPass::readbackScreenProbeCounters(RenderContext* pRenderContext)
{
    if (!mScreenProbes.counterReadbackPending || !mScreenProbes.pCountersReadback)
        return;
    const LumenScreenProbe::Counters* pCounters =
        static_cast<const LumenScreenProbe::Counters*>(mScreenProbes.pCountersReadback->map());
    if (pCounters)
    {
        mScreenProbeStats.probeCount = mScreenProbes.probeCount;
        mScreenProbeStats.directionsPerProbe = mProbeDirectionsPerProbe;
        const uint32_t interval = LumenScreenProbe::updateInterval(mScreenProbes.probeCount, mProbeMaxProbesPerFrame);
        mScreenProbeStats.updateInterval = interval;
        mScreenProbeStats.expectedProbesPerFrame =
            LumenScreenProbe::expectedProbesPerFrame(mScreenProbes.probeCount, interval);
        mScreenProbeStats.screenHits = pCounters->screenHits;
        mScreenProbeStats.fallbackAttempts = pCounters->fallbackAttempts;
        mScreenProbeStats.fallbackHits = pCounters->fallbackHits;
        mScreenProbeStats.fallbackMisses = pCounters->fallbackMisses;
        mScreenProbeStats.fallbackUnavailable = pCounters->fallbackUnavailable;
        mScreenProbeStats.inactiveProbes = pCounters->inactiveProbes;
        mScreenProbeStats.budgetSkipped = pCounters->budgetSkipped;
        mScreenProbeStats.directionsTraced = pCounters->directionsTraced;
        mScreenProbes.pCountersReadback->unmap();
    }
    mScreenProbes.counterReadbackPending = false;
}

void LumenGIPass::runScreenProbeTrace(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene)
        return;
    const ref<Texture> pLinearZ = renderData.getTexture("linearZ");
    if (!pLinearZ)
        return; // linearZ is a required input; without it there is nothing to probe.

    // S4-alignment fix (S5-enabling): the probe path builds its OWN native HZB chain
    // (mScreenProbes.pHZBNative) directly from linearZ further down, so it only needs the HZB
    // BUILD PROGRAM -- NOT the S4-A1 screenTrace mip chain, which lives behind the optional
    // "screenTrace" graph output (runScreenTrace early-returns without that channel). Create the
    // build pass lazily so probes (and therefore the S4.3 interpolate -> S5-B1 temporal filter)
    // work even when the graph does not allocate the screenTrace output (e.g. the S5 test graphs).
    if (!mScreenTrace.pHZBBuild)
        createHZBBuildProgram();

    // Read back the previous dispatch's counters (one-frame lag, same as the other paths).
    readbackScreenProbeCounters(pRenderContext);

    ensureScreenProbeResources(pRenderContext);
    if (!mScreenProbes.pUpdate || !mScreenProbes.pTrace || !mScreenProbes.pFinalize ||
        !mScreenProbes.pIntegrate || !mScreenProbes.pInterpolate ||
        !mScreenProbes.pMetadata || !mScreenProbes.pHitRecords || !mScreenProbes.pRadiance ||
        mScreenProbes.probeCount == 0)
    {
        return;
    }

    const uint32_t probeCount = mScreenProbes.probeCount;
    const uint32_t gridX = LumenScreenProbe::probeGridDims(mFrameDim).x;
    uint32_t directionsPerProbe =
        std::clamp<uint32_t>(mProbeDirectionsPerProbe, 1u, LumenScreenProbe::kMaxDirectionsPerProbe);
    const uint32_t interval = LumenScreenProbe::updateInterval(probeCount, mProbeMaxProbesPerFrame);

    // D3D12 dispatch cap: 65535 thread groups in X (64 threads/group). Only reachable for
    // extreme frames (8K+ at the max direction count); clamp defensively.
    constexpr uint64_t kMaxDispatchThreadsX = 65535ull * 64ull;
    if ((uint64_t)probeCount * directionsPerProbe > kMaxDispatchThreadsX)
        directionsPerProbe = std::max<uint32_t>(1u, (uint32_t)(kMaxDispatchThreadsX / probeCount));

    // Optional inputs, gate the per-frame is_valid defines (the screenTrace output, the
    // probeRadiance output and the S4.3 probeInterpolated output are graph-allocated; the
    // rest are always present).
    const bool hasScreenTrace = renderData.getTexture("screenTrace") != nullptr;
    const bool hasProbeRadiance = renderData.getTexture("probeRadiance") != nullptr;
    const bool hasProbeInterpolated = renderData.getTexture("probeInterpolated") != nullptr;
    const bool hasNormal = renderData.getTexture("normWRoughnessMaterialID") != nullptr;
    const bool hasDiffuseRadiance = renderData.getTexture("diffuseRadianceHitDist") != nullptr;

    // Per-frame program specialization (all three trace passes share the same shader + defines).
    bool programChanged = false;
    for (const ref<ComputePass>& pPass :
         {mScreenProbes.pUpdate, mScreenProbes.pTrace, mScreenProbes.pFinalize})
    {
        ref<Program> pProgram = pPass->getProgram();
        programChanged |= pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
        programChanged |= pProgram->addDefine("is_valid_gScreenTraceResult", hasScreenTrace ? "1" : "0");
        programChanged |= pProgram->addDefine("is_valid_gDiffuseRadianceHitDist", hasDiffuseRadiance ? "1" : "0");
        programChanged |= pProgram->addDefine("is_valid_gNormalRoughnessMaterialID", hasNormal ? "1" : "0");
        programChanged |= pProgram->addDefine("is_valid_gProbeRadiance", hasProbeRadiance ? "1" : "0");
    }

    // S4.3 integrate/interpolate passes: all inputs are internal + always bound, so their
    // is_valid defines are pinned to 1 (gProbeRadiance here is the INTERNAL pRadiance texture,
    // independent of the graph "probeRadiance" channel). gGIOutput is the graph-optional
    // "probeInterpolated" output; the passes are skipped entirely when it is not allocated.
    if (hasProbeInterpolated && hasNormal)
    {
        for (const ref<ComputePass>& pPass : {mScreenProbes.pIntegrate, mScreenProbes.pInterpolate})
        {
            ref<Program> pProgram = pPass->getProgram();
            programChanged |= pProgram->addDefine("is_valid_gProbeMeta", "1");
            programChanged |= pProgram->addDefine("is_valid_gProbeHitRecords", "1");
            programChanged |= pProgram->addDefine("is_valid_gProbeRadiance", "1");
            programChanged |= pProgram->addDefine("is_valid_gLinearZ", "1");
            programChanged |= pProgram->addDefine("is_valid_gNormalRoughnessMaterialID", "1");
            programChanged |= pProgram->addDefine("is_valid_gGIOutput", "1");
        }
    }
    if (programChanged)
    {
        mScreenProbes.pUpdate->setVars(nullptr);
        mScreenProbes.pTrace->setVars(nullptr);
        mScreenProbes.pFinalize->setVars(nullptr);
        mScreenProbes.pIntegrate->setVars(nullptr);
        mScreenProbes.pInterpolate->setVars(nullptr);
    }

    // Camera parameters (S4-A1 conventions: focal in pixels, principal = frame center, and the
    // camera world basis (view +x = right, +y = up, +z = forward toward the camera) for the
    // probe direction conversion / unprojection / projection).
    const ref<Camera>& pCamera = mpScene->getCamera();
    const float focalLengthPx = pCamera->getFocalLength() * (float)mFrameDim.y / pCamera->getFrameHeight();
    const float3 camPos = pCamera->getPosition();
    const float3 camForward = normalize(camPos - pCamera->getTarget()); // view +z (toward the camera).
    const float3 camRight = normalize(cross(pCamera->getUpVector(), camForward));
    const float3 camUp = cross(camForward, camRight);

    // Build the probe HZB into pHZBNative: a native floor-halved R32F mip chain (mip 0 = copy
    // of linearZ.x, mip m+1 = 2x2 max of mip m, edge-clamped), dispatched with the S4-A1 HZB
    // build pass using floor dims. The probe march indexes it with explicit mip levels.
    if (mScreenTrace.pHZBBuild && mScreenProbes.pHZBNative)
    {
        const uint32_t hzbMips = mScreenProbes.pHZBNative->getMipCount();
        ShaderVar hzbVar = mScreenTrace.pHZBBuild->getRootVar();
        ShaderVar hzbCb = hzbVar["LumenHZBBuildCB"];
        hzbCb["gSourceMipSize"] = mFrameDim;
        hzbCb["gTargetMipSize"] = mFrameDim;
        hzbCb["gSourceIsLinearZ"] = 1u;
        hzbCb["gPad"] = 0u;
        hzbVar["gLinearZSource"] = pLinearZ;
        hzbVar["gHZBTarget"].setUav(mScreenProbes.pHZBNative->getUAV(0, 1));
        mScreenTrace.pHZBBuild->execute(
            pRenderContext, ((mFrameDim.x + 15u) / 16u) * 16u, ((mFrameDim.y + 15u) / 16u) * 16u, 1
        );
        for (uint32_t mip = 1u; mip < hzbMips; ++mip)
        {
            const uint2 srcDims = uint2(std::max(mFrameDim.x >> (mip - 1u), 1u), std::max(mFrameDim.y >> (mip - 1u), 1u));
            const uint2 dstDims = uint2(std::max(mFrameDim.x >> mip, 1u), std::max(mFrameDim.y >> mip, 1u));
            hzbCb["gSourceMipSize"] = srcDims;
            hzbCb["gTargetMipSize"] = dstDims;
            hzbCb["gSourceIsLinearZ"] = 0u;
            hzbCb["gPad"] = 0u;
            hzbVar["gHZBSource"].setSrv(mScreenProbes.pHZBNative->getSRV(mip - 1u, 1));
            hzbVar["gHZBTarget"].setUav(mScreenProbes.pHZBNative->getUAV(mip, 1));
            mScreenTrace.pHZBBuild->execute(
                pRenderContext, ((dstDims.x + 15u) / 16u) * 16u, ((dstDims.y + 15u) / 16u) * 16u, 1
            );
        }
    }

    const auto bindPass = [&](const ref<ComputePass>& pPass, const char* entry)
    {
        ShaderVar var = pPass->getRootVar();
        ShaderVar cb = var["LumenScreenProbeCB"];
        cb["gFrameDim"] = mFrameDim;
        cb["gFrameIndex"] = mFrameIndex;
        cb["gDirectionsPerProbe"] = directionsPerProbe;
        cb["gProbeGridDims"] = uint2(gridX, (probeCount + gridX - 1u) / gridX);
        cb["gMaxHitRecordStride"] = LumenScreenProbe::kMaxDirectionsPerProbe;
        cb["gMinThickness"] = LumenScreenProbe::kMinThickness;
        cb["gThicknessScale"] = LumenScreenProbe::kThicknessScale;
        cb["gMaxMarchSteps"] = LumenScreenProbe::kMaxMarchSteps;
        cb["gStepEpsilon"] = LumenScreenProbe::kStepEpsilon;
        cb["gCameraFocalPx"] = focalLengthPx;
        cb["gPrincipalPoint"] = float2(0.5f * (float)mFrameDim.x, 0.5f * (float)mFrameDim.y);
        cb["gInvFrameDim"] = float2(1.f / (float)mFrameDim.x, 1.f / (float)mFrameDim.y);
        cb["gUpdateInterval"] = interval;
        cb["gSeed"] = LumenScreenProbe::kSeed;
        cb["gDepthChangeThreshold"] = LumenScreenProbe::kDepthChangeThreshold;
        cb["gMaxMip"] = std::min<uint32_t>(LumenScreenProbe::kMaxMip, mScreenProbes.pHZBNative ? mScreenProbes.pHZBNative->getMipCount() - 1u : 0u);
        cb["gCameraPosW"] = camPos;
        cb["gCameraRightW"] = camRight;
        cb["gCameraUpW"] = camUp;
        cb["gCameraForwardW"] = camForward;
        cb["gEnvFallbackRadiance"] = float3(0.f);
        cb["gDebugMode"] = static_cast<uint32_t>(mDebugMode);
        cb["gProbeCount"] = probeCount;
        cb["gWeightMode"] = mProbeIntegrateWeightMode; // S4.3 integrate weight mode (0 = cosine hemisphere).

        // Resources (shared by all three entry points).
        var["gLinearZ"] = pLinearZ;
        if (mScreenProbes.pHZBNative)
            var["gHZBMips"] = mScreenProbes.pHZBNative;
        if (hasNormal)
            var["gNormalRoughnessMaterialID"] = renderData.getTexture("normWRoughnessMaterialID");
        if (hasDiffuseRadiance)
            var["gDiffuseRadianceHitDist"] = renderData.getTexture("diffuseRadianceHitDist");
        if (hasScreenTrace)
            var["gScreenTraceResult"] = renderData.getTexture("screenTrace");
        var["gProbeMeta"] = mScreenProbes.pMetadata;
        var["gProbeHitRecords"] = mScreenProbes.pHitRecords;
        var["gProbeCounters"] = mScreenProbes.pCounters;
        if (hasProbeRadiance)
            var["gProbeRadiance"] = renderData.getTexture("probeRadiance");

        // Scene block + raytracing data for the scene-mode fallback (cache-lighting pattern).
        mpScene->bindShaderData(var["gScene"]);
        mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"], 0u);
    };
    bindPass(mScreenProbes.pUpdate, "updateMain");
    bindPass(mScreenProbes.pTrace, "traceMain");
    bindPass(mScreenProbes.pFinalize, "finalizeMain");

    // Counters cleared before the dispatch; copied to the readback buffer after.
    if (mScreenProbes.pCounters)
        pRenderContext->clearUAV(mScreenProbes.pCounters->getUAV().get(), uint4(0));

    // Dispatch order: update (metadata) -> trace (directions) -> finalize (probe radiance).
    // 64-thread groups; the thread counts are multiples of 64 (probeCount and
    // probeCount * directions, both rounded up).
    const uint32_t updateThreads = ((probeCount + 63u) / 64u) * 64u;
    mScreenProbes.pUpdate->execute(pRenderContext, updateThreads, 1, 1);

    const uint64_t traceTotal = (uint64_t)probeCount * directionsPerProbe;
    const uint32_t traceThreads = (uint32_t)((traceTotal + 63u) / 64u) * 64u;
    mScreenProbes.pTrace->execute(pRenderContext, traceThreads, 1, 1);

    mScreenProbes.pFinalize->execute(pRenderContext, updateThreads, 1, 1);

    // ---- S4.3: probe integrate + interpolate (S4-B3) ---------------------------------
    // Gate: the graph must allocate the "probeInterpolated" output (and the normal input is
    // present -- the interpolate pass needs it for the normal/material weights). The radiance
    // intermediate is the INTERNAL pRadiance texture (not the graph "probeRadiance" channel).
    if (hasProbeInterpolated && hasNormal)
    {
        const ref<Texture> pGIOutput = renderData.getTexture("probeInterpolated");

        // Shared CB + shared inputs for the S4.3 passes (mirror the trace bindings; gProbeRadiance
        // is the internal integrate output). No scene block: these passes have no scene imports.
        const auto bindProbeCompute = [&](const ref<ComputePass>& pPass)
        {
            ShaderVar var = pPass->getRootVar();
            ShaderVar cb = var["LumenScreenProbeCB"];
            cb["gFrameDim"] = mFrameDim;
            cb["gFrameIndex"] = mFrameIndex;
            cb["gDirectionsPerProbe"] = directionsPerProbe;
            cb["gProbeGridDims"] = uint2(gridX, (probeCount + gridX - 1u) / gridX);
            cb["gMaxHitRecordStride"] = LumenScreenProbe::kMaxDirectionsPerProbe;
            cb["gUpdateInterval"] = interval;
            cb["gSeed"] = LumenScreenProbe::kSeed;
            cb["gCameraFocalPx"] = focalLengthPx;
            cb["gPrincipalPoint"] = float2(0.5f * (float)mFrameDim.x, 0.5f * (float)mFrameDim.y);
            cb["gInvFrameDim"] = float2(1.f / (float)mFrameDim.x, 1.f / (float)mFrameDim.y);
            cb["gCameraPosW"] = camPos;
            cb["gCameraRightW"] = camRight;
            cb["gCameraUpW"] = camUp;
            cb["gCameraForwardW"] = camForward;
            cb["gProbeCount"] = probeCount;
            cb["gWeightMode"] = mProbeIntegrateWeightMode;
            var["gLinearZ"] = pLinearZ;
            if (hasNormal)
                var["gNormalRoughnessMaterialID"] = renderData.getTexture("normWRoughnessMaterialID");
            var["gProbeMeta"] = mScreenProbes.pMetadata;
            var["gProbeHitRecords"] = mScreenProbes.pHitRecords;
            var["gProbeRadiance"] = mScreenProbes.pRadiance;
        };

        bindProbeCompute(mScreenProbes.pIntegrate);
        mScreenProbes.pIntegrate->execute(pRenderContext, updateThreads, 1, 1);

        // Interpolate: bind the interpolate CB (its own cbuffer with the 5 weight params) plus
        // the shared CB + resources, then dispatch ceil(frameDim / 8) x ceil(frameDim / 8) threads.
        bindProbeCompute(mScreenProbes.pInterpolate);
        {
            ShaderVar var = mScreenProbes.pInterpolate->getRootVar();
            ShaderVar cb = var["LumenScreenProbeInterpolateCB"];
            // gFrameDim is read from the shared LumenScreenProbeCB (bound above); the
            // interpolate CB carries only the grid dim + the 5 weight parameters.
            cb["gProbeGridDim"] = uint2(gridX, (probeCount + gridX - 1u) / gridX);
            cb["gDepthThreshold"] = mProbeInterpDepthThreshold;              // meters; depthW dead zone (default 0.02).
            cb["gDepthSigmaInv"] = mProbeInterpDepthSigmaInv;               // 1/meters; falloff beyond the zone (default 4.0).
            cb["gNormalExponent"] = mProbeInterpNormalExponent;             // pow on the normal dot (default 8.0).
            cb["gMaterialMismatchWeight"] = mProbeInterpMaterialMismatchWeight; // material gate residual (default 0.05).
            cb["gFallbackConfidenceScale"] = mProbeInterpFallbackConfidenceScale; // degraded-sample confidence scale (default 0.25).
            cb["gPad"] = float2(0.f);
            var["gGIOutput"] = pGIOutput;
        }
        const uint32_t interpThreadsX = ((mFrameDim.x + 7u) / 8u) * 8u;
        const uint32_t interpThreadsY = ((mFrameDim.y + 7u) / 8u) * 8u;
        mScreenProbes.pInterpolate->execute(pRenderContext, interpThreadsX, interpThreadsY, 1);
    }

    if (mScreenProbes.pCounters && mScreenProbes.pCountersReadback)
    {
        pRenderContext->copyResource(mScreenProbes.pCountersReadback.get(), mScreenProbes.pCounters.get());
        mScreenProbes.counterReadbackPending = true;
    }
}

// ------------------------------------------------------------------------------------------
// S5: temporal filter host (S5-A1 history double buffer + S5-B1 pass wiring)
// ------------------------------------------------------------------------------------------

void LumenGIPass::createTemporalFilterProgram()
{
    // S5-B1 temporal filter (LumenTemporalFilter.cs.slang, entry "main"). The REQUIRED inputs are
    // always bound every dispatch (gLinearZ / gCurrent / gMotionVector / gPrevDepth / gPrevGI /
    // gTemporalOutput), so their is_valid defines are pinned to 1. The OPTIONAL UAVs
    // (gTemporalAlpha / gTemporalConfidence) are pinned to 0 here and specialized per-frame in
    // runTemporalFilter based on graph allocation. The optional VALIDATION inputs (normal +
    // material, hit distance, separate history length / prev confidence) stay unbound in the MVP:
    // the S4.3 interpolate pass already weight-validates against depth/normal/material, and the
    // confidence is carried in gCurrent.a / gPrevGI.a, so the filter's core validations (depth,
    // motion, confidence) are active without them.
    DefineList defines;
    defines.add("is_valid_gLinearZ", "1");
    defines.add("is_valid_gCurrent", "1");
    defines.add("is_valid_gMotionVector", "1");
    defines.add("is_valid_gPrevDepth", "1");
    defines.add("is_valid_gPrevGI", "1");
    defines.add("is_valid_gTemporalOutput", "1");
    defines.add("is_valid_gTemporalAlpha", "0");
    defines.add("is_valid_gTemporalConfidence", "0");
    mTemporalFilter.pFilter = ComputePass::create(mpDevice, kTemporalFilterShaderFile, "main", defines);
}

void LumenGIPass::ensureTemporalFilterResources(RenderContext* pRenderContext)
{
    if (any(mFrameDim == uint2(0u, 0u)))
        return;

    if (!mTemporalFilter.pFilter)
        createTemporalFilterProgram();

    const bool sizeChanged = any(mTemporalFilter.resourceDim != mFrameDim);
    if (!mTemporalFilter.pHistory[0] || !mTemporalFilter.pHistory[1] || !mTemporalFilter.pPrevDepth || sizeChanged)
    {
        // S5-A1 history ping-pong: RGBA16F (RGB = smoothed irradiance, A = history length),
        // full-res, SRV + UAV (the filter reads the previous slot and writes the current slot).
        // Created zeroed so a fresh start (or a resize) begins with no history.
        for (uint32_t i = 0; i < 2; ++i)
        {
            mTemporalFilter.pHistory[i] = mpDevice->createTexture2D(
                mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mTemporalFilter.pHistory[i]->setName(
                "LumenGIPass::TemporalFilter::History" + std::string(i == 0 ? "A" : "B")
            ); // RGBA16F (RGB = irradiance, A = history length), frame-scoped, S5-A1 double buffer.
            pRenderContext->clearUAV(mTemporalFilter.pHistory[i]->getUAV().get(), float4(0.f));
        }
        // S5-A1 previous-frame linear depth: R32F copy of GBufferRT.linearZ.x. The RenderTarget
        // flag enables the per-frame blit copy (RenderContext::blit writes through an RTV). Cleared
        // to zero so the very first frame (and any reset) sees prevZ == 0 => validation weight 0.
        mTemporalFilter.pPrevDepth = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::R32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget
        );
        mTemporalFilter.pPrevDepth->setName("LumenGIPass::TemporalFilter::PrevDepth"); // R32F, frame-scoped.
        pRenderContext->clearUAV(mTemporalFilter.pPrevDepth->getUAV().get(), float4(0.f));

        mTemporalFilter.historyCurrIndex = 0;
        mTemporalFilter.resourceDim = mFrameDim;
        mTemporalFilter.historyResetPending = true; // fresh zeroed buffers == reset state.
    }
}

void LumenGIPass::runTemporalFilter(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Allocation gates: gCurrent is the S4.3 interpolate graph output (only produced when the
    // probe path ran), gTemporalOutput is the S5 graph output this pass feeds. The graph channel
    // names mirror kOutputChannels, so renderData.getTexture() resolves them by name.
    const ref<Texture> pCurrent = renderData.getTexture(kProbeInterpolated);
    const ref<Texture> pOutput = renderData.getTexture(kTemporalFiltered);
    if (!pCurrent || !pOutput)
        return;

    const ref<Texture> pLinearZ = renderData.getTexture("linearZ");
    const ref<Texture> pMotionVector = renderData.getTexture("mvec");
    if (!pLinearZ || !pMotionVector)
        return;

    ensureTemporalFilterResources(pRenderContext);
    if (!mTemporalFilter.pFilter || !mTemporalFilter.pHistory[0] || !mTemporalFilter.pHistory[1] ||
        !mTemporalFilter.pPrevDepth)
    {
        return;
    }

    // S5-A1: camera cut / resize / scene-change reset (historyResetPending is set by
    // resetHistory()). Clear both prev buffers so every pixel takes the disocclusion path for one
    // frame: prevZ == 0 makes the per-tap validation weight 0, so the gather fails and the filter
    // outputs the current frame with history length 1 ("history immediately invalid after a cut").
    if (mTemporalFilter.historyResetPending)
    {
        for (const ref<Texture>& pHist : mTemporalFilter.pHistory)
            pRenderContext->clearUAV(pHist->getUAV().get(), float4(0.f));
        pRenderContext->clearUAV(mTemporalFilter.pPrevDepth->getUAV().get(), float4(0.f));
        mTemporalFilter.historyResetPending = false;
    }

    const bool hasAlpha = renderData.getTexture(kTemporalAlpha) != nullptr;
    const bool hasConfidence = renderData.getTexture(kTemporalConfidence) != nullptr;

    // Per-frame program specialization for the optional UAVs (graph allocation is fixed per graph,
    // so this changes only once per graph build).
    ref<Program> pProgram = mTemporalFilter.pFilter->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("is_valid_gTemporalAlpha", hasAlpha ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gTemporalConfidence", hasConfidence ? "1" : "0");
    if (programChanged)
        mTemporalFilter.pFilter->setVars(nullptr);

    // Constant buffer (LumenTemporalFilterCB; every field filled every dispatch, defaults per Z5).
    ShaderVar var = mTemporalFilter.pFilter->getRootVar();
    ShaderVar cb = var["LumenTemporalFilterCB"];
    cb["gFrameDim"] = mFrameDim;
    cb["gFrameIndex"] = mFrameIndex;
    cb["gClampHistory"] = mTemporalClampHistory ? 1u : 0u;
    cb["gHistoryAlpha"] = mTemporalHistoryAlpha;
    cb["gHistoryLengthCap"] = mTemporalHistoryLengthCap;
    cb["gClampBoxMargin"] = kTemporalClampBoxMargin;
    cb["gDepthThreshold"] = mTemporalDepthThreshold;
    cb["gDepthSigmaInv"] = mTemporalDepthSigmaInv;
    cb["gDepthRelativeThreshold"] = mTemporalDepthRelativeThreshold;
    cb["gNormalCosMin"] = kTemporalNormalCosMin;
    cb["gNormalExponent"] = kTemporalNormalExponent;
    cb["gMaterialMismatchWeight"] = kTemporalMaterialMismatchWeight;
    cb["gHitDistanceThreshold"] = kTemporalHitDistanceThreshold;
    cb["gConfidenceWeight"] = kTemporalConfidenceWeight;
    cb["gMaxRejectAlpha"] = mTemporalMaxRejectAlpha;
    cb["gMotionVectorScale"] = 1.f; // Falcor calcMotionVector convention: normalized screen space.
    cb["gMotionLengthThreshold"] = mMotionLengthThreshold;
    cb["gFireflyMaxRadiance"] = kTemporalFireflyMaxRadiance;
    cb["gInvFrameDim"] = float2(1.f / (float)mFrameDim.x, 1.f / (float)mFrameDim.y);
    cb["gPad0"] = 0u;
    cb["gPad1"] = 0u;
    cb["gPad2"] = 0u;

    // S5-A1 history double buffer: gPrevGI = the PREVIOUS frame's output (slot 1-historyCurrIndex),
    // gTemporalOutput = THIS frame's output slot (historyCurrIndex). The ping-pong is required:
    // the pass reads gPrevGI while writing gTemporalOutput, and the two must be distinct resources.
    var["gLinearZ"] = pLinearZ;
    var["gCurrent"] = pCurrent;
    var["gMotionVector"] = pMotionVector;
    var["gPrevDepth"] = mTemporalFilter.pPrevDepth;
    var["gPrevGI"] = mTemporalFilter.pHistory[1 - mTemporalFilter.historyCurrIndex];
    var["gTemporalOutput"] = mTemporalFilter.pHistory[mTemporalFilter.historyCurrIndex];
    if (hasAlpha)
        var["gTemporalAlpha"] = renderData.getTexture(kTemporalAlpha);
    if (hasConfidence)
        var["gTemporalConfidence"] = renderData.getTexture(kTemporalConfidence);

    // Dispatch ceil(gFrameDim / 8) x ceil(gFrameDim / 8) threads (8x8 thread groups per the frozen
    // LumenTemporalFilterData.slang contract).
    mTemporalFilter.pFilter->execute(
        pRenderContext,
        ((mFrameDim.x + 7u) / 8u) * 8u,
        ((mFrameDim.y + 7u) / 8u) * 8u,
        1
    );

    // S5-A1 frame-end updates: copy the freshly filtered history into the graph "temporalFiltered"
    // output (same RGBA16F format -> full-resource copy) and blit the current linear depth into
    // pPrevDepth so the NEXT frame validates against THIS frame. Then flip the ping-pong: the slot
    // written this frame becomes the previous frame's input next frame.
    pRenderContext->copyResource(pOutput.get(), mTemporalFilter.pHistory[mTemporalFilter.historyCurrIndex].get());
    pRenderContext->blit(pLinearZ->getSRV(), mTemporalFilter.pPrevDepth->getRTV());
    mTemporalFilter.historyCurrIndex ^= 1u;
}

// ------------------------------------------------------------------------------------------
// S5: spatial filter host (S5-B2 pass wiring + S5-A2 reconstruction CB)
// ------------------------------------------------------------------------------------------

void LumenGIPass::createSpatialFilterProgram()
{
    // S5-B2 variance-guided spatial filter (LumenSpatialFilter.cs.slang, entry "main"). The
    // REQUIRED resources (gGIInput / gLinearZ / gFilteredOutput) are always bound every dispatch,
    // so their is_valid defines are pinned to 1. The OPTIONAL inputs (normal/material, the S5-A1
    // confidence R32F, temporal variance / moments, the confidence / variance UAVs) are pinned to
    // 0 here and specialized per-frame in runSpatialFilter based on graph allocation. The S5-B1
    // temporal-variance inputs (gVariance / gMoments) stay unbound in the MVP -- the pass's own
    // depth-gated local variance drives the adaptive radius (the temporal variance term adds
    // history-noise information the S4.3 probe input does not currently provide).
    DefineList defines;
    defines.add("is_valid_gGIInput", "1");
    defines.add("is_valid_gLinearZ", "1");
    defines.add("is_valid_gFilteredOutput", "1");
    defines.add("is_valid_gNormalRoughnessMaterialID", "0");
    defines.add("is_valid_gConfidenceInput", "0");
    defines.add("is_valid_gVariance", "0");
    defines.add("is_valid_gMoments", "0");
    defines.add("is_valid_gFilteredConfidence", "0");
    defines.add("is_valid_gFilteredVariance", "0");
    mSpatialFilter.pFilter = ComputePass::create(mpDevice, kSpatialFilterShaderFile, "main", defines);
}

void LumenGIPass::ensureSpatialFilterResources(RenderContext* pRenderContext)
{
    if (!mSpatialFilter.pFilter)
        createSpatialFilterProgram();
}

void LumenGIPass::runSpatialFilter(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Allocation gates: gGIInput is the S5-B1 temporalFiltered graph output (the S5 main output),
    // gFilteredOutput is the S5-B2 spatialFiltered graph output this pass feeds. The graph channel
    // names mirror kOutputChannels, so renderData.getTexture() resolves them by name.
    const ref<Texture> pInput = renderData.getTexture(kTemporalFiltered);
    const ref<Texture> pOutput = renderData.getTexture(kSpatialFiltered);
    if (!pInput || !pOutput)
        return;

    const ref<Texture> pLinearZ = renderData.getTexture("linearZ");
    if (!pLinearZ)
        return;

    ensureSpatialFilterResources(pRenderContext);
    if (!mSpatialFilter.pFilter)
        return;

    // S5-B2 confidence source: the S5-A1 temporalConfidence R32F graph channel. This is the
    // chosen confidence source -- temporalFiltered.a carries the S5-B1 HISTORY LENGTH, not a
    // confidence, so it cannot be passed through gGIInput.a. When the graph allocates the
    // channel the host binds it as the pass's optional gConfidenceInput (overriding gGIInput.a);
    // otherwise the pass falls back to gGIInput.a (degraded proxy).
    const bool hasConfidence = renderData.getTexture(kTemporalConfidence) != nullptr;
    const bool hasNormal = renderData.getTexture("normWRoughnessMaterialID") != nullptr;

    // Per-frame program specialization for the optional resources (graph allocation is fixed per
    // graph, so this changes only once per graph build).
    ref<Program> pProgram = mSpatialFilter.pFilter->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("is_valid_gNormalRoughnessMaterialID", hasNormal ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gConfidenceInput", hasConfidence ? "1" : "0");
    if (programChanged)
        mSpatialFilter.pFilter->setVars(nullptr);

    // S5-A2 reconstruction CB: built through LumenReconstruction::makeSpatialFilterCB (frame dims
    // + inverse dims) and overridden with the quality-preset tuning. The S5 MVP runs the filter at
    // FULL frame resolution (the whole pipeline is full-res today); half / quarter GI (2x / 4x
    // upscale) is an S8 quality-preset item -- the CB is resolution-agnostic, so switching to
    // LumenReconstruction::makeDimensions(..., LumenGIResolutionQuality::Half) and dispatching on
    // giW/giH is all S8 needs.
    const LumenReconstruction::Dimensions dims = LumenReconstruction::makeDimensions(
        mFrameDim.x, mFrameDim.y, LumenGIResolutionQuality::Full
    );
    LumenReconstruction::SpatialFilterConstantBuffer cb = LumenReconstruction::makeSpatialFilterCB(dims);
    cb.filterEnabled = 1u;
    cb.fireflyClamp = mSpatialFireflyClamp ? 1u : 0u;
    cb.fireflyMaxRadiance = 10000.f;   // LUMEN_GI_MAX_RADIANCE default (frozen, matches the shader).
    cb.fireflyStdDevFactor = 4.0f;     // Frozen with LumenSpatialFilterData.slang.
    cb.varianceThresholdLow = mSpatialVarianceThresholdLow;
    cb.varianceThresholdHigh = mSpatialVarianceThresholdHigh;
    cb.radiusMin = mSpatialRadiusMin;
    cb.radiusMax = mSpatialRadiusMax;
    cb.spatialSigmaScale = 0.5f;       // Frozen with LumenSpatialFilterData.slang (sigma = r/2).
    cb.depthThreshold = mSpatialDepthThreshold;
    cb.depthSigmaInv = mSpatialDepthSigmaInv;
    cb.normalExponent = mSpatialNormalExponent;
    cb.materialMismatchWeight = mSpatialMaterialMismatchWeight;
    cb.temporalVarianceWeight = mSpatialTemporalVarianceWeight;
    cb.maxVarianceClamp = 1e4f;        // Frozen with LumenSpatialFilterData.slang.
    cb.varianceEpsilon = 1e-6f;        // Frozen with LumenSpatialFilterData.slang.
    cb.neighborhoodRadius = mSpatialNeighborhoodRadius;

    // Constant buffer (LumenSpatialFilterCB; every field filled every dispatch through the
    // ShaderVar binding, defaults per the frozen LumenSpatialFilterData.slang contract).
    ShaderVar var = mSpatialFilter.pFilter->getRootVar();
    ShaderVar cbs = var["LumenSpatialFilterCB"];
    cbs["gFrameDim"] = uint2(cb.frameDim[0], cb.frameDim[1]);
    cbs["gFilterEnabled"] = cb.filterEnabled;
    cbs["gFireflyClamp"] = cb.fireflyClamp;
    cbs["gFireflyMaxRadiance"] = cb.fireflyMaxRadiance;
    cbs["gFireflyStdDevFactor"] = cb.fireflyStdDevFactor;
    cbs["gVarianceThresholdLow"] = cb.varianceThresholdLow;
    cbs["gVarianceThresholdHigh"] = cb.varianceThresholdHigh;
    cbs["gRadiusMin"] = cb.radiusMin;
    cbs["gRadiusMax"] = cb.radiusMax;
    cbs["gSpatialSigmaScale"] = cb.spatialSigmaScale;
    cbs["gDepthThreshold"] = cb.depthThreshold;
    cbs["gDepthSigmaInv"] = cb.depthSigmaInv;
    cbs["gNormalExponent"] = cb.normalExponent;
    cbs["gMaterialMismatchWeight"] = cb.materialMismatchWeight;
    cbs["gTemporalVarianceWeight"] = cb.temporalVarianceWeight;
    cbs["gMaxVarianceClamp"] = cb.maxVarianceClamp;
    cbs["gVarianceEpsilon"] = cb.varianceEpsilon;
    cbs["gInvFrameDim"] = float2(cb.invFrameDim[0], cb.invFrameDim[1]);
    cbs["gNeighborhoodRadius"] = cb.neighborhoodRadius;

    // Resources: gGIInput = temporalFiltered (RGB), gConfidenceInput = temporalConfidence when
    // allocated, gLinearZ / gNormalRoughnessMaterialID from the GBuffer inputs, and gFilteredOutput
    // = the spatialFiltered graph channel (direct UAV binding, same pattern as the S4.3 interpolate
    // writing probeInterpolated). No host-owned buffers: the pass reads the previous stage and
    // writes the graph output in one dispatch, so no ping-pong or copy is needed.
    var["gGIInput"] = pInput;
    if (hasConfidence)
        var["gConfidenceInput"] = renderData.getTexture(kTemporalConfidence);
    var["gLinearZ"] = pLinearZ;
    if (hasNormal)
        var["gNormalRoughnessMaterialID"] = renderData.getTexture("normWRoughnessMaterialID");
    var["gFilteredOutput"] = pOutput;

    // Dispatch ceil(gFrameDim / 8) x ceil(gFrameDim / 8) threads (8x8 thread groups per the frozen
    // LumenSpatialFilterData.slang contract). The pass self-guards out-of-frame threads.
    mSpatialFilter.pFilter->execute(
        pRenderContext,
        ((mFrameDim.x + 7u) / 8u) * 8u,
        ((mFrameDim.y + 7u) / 8u) * 8u,
        1
    );
}

// =====================================================================================
// S6: Mesh SDF + Global Distance Field host implementation
// -------------------------------------------------------------------------------------
// Data pipeline (S6-A):  scene static instances -> LumenMeshSDFScene (S6-A2 disk cache ->
// builder -> volume -> atlas -> instance table) -> GPU atlas textures/buffers.
// GDF compose (S6-B3):     LumenGlobalDistanceField clipmap -> dirty regions -> instance list ->
//                          LumenGDFCompose.cs.slang dispatch into the GDF clipmap textures.
// Sphere trace (S6-B4):    LumenGDFTrace.cs.slang over the screen; TraceMode::MeshSDF writes the
//                          S1 outputs (primary software path), Hybrid writes the gdfTrace channel.
// =====================================================================================

void LumenGIPass::invalidateMeshSDF()
{
    mSDF = {};
}

void LumenGIPass::ensureMeshSDFScene()
{
    namespace s6 = LumenGI::MeshSDF;
    namespace s6scene = LumenGI::MeshSDF::Scene;
    if (mSDF.pScene || !mpScene)
        return;

    const std::filesystem::path cacheDir =
        mMeshSDFCacheDir.empty() ? s6::Cache::defaultCacheDir() : mMeshSDFCacheDir;

    auto pScene = std::make_unique<s6scene::LumenMeshSDFScene>(
        cacheDir, mMeshSDFBudgetBytes, 0, s6::kLumenMeshSDFAtlasDefaultMinResidencyFrames
    );

    // Builder selection: external MeshSDFBuilder.exe when configured and present (runs on a box OBJ
    // derived from the mesh's padded object AABB), otherwise the built-in analytic box-SDF builder.
    if (!mMeshSDFBuilderPath.empty() && std::filesystem::exists(mMeshSDFBuilderPath))
    {
        const std::filesystem::path exe = mMeshSDFBuilderPath;
        pScene->setBuilder(
            [exe](const s6scene::LumenMeshSDFSceneMeshDesc& mesh, const std::filesystem::path& target, std::string& err) {
                return s6ExternalMeshSDFBuilder(exe, mesh, target, err);
            }
        );
    }
    else
    {
        pScene->setBuilder(&s6BoxSDFBuilder);
    }

    // Register static triangle-mesh instances (mirrors LumenCardScene::rebuild(): instance loop,
    // global-matrix transform, static-only filter). Non-triangle, dynamic or missing-transform
    // instances are skipped with a warning (the S6 "unsupported geometry falls back" rule).
    const Falcor::AnimationController* pAnimationController = mpScene->getAnimationController();
    std::vector<Falcor::float4x4> emptyMatrices;
    const std::vector<Falcor::float4x4>& globalMatrices =
        pAnimationController ? pAnimationController->getGlobalMatrices() : emptyMatrices;

    const uint32_t res = meshSDFResolution();
    const s6::Quality quality = meshSDFQuality();
    const uint32_t instanceCount = mpScene->getGeometryInstanceCount();
    std::string err;

    std::vector<uint32_t> handles;
    std::vector<s6scene::LumenMeshSDFSceneMeshDesc> descs;
    handles.reserve(instanceCount);
    descs.reserve(instanceCount);

    for (uint32_t si = 0; si < instanceCount; ++si)
    {
        const Falcor::GeometryInstanceData& inst = mpScene->getGeometryInstance(si);
        if (inst.getType() != Falcor::Scene::GeometryType::TriangleMesh &&
            inst.getType() != Falcor::Scene::GeometryType::DisplacedTriangleMesh)
            continue;
        if (inst.globalMatrixID >= globalMatrices.size())
            continue;
        if (inst.isDynamic())
            continue;
        const Falcor::MeshDesc& mesh = mpScene->getMesh(Falcor::MeshID::fromSlang(inst.geometryID));
        if (mesh.isDynamic())
            continue;
        const Falcor::AABB oabb = mpScene->getMeshBounds(inst.geometryID);
        if (!oabb.valid() || !std::isfinite(oabb.minPoint.x) || !std::isfinite(oabb.maxPoint.x))
            continue;

        s6scene::LumenMeshSDFSceneMeshDesc desc;
        desc.meshContentHash = s6MeshContentHash(inst.geometryID, oabb, mesh.vertexCount, mesh.indexCount);
        desc.cacheParams.resolution = {res, res, res};
        desc.cacheParams.quality = quality;
        desc.cacheParams.pooling = s6::MipPooling::MinAbs;
        desc.cacheParams.gridBounds = s6PaddedGridBounds(oabb, res, mMeshSDFPadding);

        s6::LumenMeshSDFAtlasInstanceDesc transform;
        const Falcor::float4x4& t = globalMatrices[inst.globalMatrixID];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                transform.forwardLinear[r * 3 + c] = t[r][c];
        transform.forwardTranslation = {t[0][3], t[1][3], t[2][3]};

        // Static meshes feed BOTH the camera-centered dynamic level 0 AND the origin-anchored
        // static levels (the mesh/GDF layer bits coincide; see LumenMeshSDFScene.h). Dynamic
        // instances are excluded, so the static far field can never be polluted by them.
        const uint32_t handle = pScene->addInstance(desc, transform, s6::kLumenMeshSDFLayerMaskAll, err);
        if (handle == s6::kLumenMeshSDFAtlasInvalidID)
        {
            logWarning("LumenGI S6: skipped scene instance {} ({})", si, err);
            continue;
        }
        handles.push_back(handle);
        descs.push_back(desc);
    }

    const Falcor::float3 camPos = mpScene->getCamera()->getPosition();
    const float cam[3] = {camPos.x, camPos.y, camPos.z};
    pScene->setCamera(cam);

    mSDF.pScene = std::move(pScene);
    mSDF.sceneInstanceHandles = std::move(handles);
    mSDF.sceneMeshDescs = std::move(descs);
    mSDF.atlasPagesPerSide = mSDF.pScene->instanceTable().atlas().pagesPerSide();
    mSDF.needsFullCompose = true;
    mSDF.gdfInstanceVersion = 0;
    mSDF.sceneStats = mSDF.pScene->getStats();

    rebuildMeshSDFAtlasImages();
}

void LumenGIPass::rebuildMeshSDFAtlasImages()
{
    namespace s6 = LumenGI::MeshSDF;
    namespace s6scene = LumenGI::MeshSDF::Scene;
    if (!mSDF.pScene)
        return;

    s6::LumenMeshSDFInstanceTable& table = mSDF.pScene->instanceTable();
    s6::LumenMeshSDFAtlas& atlas = table.atlas();
    const std::vector<s6::LumenMeshSDFAtlasInstance>& instTable = atlas.getInstanceTable();
    const std::vector<uint32_t>& pageTable = atlas.getPageTableBuffer();
    const std::vector<s6::LumenMeshSDFVolumeDescriptor>& volumes = atlas.getVolumeDescriptors();

    const uint32_t meshCount = static_cast<uint32_t>(volumes.size());
    mSDF.meshMipFloats.assign(meshCount, {});
    std::vector<bool> parsed(meshCount, false);

    const uint32_t P = std::max<uint32_t>(mSDF.atlasPagesPerSide, 1u);
    const size_t cap = size_t(P) * P * P;
    constexpr size_t pageVox = size_t(s6::kLumenMeshSDFAtlasPageSize) * s6::kLumenMeshSDFAtlasPageSize *
                               s6::kLumenMeshSDFAtlasPageSize;
    const size_t atlasVox = cap * pageVox;
    mSDF.fineImage.assign(atlasVox, 0.f);
    mSDF.coarseImage.assign(atlasVox, 0.f);

    // Parse each unique mesh volume from the disk cache + reference-convert to mip floats
    // (the exact encoding the atlas textures hold: fine = raw float, coarse = clamp(d/R,-1,1)).
    for (size_t h = 0; h < mSDF.sceneInstanceHandles.size(); ++h)
    {
        const uint32_t handle = mSDF.sceneInstanceHandles[h];
        const s6::LumenMeshSDFInstanceAtlasMapping map = mSDF.pScene->instanceToAtlas(handle);
        if (map.meshID == s6::kLumenMeshSDFAtlasInvalidID || map.meshID >= meshCount)
            continue;
        if (parsed[map.meshID])
            continue;
        parsed[map.meshID] = true;

        const s6scene::LumenMeshSDFSceneMeshDesc& desc = mSDF.sceneMeshDescs[h];
        const std::string key = s6scene::LumenMeshSDFScene::meshKey(desc);
        const std::filesystem::path path = mSDF.pScene->cacheDirectory() / (key + ".msdf");
        s6::MSDFParseResult parsedVol;
        std::string perr;
        if (!s6::Cache::findCached(path, parsedVol, perr))
        {
            logWarning("LumenGI S6: cannot parse cached volume {} ({})", path.string(), perr);
            continue;
        }
        s6::BuildParams bp;
        bp.quality = desc.cacheParams.quality;
        bp.pooling = desc.cacheParams.pooling;
        bp.meshContentHash = desc.meshContentHash;
        s6scene::LumenMeshSDFConvertedVolume cv;
        std::string cerr;
        if (!s6scene::referenceConverter(parsedVol, bp, cv, cerr))
        {
            logWarning("LumenGI S6: reference conversion failed for {} ({})", path.string(), cerr);
            continue;
        }
        mSDF.meshMipFloats[map.meshID] = std::move(cv.mipFloats);
    }

    // Tile into the host atlas images (GPU layout: page slot s -> brick origin
    // (s % P, (s / P) % P, s / (P*P)) * pageSize; matches the fixed atlas sampler).
    for (size_t i = 0; i < instTable.size(); ++i)
    {
        const s6::LumenMeshSDFAtlasInstance& inst = instTable[i];
        if (inst.meshID == s6::kLumenMeshSDFAtlasInvalidID || inst.meshID >= meshCount)
            continue;
        const auto& mips = mSDF.meshMipFloats[inst.meshID];
        if (mips.empty())
            continue;
        const s6::LumenMeshSDFVolumeDescriptor& vd = volumes[inst.meshID];
        const uint32_t mipCount = std::min<uint32_t>(vd.mipCount, static_cast<uint32_t>(mips.size()));
        for (uint32_t m = 0; m < mipCount; ++m)
        {
            const uint32_t baseSlot = pageTable[i * s6::kLumenMeshSDFMaxMipCount + m];
            if (baseSlot == s6::kLumenMeshSDFNotResident)
                continue;
            const bool fine = (m == 0 && vd.formatMip0 == static_cast<uint32_t>(s6::VolumeFormat::R16Float));
            std::vector<float>& image = fine ? mSDF.fineImage : mSDF.coarseImage;
            s6TileMipIntoAtlasImage(
                mips[m], std::array<uint32_t, 3>{vd.resolution[0], vd.resolution[1], vd.resolution[2]}, m, baseSlot, P, image
            );
        }
    }

    ++mSDF.atlasUploadVersion;
}

void LumenGIPass::ensureGDFResources(RenderContext* pRenderContext)
{
    namespace s6 = LumenGI::MeshSDF;
    namespace s6gdf = LumenGI::GlobalDistanceField;
    if (!mSDF.pScene)
        return;

    // (Re)build the CPU clipmap when the config changed.
    if (!mSDF.gdf || mSDF.gdf->getLevelCount() != mGDFLevelCount || mSDF.gdf->getResolution() != mGDFResolution)
    {
        mSDF.gdf = std::make_unique<s6gdf::LumenGlobalDistanceField>(mGDFBaseExtent, mGDFLevelCount, mGDFResolution);
        mSDF.needsFullCompose = true;
    }

    const uint32_t levelCount = mSDF.gdf->getLevelCount();
    const uint32_t res = mSDF.gdf->getResolution();

    // GDF clipmap textures. ALL levels use R32Float: the compose shader binds gGDFLevels as a
    // single RWTexture3D<float>[kLumenGDFMaxLevels] array, which requires every element to share
    // one format AND a format the untyped-float UAV type accepts (R32F is the Falcor-tested one
    // for float UAV arrays). The encode/decode is then the identity (raw world distance).
    if (mSDF.levels.size() != levelCount || (!mSDF.levels.empty() && mSDF.levels[0]->getWidth() != res))
    {
        mSDF.levels.clear();
        for (uint32_t m = 0; m < levelCount; ++m)
        {
            auto pTex = mpDevice->createTexture3D(
                res, res, res, ResourceFormat::R32Float, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            pTex->setName("LumenGI::GDFLevel" + std::to_string(m));
            mSDF.levels.push_back(pTex);
        }
        mSDF.needsFullCompose = true;
    }

    if (!mSDF.pLevelTable)
        mSDF.pLevelTable = mpDevice->createStructuredBuffer(
            sizeof(LumenGDFLevelParamsHost), levelCount, ResourceBindFlags::ShaderResource
        );
    if (!mSDF.pGDFInstances)
        mSDF.pGDFInstances = mpDevice->createStructuredBuffer(
            sizeof(LumenGI::MeshSDF::Scene::LumenMeshSDFGDFInstance), s6::kLumenMeshSDFAtlasMaxInstances,
            ResourceBindFlags::ShaderResource
        );
    if (!mSDF.pDirtyRegions)
        mSDF.pDirtyRegions = mpDevice->createStructuredBuffer(
            sizeof(LumenGDFDirtyRegionHost), 64, ResourceBindFlags::ShaderResource
        );
    if (!mSDF.pTraceStats)
    {
        mSDF.pTraceStats = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), kGDFTraceStatCount, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mSDF.pTraceStatsReadback = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), kGDFTraceStatCount, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        pRenderContext->clearUAV(mSDF.pTraceStats->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mSDF.pTraceStatsReadback->getUAV().get(), uint4(0));
    }

    // Mesh SDF atlas GPU mirror (fine = R16Float mip0 pages, coarse = R8Snorm mips >= 1).
    const uint32_t P = std::max<uint32_t>(mSDF.atlasPagesPerSide, 1u);
    const uint32_t texels = P * s6::kLumenMeshSDFAtlasPageSize;
    if (!mSDF.pFineAtlas || mSDF.pFineAtlas->getWidth() != texels)
    {
        mSDF.pFineAtlas = mpDevice->createTexture3D(
            texels, texels, texels, ResourceFormat::R16Float, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mSDF.pFineAtlas->setName("LumenGIPass::MSDFFineAtlas");
        mSDF.pCoarseAtlas = mpDevice->createTexture3D(
            texels, texels, texels, ResourceFormat::R8Snorm, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mSDF.pCoarseAtlas->setName("LumenGIPass::MSDFCoarseAtlas");
    }
    if (!mSDF.pPageTable)
        mSDF.pPageTable = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), s6::kLumenMeshSDFAtlasMaxInstances * s6::kLumenMeshSDFMaxMipCount,
            ResourceBindFlags::ShaderResource
        );
    if (!mSDF.pVolumes)
        mSDF.pVolumes = mpDevice->createStructuredBuffer(
            sizeof(s6::LumenMeshSDFVolumeDescriptor), 256, ResourceBindFlags::ShaderResource
        );
    if (!mSDF.pAtlasInstances)
        mSDF.pAtlasInstances = mpDevice->createStructuredBuffer(
            sizeof(s6::LumenMeshSDFAtlasInstance), s6::kLumenMeshSDFAtlasMaxInstances,
            ResourceBindFlags::ShaderResource
        );

    // Compose pass (LumenGDFCompose.cs.slang, entry "main"). The trace pass is created lazily by
    // runGDFSphereTrace because its is_valid defines depend on the graph channel allocation.
    if (!mSDF.pCompose)
        mSDF.pCompose = ComputePass::create(mpDevice, kGDFComposeShaderFile, "main", DefineList());

    uploadMeshSDFAtlas(pRenderContext);
}

void LumenGIPass::uploadMeshSDFAtlas(RenderContext* pRenderContext)
{
    namespace s6 = LumenGI::MeshSDF;
    if (!mSDF.pScene || !mSDF.pFineAtlas)
        return;
    if (mSDF.atlasUploadVersion == mSDF.uploadedAtlasVersion)
        return;

    s6::LumenMeshSDFAtlas& atlas = mSDF.pScene->instanceTable().atlas();

    // Upload the host atlas images (fine = R16Float f16 bits, coarse = R8Snorm int8 codes).
    if (!mSDF.fineImage.empty())
    {
        std::vector<uint16_t> f16(mSDF.fineImage.size());
        for (size_t i = 0; i < mSDF.fineImage.size(); ++i)
            f16[i] = Falcor::math::float32ToFloat16(mSDF.fineImage[i]);
        pRenderContext->updateTextureData(mSDF.pFineAtlas.get(), f16.data());
    }
    if (!mSDF.coarseImage.empty())
    {
        std::vector<int8_t> codes;
        s6CoarseImageToCodes(mSDF.coarseImage, codes);
        pRenderContext->updateTextureData(mSDF.pCoarseAtlas.get(), codes.data());
    }

    // Upload the page table, volume descriptors and the atlas instance table.
    const std::vector<uint32_t>& pageTable = atlas.getPageTableBuffer();
    if (!pageTable.empty())
        pRenderContext->updateBuffer(mSDF.pPageTable.get(), pageTable.data(), 0, pageTable.size() * sizeof(uint32_t));
    const std::vector<s6::LumenMeshSDFVolumeDescriptor>& volumes = atlas.getVolumeDescriptors();
    if (!volumes.empty())
        pRenderContext->updateBuffer(
            mSDF.pVolumes.get(), volumes.data(), 0, volumes.size() * sizeof(s6::LumenMeshSDFVolumeDescriptor)
        );
    const std::vector<s6::LumenMeshSDFAtlasInstance>& instTable = atlas.getInstanceTable();
    if (!instTable.empty())
        pRenderContext->updateBuffer(
            mSDF.pAtlasInstances.get(), instTable.data(), 0, instTable.size() * sizeof(s6::LumenMeshSDFAtlasInstance)
        );

    mSDF.uploadedAtlasVersion = mSDF.atlasUploadVersion;
}

void LumenGIPass::runGDFCompose(RenderContext* pRenderContext)
{
    namespace s6 = LumenGI::MeshSDF;
    namespace s6scene = LumenGI::MeshSDF::Scene;
    namespace s6gdf = LumenGI::GlobalDistanceField;
    if (!mSDF.pScene || !mSDF.gdf || !mSDF.pCompose)
        return;

    // S6-in-progress guard: the compose pass currently reads the mesh-SDF atlas resources
    // (gFineAtlas / gCoarseAtlas / gPageTable / gVolumes / gInstances) that the host wiring
    // does not create yet. Dispatching with them unbound aborts with D3D12 E_INVALIDARG.
    // Skip the dispatch (and the dependent trace) until the full S6 data pipeline is wired.
    if (!mSDF.pFineAtlas || !mSDF.pCoarseAtlas || !mSDF.pPageTable || !mSDF.pVolumes || !mSDF.pAtlasInstances)
    {
        logWarning("LumenGI: GDF compose skipped (mesh-SDF atlas resources not wired yet; S6 integration pending).");
        return;
    }

    // Push the camera anchor into the clipmap (scroll bookkeeping; static levels never scroll).
    const Falcor::float3 camPos = mpScene->getCamera()->getPosition();
    mSDF.gdf->setCamera(s6gdf::float3(camPos.x, camPos.y, camPos.z));
    const float cam[3] = {camPos.x, camPos.y, camPos.z};
    mSDF.pScene->setCamera(cam);

    // GDF instance list (resident instances; the compose filter layer = All).
    const std::vector<s6scene::LumenMeshSDFGDFInstance> gdfInstances =
        mSDF.pScene->buildGDFInstanceList(s6::kLumenMeshSDFLayerMaskAll);

    // Dirty regions: full-level compose on the first frame / instance-list change, otherwise the
    // scroll slabs (added + removed) of the dynamic level only.
    const uint32_t R = mSDF.gdf->getResolution();
    const uint32_t levelCount = mSDF.gdf->getLevelCount();
    std::vector<LumenGDFDirtyRegionHost> regions;
    if (mSDF.needsFullCompose)
    {
        regions.reserve(levelCount);
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            LumenGDFDirtyRegionHost r;
            r.level = level;
            r.axis = 0xFFFFFFFFu;
            r.min[0] = r.min[1] = r.min[2] = 0;
            r.max[0] = r.max[1] = r.max[2] = static_cast<int32_t>(R - 1u);
            r.flags = kLumenGDFDirtyFlagAdded;
            regions.push_back(r);
        }
        mSDF.needsFullCompose = false;
    }
    else
    {
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            for (const s6gdf::VoxelRange& vr : mSDF.gdf->dirtyRegions(level))
            {
                LumenGDFDirtyRegionHost r;
                r.level = level;
                r.axis = static_cast<uint32_t>(vr.axis);
                r.min[0] = vr.min.x;
                r.min[1] = vr.min.y;
                r.min[2] = vr.min.z;
                r.max[0] = vr.max.x;
                r.max[1] = vr.max.y;
                r.max[2] = vr.max.z;
                r.flags = vr.added ? kLumenGDFDirtyFlagAdded : kLumenGDFDirtyFlagRemoved;
                regions.push_back(r);
            }
            for (const s6gdf::VoxelRange& vr : mSDF.gdf->removedRegions(level))
            {
                LumenGDFDirtyRegionHost r;
                r.level = level;
                r.axis = static_cast<uint32_t>(vr.axis);
                r.min[0] = vr.min.x;
                r.min[1] = vr.min.y;
                r.min[2] = vr.min.z;
                r.max[0] = vr.max.x;
                r.max[1] = vr.max.y;
                r.max[2] = vr.max.z;
                r.flags = kLumenGDFDirtyFlagRemoved;
                regions.push_back(r);
            }
        }
    }

    // Level table (LumenGDFLevelParams per level).
    std::vector<LumenGDFLevelParamsHost> levelTable(levelCount);
    for (uint32_t m = 0; m < levelCount; ++m)
    {
        const s6gdf::LumenGDFLevel& lvl = mSDF.gdf->getLevel(m);
        levelTable[m].resolution = lvl.resolution;
        levelTable[m].format = static_cast<uint32_t>(s6::VolumeFormat::R16Float); // homogeneous R16Float clipmap (see ensureGDFResources).
        levelTable[m].worldExtent = lvl.worldExtent;
        levelTable[m].voxelSize = lvl.voxelSize;
        const float emptyDist = std::max(mGDFEmptyDistanceScale * lvl.voxelSize, 1e-6f);
        levelTable[m].emptyDistance = emptyDist;
        levelTable[m].quantRange = 0.f; // R16Float codec is the identity; no R8Snorm scale.
    }

    // Upload the small host buffers.
    if (!levelTable.empty())
        pRenderContext->updateBuffer(
            mSDF.pLevelTable.get(), levelTable.data(), 0, levelTable.size() * sizeof(LumenGDFLevelParamsHost)
        );
    if (!gdfInstances.empty())
        pRenderContext->updateBuffer(
            mSDF.pGDFInstances.get(), gdfInstances.data(), 0,
            gdfInstances.size() * sizeof(LumenGI::MeshSDF::Scene::LumenMeshSDFGDFInstance)
        );
    if (!regions.empty())
        pRenderContext->updateBuffer(
            mSDF.pDirtyRegions.get(), regions.data(), 0, regions.size() * sizeof(LumenGDFDirtyRegionHost)
        );

    if (regions.empty())
        return; // no dirty voxels this frame (static camera, no instance changes).

    // Bind the compose pass.
    ShaderVar var = mSDF.pCompose->getRootVar();
    ShaderVar cb = var["LumenGDFComposeCB"];
    ShaderVar clip = cb["gClipmap"];
    clip["cameraCenter"] = float3(mSDF.gdf->getCameraCenter().x, mSDF.gdf->getCameraCenter().y, mSDF.gdf->getCameraCenter().z);
    clip["emptyDistance"] = 0.f; // levels set their own emptyDistance.
    clip["levelCount"] = levelCount;
    clip["dynamicLevelCount"] = s6gdf::kDynamicLevels;
    clip["resolution"] = R;
    clip["instanceCount"] = static_cast<uint32_t>(gdfInstances.size());
    clip["dirtyRegionCount"] = static_cast<uint32_t>(regions.size());
    clip["frameIndex"] = mFrameIndex;
    const s6gdf::index3 scroll = mSDF.gdf->scrollFromCameraMove();
    clip["scroll"] = int3(scroll.x, scroll.y, scroll.z);

    // FIXME(S6-diag): strip to CB + UAV array only.
    // var["gGDFLevelTable"] = mSDF.pLevelTable;
    // var["gGDFInstances"] = mSDF.pGDFInstances;
    // var["gGDFDirtyRegions"] = mSDF.pDirtyRegions;
    // Fill EVERY slot of the gGDFLevels[kLumenGDFMaxLevels] array (repeat the last level) so no
    // descriptor is left invalid: D3D12 validates the whole UAV array at dispatch and a null
    // entry aborts with E_INVALIDARG. Only [0, levelCount) is ever accessed by the shader.
    // Assignment (not setUav) is the Falcor binding contract for texture arrays (TextureArrays test).
    for (uint32_t m = 0; m < kLumenGDFMaxLevelsHost; ++m)
        var["gGDFLevels"][m] = mSDF.levels[std::min<uint32_t>(m, levelCount - 1u)];
    // var["gFineAtlas"] = mSDF.pFineAtlas;
    // var["gCoarseAtlas"] = mSDF.pCoarseAtlas;
    // var["gPageTable"] = mSDF.pPageTable;
    // var["gVolumes"] = mSDF.pVolumes;
    // var["gInstances"] = mSDF.pAtlasInstances;
    // var["gAtlasInstanceCount"] = mSDF.pScene->instanceTable().instanceCount();
    // var["gAtlasVolumeCount"] = mSDF.pScene->instanceTable().meshCount();
    // var["gAtlasPagesPerSide"] = mSDF.atlasPagesPerSide;

    // Dispatch: (ceil(maxRegionDimsX / 8), regionCount, ceil(maxRegionDimsYZ / 8)) with
    // numthreads(8, 1, 8); the shader bounds-checks every thread against its region.
    uint32_t maxDx = 1u, maxDyz = 1u;
    for (const LumenGDFDirtyRegionHost& r : regions)
    {
        const uint32_t dx = static_cast<uint32_t>(std::max(0, r.max[0] - r.min[0] + 1));
        const uint32_t dyz = static_cast<uint32_t>(std::max(0, r.max[1] - r.min[1] + 1)) *
                             static_cast<uint32_t>(std::max(0, r.max[2] - r.min[2] + 1));
        maxDx = std::max(maxDx, std::max(dx, 1u));
        maxDyz = std::max(maxDyz, std::max(dyz, 1u));
    }
    logInfo("S6 compose dispatch: x={} y={} z={} (regions={}, maxDx={}, maxDyz={})", ((maxDx + 7u) / 8u) * 8u, static_cast<uint32_t>(regions.size()), ((maxDyz + 7u) / 8u) * 8u, regions.size(), maxDx, maxDyz);
    // FIXME(S6-diag): minimal dispatch to isolate E_INVALIDARG (binding vs dims).
    mSDF.pCompose->execute(pRenderContext, 8u, 1u, 8u);

    mSDF.sceneStats = mSDF.pScene->getStats();
}

void LumenGIPass::runGDFSphereTrace(RenderContext* pRenderContext, const RenderData& renderData)
{
    namespace s6 = LumenGI::MeshSDF;
    if (!mSDF.pScene || !mSDF.gdf || !mSDF.pCompose || mSDF.levels.empty())
        return;

    const bool primary = (mTraceMode == TraceMode::MeshSDF);
    const bool hasDebug = renderData.getTexture(kGDFTrace) != nullptr;
    if (!primary && !hasDebug)
        return; // nothing to write (Hybrid without the diagnostic channel).

    // The trace pass is specialized per config (primary writes the S1 outputs; the diagnostic
    // channel is bound only when the graph allocates it). Recreate on config change.
    const uint32_t config = (primary ? 1u : 0u) | (hasDebug ? 2u : 0u);
    if (!mSDF.pTrace)
    {
        DefineList defines;
        defines.add("is_valid_gDiffuseGI", primary ? "1" : "0");
        defines.add("is_valid_gDiffuseRadianceHitDist", primary ? "1" : "0");
        defines.add("is_valid_gConfidence", primary ? "1" : "0");
        defines.add("is_valid_gGDFTraceDebug", hasDebug ? "1" : "0");
        mSDF.pTrace = ComputePass::create(mpDevice, kGDFTraceShaderFile, "main", defines);
    }

    const uint32_t R = mSDF.gdf->getResolution();
    const uint32_t levelCount = mSDF.gdf->getLevelCount();

    // Clear + readback the sphere-trace counters.
    pRenderContext->clearUAV(mSDF.pTraceStats->getUAV().get(), uint4(0));

    const ref<Camera>& pCamera = mpScene->getCamera();
    const float focalLengthPx = pCamera->getFocalLength() * (float)mFrameDim.y / pCamera->getFrameHeight();
    const float3 camPos = pCamera->getPosition();
    const float3 camForward = normalize(camPos - pCamera->getTarget());
    const float3 camRight = normalize(cross(pCamera->getUpVector(), camForward));
    const float3 camUp = cross(camForward, camRight);

    ShaderVar var = mSDF.pTrace->getRootVar();
    ShaderVar cb = var["LumenGDFTraceCB"];
    ShaderVar clip = cb["gClipmap"];
    clip["cameraCenter"] = float3(mSDF.gdf->getCameraCenter().x, mSDF.gdf->getCameraCenter().y, mSDF.gdf->getCameraCenter().z);
    clip["emptyDistance"] = 0.f;
    clip["levelCount"] = levelCount;
    clip["dynamicLevelCount"] = LumenGI::GlobalDistanceField::kDynamicLevels;
    clip["resolution"] = R;
    clip["instanceCount"] = 0; // not used by the trace pass.
    clip["dirtyRegionCount"] = 0;
    clip["frameIndex"] = mFrameIndex;
    const LumenGI::GlobalDistanceField::index3 scroll = mSDF.gdf->scrollFromCameraMove();
    clip["scroll"] = int3(scroll.x, scroll.y, scroll.z);
    cb["gCameraPosW"] = camPos;
    cb["gCameraFocalPx"] = focalLengthPx;
    cb["gPrincipalPoint"] = float2(0.5f * (float)mFrameDim.x, 0.5f * (float)mFrameDim.y);
    cb["gFrameDim"] = mFrameDim;
    cb["gMaxSteps"] = mGDFTraceMaxSteps;
    cb["gCameraRightW"] = camRight;
    cb["gCameraUpW"] = camUp;
    cb["gCameraForwardW"] = camForward;
    cb["gTraceMaxDistance"] = mGDFTraceMaxDistance;

    var["gGDFLevelTable"] = mSDF.pLevelTable;
    for (uint32_t m = 0; m < kLumenGDFMaxLevelsHost; ++m)
        var["gGDFLevels"][m] = mSDF.levels[std::min<uint32_t>(m, levelCount - 1u)];
    if (renderData.getTexture("linearZ"))
        var["gLinearZ"] = renderData.getTexture("linearZ");
    if (primary)
    {
        var["gDiffuseGI"] = renderData.getTexture("diffuseGI");
        var["gDiffuseRadianceHitDist"] = renderData.getTexture("diffuseRadianceHitDist");
        var["gConfidence"] = renderData.getTexture("confidence");
    }
    if (hasDebug)
        var["gGDFTraceDebug"] = renderData.getTexture(kGDFTrace);
    var["gGDFTraceStats"] = mSDF.pTraceStats;

    mSDF.pTrace->execute(
        pRenderContext,
        ((mFrameDim.x + 7u) / 8u) * 8u,
        ((mFrameDim.y + 7u) / 8u) * 8u,
        1
    );

    if (mSDF.pTraceStats && mSDF.pTraceStatsReadback)
    {
        pRenderContext->copyResource(mSDF.pTraceStatsReadback.get(), mSDF.pTraceStats.get());
        mSDF.traceStatsReadbackPending = true;
    }
}

void LumenGIPass::readbackGDFTraceStats(RenderContext* pRenderContext)
{
    if (!mSDF.traceStatsReadbackPending || !mSDF.pTraceStatsReadback)
        return;
    mSDF.traceStatsReadbackPending = false;
    const uint32_t* pData = static_cast<const uint32_t*>(mSDF.pTraceStatsReadback->map());
    if (!pData)
        return;
    for (uint32_t i = 0; i < kGDFTraceStatCount && i < mSDF.traceStats.size(); ++i)
        mSDF.traceStats[i] = pData[i];
    mSDF.pTraceStatsReadback->unmap();
}

std::map<std::string, double> LumenGIPass::getGDFStats() const
{
    std::map<std::string, double> stats;
    stats["active"] = (mUseGDF || mTraceMode != TraceMode::HardwareRT) ? 1.0 : 0.0;
    stats["traceMode"] = static_cast<double>(static_cast<uint32_t>(mTraceMode));
    stats["useGDF"] = mUseGDF ? 1.0 : 0.0;

    if (mSDF.pScene)
    {
        const LumenGI::MeshSDF::Scene::LumenMeshSDFSceneStats& s = mSDF.sceneStats;
        stats["registeredMeshes"] = (double)s.registeredMeshes;
        stats["activeInstances"] = (double)s.activeInstances;
        stats["residentInstances"] = (double)s.residentInstances;
        stats["evictedInstances"] = (double)s.evictedInstances;
        stats["cacheLookups"] = (double)s.cacheLookups;
        stats["cacheHits"] = (double)s.cacheHits;
        stats["cacheMisses"] = (double)s.cacheMisses;
        stats["corruptionsDetected"] = (double)s.corruptionsDetected;
        stats["builds"] = (double)s.builds;
        stats["conversions"] = (double)s.conversions;
        stats["evictions"] = (double)s.evictions;
        stats["restores"] = (double)s.restores;
        stats["estimatedGpuBytes"] = (double)s.estimatedGpuBytes;
        stats["residentBytes"] = (double)s.residentBytes;
        stats["budgetBytes"] = (double)s.budgetBytes;
    }

    if (mSDF.gdf)
    {
        stats["gdfLevelCount"] = (double)mSDF.gdf->getLevelCount();
        stats["gdfResolution"] = (double)mSDF.gdf->getResolution();
        stats["gdfBaseExtent"] = (double)mSDF.gdf->getBaseExtent();
    }

    const uint32_t traced = mSDF.traceStats[0];
    stats["sphereTraced"] = (double)traced;
    stats["sphereHit"] = (double)mSDF.traceStats[1];
    stats["sphereMiss"] = (double)mSDF.traceStats[2];
    stats["sphereMaxSteps"] = (double)mSDF.traceStats[3];
    stats["sphereNoGrid"] = (double)mSDF.traceStats[4];
    stats["sphereHitRate"] = traced > 0 ? (double)mSDF.traceStats[1] / (double)traced : 0.0;
    return stats;
}
