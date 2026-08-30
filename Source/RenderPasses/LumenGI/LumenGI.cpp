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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
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
const char kPageClearShaderFile[] = "RenderPasses/LumenGI/Capture/LumenSurfaceCachePageClear.cs.slang";
const char kCacheLightingShaderFile[] = "RenderPasses/LumenGI/Lighting/LumenSurfaceCacheLighting.cs.slang";
const char kHZBBuildShaderFile[] = "RenderPasses/LumenGI/ScreenTrace/LumenHZBBuild.cs.slang";
const char kScreenTraceShaderFile[] = "RenderPasses/LumenGI/ScreenTrace/LumenScreenTrace.cs.slang";
const char kScreenProbeShaderFile[] = "RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeTrace.cs.slang";
const char kScreenRadianceHistoryShaderFile[] = "RenderPasses/LumenGI/ScreenProbe/LumenScreenRadianceHistory.cs.slang";
const char kRadianceCacheShaderFile[] = "RenderPasses/LumenGI/RadianceCache/LumenRadianceCacheInterpolate.cs.slang";
const char kRadianceCacheTraceShaderFile[] = "RenderPasses/LumenGI/RadianceCache/LumenRadianceCacheTrace.cs.slang";
constexpr uint32_t kLumenRadianceCacheLevelStride = 8u;
const char kScreenProbeIntegrateShaderFile[] = "RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeIntegrate.cs.slang";
const char kScreenProbeInterpolateShaderFile[] = "RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeInterpolate.cs.slang";
const char kTemporalFilterShaderFile[] = "RenderPasses/LumenGI/Temporal/LumenTemporalFilter.cs.slang";
const char kSpatialFilterShaderFile[] = "RenderPasses/LumenGI/Spatial/LumenSpatialFilter.cs.slang";
const char kFinalResolveShaderFile[] = "RenderPasses/LumenGI/Resolve/LumenFinalResolve.cs.slang";

///< S5-B1 LumenTemporalFilterCB fields that are not exposed as tunable members; defaults frozen
///< with Z5's LumenTemporalFilterData.slang comments (all but the last three are inert while the
///< corresponding optional validation inputs are unbound).
constexpr float kTemporalClampBoxMargin = 0.0f;      ///< gClampBoxMargin (fraction of the neighborhood range).
constexpr float kTemporalNormalCosMin = 0.8f;        ///< gNormalCosMin (UE-style 45 degree rejection floor).
constexpr float kTemporalNormalExponent = 8.0f;      ///< gNormalExponent (UE-style normal affinity).
constexpr float kTemporalMaterialMismatchWeight = 0.05f; ///< gMaterialMismatchWeight.
constexpr float kTemporalHitDistanceThreshold = 0.5f; ///< gHitDistanceThreshold (m; no hit-distance input in the MVP).
///< gConfidenceWeight (confidence gating strength in wConf). The current 0.2 value is deliberately
///< conservative: the S4.3 confidence remains low on sparse-probe misses, so a value of 1.0 would
///< reject nearly all history. Spatial now receives the separate temporalConfidence channel, which
///< allows a future preset sweep (0.25 -> 0.5 -> 1.0) after producer-validity telemetry lands.
constexpr float kTemporalConfidenceWeight = 0.2f;
///< gFireflyMaxRadiance; mirrors the kLumenGIMaxRadiance default in LumenGIData.slang / the
///< LUMEN_GI_MAX_RADIANCE fallback in LumenTemporalFilterData.slang.
constexpr float kTemporalFireflyMaxRadiance = 10.f;

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

// C1 production variant: the environment importance sampler is bound through a
// ParameterBlock<EnvMapSampler>. The uniform-sphere path remains available only
// when the scene has no environment sampler; toggling the resource shape requires
// rebuilding the ComputePass so the D3D12 root signature stays stable.
constexpr bool kUseCacheLightingEnvImportanceSampler = true;

const char kEnabled[] = "enabled";
const char kTraceMode[] = "traceMode";
const char kQualityPreset[] = "qualityPreset";
const char kDebugMode[] = "debugMode";
const char kUseSurfaceCache[] = "useSurfaceCache";
const char kUseCacheLighting[] = "useCacheLighting";
const char kUseCacheCardGrid[] = "useCacheCardGrid";
const char kCacheLightingFeedback[] = "cacheLightingFeedback";
const char kCacheLightingFeedbackStrength[] = "cacheLightingFeedbackStrength";
const char kCacheLightingFeedbackMaxBounces[] = "cacheLightingFeedbackMaxBounces";
const char kUseScreenTrace[] = "useScreenTrace";
const char kUseScreenProbes[] = "useScreenProbes";
const char kProbeDirectionsPerProbe[] = "probeDirectionsPerProbe";
const char kProbeMaxProbesPerFrame[] = "probeMaxProbesPerFrame";
const char kUseTemporalFilter[] = "useTemporalFilter";
const char kUseScreenRadianceMoments[] = "useScreenRadianceMoments";
const char kTemporalHistoryLengthCap[] = "temporalHistoryLengthCap";
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
const char kGDFDiagnosticStage[] = "gdfDiagnosticStage";

// S6 shader files.
const char kGDFComposeShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFCompose.cs.slang";
const char kGDFComposeDiagShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiag.cs.slang";
const char kGDFComposeDiagAllShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiagAll.cs.slang";
const char kGDFComposeDiagBuffersShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiagBuffers.cs.slang";
const char kGDFComposeDiagAtlasShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiagAtlas.cs.slang";
const char kGDFComposeDiagBuffersScalarShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiagBuffersScalar.cs.slang";
const char kGDFComposeDiagCBScalarShaderFile[] = "RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiagCBScalar.cs.slang";
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
    { "probeHistory",                  "gProbeHistory",                  "C7 diagnostic probe history mirror: RGB=running incident-irradiance mean, A=accumulated traced-direction count.", true, ResourceFormat::RGBA16Float },
    { "temporalFiltered",              "gTemporalOutput",                "S5-B1 temporal filter: RGB=temporally filtered incident irradiance, A=NEW history length (capped). S5 main output.", true, ResourceFormat::RGBA16Float },
    { "temporalAlpha",                 "gTemporalAlpha",                 "S5-B1 effective EMA alpha (1 = full reject / reset). Accept/reject cross-check.", true, ResourceFormat::R32Float },
    { "temporalConfidence",            "gTemporalConfidence",            "S5-B1 updated confidence; input to the S5-B2 spatial filter.", true, ResourceFormat::R32Float },
    { "temporalMoments",               "gTemporalMoments",               "C8 diagnostic running luminance moments: R=mean, G=mean square.", true, ResourceFormat::RG32Float },
    { "screenRadianceLightingGeneration", "gScreenRadianceLightingGeneration", "C7 diagnostic screen-radiance lighting epoch (R32Uint; zero means invalid).", true, ResourceFormat::R32Uint },
    { "screenRadianceHistoryAge",      "gScreenRadianceHistoryAge",      "A2 diagnostic raw screen-radiance history age (R32Uint; zero means invalid/reset).", true, ResourceFormat::R32Uint },
    { "screenRadianceHistoryValidity", "gScreenRadianceHistoryValidity", "A2 diagnostic raw screen-radiance validity sidecar (R32Uint; 1 means valid, 0 means reset/miss).", true, ResourceFormat::R32Uint },
    { "radianceCache",                 "gRadianceCacheOutput",            "C10 GPU Radiance Cache output: RGB incident radiance, A confidence/validity.", true, ResourceFormat::RGBA16Float },
    { "radianceCacheHitDist",          "gRadianceCacheHitDist",             "C10 GPU Radiance Cache hit-distance output: RGB incident radiance, A hit distance.", true, ResourceFormat::RGBA16Float },
    { "radianceCacheValidity",         "gRadianceCacheValidityOutput",      "C10 GPU Radiance Cache validity bitmask: hit/sky/radiance/producer bits; zero means invalid.", true, ResourceFormat::R32Uint },
    { "roughSpecularIndirect",         "gRoughSpecularIndirect",             "E1 diagnostic rough-specular indirect radiance; disabled until directional producer is bound.", true, ResourceFormat::RGBA16Float },
    { "roughSpecularValidity",         "gRoughSpecularValidity",             "E1 rough-specular validity bitmask; diagnostic only.", true, ResourceFormat::R32Uint },
    { "transmissionIndirect",           "gTransmissionIndirect",              "E1 reference-only transmission radiance; disabled until medium-aware producer is bound.", true, ResourceFormat::RGBA16Float },
    { "transmissionValidity",           "gTransmissionValidityOutput",        "E1 transmission validity bitmask; reference-only diagnostic.", true, ResourceFormat::R32Uint },
    { "spatialFiltered",               "gSpatialOutput",                 "S5-B2 spatial filter: RGB=variance-guided filtered incident irradiance, A=filtered confidence. Consumes temporalFiltered + temporalConfidence.", true, ResourceFormat::RGBA16Float },
    { "filteredVariance",              "gFilteredVariance",               "C8 diagnostic combined temporal/spatial luminance variance.", true, ResourceFormat::R32Float },
    { "resolvedDiffuseGI",             "gResolvedDiffuseGI",              "C9 final resolve: filtered incident irradiance modulated by diffuse reflectance / PI, or HWRT radiance passthrough.", true, ResourceFormat::RGBA16Float },
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

/// Legacy CPU helper for the source R8_SNORM cache codec. Runtime atlas staging now uploads
/// normalized floats into R32Float views, so this helper is retained only for diagnostics.
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
    // Scriptable S4/C2/C7 probe-resource and counter snapshot.
    pass.def_property_readonly("screenProbeStats", &LumenGIPass::getScreenProbeStats);
    // C10 CPU clipmap preparation snapshot. GPU channels are intentionally separate
    // and are not reported as produced until the bounded trace/commit path exists.
    pass.def_property_readonly("radianceCacheStats", &LumenGIPass::getRadianceCacheStats);
    // C11 effective preset/derived configuration snapshot for hot-switch validation.
    pass.def_property_readonly("qualityPresetStats", &LumenGIPass::getQualityPresetStats);
    // C6 card-specific request -> capture -> ready -> hit provenance.  This is kept
    // separate from the aggregate surfaceCacheStats map so a validator cannot infer
    // ownership or frame ordering from unrelated cumulative counters.
    pass.def_property_readonly("surfaceCacheEvents", &LumenGIPass::getSurfaceCacheEvents);
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

    parseProperties(props, true);

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

void LumenGIPass::setProperties(const Properties& props)
{
    // A graph update supplies only the fields that changed.  Apply the preset
    // defaults only when the update explicitly changes qualityPreset; partial
    // updates must preserve the other derived values and user overrides.
    parseProperties(props, props.has(kQualityPreset));
    mOptionsChanged = true;
    resetHistory(HistoryResetReason::SceneChange);
}

void LumenGIPass::parseProperties(const Properties& props, bool applyPresetDefaults)
{
    if (applyPresetDefaults)
    {
        // Resolve the quality preset before applying its defaults. Every derived value remains
        // explicitly overridable through Properties, so a graph can pin one dimension while still
        // using the preset for the rest. The mapping is intentionally monotonic: higher presets
        // spend more probe directions, history capacity, cache-lighting work and GDF trace budget;
        // it never enables a producer that the graph did not request.
        const QualityPreset requestedPreset = props.has(kQualityPreset) ? props[kQualityPreset] : mQualityPreset;
        mQualityPreset = requestedPreset;
        struct PresetDefaults
        {
            uint32_t probeDirections;
            uint32_t capturePages;
            uint32_t cacheBounces;
            float spatialRadiusMin;
            float spatialRadiusMax;
            uint32_t spatialNeighborhood;
            float temporalHistoryCap;
            uint32_t gdfTraceSteps;
            float gdfTraceDistance;
            uint32_t meshSDFResolution;
            uint32_t meshSDFQuality;
        } defaults{};
        switch (requestedPreset)
        {
        case QualityPreset::Low:
            defaults = {8u, 16u, 1u, 1.0f, 2.0f, 1u, 4.0f, 32u, 10.0f, 32u, 1u};
            break;
        case QualityPreset::Medium:
            defaults = {16u, 32u, 2u, 1.5f, 3.0f, 1u, 6.0f, 48u, 15.0f, 40u, 0u};
            break;
        case QualityPreset::High:
            defaults = {32u, 64u, 4u, 2.0f, 4.0f, 2u, 10.0f, 64u, 20.0f, 48u, 0u};
            break;
        case QualityPreset::Reference:
            defaults = {64u, 128u, 8u, 2.0f, 5.0f, 2u, 16.0f, 96u, 40.0f, 64u, 0u};
            break;
        default:
            defaults = {32u, 64u, 4u, 2.0f, 4.0f, 2u, 10.0f, 64u, 20.0f, 48u, 0u};
            break;
        }
        if (!props.has(kProbeDirectionsPerProbe))
            mProbeDirectionsPerProbe = std::clamp<uint32_t>(defaults.probeDirections, 1u, LumenScreenProbe::kMaxDirectionsPerProbe);
        if (!props.has(kCaptureMaxPagesPerFrame))
            mCaptureMaxPagesPerFrame = defaults.capturePages;
        if (!props.has(kCacheLightingFeedbackMaxBounces))
            mCacheLightingFeedbackMaxBounces = defaults.cacheBounces;
        if (!props.has(kSpatialRadiusMin))
            mSpatialRadiusMin = defaults.spatialRadiusMin;
        if (!props.has(kSpatialRadiusMax))
            mSpatialRadiusMax = defaults.spatialRadiusMax;
        if (!props.has(kSpatialNeighborhoodRadius))
            mSpatialNeighborhoodRadius = std::clamp<uint32_t>(defaults.spatialNeighborhood, 1u, 2u);
        if (!props.has(kTemporalHistoryLengthCap))
            mTemporalHistoryLengthCap = defaults.temporalHistoryCap;
        if (!props.has(kGDFTraceMaxSteps))
            mGDFTraceMaxSteps = defaults.gdfTraceSteps;
        if (!props.has(kGDFTraceMaxDistance))
            mGDFTraceMaxDistance = defaults.gdfTraceDistance;
        if (!props.has(kMeshSDFResolution))
            mMeshSDFResolution = std::max<uint32_t>(defaults.meshSDFResolution, 8u);
        if (!props.has(kMeshSDFQuality))
            mMeshSDFQuality = defaults.meshSDFQuality;
    }

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
        else if (key == kUseCacheCardGrid)
            mUseCacheCardGrid = value;
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
        else if (key == kUseScreenRadianceMoments)
            mUseScreenRadianceMoments = value;
        else if (key == kTemporalHistoryLengthCap)
            mTemporalHistoryLengthCap = std::clamp<float>(value, 1.0f, 64.0f);
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
        else if (key == kGDFDiagnosticStage)
        {
            const uint32_t v = value;
            mGDFDiagnosticStage = std::min<uint32_t>(v, 6u);
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
    props[kUseCacheCardGrid] = mUseCacheCardGrid;
    props[kCacheLightingFeedback] = mCacheLightingFeedbackEnabled;
    props[kCacheLightingFeedbackStrength] = mCacheLightingFeedbackStrength;
    props[kCacheLightingFeedbackMaxBounces] = mCacheLightingFeedbackMaxBounces;
    props[kUseScreenTrace] = mUseScreenTrace;
    props[kUseScreenProbes] = mUseScreenProbes;
    props[kProbeDirectionsPerProbe] = mProbeDirectionsPerProbe;
    props[kProbeMaxProbesPerFrame] = mProbeMaxProbesPerFrame;
    props[kUseTemporalFilter] = mUseTemporalFilter;
    props[kUseScreenRadianceMoments] = mUseScreenRadianceMoments;
    props[kTemporalHistoryLengthCap] = mTemporalHistoryLengthCap;
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
    props[kGDFDiagnosticStage] = mGDFDiagnosticStage;
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
    auto& cacheCapture = reflector.addOutput(kCacheCaptureRadiance, "Surface cache capture radiance atlas before lighting");
    cacheCapture.texture2D(mAtlasSizeTexels, mAtlasSizeTexels);
    cacheCapture.format(ResourceFormat::RGBA16Float);
    cacheCapture.bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget);
    cacheCapture.flags(RenderPassReflection::Field::Flags::Optional);

    // C7 producer validity sidecar: one packed uint4 per probe direction. The buffer is
    // optional and only allocated by validity/convergence graphs; production GI never depends
    // on the diagnostic readback. x packs backend/geometry/radiance/reset bits, y producer frame,
    // z history generation, and w probe age.
    const uint64_t probeCount = ((uint64_t)compileData.defaultTexDims.x + LumenScreenProbe::kTileSize - 1u) /
        LumenScreenProbe::kTileSize * (((uint64_t)compileData.defaultTexDims.y + LumenScreenProbe::kTileSize - 1u) /
        LumenScreenProbe::kTileSize);
    const uint64_t validityBytes = probeCount * LumenScreenProbe::kMaxDirectionsPerProbe * sizeof(uint4);
    FALCOR_ASSERT(validityBytes <= std::numeric_limits<uint32_t>::max());
    auto& validity = reflector.addOutput("probeValidity", "C7 per-direction producer validity sidecar");
    validity.rawBuffer((uint32_t)validityBytes);
    validity.bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    validity.flags(RenderPassReflection::Field::Flags::Optional);
    return reflector;
}

void LumenGIPass::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    if (any(mFrameDim != compileData.defaultTexDims))
    {
        mFrameDim = compileData.defaultTexDims;
        resetHistory(HistoryResetReason::Resize);
    }
}

void LumenGIPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Surface Cache publication/request fences use the scheduler clock, not
    // the GI history clock. resetHistory() may rewind mFrameIndex on material,
    // geometry, or camera invalidation while the page scheduler remains live.
    mSurfaceCacheFrameIndex = static_cast<uint32_t>(mCaptureScheduler.getFrameIndex());
    mHistoryResetThisFrame = false;
    mSurfaceCacheRequestRawThisFrame = 0;
    mSurfaceCacheRequestCardsThisFrame = 0;
    mSurfaceCacheRequestCaptureCompletedThisFrame = 0;
    mSurfaceCachePageMetadataPendingThisFrame = 0;
    mSurfaceCachePageMetadataReadyThisFrame = 0;
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
        resetHistory(HistoryResetReason::Resize);
    }

    // Snapshot the accumulated scene updates before they are consumed below: both the
    // existing trace path and the S2 capture path need the same flag value.
    const IScene::UpdateFlags sceneUpdates = mSceneUpdates;
    if (mSceneUpdates != IScene::UpdateFlags::None)
    {
        const bool lightingChanged =
            is_set(mSceneUpdates, IScene::UpdateFlags::LightsMoved) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::LightIntensityChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::LightPropertiesChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::LightCollectionChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::MaterialsChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::EmissiveMaterialsChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::EnvMapChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::EnvMapPropertiesChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::LightCountChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::RenderSettingsChanged);
        if (lightingChanged)
            ++mLightingGeneration;
        if (is_set(mSceneUpdates, IScene::UpdateFlags::EnvMapChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::EnvMapPropertiesChanged))
        {
            // The importance map is built from the scene's current env map;
            // discard both it and the reflected variant before rebinding.
            mpEnvMapSampler = nullptr;
            mCacheLighting.pPass = nullptr;
            mCacheLighting.envSamplerVariant = false;
        }
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
            resetHistory(HistoryResetReason::SceneChange);

        // MeshSDF/GDF resources are scene-geometry scoped. Keeping the CPU scene, instance
        // table, or composed clipmap across a geometry update can make a static camera continue
        // tracing the old geometry. Rebuild lazily on the next GDF frame; material/light-only
        // updates keep the distance field and only invalidate lighting/history domains.
        const bool geometryChanged =
            is_set(mSceneUpdates, IScene::UpdateFlags::GeometryChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::MeshesChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::GeometryMoved) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::SceneGraphChanged) ||
            is_set(mSceneUpdates, IScene::UpdateFlags::RecompileNeeded);
        if (geometryChanged && (mUseGDF || mTraceMode != TraceMode::HardwareRT))
            invalidateMeshSDF();
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
            resetHistory(HistoryResetReason::CameraCut); // hard reset: clear the prev history/depth double buffer on the cut frame.
        mPrevCameraPosition = camPos;
    }

    // C10 preparation: keep the reusable CPU clipmap scheduler alive and feed it the
    // snapped camera position. This is deliberately bookkeeping-only; it does not
    // replace HWRT radiance or expose a fake radianceCache render output.
    if (mUseRadianceCache && mpScene)
    {
        if (!mRadianceCache)
            mRadianceCache = std::make_unique<LumenRadianceCache>();
        if (mRadianceCacheResetPending || mHistoryResetThisFrame || sceneUpdates != IScene::UpdateFlags::None)
        {
            mRadianceCache->reset();
            if (mRadianceCacheGpu.generation == std::numeric_limits<uint32_t>::max())
                mRadianceCacheGpu.generation = 1u;
            else
                ++mRadianceCacheGpu.generation;
            mRadianceCacheGpu.currIndex = 0u;
            mRadianceCacheGpu.lastReadyFrame = 0u;
            mRadianceCacheGpu.producedThisFrame = false;
            mRadianceCacheGpu.queryAttempts = 0u;
            mRadianceCacheGpu.queryHits = 0u;
            mRadianceCacheGpu.queryMisses = 0u;
            mRadianceCacheGpu.queryCountersSubmittedFrame = 0u;
            mRadianceCacheGpu.queryCountersFrame = 0u;
            mRadianceCacheGpu.queryCountersReadbackPending = false;
            mRadianceCacheGpu.levelQueryCountersSubmittedFrame = 0u;
            mRadianceCacheGpu.levelQueryCountersFrame = 0u;
            mRadianceCacheGpu.levelQueryCountersReadbackPending = false;
            mRadianceCacheGpu.levelQueryAttempts.fill(0u);
            mRadianceCacheGpu.levelQueryHits.fill(0u);
            mRadianceCacheGpu.levelQueryMisses.fill(0u);
            mRadianceCacheGpu.levelSampleCount.fill(0u);
            mRadianceCacheGpu.levelValidHitDistanceCount.fill(0u);
            mRadianceCacheGpu.levelFallbackSampleCount.fill(0u);
            mRadianceCacheGpu.levelProjectedProbeCount.fill(0u);
            mRadianceCacheGpu.levelInBoundsProbeCount.fill(0u);
            if (mRadianceCacheGpu.pQueryCounters)
                pRenderContext->clearUAV(mRadianceCacheGpu.pQueryCounters->getUAV().get(), uint4(0u));
            if (mRadianceCacheGpu.pLevelQueryCounters)
                pRenderContext->clearUAV(mRadianceCacheGpu.pLevelQueryCounters->getUAV().get(), uint4(0u));
            if (mRadianceCacheGpu.pProbeRadiance[0])
                pRenderContext->clearUAV(mRadianceCacheGpu.pProbeRadiance[0]->getUAV().get(), float4(0.f));
            if (mRadianceCacheGpu.pProbeRadiance[1])
                pRenderContext->clearUAV(mRadianceCacheGpu.pProbeRadiance[1]->getUAV().get(), float4(0.f));
            if (mRadianceCacheGpu.pProbeValidity[0])
                pRenderContext->clearUAV(mRadianceCacheGpu.pProbeValidity[0]->getUAV().get(), uint4(0u));
            if (mRadianceCacheGpu.pProbeValidity[1])
                pRenderContext->clearUAV(mRadianceCacheGpu.pProbeValidity[1]->getUAV().get(), uint4(0u));
            mRadianceCacheResetPending = false;
        }
        const float3 cacheCamera = mpScene->getCamera()->getPosition();
        mRadianceCache->setCamera(LumenRadianceCache::float3(cacheCamera.x, cacheCamera.y, cacheCamera.z));
        mRadianceCache->tick();
    }

    clearOutputs(pRenderContext, renderData);
    mScreenProbes.producedThisFrame = false;
    mTemporalFilter.producedThisFrame = false;
    mSpatialFilter.producedThisFrame = false;
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

    // S6 fallback contract: the current GDF shader produces geometry distance/debug
    // data, not material-lighted radiance. Keep the HWRT radiance path alive for
    // MeshSDF/Hybrid until a real GDF hit-lighting router is available; this avoids
    // turning a valid trace mode into a black frame.
    constexpr bool kGDFProvidesDiffuseRadiance = false;
    const bool sdfPrimaryPath = (mTraceMode == TraceMode::MeshSDF) && kGDFProvidesDiffuseRadiance;
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
    // Consume the previous frame's validated GPU page-hit feedback before the scheduler runs,
    // so touchPage() updates LRU/last-used ordering before this frame's request/capture budget
    // is selected. This is intentionally outside runScreenProbeTrace(), which executes after
    // capture and would be one phase too late for demand scheduling.
    readbackScreenProbeCounters(pRenderContext);
    // Requested pages carry a readyFrame fence in page metadata.  Count readiness
    // only for pages that came from the deferred request set; initial resident pages
    // must not make an unrelated request look like same-frame publication.
    for (auto it = mSurfaceCachePendingReadyPages.begin(); it != mSurfaceCachePendingReadyPages.end();)
    {
        const uint32_t pageID = *it;
        if (pageID < mPageMetadataData.size() && mPageMetadataData[pageID].x != 0u &&
            mPageMetadataData[pageID].z <= mSurfaceCacheFrameIndex)
        {
            ++mSurfaceCachePageMetadataReadyThisFrame;
            const uint32_t readyGeneration = mPageMetadataData[pageID].x;
            for (auto eventIt = mSurfaceCacheRequestEvents.rbegin();
                 eventIt != mSurfaceCacheRequestEvents.rend(); ++eventIt)
            {
                if (eventIt->sceneGeneration == mSurfaceCacheSceneGeneration &&
                    eventIt->pageID == pageID && eventIt->generation == readyGeneration &&
                    eventIt->captureFrame != 0u && eventIt->readyFrame == 0u &&
                    eventIt->captureFrame < mSurfaceCacheFrameIndex)
                {
                    eventIt->readyFrame = mSurfaceCacheFrameIndex;
                    eventIt->state = 3u;
                    break;
                }
            }
            it = mSurfaceCachePendingReadyPages.erase(it);
        }
        else
            ++it;
    }
    if (mUseSurfaceCache)
        runSurfaceCacheCapture(pRenderContext, sceneUpdates);

    // S3: Surface Cache direct lighting (S3-B1). Runs AFTER the capture pass so this frame's
    // freshly captured pages are lit immediately. Data dependency: the radiance atlas is only
    // consumed by later stages (S3-B2 feedback, S4 cache queries) in FUTURE frames; the screen
    // trace neither writes nor reads it, so placement after the trace/capture is safe either way.
    // Requires useSurfaceCache (the atlases and card->page mirror only exist then).
    if (mUseCacheLighting && mUseSurfaceCache)
    {
        // Capture writes these atlas resources before cache lighting reads and, for the
        // radiance atlas, writes them again.  They are pass-owned resources rather than
        // graph edges, so make the producer/consumer UAV dependency explicit.  Without this
        // barrier a second LumenGI instance can observe an incompletely published tile even
        // when its host page/card tables are byte-identical to the first instance.
        if (mCapture.pMaterialAtlas)
            pRenderContext->uavBarrier(mCapture.pMaterialAtlas.get());
        if (mCapture.pMetadataAtlas)
            pRenderContext->uavBarrier(mCapture.pMetadataAtlas.get());
        if (mCapture.pRadianceAtlas)
            pRenderContext->uavBarrier(mCapture.pRadianceAtlas.get());
        exportCacheCaptureRadiance(pRenderContext, renderData);
        runCacheLighting(pRenderContext);
        // Cache lighting writes the radiance/visibility atlases through UAVs.  The direct
        // diagnostic blit and the later ScreenProbe lookup consume those same resources as
        // SRVs; publish the compute results before either consumer.  Without this edge the
        // paired full-scan/grid passes can observe different subsets of the just-lit pages.
        if (mCapture.pRadianceAtlas)
            pRenderContext->uavBarrier(mCapture.pRadianceAtlas.get());
        if (mCapture.pCaptureOrderAtlas)
            pRenderContext->uavBarrier(mCapture.pCaptureOrderAtlas.get());
        if (mCacheLighting.pVisibilityAtlas)
            pRenderContext->uavBarrier(mCacheLighting.pVisibilityAtlas.get());
    }
    else
    {
        exportCacheCaptureRadiance(pRenderContext, renderData);
    }

    // Scriptable S3 gate channel: copy the internal radiance atlas into the optional graph
    // output (atlas-sized) so tests/lumengi can read the cache-direct radiance directly.
    exportCacheDirectRadiance(pRenderContext, renderData);

    // S6: build/trace the GDF before Screen Probe integration so backend hits are
    // available to the probe router. MeshSDF still falls back to HWRT for radiance
    // until a material-lighting contract exists; GDF remains a backend diagnostic.
    if (mUseGDF)
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

    // C10 bounded source-backed cache producer. It publishes generation/ready-
    // validated cache samples and exposes them to final resolve as a fallback;
    // the persistent UE-style allocator/query lifecycle remains a follow-up wave.
    if (mUseRadianceCache)
        runRadianceCache(pRenderContext, renderData);

    // E1 diagnostic endpoints stay separate from the diffuse resolve. They
    // are no-ops unless a graph explicitly allocates the channels; their
    // compile-time production switches remain disabled until the directional
    // and medium-aware producers are integrated.
    runRoughSpecularDiagnostic(pRenderContext, renderData);
    runTransmissionDiagnostic(pRenderContext, renderData);

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

    // C9: resolve the selected production irradiance chain back into the public
    // diffuseGI contract. This is intentionally after all optional producers,
    // including GDF fallback/debug work, so graph consumers see one stable output.
    runFinalResolve(pRenderContext, renderData);

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
    const QualityPreset previousPreset = mQualityPreset;
    dirty |= widget.checkbox("Enabled", mEnabled);
    dirty |= widget.dropdown("Trace mode", mTraceMode);
    dirty |= widget.dropdown("Quality preset", mQualityPreset);
    dirty |= widget.dropdown("Debug output", mDebugMode);

    // The preset dropdown mutates the enum directly. Re-run the same derived-default
    // transaction used by RenderGraph::updatePass() so UI hot-switches are real runtime
    // changes rather than a label-only update. Explicit per-property edits remain intact
    // because parseProperties() applies defaults before the property loop.
    if (mQualityPreset != previousPreset)
    {
        Properties presetProps;
        presetProps[kQualityPreset] = mQualityPreset;
        parseProperties(presetProps, true);
        dirty = true;
    }

    if (auto group = widget.group("Features", true))
    {
        dirty |= group.checkbox("Surface cache", mUseSurfaceCache);
        dirty |= group.checkbox("Cache lighting", mUseCacheLighting);
        dirty |= group.checkbox("Screen trace", mUseScreenTrace);
        dirty |= group.checkbox("Screen probes", mUseScreenProbes);
        dirty |= group.checkbox("Temporal filter", mUseTemporalFilter);
        dirty |= group.checkbox("Screen-radiance moments", mUseScreenRadianceMoments);
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
        resetHistory(HistoryResetReason::SceneChange);
    }
}

void LumenGIPass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mUpdateFlagsConnection = {};
    mSceneUpdates = IScene::UpdateFlags::None;
    mpScene = pScene;
    // The CPU Radiance Cache is scene-owned bookkeeping even before the GPU
    // producer lands. Drop its clipmap slots and last-used state at the scene
    // boundary so a later enable cannot query stale keys from the previous TLAS.
    if (mRadianceCache)
        mRadianceCache->reset();
    mRadianceCacheResetPending = true;
    // Keep the allocator's page generations scene-local (reset() clears them), but expose a
    // monotonic scene epoch for stale-history/page-lifecycle validation and reload gates.
    if (mSurfaceCacheSceneGeneration == std::numeric_limits<uint32_t>::max())
        mSurfaceCacheSceneGeneration = 1u;
    else
        ++mSurfaceCacheSceneGeneration;
    if (mSurfaceCacheResetCount != std::numeric_limits<uint32_t>::max())
        ++mSurfaceCacheResetCount;
    ++mLightingGeneration;
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
    resetHistory(HistoryResetReason::SetScene);

    // S2: rebuild the card scene and reset the CPU components. invalidateCaptureResources()
    // drops every GPU resource that references the old scene's mesh buffers (atlases persist:
    // they are fixed-size and their contents are re-captured). The scheduler releases all
    // pages it holds; the cache reset afterwards is a no-op for those pages, so both orders
    // are safe (the scheduler contract allows either).
    invalidateCaptureResources();
    mpCardScene = pScene ? std::make_unique<LumenCardScene>(pScene) : nullptr;
    mPageCache.reset();
    mSurfaceCacheRequestedCards.clear();
    mSurfaceCacheDeferredRequestCards.clear();
    mSurfaceCacheDeferredRequestFrameByCard.clear();
    mSurfaceCachePendingReadyPages.clear();
    mSurfaceCacheRequestEvents.clear();
    mSurfaceCacheRequestEventSequence = 0;
    mSurfaceCacheRequestEventDropped = 0;
    mSurfaceCacheDeferredRequestFrame = 0;
    mSurfaceCacheFrameIndex = 0;
    mScreenProbeCountersSubmittedFrame = 0;
    mScreenProbes.cacheFeedbackReadbackPending = false;
    mScreenProbes.cacheFeedbackSubmittedFrame = 0;
    mScreenProbes.cacheRequestReadbackPending = false;
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
        // Shader reloads invalidate producer layouts and any future RC payload
        // ABI. Keep the CPU scheduler deterministic until the next frame rebuild.
        if (mRadianceCache)
            mRadianceCache->reset();
        mRadianceCacheResetPending = true;
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
        mFinalResolve.pPass = nullptr;     // C9 pure compute pass; recreated lazily.
        mSDF.pCompose = nullptr;           // S6: pure compute, no scene deps; recreated lazily.
        mSDF.pComposeDiag = nullptr;       // C4 diagnostic pass; recreated lazily.
        mSDF.pComposeDiagAll = nullptr;    // C4 diagnostic pass; recreated lazily.
        mSDF.pComposeDiagBuffers = nullptr; // C4 E2a diagnostic pass.
        mSDF.pComposeDiagAtlas = nullptr;   // C4 E2b diagnostic pass.
        mSDF.pComposeDiagBuffersScalar = nullptr; // C4 E2c diagnostic pass.
        mSDF.pComposeDiagCBScalar = nullptr; // C4 E2d diagnostic pass.
        mSDF.pTrace = nullptr;
        resetHistory(HistoryResetReason::HotReload);
    }
}

void LumenGIPass::resetHistory(HistoryResetReason reason)
{
    mFrameIndex = 0;
    // Page-clear telemetry is epoch-scoped. A stale-texel proof from a previous
    // scene/history epoch must not satisfy the current pressure gate.
    mSurfaceCachePageClearCommandsTotal = 0u;
    mSurfaceCachePageClearTexelsTotal = 0u;
    if (!mHistoryResetThisFrame)
    {
        ++mHistoryGeneration;
        ++mHistoryResetCount;
        mLastHistoryResetReason = reason;
        mHistoryResetThisFrame = true;
    }
    // S5-A1: mark the prev history/depth double buffer for a hard clear (camera cut / resize /
    // scene change). The actual clear is emitted inside runTemporalFilter (it needs a
    // RenderContext, and setScene/onHotReload call this before the buffers exist). Clearing the
    // prev buffers makes every pixel take the disocclusion path for one frame (prev depth 0 =>
    // validation weight 0), which is the "history immediately invalid after a cut" gate.
    mTemporalFilter.historyResetPending = true;
    mScreenProbes.historyResetPending = true;
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
    // Deterministic page-publication fingerprints used by paired C5 diagnostics.
    // These are host-table hashes only; they never participate in shader decisions.
    auto hashWords = [](const auto& words, uint64_t seed)
    {
        uint64_t hash = seed;
        for (const auto& word : words)
        {
            const uint32_t values[] = {word.x, word.y, word.z, word.w};
            for (uint32_t value : values)
            {
                hash ^= uint64_t(value);
                hash *= 1099511628211ull;
            }
        }
        return hash;
    };
    auto hashScalars = [](const auto& words, uint64_t seed)
    {
        uint64_t hash = seed;
        for (const auto& word : words)
        {
            hash ^= uint64_t(word);
            hash *= 1099511628211ull;
        }
        return hash;
    };
    auto hashCards = [](const auto& cardScene, uint64_t seed)
    {
        if (!cardScene)
            return seed;
        uint64_t hash = seed;
        for (uint32_t index = 0; index < cardScene->getCardCount(); ++index)
        {
            const auto card = cardScene->getCard(index);
            const auto* bytes = reinterpret_cast<const uint8_t*>(&card);
            for (size_t byte = 0; byte < sizeof(card); ++byte)
            {
                hash ^= uint64_t(bytes[byte]);
                hash *= 1099511628211ull;
            }
        }
        return hash;
    };
    const uint64_t pageMetadataHash = hashWords(mPageMetadataData, 1469598103934665603ull);
    const uint64_t pageToCardHash = hashScalars(mPageToCardData, 1469598103934665603ull);
    const uint64_t renderListHash = hashScalars(mRenderListData, 1469598103934665603ull);
    const uint64_t cardBufferHash = hashCards(mpCardScene, 1469598103934665603ull);
    stats["pageMetadataHash"] = static_cast<double>(pageMetadataHash);
    stats["pageToCardHash"] = static_cast<double>(pageToCardHash);
    stats["renderListHash"] = static_cast<double>(renderListHash);
    stats["cardBufferHash"] = static_cast<double>(cardBufferHash);
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
    stats["surfaceCacheFrameIndex"] = (double)mSurfaceCacheFrameIndex;
    stats["minResidencyFrames"] = (double)cacheStats.minResidencyFrames;
    stats["allocatedPages"] = (double)cacheStats.allocatedPageCount;
    stats["freePages"] = (double)cacheStats.freePageCount;
    stats["evictedPendingPages"] = (double)cacheStats.evictedPendingCount;
    stats["coverage"] = cacheStats.pageCount > 0 ? (double)cacheStats.allocatedPageCount / (double)cacheStats.pageCount : 0.0;
    // Keep exact byte counters alongside the legacy MiB field.  The benchmark
    // and release gates use these to distinguish Surface Cache residency from
    // total process/GPU VRAM (which is reported separately by the profiler).
    stats["residentBytes"] = (double)cacheStats.residentBytes;
    stats["memoryBudgetBytes"] = (double)cacheStats.memoryBudgetBytes;
    stats["residentBytesMB"] = (double)(cacheStats.residentBytes >> 20);
    stats["memoryBudgetMB"] = (double)(cacheStats.memoryBudgetBytes >> 20);
    stats["allocations"] = (double)cacheStats.allocationCount;
    stats["releases"] = (double)cacheStats.releaseCount;
    stats["evictions"] = (double)cacheStats.evictionCount;
    stats["lastAllocatedPageID"] = (double)cacheStats.lastAllocatedPageID;
    stats["lastAllocatedGeneration"] = (double)cacheStats.lastAllocatedGeneration;
    stats["lastAllocatedFrame"] = (double)cacheStats.lastAllocatedFrame;
    stats["lastEvictedPageID"] = (double)cacheStats.lastEvictedPageID;
    stats["lastEvictedGeneration"] = (double)cacheStats.lastEvictedGeneration;
    stats["lastEvictedFrame"] = (double)cacheStats.lastEvictedFrame;
    stats["lastTouchedPageID"] = (double)cacheStats.lastTouchedPageID;
    stats["lastTouchedFrame"] = (double)cacheStats.lastTouchedFrame;
    stats["invalidations"] = (double)cacheStats.invalidationCount;
    uint32_t metadataAllocated = 0u;
    uint32_t metadataTouched = 0u;
    uint32_t metadataInvalid = 0u;
    uint32_t maxPageGeneration = 0u;
    for (size_t pageID = 1u; pageID < mPageMetadataData.size(); ++pageID)
    {
        maxPageGeneration = std::max(maxPageGeneration, mPageMetadataData[pageID].x);
        switch (static_cast<LumenSurfaceCachePageState>(mPageMetadataData[pageID].y))
        {
        case LumenSurfaceCachePageState::Allocated: ++metadataAllocated; break;
        case LumenSurfaceCachePageState::Touched: ++metadataTouched; break;
        default: ++metadataInvalid; break;
        }
    }
    stats["pageMetadataAllocated"] = (double)metadataAllocated;
    stats["pageMetadataTouched"] = (double)metadataTouched;
    stats["pageMetadataInvalid"] = (double)metadataInvalid;
    uint32_t metadataPending = 0u;
    for (size_t pageID = 1u; pageID < mPageMetadataData.size(); ++pageID)
    {
        if (mPageMetadataData[pageID].x != 0u && mPageMetadataData[pageID].z > mSurfaceCacheFrameIndex)
            ++metadataPending;
    }
    stats["pageMetadataPending"] = (double)metadataPending;
    stats["pageMetadataReady"] = (double)(metadataAllocated + metadataTouched - metadataPending);
    stats["surfaceCacheSceneGeneration"] = (double)mSurfaceCacheSceneGeneration;
    stats["surfaceCacheResetCount"] = (double)mSurfaceCacheResetCount;
    stats["cacheFeedbackStatsFrame"] = (double)mScreenProbes.cacheFeedbackSubmittedFrame;
    stats["generationRejects"] = (double)mSurfaceCacheGenerationRejects;
    // C6 page-lifecycle telemetry. These are host-authoritative counters; no image-derived
    // inference is used by the runtime gate. pageGeneration is the highest resident allocator
    // generation currently published to the GPU page metadata buffer.
    stats["pageGeneration"] = (double)maxPageGeneration;
    stats["generationMismatchRejects"] = (double)mSurfaceCacheGenerationRejects;
    stats["stateMismatchRejects"] = (double)mSurfaceCacheStateRejects;
    stats["staleOwnerRejects"] = (double)mSurfaceCacheStaleOwnerRejects;
    stats["cardGridDim"] = (double)LumenScreenProbe::kCacheCardGridDim;
    stats["cardGridMaxCandidates"] = (double)LumenScreenProbe::kCacheCardGridMaxCandidates;
    stats["cardGridCandidateCount"] = (double)mCardGridCandidateCount;
    stats["cardGridOverflowCells"] = (double)mCardGridOverflowCells;
    stats["cardGridCardsIndexed"] = (double)mCardGridCardsIndexed;
    stats["cardGridMissingCards"] = mpCardScene ?
        (double)std::max<int64_t>(0, int64_t(mpCardScene->getCardCount()) - int64_t(mCardGridCardsIndexed)) : 0.0;

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
    // C6.1 GPU demand feedback is reported separately from scheduler worklist deduplication.
    // These counters are monotonically accumulated only after host-side scene/generation/state
    // validation, so they are safe evidence of actual cache demand rather than image inference.
    stats["surfaceCacheFeedbackHits"] = (double)mSurfaceCacheFeedbackHits;
    stats["surfaceCacheFeedbackPages"] = (double)mSurfaceCacheFeedbackPages;
    stats["surfaceCacheFeedbackDedup"] = (double)mSurfaceCacheFeedbackDedup;
    stats["surfaceCacheFeedbackStaleRejects"] = (double)mSurfaceCacheFeedbackStaleRejects;
    stats["surfaceCacheRequestRaw"] = (double)mSurfaceCacheRequestRaw;
    stats["surfaceCacheRequestCards"] = (double)mSurfaceCacheRequestCards;
    stats["surfaceCacheRequestDedup"] = (double)mSurfaceCacheRequestDedup;
    stats["surfaceCacheRequestStaleRejects"] = (double)mSurfaceCacheRequestStaleRejects;
    stats["surfaceCacheRequestCaptureCompleted"] = (double)mSurfaceCacheRequestCaptureCompleted;
    // Per-host-frame event telemetry. These values are intentionally not cumulative;
    // they are the only counters consumed by the strict C6 N->N+1 validator.
    stats["requestRawThisFrame"] = (double)mSurfaceCacheRequestRawThisFrame;
    stats["requestCardsThisFrame"] = (double)mSurfaceCacheRequestCardsThisFrame;
    stats["requestCaptureCompletedThisFrame"] = (double)mSurfaceCacheRequestCaptureCompletedThisFrame;
    stats["pageMetadataPendingThisFrame"] = (double)mSurfaceCachePageMetadataPendingThisFrame;
    stats["pageMetadataReadyThisFrame"] = (double)mSurfaceCachePageMetadataReadyThisFrame;
    stats["requestObservedFrame"] = (double)mSurfaceCacheRequestObservedFrame;
    stats["requestCaptureFrame"] = (double)mSurfaceCacheRequestCaptureFrame;
    stats["surfaceCacheRequestReasonUnmapped"] = (double)mSurfaceCacheRequestUnmapped;
    stats["surfaceCacheRequestReasonStaleOwner"] = (double)mSurfaceCacheRequestStaleOwner;
    stats["surfaceCacheRequestReasonMetadataInvalid"] = (double)mSurfaceCacheRequestMetadataInvalid;
    stats["surfaceCacheRequestReasonVisibilityInvalid"] = (double)mSurfaceCacheRequestVisibilityInvalid;
    stats["surfaceCacheEventCount"] = (double)mSurfaceCacheRequestEvents.size();
    stats["surfaceCacheEventDropped"] = (double)mSurfaceCacheRequestEventDropped;
    // Mirror the one-frame-lagged probe lookup counters into surfaceCacheStats so the C6
    // activity gate can prove that resident-page cache hits have corresponding GPU feedback
    // without joining two unrelated graph outputs in the test harness.
    stats["cacheLookupHits"] = (double)mScreenProbeStats.cacheLookupHits;
    stats["cacheLookupAttempts"] = (double)mScreenProbeStats.cacheLookupAttempts;
    stats["cachePageRejects"] = (double)mScreenProbeStats.cachePageRejects;
    stats["cacheCoverageRejects"] = (double)mScreenProbeStats.cacheCoverageRejects;
    stats["cacheMetadataRejects"] = (double)mScreenProbeStats.cacheMetadataRejects;
    stats["cacheVisibilityRejects"] = (double)mScreenProbeStats.cacheVisibilityRejects;
    stats["cacheDepthRejects"] = (double)mScreenProbeStats.cacheDepthRejects;
    stats["cacheAxisRejects"] = (double)mScreenProbeStats.cacheAxisRejects;
    stats["cacheFacingRejects"] = (double)mScreenProbeStats.cacheFacingRejects;
    stats["cacheOwnerValid"] = (double)mScreenProbeStats.cacheOwnerValid;
    stats["cacheLookupHitsThisFrame"] = (double)mScreenProbeStats.cacheLookupHits;
    stats["cacheLookupAttemptsThisFrame"] = (double)mScreenProbeStats.cacheLookupAttempts;
    stats["cacheLookupStatsFrame"] = (double)mScreenProbeStatsFrame;
    stats["surfaceCacheHits"] = (double)mScreenProbeStats.cacheLookupHits;
    stats["schedulerRequestDedup"] = (double)schedulerStats.totalRequestDeduplications;
    stats["requestDedup"] = (double)(schedulerStats.totalRequestDeduplications + mSurfaceCacheFeedbackDedup);
    stats["lastUsed"] = (double)(schedulerStats.totalTouches + mSurfaceCacheFeedbackPages);
    stats["schedCompletedCaptures"] = (double)schedulerStats.completedCaptures;
    stats["avgQueuedFrames"] = schedulerStats.averageQueuedFrames;
    stats["maxQueuedFrames"] = (double)schedulerStats.maxQueuedFrames;
    stats["pendingQueueDepth"] = (double)schedulerStats.pendingQueueDepth;
    stats["maxPendingDepth"] = (double)schedulerStats.maxPendingDepth;
    stats["schedStructuralRebuilds"] = (double)schedulerStats.structuralRebuildCount;
    // C6 identity telemetry: the last emitted command identifies the actual card/page pair
    // selected by the scheduler. Invalid IDs are exported as uint32 sentinels so the runner
    // can distinguish "no command" from a valid card/page zero.
    stats["lastCommandCardID"] = (double)schedulerStats.lastCommandCardID;
    stats["lastCommandPageID"] = (double)schedulerStats.lastCommandPageID;
    stats["lastCommandGeneration"] = (double)schedulerStats.lastCommandGeneration;
    stats["lastRequestedCardID"] = (double)schedulerStats.lastCommandCardID;
    stats["lastCapturedCardID"] = (double)schedulerStats.lastCommandCardID;
    stats["lastCardID"] = (double)schedulerStats.lastCommandCardID;
    stats["cardID"] = (double)schedulerStats.lastCommandCardID;

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
    stats["lastFrameCommandCardID"] = (double)last.lastCommandCardID;
    stats["lastFrameCommandPageID"] = (double)last.lastCommandPageID;
    stats["lastFrameCommandGeneration"] = (double)last.lastCommandGeneration;
    stats["pageClearCommands"] = (double)mSurfaceCachePageClearCommands;
    stats["pageClearTexels"] = (double)mSurfaceCachePageClearTexels;
    stats["pageClearCommandsTotal"] = (double)mSurfaceCachePageClearCommandsTotal;
    stats["pageClearTexelsTotal"] = (double)mSurfaceCachePageClearTexelsTotal;
    // The page-local clear is the stale-texel fence: every capture command clears a complete
    // 16x16 tile before raster writes. Export a typed sentinel only when the command/texel
    // invariant is true; the C6 runner treats this as a clear-fence proof, not image inference.
    stats["staleTexelSentinel"] =
        (mSurfaceCachePageClearCommandsTotal > 0u &&
         mSurfaceCachePageClearTexelsTotal == mSurfaceCachePageClearCommandsTotal * 16u * 16u) ? 1.0 : 0.0;

    // S3: Surface Cache lighting state and counters. cacheLightingPagesLit is the dispatch
    // size of the last run; the counters are the last completed dispatch's readback.
    stats["useCacheLighting"] = mUseCacheLighting ? 1.0 : 0.0;
    stats["useCacheCardGrid"] = mUseCacheCardGrid ? 1.0 : 0.0;
    stats["cacheLightingActive"] = (mUseCacheLighting && mUseSurfaceCache) ? 1.0 : 0.0;
    stats["cacheLightingPagesLit"] = (double)mLastCacheLightingPageCount;
    stats["cacheLightingSamplesPerTexel"] = (double)cacheLightingSamplesPerTexel();
    stats["cacheLightingCounterNanInf"] = (double)mCacheLightingCounters.nanInfSamples;
    stats["cacheLightingCounterFirefly"] = (double)mCacheLightingCounters.fireflySamples;
    stats["cacheLightingCounterNegative"] = (double)mCacheLightingCounters.negativeSamples;
    stats["cacheLightingCounterTraced"] = (double)mCacheLightingCounters.tracedSamples;
    // C5 producer provenance.  These are read-only host-side inputs to the cache-lighting
    // dispatch, exported so paired runs can prove that seed/frame/TLAS/variant state matches
    // before attributing an atlas delta to the producer.  They never participate in shading.
    constexpr uint32_t kCacheLightingSeedTelemetry = 0x51B8DC0Du;
    const bool cacheLightingTlasPresent = mpScene && mpScene->getSceneStats().tlasCount > 0u;
    const bool cacheLightingShadowsOff = []()
    {
        const char* value = std::getenv("LUMEN_C5_DISABLE_CACHE_SHADOWS");
        return value && (std::string(value) == "1" || std::string(value) == "true");
    }();
    stats["cacheLightingSeed"] = (double)kCacheLightingSeedTelemetry;
    stats["cacheLightingFrameIndex"] = (double)mFrameIndex;
    stats["cacheLightingSurfaceCacheFrameIndex"] = (double)mSurfaceCacheFrameIndex;
    stats["cacheLightingRayTypeCount"] = 1.0;
    stats["cacheLightingTlasPresent"] = cacheLightingTlasPresent ? 1.0 : 0.0;
    stats["cacheLightingUseEnvLight"] = mpScene && mpScene->useEnvLight() ? 1.0 : 0.0;
    stats["cacheLightingUseAnalyticLights"] = mpScene && mpScene->useAnalyticLights() ? 1.0 : 0.0;
    stats["cacheLightingUseEmissiveLights"] = mpScene && mpScene->useEmissiveLights() ? 1.0 : 0.0;
    stats["cacheLightingEnvSampler"] = mpEnvMapSampler ? 1.0 : 0.0;
    stats["cacheLightingEmissiveSampler"] = mpEmissiveLightSampler ? 1.0 : 0.0;
    stats["cacheLightingShadowsEnabled"] = cacheLightingShadowsOff ? 0.0 : 1.0;
    stats["cacheLightingFeedbackEnabled"] =
        (mCacheLightingFeedbackEnabled && mUseCacheLighting && mUseSurfaceCache) ? 1.0 : 0.0;
    // A compact variant fingerprint makes paired diagnostics robust to flag ordering and
    // provides one scalar to compare in scripts without exposing a production shader hash.
    uint64_t cacheLightingVariant = 1469598103934665603ull;
    const uint32_t variantFlags[] = {
        (uint32_t)(mpScene && mpScene->useEnvLight()),
        (uint32_t)(mpScene && mpScene->useAnalyticLights()),
        (uint32_t)(mpScene && mpScene->useEmissiveLights()),
        (uint32_t)(mpEnvMapSampler != nullptr),
        (uint32_t)(mpEmissiveLightSampler != nullptr),
        (uint32_t)!cacheLightingShadowsOff,
        (uint32_t)(mCacheLightingFeedbackEnabled && mUseCacheLighting && mUseSurfaceCache),
        1u, // canonical one-ray-type TLAS binding
    };
    for (const uint32_t flag : variantFlags)
    {
        cacheLightingVariant ^= (uint64_t)flag;
        cacheLightingVariant *= 1099511628211ull;
    }
    stats["cacheLightingVariantFingerprint"] = (double)cacheLightingVariant;

    return stats;
}

std::vector<std::map<std::string, double>> LumenGIPass::getSurfaceCacheEvents() const
{
    std::vector<std::map<std::string, double>> result;
    result.reserve(mSurfaceCacheRequestEvents.size());
    for (const auto& event : mSurfaceCacheRequestEvents)
    {
        std::map<std::string, double> record;
        record["sequence"] = (double)event.sequence;
        record["sceneGeneration"] = (double)event.sceneGeneration;
        record["cardID"] = (double)event.cardIndex;
        record["pageID"] = (double)event.pageID;
        record["generation"] = (double)event.generation;
        record["requestFrame"] = (double)event.requestFrame;
        record["captureFrame"] = (double)event.captureFrame;
        record["readyFrame"] = (double)event.readyFrame;
        record["firstHitFrame"] = (double)event.firstHitFrame;
        record["reasonBits"] = (double)event.reasonBits;
        record["requestCount"] = (double)event.requestCount;
        record["lookupHits"] = (double)event.lookupHits;
        record["state"] = (double)event.state;
        result.emplace_back(std::move(record));
    }
    return result;
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
    mCapture.pPageGeneration = nullptr;
    mCapture.pDrawArgs = nullptr;
    mCardPageTable.clear();
    mCardPageGeneration.clear();
    mPageMetadataData.clear();
    mSurfaceCacheRequestedCards.clear();
    mSurfaceCacheDeferredRequestCards.clear();
    mSurfaceCacheDeferredRequestFrameByCard.clear();
    mSurfaceCachePendingReadyPages.clear();
    mSurfaceCacheRequestEvents.clear();
    mSurfaceCacheRequestEventSequence = 0;
    mCardGridData.clear();
    mCardGridMin = float3(0.f);
    mCardGridInvCellSize = float3(1.f);
    mCardGridOverflowCells = 0u;
    mCardGridCandidateCount = 0u;
    mCardGridCardsIndexed = 0u;
    // S3: the cache lighting program carries the scene defines/type conformances and must be
    // recreated on scene/geometry rebuilds. The pageToCard/renderList buffers and the visibility
    // atlas are atlas-lifetime (fixed size) and are deliberately kept; their contents are rebuilt
    // every frame.
    mCacheLighting.pPass = nullptr;
    mCacheLighting.envSamplerVariant = false;
    // The C10 producer embeds scene modules/type conformances in its program;
    // rebuild it whenever the scene-scoped capture programs are invalidated.
    mRadianceCacheGpu.pBuild = nullptr;
    mRadianceCacheGpu.pInterpolate = nullptr;
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
    if (!mCapture.pCaptureOrderAtlas)
    {
        mCapture.pCaptureOrderAtlas = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::R32Uint, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCapture.pCaptureOrderAtlas->setName("LumenGIPass::Capture::OrderAtlas");
        pRenderContext->clearUAV(mCapture.pCaptureOrderAtlas->getUAV().get(), uint4(0xffffffffu));
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
        mCapture.pPageGeneration = mpDevice->createStructuredBuffer(sizeof(uint32_t), cardCount, ResourceBindFlags::ShaderResource);
        mCapture.pPageGeneration->setName("LumenGIPass::Capture::PageGeneration"); // cardIndex -> capture generation.
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

    // Page-local clear resources are atlas-lifetime and independent of the scene program.
    // The page-ID list is resized with the capture budget; the shader clears exactly one
    // unique 16x16 page per Z slice before raster capture.
    if (!mCapture.pPageClear)
        mCapture.pPageClear = ComputePass::create(mpDevice, kPageClearShaderFile, "main");
    if (!mCapture.pPageIDs || mCapture.pPageIDs->getElementCount() < maxCommands)
    {
        mCapture.pPageIDs = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), maxCommands, ResourceBindFlags::ShaderResource
        );
        mCapture.pPageIDs->setName("LumenGIPass::Capture::PageClearIDs");
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

    const uint32_t pageCount = mPageCache.getPageCount();
    if (mPageMetadataData.size() != pageCount + 1u)
        mPageMetadataData.assign(pageCount + 1u, uint4(0u));

    // Promote miss requests observed at the start of this host frame only now,
    // immediately before scheduling capture.  This one-frame handoff is the
    // explicit C6 N->N+1 publication fence; readback itself never enqueues a
    // capture on the request-observation frame.
    if (!mSurfaceCacheDeferredRequestCards.empty())
    {
        std::vector<uint32_t> readyRequests;
        readyRequests.reserve(mSurfaceCacheDeferredRequestCards.size());
        for (const uint32_t cardIndex : mSurfaceCacheDeferredRequestCards)
        {
            const auto frameIt = mSurfaceCacheDeferredRequestFrameByCard.find(cardIndex);
            if (frameIt != mSurfaceCacheDeferredRequestFrameByCard.end() && frameIt->second < mSurfaceCacheFrameIndex)
                readyRequests.push_back(cardIndex);
        }
        for (const uint32_t cardIndex : readyRequests)
        {
            if (mCaptureScheduler.enqueueFeedbackRequest(cardIndex))
                mSurfaceCacheRequestedCards.insert(cardIndex);
            else
            {
                // The scheduler can reject a deferred request after the allocator has become
                // saturated.  Close its ledger event explicitly instead of leaving a permanent
                // state=1 request that can never reach capture/ready.
                for (auto eventIt = mSurfaceCacheRequestEvents.rbegin();
                     eventIt != mSurfaceCacheRequestEvents.rend(); ++eventIt)
                {
                    if (eventIt->sceneGeneration == mSurfaceCacheSceneGeneration &&
                        eventIt->cardIndex == cardIndex && eventIt->state >= 1u && eventIt->state < 3u)
                    {
                        eventIt->reasonBits |= 2u;
                        eventIt->state = 5u;
                        break;
                    }
                }
            }
            mSurfaceCacheDeferredRequestCards.erase(cardIndex);
            mSurfaceCacheDeferredRequestFrameByCard.erase(cardIndex);
        }
    }

    const LumenCaptureFrame frame = mCaptureScheduler.scheduleFrame(updateFlags);
    // scheduleFrame() is the authoritative monotonic Surface Cache clock. It
    // is intentionally independent of mFrameIndex/history resets.
    const uint64_t schedulerFrame = mCaptureScheduler.getFrameIndex();
    mSurfaceCacheFrameIndex = static_cast<uint32_t>(schedulerFrame > 0u ? schedulerFrame - 1u : 0u);
    mLastCaptureFrameStats = frame.stats;
    mSurfaceCachePageClearCommands = 0u;
    mSurfaceCachePageClearTexels = 0u;

    // Requests that remain unassigned for a bounded scheduler interval are explicit drops,
    // rather than immortal state=1 records.  This is especially important under a tiny atlas:
    // the allocator can legitimately reject work for many frames while it protects residency.
    // Marking the timeout as a terminal stale outcome preserves the identity/latency evidence
    // without pretending that a capture happened.
    for (auto& event : mSurfaceCacheRequestEvents)
    {
        if (event.sceneGeneration != mSurfaceCacheSceneGeneration || event.state != 1u ||
            event.requestFrame + kLumenMinResidencyFrames >= mSurfaceCacheFrameIndex ||
            event.pageID != kInvalidPageID && event.generation != 0u)
        {
            continue;
        }
        event.reasonBits |= 2u;
        event.state = 5u;
        ++mSurfaceCacheRequestStaleRejects;
    }

    // Record an explicit terminal stale-owner event whenever a scheduler command reuses a
    // page at a new generation.  Aggregate allocator stats expose only the most recent victim,
    // while the strict pressure gate needs the old pageID/generation pair to prove reuse.  Keep
    // this helper local so the event ring remains bounded and no production page state is changed.
    auto recordStaleOwnerEvent = [&](uint32_t cardIndex, uint32_t pageID, uint32_t generation)
    {
        if (pageID == kInvalidPageID || generation == 0u)
            return;
        const bool alreadyRecorded = std::any_of(
            mSurfaceCacheRequestEvents.begin(), mSurfaceCacheRequestEvents.end(),
            [&](const SurfaceCacheRequestEvent& event)
            {
                return event.sceneGeneration == mSurfaceCacheSceneGeneration &&
                    event.pageID == pageID && event.generation == generation && event.state == 5u;
            }
        );
        if (alreadyRecorded)
            return;
        // Keep enough identity records for a bounded tiny-atlas pressure window.  This is
        // telemetry capacity only; the strict gate still rejects unresolved events and never
        // treats a larger ring as proof of capture/ready completion.
        if (mSurfaceCacheRequestEvents.size() >= kMaxSurfaceCacheRequestEvents)
        {
            mSurfaceCacheRequestEvents.erase(mSurfaceCacheRequestEvents.begin());
            ++mSurfaceCacheRequestEventDropped;
        }
        SurfaceCacheRequestEvent staleEvent;
        staleEvent.sequence = ++mSurfaceCacheRequestEventSequence;
        staleEvent.sceneGeneration = mSurfaceCacheSceneGeneration;
        staleEvent.cardIndex = cardIndex;
        staleEvent.pageID = pageID;
        staleEvent.generation = generation;
        staleEvent.requestFrame = mSurfaceCacheFrameIndex;
        staleEvent.reasonBits = 2u; // stale-owner terminal outcome.
        staleEvent.requestCount = 1u;
        staleEvent.state = 5u;
        mSurfaceCacheRequestEvents.push_back(staleEvent);
    };

    // Preserve the allocator's victim identity in the same per-card event
    // ledger consumed by the strict C6 pressure gate.  Without this terminal
    // record, the page generation transition is visible only in aggregate
    // stats and an old owner can remain an unresolved request forever.  The
    // event is deliberately state=5 with no publication fields: it records a
    // stale-owner rejection, not a successful capture/ready transition.
    const auto cacheStatsAtSchedule = mPageCache.getStats();
    if (cacheStatsAtSchedule.lastEvictedPageID != kInvalidPageID &&
        cacheStatsAtSchedule.lastEvictedGeneration != 0u &&
        cacheStatsAtSchedule.lastEvictedFrame == mSurfaceCacheFrameIndex)
    {
        const uint32_t victimPage = cacheStatsAtSchedule.lastEvictedPageID;
        const uint32_t victimGeneration = cacheStatsAtSchedule.lastEvictedGeneration;
        const uint32_t victimCard = victimPage < mPageToCardData.size()
            ? mPageToCardData[victimPage]
            : kLumenCardInvalidID;
        recordStaleOwnerEvent(victimCard, victimPage, victimGeneration);
    }
    std::vector<LumenCaptureCommand> requestCaptureCommands;
    requestCaptureCommands.reserve(frame.commands.size());
    for (const LumenCaptureCommand& command : frame.commands)
    {
        // Before overwriting the host card->page mirror, retain the previous owner/generation
        // for this page.  A page reused by a different card (or by the same card after an
        // allocator generation bump) must leave an identity-bearing stale event for the old
        // generation; otherwise the strict gate cannot distinguish reuse from a fresh page.
        const uint32_t previousPageForCard = command.cardIndex < mCardPageTable.size()
            ? mCardPageTable[command.cardIndex]
            : kInvalidPageID;
        const uint32_t previousGenerationForCard = command.cardIndex < mCardPageGeneration.size()
            ? mCardPageGeneration[command.cardIndex]
            : 0u;
        const uint32_t previousOwner = command.pageID < mPageToCardData.size()
            ? mPageToCardData[command.pageID]
            : kLumenCardInvalidID;
        const uint32_t previousOwnerGeneration = previousOwner < mCardPageGeneration.size()
            ? mCardPageGeneration[previousOwner]
            : 0u;
        // mPageMetadataData is the last published page-cache snapshot.  It remains the
        // authoritative old generation even when the previous owner was already filtered from
        // mPageToCardData by a stale generation check, so use it as the fallback identity source.
        const uint32_t previousPageGeneration = command.pageID < mPageMetadataData.size()
            ? mPageMetadataData[command.pageID].x
            : 0u;
        // A request event may already carry the old page identity even when the allocator's
        // current snapshot has advanced past it.  Close that event as stale before publishing
        // the new generation; otherwise a pending old request remains unresolved forever and the
        // strict ledger cannot observe a same-page generation transition.
        for (auto& event : mSurfaceCacheRequestEvents)
        {
            if (event.sceneGeneration == mSurfaceCacheSceneGeneration &&
                event.pageID == command.pageID && event.generation != 0u &&
                event.generation != command.generation && event.state >= 1u && event.state < 3u)
            {
                event.reasonBits |= 2u;
                event.state = 5u;
            }
        }
        if (previousPageForCard == command.pageID && previousGenerationForCard != 0u &&
            previousGenerationForCard != command.generation)
        {
            recordStaleOwnerEvent(command.cardIndex, command.pageID, previousGenerationForCard);
        }
        if (previousOwner != kLumenCardInvalidID && previousOwner != command.cardIndex &&
            previousOwnerGeneration != 0u && previousOwnerGeneration != command.generation)
        {
            recordStaleOwnerEvent(previousOwner, command.pageID, previousOwnerGeneration);
        }
        if (previousPageGeneration != 0u && previousPageGeneration != command.generation &&
            previousPageGeneration != previousGenerationForCard &&
            previousPageGeneration != previousOwnerGeneration)
        {
            const uint32_t staleCard = previousOwner != kLumenCardInvalidID ? previousOwner : command.cardIndex;
            recordStaleOwnerEvent(staleCard, command.pageID, previousPageGeneration);
        }

        const bool requestedCard = mSurfaceCacheRequestedCards.find(command.cardIndex) != mSurfaceCacheRequestedCards.end();
        if (requestedCard)
            requestCaptureCommands.push_back(command);

        // A newly allocated/reused page is not visible to ScreenProbe lookup until the
        // following frame.  Cache lighting may populate it in this frame, but the producer
        // contract keeps page ownership and lighting publication on separate frame boundaries.
        // Keep the previous ready frame for same-generation recaptures so dirty relighting does
        // not unnecessarily force a miss.
        if (command.pageID < mPageMetadataData.size())
        {
            const uint32_t previousGeneration = mPageMetadataData[command.pageID].x;
            if (requestedCard)
            {
                // A demand request must cross a publication fence even when the
                // scheduler recaptures the same generation.  Mark the page pending
                // now and expose it as ready only from the next host frame.
                mPageMetadataData[command.pageID].z = mSurfaceCacheFrameIndex + 1u;
                if (mSurfaceCachePendingReadyPages.insert(command.pageID).second)
                    ++mSurfaceCachePageMetadataPendingThisFrame;
            }
            else if (previousGeneration != command.generation)
            {
                mPageMetadataData[command.pageID].z = mSurfaceCacheFrameIndex + 1u;
            }
        }
    }

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
    if (mCapture.pPageGeneration && !mCardPageGeneration.empty())
    {
        mCapture.pPageGeneration->setBlob(
            mCardPageGeneration.data(), 0, mCardPageGeneration.size() * sizeof(uint32_t)
        );
    }

    if (!frame.commands.empty() && mCapture.pPageClear && mCapture.pPageIDs)
    {
        std::vector<uint32_t> clearPageIDs;
        clearPageIDs.reserve(frame.commands.size());
        for (const LumenCaptureCommand& command : frame.commands)
        {
            if (command.pageID == kInvalidPageID)
                continue;
            if (std::find(clearPageIDs.begin(), clearPageIDs.end(), command.pageID) == clearPageIDs.end())
                clearPageIDs.push_back(command.pageID);
        }
        if (!clearPageIDs.empty())
        {
            mCapture.pPageIDs->setBlob(clearPageIDs.data(), 0, clearPageIDs.size() * sizeof(uint32_t));
            ShaderVar clearVar = mCapture.pPageClear->getRootVar();
            clearVar["gPageIDs"] = mCapture.pPageIDs;
            clearVar["gMaterialAtlas"] = mCapture.pMaterialAtlas;
            clearVar["gRadianceAtlas"] = mCapture.pRadianceAtlas;
            clearVar["gMetadataAtlas"] = mCapture.pMetadataAtlas;
            clearVar["gCaptureOrderAtlas"] = mCapture.pCaptureOrderAtlas;
            ShaderVar clearCB = clearVar["LumenSurfaceCachePageClearCB"];
            clearCB["gPageCount"] = (uint32_t)clearPageIDs.size();
            clearCB["gPagesPerSide"] = mCapturePagesPerSide;
            clearCB["gAtlasSize"] = uint2(mAtlasSizeTexels, mAtlasSizeTexels);
            mCapture.pPageClear->execute(pRenderContext, kLumenSurfaceCacheTileSize, kLumenSurfaceCacheTileSize,
                                         (uint32_t)clearPageIDs.size());
            // Page clear is a compute UAV producer and the following capture pass writes the
            // same atlas resources through raster targets.  Publish the cleared tiles before
            // switching pass/resource classes; otherwise sparse stale texels can survive on
            // one of two otherwise identical cache-lighting variants.
            if (mCapture.pMaterialAtlas)
                pRenderContext->uavBarrier(mCapture.pMaterialAtlas.get());
            if (mCapture.pMetadataAtlas)
                pRenderContext->uavBarrier(mCapture.pMetadataAtlas.get());
            if (mCapture.pRadianceAtlas)
                pRenderContext->uavBarrier(mCapture.pRadianceAtlas.get());
            if (mCapture.pCaptureOrderAtlas)
                pRenderContext->uavBarrier(mCapture.pCaptureOrderAtlas.get());
            mSurfaceCachePageClearCommands = (uint32_t)clearPageIDs.size();
            mSurfaceCachePageClearTexels =
                (uint64_t)clearPageIDs.size() * kLumenSurfaceCacheTileSize * kLumenSurfaceCacheTileSize;
            mSurfaceCachePageClearCommandsTotal += clearPageIDs.size();
            mSurfaceCachePageClearTexelsTotal +=
                (uint64_t)clearPageIDs.size() * kLumenSurfaceCacheTileSize * kLumenSurfaceCacheTileSize;
        }
    }

    if (!frame.commands.empty())
        runCapturePass(pRenderContext, frame);

    mCaptureScheduler.completeCaptures(frame.commands);
    // Count request completions only after the capture pass and scheduler state transition.
    // The previous placement counted command emission, which could over-report a request
    // even when the raster capture was not completed.
    for (const LumenCaptureCommand& command : requestCaptureCommands)
    {
        if (!mCaptureScheduler.isCaptureComplete(command))
            continue;
        const uint32_t cardIndex = command.cardIndex;
        if (mSurfaceCacheRequestedCards.erase(cardIndex) != 0u)
        {
            ++mSurfaceCacheRequestCaptureCompleted;
            ++mSurfaceCacheRequestCaptureCompletedThisFrame;
            mSurfaceCacheRequestCaptureFrame = mSurfaceCacheFrameIndex;
            for (auto it = mSurfaceCacheRequestEvents.rbegin();
                 it != mSurfaceCacheRequestEvents.rend(); ++it)
            {
                if (it->sceneGeneration == mSurfaceCacheSceneGeneration &&
                    it->cardIndex == cardIndex && it->requestFrame < mSurfaceCacheFrameIndex &&
                    it->captureFrame == 0u && it->state >= 1u && it->state < 3u)
                {
                    it->pageID = command.pageID;
                    it->generation = command.generation;
                    it->captureFrame = mSurfaceCacheFrameIndex;
                    it->state = 2u;
                    break;
                }
            }
        }
    }

    // A request accepted by the deferred queue but not emitted by the very next scheduler
    // frame has no valid N->N+1 publication path under the current pressure budget.  Close it as
    // an explicit stale/rejected outcome so the per-card ledger does not retain an immortal
    // state=1 record.  Requests that did emit a command above have already advanced to state=2.
    for (auto& event : mSurfaceCacheRequestEvents)
    {
        if (event.sceneGeneration == mSurfaceCacheSceneGeneration && event.state == 1u &&
            event.requestFrame < mSurfaceCacheFrameIndex)
        {
            event.reasonBits |= 2u;
            event.state = 5u;
            ++mSurfaceCacheRequestStaleRejects;
        }
    }
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
    var["gCaptureOrderAtlas"] = mCapture.pCaptureOrderAtlas;

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
    mCapture.pProgram->addDefine("is_valid_gCaptureOrderAtlas", "1");
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
    if (mPageMetadataData.size() != pageCount + 1)
        mPageMetadataData.assign(pageCount + 1, uint4(0u));
    std::fill(mPageToCardData.begin(), mPageToCardData.end(), kLumenCardInvalidID);
    for (uint32_t pageID = 1u; pageID <= pageCount; ++pageID)
    {
        const bool allocated = mPageCache.isPageAllocated(pageID);
        const uint32_t previousReadyFrame = mPageMetadataData[pageID].z;
        mPageMetadataData[pageID] = uint4(
            allocated ? mPageCache.getGeneration(pageID) : 0u,
            allocated ? static_cast<uint32_t>(mPageCache.getPageState(pageID)) : 0u,
            allocated ? previousReadyFrame : 0u,
            0u
        );
    }

    const uint32_t cardCount = mpCardScene ? mpCardScene->getCardCount() : 0u;
    mSurfaceCacheGenerationRejects = 0u;
    mSurfaceCacheStateRejects = 0u;
    mSurfaceCacheStaleOwnerRejects = 0u;
    if (mCardPageTable.size() != cardCount || mCardPageGeneration.size() != cardCount)
        return 0u; // capture resources not initialized yet (no cards captured this frame).

    for (uint32_t card = 0; card < cardCount; ++card)
    {
        const uint32_t pageID = mCardPageTable[card];
        if (pageID == kInvalidPageID || pageID > pageCount)
            continue;
        if (!mPageCache.isPageAllocated(pageID))
            continue;
        const auto pageState = mPageCache.getPageState(pageID);
        if (pageState != LumenSurfaceCachePageState::Allocated &&
            pageState != LumenSurfaceCachePageState::Touched)
        {
            ++mSurfaceCacheStateRejects;
            continue;
        }
        if (mCardPageGeneration[card] != mPageCache.getGeneration(pageID))
        {
            ++mSurfaceCacheGenerationRejects;
            ++mSurfaceCacheStaleOwnerRejects;
            continue;
        }
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

    // Build a bounded world-space card candidate grid for probe cache lookup. The
    // page/card generation checks remain authoritative in the shader; this index
    // only narrows the candidate set. A per-cell overflow bit preserves a full
    // scan fallback for pathological giant/overlapping cards.
    constexpr uint32_t kGridDim = LumenScreenProbe::kCacheCardGridDim;
    constexpr uint32_t kMaxCandidates = LumenScreenProbe::kCacheCardGridMaxCandidates;
    constexpr uint32_t kCellStride = LumenScreenProbe::kCacheCardGridCellStride;
    const size_t gridCellCount = size_t(kGridDim) * size_t(kGridDim) * size_t(kGridDim);
    mCardGridData.assign(gridCellCount * kCellStride, 0u);
    mCardGridOverflowCells = 0u;
    mCardGridCandidateCount = 0u;
    mCardGridCardsIndexed = 0u;

    const float inf = std::numeric_limits<float>::max();
    float3 gridMin(inf);
    float3 gridMax(-inf);
    auto cacheGridBounds = [](const LumenCard& card, float3& outMin, float3& outMax)
    {
        // Keep the CPU broadphase conservative with the shader's fixed lookup
        // epsilon (LumenScreenProbeIntegrate.cs.slang). Without this pad a
        // probe on a cell boundary can be present in the full-card scan but
        // absent from the grid candidate list, changing winner/request order.
        constexpr float kLookupCandidateEpsilon = 0.04f;
        outMin = float3(card.boundsMin.x, card.boundsMin.y, card.boundsMin.z);
        outMax = float3(card.boundsMax.x, card.boundsMax.y, card.boundsMax.z);
        // The lookup slab intentionally accepts up to one card extent behind the
        // capture face. Expand only that face axis so the candidate index remains
        // bounded without dropping valid slab hits outside the raw AABB.
        const uint32_t axis = std::min<uint32_t>(card.faceIndex >> 1u, 2u);
        const float pad = (&card.extent.x)[axis];
        (&outMin.x)[axis] -= pad;
        (&outMax.x)[axis] += pad;
        outMin -= float3(kLookupCandidateEpsilon);
        outMax += float3(kLookupCandidateEpsilon);
    };
    // Index every active card, not only cards that currently own a resident
    // page.  Non-resident cards are exactly the candidates that must survive
    // grid lookup so the shader can emit a C6.2 capture request; page state,
    // generation and metadata remain strict shader-side validity fences.
    std::vector<uint32_t> indexedCards;
    indexedCards.reserve(cardCount);
    for (uint32_t cardIndex = 0u; cardIndex < cardCount; ++cardIndex)
    {
        const LumenCard& card = mpCardScene->getCard(cardIndex);
        if (card.faceIndex == kLumenCardInvalidID)
            continue;
        if (!std::isfinite(card.boundsMin.x) || !std::isfinite(card.boundsMin.y) ||
            !std::isfinite(card.boundsMin.z) || !std::isfinite(card.boundsMax.x) ||
            !std::isfinite(card.boundsMax.y) || !std::isfinite(card.boundsMax.z))
            continue;
        float3 cardMin, cardMax;
        cacheGridBounds(card, cardMin, cardMax);
        gridMin = min(gridMin, cardMin);
        gridMax = max(gridMax, cardMax);
        indexedCards.push_back(cardIndex);
    }
    mCardGridCardsIndexed = (uint32_t)indexedCards.size();
    if (indexedCards.empty())
    {
        gridMin = float3(-1.f);
        gridMax = float3(1.f);
    }
    const float3 gridExtent = max(gridMax - gridMin, float3(1e-3f));
    mCardGridMin = gridMin;
    mCardGridInvCellSize = float3(
        float(kGridDim) / gridExtent.x,
        float(kGridDim) / gridExtent.y,
        float(kGridDim) / gridExtent.z
    );

    auto clampCell = [&](const float3& p) -> uint3
    {
        const float3 f = (p - mCardGridMin) * mCardGridInvCellSize;
        return uint3(
            uint32_t(std::clamp(int(std::floor(f.x)), 0, int(kGridDim - 1u))),
            uint32_t(std::clamp(int(std::floor(f.y)), 0, int(kGridDim - 1u))),
            uint32_t(std::clamp(int(std::floor(f.z)), 0, int(kGridDim - 1u)))
        );
    };
    auto cellBase = [&](const uint3& cell) -> size_t
    {
        return (size_t(cell.z) * kGridDim * kGridDim + size_t(cell.y) * kGridDim + cell.x) * kCellStride;
    };
    for (uint32_t cardIndex : indexedCards)
    {
        const LumenCard& card = mpCardScene->getCard(cardIndex);
        float3 cardMin, cardMax;
        cacheGridBounds(card, cardMin, cardMax);
        const uint3 minCell = clampCell(cardMin);
        const uint3 maxCell = clampCell(cardMax);
        for (uint32_t z = minCell.z; z <= maxCell.z; ++z)
        {
            for (uint32_t y = minCell.y; y <= maxCell.y; ++y)
            {
                for (uint32_t x = minCell.x; x <= maxCell.x; ++x)
                {
                    const size_t base = cellBase(uint3(x, y, z));
                    uint32_t& packedCount = mCardGridData[base];
                    const bool overflow = (packedCount & 0x80000000u) != 0u;
                    const uint32_t count = packedCount & 0x7fffffffu;
                    if (overflow)
                        continue;
                    if (count >= kMaxCandidates)
                    {
                        packedCount |= 0x80000000u;
                        ++mCardGridOverflowCells;
                        continue;
                    }
                    mCardGridData[base + 1u + count] = cardIndex;
                    packedCount = count + 1u;
                    ++mCardGridCandidateCount;
                }
            }
        }
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
    // Freeze the resource-shape defines at program creation.  In particular,
    // EnvMapSampler is a conditional global parameter; creating the pass with
    // the default (sampler absent) layout and changing it only after the first
    // compile can leave D3D12's root object/pipeline layout out of sync.
    defines.add("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    defines.add("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    defines.add("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    // Diagnostic-only switch for isolating inline shadow-ray variance. Production
    // keeps shadows enabled; C5 may set this environment variable to 1 for a
    // bounded control without changing the frozen lighting contract.
    const char* disableCacheLightingShadows = std::getenv("LUMEN_C5_DISABLE_CACHE_SHADOWS");
    const bool cacheLightingShadowsOff = disableCacheLightingShadows &&
        (std::string(disableCacheLightingShadows) == "1" || std::string(disableCacheLightingShadows) == "true");
    defines.add("LUMEN_GI_CACHE_LIGHTING_SHADOWS", cacheLightingShadowsOff ? "0" : "1");
    defines.add(
        "LUMEN_GI_HAS_ENVIRONMENT_SAMPLER",
        (kUseCacheLightingEnvImportanceSampler && mpEnvMapSampler) ? "1" : "0"
    );
    defines.add("LUMEN_GI_HAS_EMISSIVE_SAMPLER", mpEmissiveLightSampler ? "1" : "0");
    if (mpEmissiveLightSampler)
        defines.add(mpEmissiveLightSampler->getDefines());
    else
        defines.add("_EMISSIVE_LIGHT_SAMPLER_TYPE", "255");
    defines.add("is_valid_gMaterialParamsAtlas", "0");
    defines.add("is_valid_gLumenVisibilityAtlas", "1");
    defines.add("is_valid_gLumenGICounters", mpCacheLightingCounters ? "1" : "0");
    defines.add("is_valid_gDebugTexture", "0");
    const bool feedbackOn = mCacheLightingFeedbackEnabled && mUseCacheLighting && mUseSurfaceCache;
    defines.add("is_valid_gIndirectPrev", feedbackOn ? "1" : "0");
    defines.add("is_valid_gIndirectCurr", "1");
    defines.add("is_valid_gBounceCountAtlas", "1");
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
    if (!mCacheLighting.pPageMetadata || mCacheLighting.pPageMetadata->getElementCount() != pageCount + 1)
    {
        mCacheLighting.pPageMetadata = mpDevice->createStructuredBuffer(
            sizeof(uint4), pageCount + 1, ResourceBindFlags::ShaderResource
        );
        mCacheLighting.pPageMetadata->setName("LumenGIPass::CacheLighting::PageMetadata"); // pageID -> {generation,state,readyFrame,reserved}.
    }
    const size_t cardGridElementCount = size_t(LumenScreenProbe::kCacheCardGridDim) *
        size_t(LumenScreenProbe::kCacheCardGridDim) * size_t(LumenScreenProbe::kCacheCardGridDim) *
        size_t(LumenScreenProbe::kCacheCardGridCellStride);
    if (!mCacheLighting.pCardGrid || mCacheLighting.pCardGrid->getElementCount() != cardGridElementCount)
    {
        mCacheLighting.pCardGrid = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), cardGridElementCount, ResourceBindFlags::ShaderResource
        );
        mCacheLighting.pCardGrid->setName("LumenGIPass::CacheLighting::CardGrid"); // cell -> bounded card IDs.
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
    // Lookup eligibility is published only after a successful render-list
    // rebuild/dispatch below. Clear the publication token first so any early
    // return (missing resources, variant rebuild, empty list) cannot leave the
    // previous frame's grid/page metadata eligible for ScreenProbe lookup.
    mLastCacheLightingPageCount = 0u;

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

    const bool envSamplerVariant = kUseCacheLightingEnvImportanceSampler && mpEnvMapSampler != nullptr;
    if (mCacheLighting.pPass && mCacheLighting.envSamplerVariant != envSamplerVariant)
    {
        // HAS_ENVIRONMENT_SAMPLER changes the reflected root-object shape. Do
        // not mutate that define on an existing ProgramVars instance.
        mCacheLighting.pPass = nullptr;
    }
    mCacheLighting.envSamplerVariant = envSamplerVariant;

    if (!mpCardScene || !mCapture.pCards || !mCapture.pMaterialAtlas || !mCapture.pRadianceAtlas ||
        !mCapture.pMetadataAtlas)
    {
        return; // capture never ran; nothing to light.
    }

    ensureCacheLightingResources(pRenderContext);
    if (!mCacheLighting.pPass || !mCacheLighting.pVisibilityAtlas || !mCacheLighting.pPageToCard ||
        !mCacheLighting.pPageMetadata || !mCacheLighting.pRenderList)
    {
        return;
    }

    const uint32_t renderPageCount = buildCacheLightingRenderData();
    if (renderPageCount == 0)
    {
        mLastCacheLightingPageCount = 0;
        // Do not let a previous frame's candidate grid/page metadata remain
        // eligible when the allocator has no published pages this frame.
        // The shader's page fences are authoritative, but the host gate must
        // also disable lookup so a zero-resident frame cannot consume stale
        // candidates or emit requests from an old publication.
        mCardGridData.clear();
        mPageMetadataData.clear();
        return;
    }

    // Upload the two tables. The render list is re-uploaded in full every frame (small).
    mCacheLighting.pPageToCard->setBlob(mPageToCardData.data(), 0, mPageToCardData.size() * sizeof(uint32_t));
    mCacheLighting.pPageMetadata->setBlob(
        mPageMetadataData.data(), 0, mPageMetadataData.size() * sizeof(uint4)
    );
    if (mCacheLighting.pCardGrid && !mCardGridData.empty())
        mCacheLighting.pCardGrid->setBlob(mCardGridData.data(), 0, mCardGridData.size() * sizeof(uint32_t));
    mCacheLighting.pRenderList->setBlob(mRenderListData.data(), 0, renderPageCount * sizeof(uint32_t));

    // Per-frame program specialization. Program::addDefine returns true only when a value
    // changed; setVars(nullptr) recreates the vars (and thus the gScene binding) on change.
    ref<Program> pProgram = mCacheLighting.pPass->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    programChanged |= pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    programChanged |= pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    const char* disableCacheLightingShadows = std::getenv("LUMEN_C5_DISABLE_CACHE_SHADOWS");
    const bool cacheLightingShadowsOff = disableCacheLightingShadows &&
        (std::string(disableCacheLightingShadows) == "1" || std::string(disableCacheLightingShadows) == "true");
    programChanged |= pProgram->addDefine("LUMEN_GI_CACHE_LIGHTING_SHADOWS", cacheLightingShadowsOff ? "0" : "1");
    programChanged |= pProgram->addDefine(
        "LUMEN_GI_HAS_ENVIRONMENT_SAMPLER",
        (kUseCacheLightingEnvImportanceSampler && mpEnvMapSampler) ? "1" : "0"
    );
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
    // Inline visibility queries do not use a shader table or per-ray-type hit
    // records.  Request the canonical one-ray-type TLAS explicitly instead of
    // inheriting the last RT-pipeline ray count from another pass in the graph.
    // This keeps two LumenGI instances in a paired graph on the same TLAS cache
    // entry and avoids cross-pass ray-query state selection.
    mpScene->bindShaderDataForRaytracing(pRenderContext, cacheVar["gScene"], 1u);

    cacheVar["gCards"] = mCapture.pCards;
    cacheVar["gMaterialAtlas"] = mCapture.pMaterialAtlas;
    cacheVar["gMetadataAtlas"] = mCapture.pMetadataAtlas;
    cacheVar["gRadianceAtlas"] = mCapture.pRadianceAtlas;
    cacheVar["gLumenVisibilityAtlas"] = mCacheLighting.pVisibilityAtlas;
    cacheVar["gLumenPageToCard"] = mCacheLighting.pPageToCard;
    cacheVar["gLumenPageMetadata"] = mCacheLighting.pPageMetadata;
    cacheVar["gLumenRenderList"] = mCacheLighting.pRenderList;
    cacheVar["gLumenGICounters"] = mpCacheLightingCounters;
    // S3-B2 feedback double buffer. When the feedback is off, gIndirectPrev is left unbound
    // (is_valid_gIndirectPrev = 0 -> shader guard turns the feedback off) and the shader clears
    // gIndirectCurr to zero, so a later enable starts from a clean single bounce.
    if (feedbackOn)
        cacheVar["gIndirectPrev"] = mCacheLighting.pIndirect[1 - mCacheLighting.indirectCurrIndex];
    cacheVar["gIndirectCurr"] = mCacheLighting.pIndirect[mCacheLighting.indirectCurrIndex];
    cacheVar["gBounceCountAtlas"] = mCacheLighting.pBounceCount;
    if (kUseCacheLightingEnvImportanceSampler && mpEnvMapSampler)
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

    // The visibility atlas is a per-dispatch output. Clear it before lighting so
    // fallback/unsupported texels from a previous page owner cannot survive a
    // recapture and feed the probe lookup on the next pass.
    if (mCacheLighting.pVisibilityAtlas)
        pRenderContext->clearUAV(mCacheLighting.pVisibilityAtlas->getUAV().get(), float4(0.f));

    // The counters must be cleared before each dispatch; copied to the readback buffer after.
    if (mpCacheLightingCounters)
        pRenderContext->clearUAV(mpCacheLightingCounters->getUAV().get(), uint4(0));

    // One 16x16 threadgroup per page: ComputePass::execute takes THREAD counts, so the group
    // count is nThreads / threadGroupSize. Passing (renderPageCount, 1, 1) threads would give
    // ceil(renderPageCount/16) groups (wrong); multiply the X axis by the tile size so the
    // dispatch issues exactly renderPageCount threadgroups (SV_GroupID.x == page index).
    const uint3 dispatchThreads(renderPageCount * kLumenSurfaceCacheTileSize, kLumenSurfaceCacheTileSize, 1u);
    const uint3 threadGroupSize = mCacheLighting.pPass->getThreadGroupSize();
    if (threadGroupSize.x == 0u || threadGroupSize.y == 0u || threadGroupSize.z == 0u)
    {
        logWarning("LumenGI cache lighting skipped: invalid shader thread-group size ({}, {}, {}).", threadGroupSize.x,
                   threadGroupSize.y, threadGroupSize.z);
        return;
    }
    const uint3 dispatchGroups = div_round_up(dispatchThreads, threadGroupSize);
    logInfo(
        "LumenGI cache lighting dispatch: frame={} pages={} threads=({}, {}, {}) groups=({}, {}, {}) tg=({}, {}, {}) "
        "env={} analytic={} emissive={} envSampler={} emissiveSampler={} tlas={} visibility={} pageToCard={} renderList={} "
        "indirectCurr={} indirectPrev={} bounceCount={} feedback={}",
        mFrameIndex, renderPageCount, dispatchThreads.x, dispatchThreads.y, dispatchThreads.z, dispatchGroups.x,
        dispatchGroups.y, dispatchGroups.z, threadGroupSize.x, threadGroupSize.y, threadGroupSize.z,
        mpScene->useEnvLight() ? 1u : 0u, mpScene->useAnalyticLights() ? 1u : 0u,
        mpScene->useEmissiveLights() ? 1u : 0u,
        (kUseCacheLightingEnvImportanceSampler && mpEnvMapSampler) ? 1u : 0u, mpEmissiveLightSampler ? 1u : 0u,
        mpScene->getSceneStats().tlasCount ? 1u : 0u, mCacheLighting.pVisibilityAtlas ? 1u : 0u,
        mCacheLighting.pPageToCard ? 1u : 0u, mCacheLighting.pRenderList ? 1u : 0u,
        mCacheLighting.pIndirect[mCacheLighting.indirectCurrIndex] ? 1u : 0u,
        mCacheLighting.pIndirect[1u - mCacheLighting.indirectCurrIndex] ? 1u : 0u,
        mCacheLighting.pBounceCount ? 1u : 0u, feedbackOn ? 1u : 0u
    );
    if (dispatchGroups.x > 65535u || dispatchGroups.y > 65535u || dispatchGroups.z > 65535u)
    {
        logWarning("LumenGI cache lighting skipped: dispatch groups exceed D3D12 limits ({}, {}, {}).", dispatchGroups.x,
                   dispatchGroups.y, dispatchGroups.z);
        return;
    }
    mCacheLighting.pPass->execute(pRenderContext, dispatchThreads);

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

void LumenGIPass::exportCacheCaptureRadiance(RenderContext* pRenderContext, const RenderData& renderData)
{
    const ref<Texture> pCacheCapture = renderData.getTexture(kCacheCaptureRadiance);
    if (!pCacheCapture)
        return;
    if (mCapture.pRadianceAtlas)
        pRenderContext->blit(mCapture.pRadianceAtlas->getSRV(), pCacheCapture->getRTV());
    else
        pRenderContext->clearRtv(pCacheCapture->getRTV().get(), float4(0.f));
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
        hzbVar["gHZBTarget"].setUav(mScreenTrace.pHZBMips[d.mip]->getUAV(0));
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
    mScreenProbes.pScreenRadianceHistoryPass = ComputePass::create(
        mpDevice, kScreenRadianceHistoryShaderFile, "main"
    );

    // S4.3 integrate + interpolate: pure compute over the frozen data contract
    // (LumenScreenProbeData.slang). They import no scene modules and trace no rays, so they
    // are created WITHOUT the scene shader modules / type conformances / gScene binding.
    // LUMEN_GI_PROBE_SCENE_TRACE=1 skips the data module's #if !LUMEN_GI_PROBE_SCENE_TRACE
    // gProbeTLAS declaration (no TLAS is ever bound to these passes).
    const auto createProbeCompute = [&](const char* shaderFile, const char* entry, bool enableHistory)
    {
        DefineList defines;
        defines.add("LUMEN_GI_PROBE_SCENE_TRACE", "1");
        if (enableHistory)
            defines.add("LUMEN_GI_PROBE_HISTORY", "1");
        return ComputePass::create(mpDevice, shaderFile, entry, defines);
    };
    mScreenProbes.pIntegrate = createProbeCompute(kScreenProbeIntegrateShaderFile, "main", true);
    mScreenProbes.pInterpolate = createProbeCompute(kScreenProbeInterpolateShaderFile, "main", false);
}

void LumenGIPass::ensureScreenProbeResources(RenderContext* pRenderContext)
{
    if (any(mFrameDim == uint2(0u, 0u)))
        return;

    const uint32_t probeCount = LumenScreenProbe::probeCount(mFrameDim);
    const uint32_t requestCardCount = mpCardScene ? std::max<uint32_t>(1u, mpCardScene->getCardCount()) : 1u;
    if (!mScreenProbes.pTrace || !mScreenProbes.pUpdate || !mScreenProbes.pFinalize ||
        !mScreenProbes.pScreenRadianceHistoryPass || !mScreenProbes.pIntegrate || !mScreenProbes.pInterpolate)
        createScreenProbePrograms();
    if (!mScreenProbes.pTrace)
        return; // scene not ready.

    // Probe count alone is not a sufficient resize key: ceil(frameDim / tileSize)
    // can stay constant while the aspect ratio changes (for example 640x360 ->
    // 640x352).  Recreate all screen-probe resources whenever either the grid
    // cardinality or the backing frame dimensions change, otherwise the old
    // metadata/HZB/radiance dimensions are reused with a new dispatch shape.
    if (!mScreenProbes.pMetadata || !mScreenProbes.pHitRecords || mScreenProbes.probeCount != probeCount ||
        any(mScreenProbes.resourceDim != mFrameDim) || !mScreenProbes.pCacheFeedback ||
        mScreenProbes.pCacheFeedback->getElementCount() != mPageCache.getPageCount() + 1u ||
        !mScreenProbes.pCacheRequests || mScreenProbes.pCacheRequests->getElementCount() != requestCardCount ||
        !mScreenProbes.pScreenRadianceValidity[0] || !mScreenProbes.pScreenRadianceValidity[1])
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
        // C6.1 demand feedback: one uint2 per page (hit count, observed page generation).
        // The UAV is cleared before each probe dispatch and copied to a readback buffer after
        // integrate.  Host validation against the current scene/page generation prevents stale
        // feedback from touching a reused page.
        const uint32_t feedbackPageCount = mPageCache.getPageCount() + 1u;
        mScreenProbes.pCacheFeedback = mpDevice->createStructuredBuffer(
            sizeof(uint2), feedbackPageCount, ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pCacheFeedback->setName("LumenGIPass::ScreenProbe::CacheFeedback");
        mScreenProbes.pCacheFeedbackReadback = mpDevice->createStructuredBuffer(
            sizeof(uint2), feedbackPageCount, ResourceBindFlags::None, MemoryType::ReadBack
        );
        mScreenProbes.pCacheFeedbackReadback->setName("LumenGIPass::ScreenProbe::CacheFeedbackReadback");
        mScreenProbes.cacheFeedbackPageCount = feedbackPageCount;
        mScreenProbes.pCacheRequests = mpDevice->createStructuredBuffer(
            sizeof(uint2), requestCardCount, ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pCacheRequests->setName("LumenGIPass::ScreenProbe::CacheRequests");
        mScreenProbes.pCacheRequestsReadback = mpDevice->createStructuredBuffer(
            sizeof(uint2), requestCardCount, ResourceBindFlags::None, MemoryType::ReadBack
        );
        mScreenProbes.pCacheRequestsReadback->setName("LumenGIPass::ScreenProbe::CacheRequestsReadback");
        mScreenProbes.cacheRequestCardCount = requestCardCount;

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
        mScreenProbes.pInterpolated = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pInterpolated->setName("LumenGIPass::ScreenProbe::InterpolatedInternal");
        mScreenProbes.pRadianceHistory = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mScreenProbes.pRadianceHistory->setName("LumenGIPass::ScreenProbe::RadianceHistory"); // RGB mean, A sample count.

        for (uint32_t i = 0; i < 2u; ++i)
        {
            mScreenProbes.pScreenRadianceHistory[i] = mpDevice->createTexture2D(
                mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mScreenProbes.pScreenRadianceHistory[i]->setName(
                i == 0u ? "LumenGIPass::ScreenProbe::ScreenRadianceHistory0"
                        : "LumenGIPass::ScreenProbe::ScreenRadianceHistory1"
            );
            mScreenProbes.pScreenRadianceDepthHistory[i] = mpDevice->createTexture2D(
                mFrameDim.x, mFrameDim.y, ResourceFormat::RG32Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mScreenProbes.pScreenRadianceDepthHistory[i]->setName(
                i == 0u ? "LumenGIPass::ScreenProbe::ScreenRadianceDepthHistory0"
                        : "LumenGIPass::ScreenProbe::ScreenRadianceDepthHistory1"
            );
            mScreenProbes.pScreenRadianceGuideHistory[i] = mpDevice->createTexture2D(
                mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mScreenProbes.pScreenRadianceGuideHistory[i]->setName(
                i == 0u ? "LumenGIPass::ScreenProbe::ScreenRadianceGuideHistory0"
                        : "LumenGIPass::ScreenProbe::ScreenRadianceGuideHistory1"
            );
            mScreenProbes.pScreenRadianceMoments[i] = mpDevice->createTexture2D(
                mFrameDim.x, mFrameDim.y, ResourceFormat::RG32Float, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mScreenProbes.pScreenRadianceMoments[i]->setName(
                i == 0u ? "LumenGIPass::ScreenProbe::ScreenRadianceMoments0"
                        : "LumenGIPass::ScreenProbe::ScreenRadianceMoments1"
            );
            mScreenProbes.pScreenRadianceLightingGeneration[i] = mpDevice->createTexture2D(
                mFrameDim.x, mFrameDim.y, ResourceFormat::R32Uint, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mScreenProbes.pScreenRadianceLightingGeneration[i]->setName(
                i == 0u ? "LumenGIPass::ScreenProbe::ScreenRadianceLightingGeneration0"
                        : "LumenGIPass::ScreenProbe::ScreenRadianceLightingGeneration1"
            );
            mScreenProbes.pScreenRadianceAge[i] = mpDevice->createTexture2D(
                mFrameDim.x, mFrameDim.y, ResourceFormat::R32Uint, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mScreenProbes.pScreenRadianceAge[i]->setName(
                i == 0u ? "LumenGIPass::ScreenProbe::ScreenRadianceAge0"
                        : "LumenGIPass::ScreenProbe::ScreenRadianceAge1"
            );
            mScreenProbes.pScreenRadianceValidity[i] = mpDevice->createTexture2D(
                mFrameDim.x, mFrameDim.y, ResourceFormat::R32Uint, 1, 1, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mScreenProbes.pScreenRadianceValidity[i]->setName(
                i == 0u ? "LumenGIPass::ScreenProbe::ScreenRadianceValidity0"
                        : "LumenGIPass::ScreenProbe::ScreenRadianceValidity1"
            );
        }

        // Prefill the static probe positions (tile centers). Everything else is zero
        // (the update pass resamples active/depth/normal/worldPos every frame).
        std::vector<LumenScreenProbe::Meta> metas(probeCount);
        const uint2 gridDims = LumenScreenProbe::probeGridDims(mFrameDim);
        for (uint32_t i = 0; i < probeCount; ++i)
        {
            const uint2 gridPos = uint2(i % gridDims.x, i / gridDims.x);
            const float2 unclamped = LumenScreenProbe::probeScreenPos(gridPos);
            const float2 frameMax = float2(
                std::max(0.5f, float(mFrameDim.x) - 0.5f),
                std::max(0.5f, float(mFrameDim.y) - 0.5f)
            );
            metas[i].screenPos = clamp(unclamped, float2(0.5f), frameMax);
            metas[i].dirty = 1u; // first update must not reuse any prior history.
        }
        mScreenProbes.pMetadata->setBlob(metas.data(), 0, metas.size() * sizeof(LumenScreenProbe::Meta));
        pRenderContext->clearUAV(mScreenProbes.pHitRecords->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mScreenProbes.pCounters->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mScreenProbes.pCacheFeedback->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mScreenProbes.pCacheRequests->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mScreenProbes.pRadiance->getUAV().get(), float4(0.f));
        pRenderContext->clearUAV(mScreenProbes.pRadianceHistory->getUAV().get(), float4(0.f));
        pRenderContext->clearUAV(mScreenProbes.pInterpolated->getUAV().get(), float4(0.f));
        for (uint32_t i = 0; i < 2u; ++i)
        {
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceHistory[i]->getUAV().get(), float4(0.f));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceDepthHistory[i]->getUAV().get(), float4(0.f));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceGuideHistory[i]->getUAV().get(), float4(0.f));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceMoments[i]->getUAV().get(), float4(0.f));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceLightingGeneration[i]->getUAV().get(), uint4(0));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceAge[i]->getUAV().get(), uint4(0));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceValidity[i]->getUAV().get(), uint4(0));
        }

        mScreenProbes.probeCount = probeCount;
        mScreenProbes.resourceDim = mFrameDim;
        mScreenProbes.screenRadianceHistoryCurrIndex = 0u;
        if (mScreenProbes.pCacheFeedback)
            pRenderContext->clearUAV(mScreenProbes.pCacheFeedback->getUAV().get(), uint4(0));
        if (mScreenProbes.pCacheRequests)
            pRenderContext->clearUAV(mScreenProbes.pCacheRequests->getUAV().get(), uint4(0));
        mScreenProbes.cacheFeedbackReadbackPending = false;
        mScreenProbes.cacheFeedbackSubmittedFrame = 0;
        mScreenProbes.cacheRequestReadbackPending = false;
        mScreenProbes.historyResetPending = true;
    }
}

void LumenGIPass::readbackScreenProbeCounters(RenderContext* pRenderContext)
{
    const bool haveCounters = mScreenProbes.counterReadbackPending && mScreenProbes.pCountersReadback;
    const bool haveFeedback = mScreenProbes.cacheFeedbackReadbackPending && mScreenProbes.pCacheFeedbackReadback;
    const bool haveRequests = mScreenProbes.cacheRequestReadbackPending && mScreenProbes.pCacheRequestsReadback;
    if (!haveCounters && !haveFeedback && !haveRequests)
        return;
    auto recordRejectedRequestEvent = [&](uint32_t cardIndex, uint32_t requestReason, uint32_t requestCount)
    {
        if (requestCount == 0u)
            return;
        const bool alreadyTerminal = std::any_of(
            mSurfaceCacheRequestEvents.begin(), mSurfaceCacheRequestEvents.end(),
            [&](const SurfaceCacheRequestEvent& event)
            {
                return event.sceneGeneration == mSurfaceCacheSceneGeneration &&
                    event.cardIndex == cardIndex && event.state == 5u &&
                    event.requestFrame == mSurfaceCacheFrameIndex;
            }
        );
        if (alreadyTerminal)
            return;
        if (mSurfaceCacheRequestEvents.size() >= kMaxSurfaceCacheRequestEvents)
        {
            mSurfaceCacheRequestEvents.erase(mSurfaceCacheRequestEvents.begin());
            ++mSurfaceCacheRequestEventDropped;
        }
        SurfaceCacheRequestEvent rejected;
        rejected.sequence = ++mSurfaceCacheRequestEventSequence;
        rejected.sceneGeneration = mSurfaceCacheSceneGeneration;
        rejected.cardIndex = cardIndex;
        rejected.requestFrame = mSurfaceCacheFrameIndex;
        rejected.reasonBits = requestReason | 2u; // explicit terminal stale/rejected outcome.
        rejected.requestCount = requestCount;
        rejected.state = 5u;
        mSurfaceCacheRequestEvents.push_back(rejected);
    };
    if (haveCounters)
    {
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
            mScreenProbeStats.gdfHits = pCounters->gdfHits;
            mScreenProbeStats.gdfMisses = pCounters->gdfMisses;
            mScreenProbeStats.cacheLookupHits = pCounters->cacheLookupHits;
            mScreenProbeStats.cacheLookupAttempts = pCounters->cacheLookupAttempts;
            mScreenProbeStats.cachePageRejects = pCounters->cachePageRejects;
            mScreenProbeStats.cacheCoverageRejects = pCounters->cacheCoverageRejects;
            mScreenProbeStats.cacheMetadataRejects = pCounters->cacheMetadataRejects;
            mScreenProbeStats.cacheVisibilityRejects = pCounters->cacheVisibilityRejects;
            mScreenProbeStats.historyAccepted = pCounters->historyAccepted;
            mScreenProbeStats.historyRejectDepth = pCounters->historyRejectDepth;
            mScreenProbeStats.historyRejectGuide = pCounters->historyRejectGuide;
            mScreenProbeStats.historyRejectMotion = pCounters->historyRejectMotion;
            mScreenProbeStats.historyRejectLighting = pCounters->historyRejectLighting;
            mScreenProbeStats.historyRejectCurrentInvalid = pCounters->historyRejectCurrentInvalid;
            mScreenProbeStats.historyRejectPreviousInvalid = pCounters->historyRejectPreviousInvalid;
            mScreenProbeStats.historyReset = pCounters->historyReset;
            mScreenProbeStats.cacheDepthRejects = pCounters->cacheDepthRejects;
            mScreenProbeStats.cacheAxisRejects = pCounters->cacheAxisRejects;
            mScreenProbeStats.cacheFacingRejects = pCounters->cacheFacingRejects;
            mScreenProbeStats.cacheOwnerValid = pCounters->cacheOwnerValid;
            mScreenProbeStatsFrame = mScreenProbeCountersSubmittedFrame;
            mScreenProbes.pCountersReadback->unmap();
        }
        mScreenProbes.counterReadbackPending = false;
    }

    if (haveFeedback)
    {
        const uint2* pFeedback = static_cast<const uint2*>(mScreenProbes.pCacheFeedbackReadback->map());
        if (pFeedback)
        {
            // Feedback is copied after integrate and consumed at the next
            // execute entry. Preserve the dispatch-domain frame for event
            // provenance instead of shifting firstHitFrame by one.
            const uint32_t feedbackFrame = mScreenProbes.cacheFeedbackSubmittedFrame;
            const uint32_t currentPageCount = mPageCache.getPageCount() + 1u;
            const uint32_t pageCount = std::min(mScreenProbes.cacheFeedbackPageCount, currentPageCount);
            const bool sameScene = mScreenProbes.cacheFeedbackSceneGeneration == mSurfaceCacheSceneGeneration;
            for (uint32_t pageID = 1u; pageID < pageCount; ++pageID)
            {
                const uint32_t hitCount = pFeedback[pageID].x;
                const uint32_t generation = pFeedback[pageID].y;
                if (hitCount == 0u)
                    continue;

                mSurfaceCacheFeedbackHits += hitCount;
                const bool validState = mPageCache.isPageAllocated(pageID) &&
                    (mPageCache.getPageState(pageID) == LumenSurfaceCachePageState::Allocated ||
                     mPageCache.getPageState(pageID) == LumenSurfaceCachePageState::Touched);
                if (!sameScene || generation == 0u || !validState || mPageCache.getGeneration(pageID) != generation)
                {
                    ++mSurfaceCacheFeedbackStaleRejects;
                    continue;
                }
                if (mPageCache.touchPage(pageID))
                {
                    ++mSurfaceCacheFeedbackPages;
                    if (hitCount > 1u)
                        mSurfaceCacheFeedbackDedup += hitCount - 1u;
                    const uint32_t cardIndex = pageID < mPageToCardData.size() ? mPageToCardData[pageID] : kLumenCardInvalidID;
                    for (auto it = mSurfaceCacheRequestEvents.rbegin();
                         it != mSurfaceCacheRequestEvents.rend(); ++it)
                    {
                        if (it->sceneGeneration == mSurfaceCacheSceneGeneration &&
                            it->pageID == pageID && it->generation == generation &&
                            (it->cardIndex == cardIndex || cardIndex == kLumenCardInvalidID) &&
                            it->readyFrame != 0u && it->firstHitFrame == 0u)
                        {
                            if (feedbackFrame >= it->readyFrame && feedbackFrame >= it->requestFrame)
                            {
                                it->firstHitFrame = feedbackFrame;
                                it->lookupHits = hitCount;
                                it->state = 4u;
                            }
                            break;
                        }
                    }
                }
            }
            mScreenProbes.pCacheFeedbackReadback->unmap();
        }
        mScreenProbes.cacheFeedbackReadbackPending = false;
        mScreenProbes.cacheFeedbackSubmittedFrame = 0;
    }

    if (haveRequests)
    {
        const uint2* pRequests = static_cast<const uint2*>(mScreenProbes.pCacheRequestsReadback->map());
        if (pRequests)
        {
            const uint32_t currentCardCount = mpCardScene ? mpCardScene->getCardCount() : 0u;
            const uint32_t cardCount = std::min(mScreenProbes.cacheRequestCardCount, currentCardCount);
            const bool sameScene = mScreenProbes.cacheRequestSceneGeneration == mSurfaceCacheSceneGeneration;
            for (uint32_t cardIndex = 0u; cardIndex < cardCount; ++cardIndex)
            {
                const uint32_t requestCount = pRequests[cardIndex].x;
                if (requestCount == 0u)
                    continue;
                // The shader stores an OR of reason bits alongside the raw
                // count. Preserve those bits as typed telemetry instead of
                // discarding the only evidence needed to prioritize miss
                // requests (unmapped, stale owner, metadata, visibility).
                const uint32_t requestReason = pRequests[cardIndex].y;
                if ((requestReason & 1u) != 0u)
                    mSurfaceCacheRequestUnmapped += requestCount;
                if ((requestReason & 2u) != 0u)
                    mSurfaceCacheRequestStaleOwner += requestCount;
                if ((requestReason & 4u) != 0u)
                    mSurfaceCacheRequestMetadataInvalid += requestCount;
                if ((requestReason & 8u) != 0u)
                    mSurfaceCacheRequestVisibilityInvalid += requestCount;
                mSurfaceCacheRequestRaw += requestCount;
                mSurfaceCacheRequestRawThisFrame += requestCount;
                mSurfaceCacheRequestObservedFrame = mSurfaceCacheFrameIndex;
                if (requestCount > 1u)
                    mSurfaceCacheRequestDedup += requestCount - 1u;
                if (!sameScene || !mpCardScene || cardIndex >= mpCardScene->getCardCount())
                {
                    ++mSurfaceCacheRequestStaleRejects;
                    recordRejectedRequestEvent(cardIndex, requestReason, requestCount);
                    continue;
                }
                // Defer scheduler insertion until the next host frame.  The
                // request was produced by the previous GPU dispatch, so this
                // preserves an explicit N -> N+1 capture/publication fence.
                const bool alreadyRequested =
                    mSurfaceCacheRequestedCards.find(cardIndex) != mSurfaceCacheRequestedCards.end() ||
                    mSurfaceCacheDeferredRequestCards.find(cardIndex) != mSurfaceCacheDeferredRequestCards.end();
                const bool canQueue = mCaptureScheduler.canAcceptFeedbackRequest(cardIndex);
                if (!alreadyRequested && canQueue && mSurfaceCacheDeferredRequestCards.insert(cardIndex).second)
                {
                    mSurfaceCacheDeferredRequestFrame = mSurfaceCacheFrameIndex;
                    mSurfaceCacheDeferredRequestFrameByCard[cardIndex] = mSurfaceCacheFrameIndex;
                    ++mSurfaceCacheRequestCards;
                    ++mSurfaceCacheRequestCardsThisFrame;
                    // Record only requests accepted by the deferred scheduler. Raw GPU misses
                    // rejected by canAcceptFeedbackRequest are not lifecycle events.
                    if (mSurfaceCacheRequestEvents.size() >= kMaxSurfaceCacheRequestEvents)
                    {
                        mSurfaceCacheRequestEvents.erase(mSurfaceCacheRequestEvents.begin());
                        ++mSurfaceCacheRequestEventDropped;
                    }
                    SurfaceCacheRequestEvent newEvent;
                    newEvent.sequence = ++mSurfaceCacheRequestEventSequence;
                    newEvent.sceneGeneration = mSurfaceCacheSceneGeneration;
                    newEvent.cardIndex = cardIndex;
                    newEvent.requestFrame = mSurfaceCacheFrameIndex;
                    newEvent.reasonBits = requestReason;
                    newEvent.requestCount = requestCount;
                    newEvent.state = 1u;
                    mSurfaceCacheRequestEvents.push_back(newEvent);
                }
                else
                {
                    if (!alreadyRequested && !canQueue)
                    {
                        ++mSurfaceCacheRequestStaleRejects;
                        recordRejectedRequestEvent(cardIndex, requestReason, requestCount);
                        continue;
                    }
                    ++mSurfaceCacheRequestDedup;
                    // Coalesce repeated misses into the outstanding card event, preserving its
                    // original request frame and card/page identity.
                    for (auto eventIt = mSurfaceCacheRequestEvents.rbegin();
                         eventIt != mSurfaceCacheRequestEvents.rend(); ++eventIt)
                    {
                        if (eventIt->sceneGeneration == mSurfaceCacheSceneGeneration &&
                            eventIt->cardIndex == cardIndex && eventIt->state < 3u)
                        {
                            eventIt->reasonBits |= requestReason;
                            eventIt->requestCount += requestCount;
                            break;
                        }
                    }
                }
            }
            mScreenProbes.pCacheRequestsReadback->unmap();
        }
        mScreenProbes.cacheRequestReadbackPending = false;
    }
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

    ensureScreenProbeResources(pRenderContext);
    if (!mScreenProbes.pUpdate || !mScreenProbes.pTrace || !mScreenProbes.pFinalize ||
        !mScreenProbes.pScreenRadianceHistoryPass ||
        !mScreenProbes.pIntegrate || !mScreenProbes.pInterpolate ||
        !mScreenProbes.pMetadata || !mScreenProbes.pHitRecords || !mScreenProbes.pRadiance ||
        !mScreenProbes.pRadianceHistory || !mScreenProbes.pInterpolated ||
        !mScreenProbes.pScreenRadianceHistory[0] || !mScreenProbes.pScreenRadianceHistory[1] ||
        !mScreenProbes.pScreenRadianceDepthHistory[0] || !mScreenProbes.pScreenRadianceDepthHistory[1] ||
        !mScreenProbes.pScreenRadianceGuideHistory[0] || !mScreenProbes.pScreenRadianceGuideHistory[1] ||
        !mScreenProbes.pScreenRadianceMoments[0] || !mScreenProbes.pScreenRadianceMoments[1] ||
        !mScreenProbes.pScreenRadianceLightingGeneration[0] || !mScreenProbes.pScreenRadianceLightingGeneration[1] ||
        !mScreenProbes.pScreenRadianceAge[0] || !mScreenProbes.pScreenRadianceAge[1] ||
        !mScreenProbes.pScreenRadianceValidity[0] || !mScreenProbes.pScreenRadianceValidity[1] ||
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
    const ref<Buffer> pProbeValidity = dynamic_ref_cast<Buffer>(renderData.getResource("probeValidity"));
    const bool hasProbeValidity = pProbeValidity != nullptr;
    const bool hasProbeInterpolated = renderData.getTexture("probeInterpolated") != nullptr;
    const bool hasProbeHistory = renderData.getTexture(kProbeHistory) != nullptr;
    const ref<Texture> pScreenRadianceLightingGenerationOutput = renderData.getTexture("screenRadianceLightingGeneration");
    const ref<Texture> pScreenRadianceHistoryAgeOutput = renderData.getTexture("screenRadianceHistoryAge");
    const ref<Texture> pScreenRadianceHistoryValidityOutput = renderData.getTexture("screenRadianceHistoryValidity");
    const bool hasNormal = renderData.getTexture("normWRoughnessMaterialID") != nullptr;
    const bool hasDiffuseRadiance = renderData.getTexture("diffuseRadianceHitDist") != nullptr;
    const bool hasSurfaceCacheLookup = mUseSurfaceCache && mUseCacheLighting && mCapture.pCards &&
        mCapture.pPageTable && mCapture.pPageGeneration && mCapture.pMetadataAtlas &&
        mCapture.pRadianceAtlas && mCacheLighting.pVisibilityAtlas && mCacheLighting.pPageMetadata &&
        mCacheLighting.pCardGrid && !mCardGridData.empty() && mLastCacheLightingPageCount > 0u;
    // C4 production router: screen miss -> composed GDF -> HWRT.  The GDF is only
    // eligible after compose resources exist; a missing level table keeps the legacy
    // screen -> HWRT path intact for the first setup frame.
    const bool hasGDFRoute = mUseGDF && mSDF.gdf && mSDF.pLevelTable && !mSDF.levels.empty();

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
        programChanged |= pProgram->addDefine("is_valid_gProbeValidity", hasProbeValidity ? "1" : "0");
        programChanged |= pProgram->addDefine("LUMEN_GI_PROBE_GDF_ROUTE", hasGDFRoute ? "1" : "0");
        programChanged |= pProgram->addDefine(
            "is_valid_gScreenRadianceHistory", (hasDiffuseRadiance && mScreenProbes.pScreenRadianceHistory[0]) ? "1" : "0"
        );
        programChanged |= pProgram->addDefine(
            "is_valid_gScreenRadianceHistoryValidity",
            (hasDiffuseRadiance && mScreenProbes.pScreenRadianceValidity[0]) ? "1" : "0"
        );
    }
    programChanged |= mScreenProbes.pTrace->getProgram()->addDefine(
        "is_valid_gScreenRadianceMoments",
        (mUseScreenRadianceMoments && hasDiffuseRadiance && mScreenProbes.pScreenRadianceMoments[0]) ? "1" : "0"
    );

    // S4.3 integrate/interpolate passes: all inputs are internal + always bound, so their
    // is_valid defines are pinned to 1 (gProbeRadiance here is the INTERNAL pRadiance texture,
    // independent of the graph "probeRadiance" channel). gGIOutput is the graph-optional
    // "probeInterpolated" output; the passes are skipped entirely when it is not allocated.
    if (hasNormal)
    {
        for (const ref<ComputePass>& pPass : {mScreenProbes.pIntegrate, mScreenProbes.pInterpolate})
        {
            ref<Program> pProgram = pPass->getProgram();
            programChanged |= pProgram->addDefine("is_valid_gProbeMeta", "1");
            programChanged |= pProgram->addDefine("is_valid_gProbeHitRecords", "1");
            programChanged |= pProgram->addDefine("is_valid_gProbeRadiance", "1");
            programChanged |= pProgram->addDefine("is_valid_gProbeRadianceHistory", "1");
            programChanged |= pProgram->addDefine("is_valid_gLinearZ", "1");
            programChanged |= pProgram->addDefine("is_valid_gNormalRoughnessMaterialID", "1");
            programChanged |= pProgram->addDefine("is_valid_gGIOutput", "1");
        }
        programChanged |= mScreenProbes.pIntegrate->getProgram()->addDefine("LUMEN_GI_PROBE_HISTORY", "1");
        programChanged |= mScreenProbes.pIntegrate->getProgram()->addDefine("is_valid_gProbeRadianceHistory", "1");
        programChanged |= mScreenProbes.pIntegrate->getProgram()->addDefine(
            "LUMEN_GI_PROBE_CACHE_LOOKUP", hasSurfaceCacheLookup ? "1" : "0"
        );
        programChanged |= mScreenProbes.pIntegrate->getProgram()->addDefine(
            "is_valid_gProbeCacheFeedback",
            (hasSurfaceCacheLookup && mScreenProbes.pCacheFeedback) ? "1" : "0"
        );
        programChanged |= mScreenProbes.pIntegrate->getProgram()->addDefine(
            "is_valid_gProbeCacheRequests",
            (hasSurfaceCacheLookup && mScreenProbes.pCacheRequests) ? "1" : "0"
        );
    }
    if (programChanged)
    {
        mScreenProbes.pUpdate->setVars(nullptr);
        mScreenProbes.pTrace->setVars(nullptr);
        mScreenProbes.pFinalize->setVars(nullptr);
        mScreenProbes.pScreenRadianceHistoryPass->setVars(nullptr);
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
        hzbVar["gHZBTarget"].setUav(mScreenProbes.pHZBNative->getUAV(0));
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
            hzbVar["gHZBTarget"].setUav(mScreenProbes.pHZBNative->getUAV(mip));
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
        cb["gHistoryGeneration"] = (uint32_t)mHistoryGeneration;
        cb["gLightingGeneration"] = (uint32_t)mLightingGeneration;
        cb["gResetReason"] = static_cast<uint32_t>(mLastHistoryResetReason);
        cb["gValidityStride"] = LumenScreenProbe::kMaxDirectionsPerProbe;
        cb["gSurfaceCacheFrameIndex"] = mSurfaceCacheFrameIndex;
        if (hasGDFRoute)
        {
            auto gdfClip = cb["gGDFClipmap"];
            const auto center = mSDF.gdf->getCameraCenter();
            const auto scroll = mSDF.gdf->scrollFromCameraMove();
            gdfClip["cameraCenter"] = float3(center.x, center.y, center.z);
            gdfClip["emptyDistance"] = 0.f;
            gdfClip["levelCount"] = mSDF.gdf->getLevelCount();
            gdfClip["dynamicLevelCount"] = LumenGI::GlobalDistanceField::kDynamicLevels;
            gdfClip["resolution"] = mSDF.gdf->getResolution();
            gdfClip["instanceCount"] = 0u;
            gdfClip["dirtyRegionCount"] = 0u;
            gdfClip["frameIndex"] = mFrameIndex;
            gdfClip["scroll"] = int3(scroll.x, scroll.y, scroll.z);
            cb["gGDFTraceMaxDistance"] = mGDFTraceMaxDistance;
            cb["gGDFMaxSteps"] = mGDFTraceMaxSteps;
            cb["gGDFRouteEnabled"] = 1u;
        }

        // Resources (shared by all three entry points).
        var["gLinearZ"] = pLinearZ;
        if (mScreenProbes.pHZBNative)
            var["gHZBMips"] = mScreenProbes.pHZBNative;
        if (hasNormal)
            var["gNormalRoughnessMaterialID"] = renderData.getTexture("normWRoughnessMaterialID");
        if (hasDiffuseRadiance)
            var["gDiffuseRadianceHitDist"] = renderData.getTexture("diffuseRadianceHitDist");
        if (hasDiffuseRadiance && mScreenProbes.pScreenRadianceHistory[0])
        {
            const uint32_t readIndex = 1u - mScreenProbes.screenRadianceHistoryCurrIndex;
            var["gScreenRadianceHistory"] = mScreenProbes.pScreenRadianceHistory[readIndex];
            var["gScreenRadianceHistoryValidity"] = mScreenProbes.pScreenRadianceValidity[readIndex];
            if (pPass.get() == mScreenProbes.pScreenRadianceHistoryPass.get())
                var["gPreviousAge"] = mScreenProbes.pScreenRadianceAge[readIndex];
            if (pPass.get() == mScreenProbes.pTrace.get() && mUseScreenRadianceMoments && mScreenProbes.pScreenRadianceMoments[0])
                var["gScreenRadianceMoments"] = mScreenProbes.pScreenRadianceMoments[readIndex];
            if (pPass.get() == mScreenProbes.pScreenRadianceHistoryPass.get())
            {
                var["gPreviousValidity"] = mScreenProbes.pScreenRadianceValidity[readIndex];
                var["gOutputValidity"].setUav(
                    mScreenProbes.pScreenRadianceValidity[mScreenProbes.screenRadianceHistoryCurrIndex]->getUAV()
                );
            }
        }
        if (hasScreenTrace)
            var["gScreenTraceResult"] = renderData.getTexture("screenTrace");
        if (hasGDFRoute)
        {
            var["gGDFLevelTable"] = mSDF.pLevelTable;
            const uint32_t levelCount = mSDF.gdf->getLevelCount();
            for (uint32_t level = 0; level < kLumenGDFMaxLevelsHost; ++level)
                var["gGDFLevels"][level] = mSDF.levels[std::min<uint32_t>(level, levelCount - 1u)];
        }
        var["gProbeMeta"] = mScreenProbes.pMetadata;
        var["gProbeHitRecords"] = mScreenProbes.pHitRecords;
        var["gProbeCounters"] = mScreenProbes.pCounters;
        if (hasProbeRadiance)
            var["gProbeRadiance"] = renderData.getTexture("probeRadiance");
        if (hasProbeValidity)
            var["gProbeValidity"].setUav(pProbeValidity->getUAV());

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

    // The C7 validity sidecar is an optional diagnostic buffer.  Clear it before
    // each probe dispatch so budget-skipped/inactive directions cannot expose
    // pooled-resource contents from an older graph execution as a backend code.
    // Production GI never allocates or reads this buffer.
    if (hasProbeValidity)
        pRenderContext->clearUAV(pProbeValidity->getUAV().get(), uint4(0));

    // Hard invalidation must happen before metadata update/trace/finalize.  In
    // particular, budget-skipped probes must not retain old-scene radiance or
    // hit records while the new frame is rebuilding its identity.
    if (mScreenProbes.historyResetPending)
    {
        pRenderContext->clearUAV(mScreenProbes.pHitRecords->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mScreenProbes.pRadiance->getUAV().get(), float4(0.f));
        pRenderContext->clearUAV(mScreenProbes.pRadianceHistory->getUAV().get(), float4(0.f));
        pRenderContext->clearUAV(mScreenProbes.pInterpolated->getUAV().get(), float4(0.f));
        for (uint32_t i = 0; i < 2u; ++i)
        {
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceHistory[i]->getUAV().get(), float4(0.f));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceDepthHistory[i]->getUAV().get(), float4(0.f));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceGuideHistory[i]->getUAV().get(), float4(0.f));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceMoments[i]->getUAV().get(), float4(0.f));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceLightingGeneration[i]->getUAV().get(), uint4(0));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceAge[i]->getUAV().get(), uint4(0));
            pRenderContext->clearUAV(mScreenProbes.pScreenRadianceValidity[i]->getUAV().get(), uint4(0));
        }
        mScreenProbes.screenRadianceHistoryCurrIndex = 0u;
        mScreenProbes.historyResetPending = false;
    }

    // Build the next full-resolution screen-radiance history before the probe
    // trace.  The trace reads the opposite slot, so the current raw HWRT
    // result never feeds back into the same frame.  This is the key temporal
    // distinction from the probe irradiance history below.
    if (hasDiffuseRadiance && mScreenProbes.pScreenRadianceHistoryPass)
    {
        const uint32_t writeIndex = mScreenProbes.screenRadianceHistoryCurrIndex;
        const uint32_t readIndex = 1u - writeIndex;
        ref<Program> pHistoryProgram = mScreenProbes.pScreenRadianceHistoryPass->getProgram();
        bool historyProgramChanged = false;
        historyProgramChanged |= pHistoryProgram->addDefine(
            "is_valid_gMotionVector", renderData.getTexture("mvec") ? "1" : "0"
        );
        historyProgramChanged |= pHistoryProgram->addDefine(
            "is_valid_gNormalRoughnessMaterialID", renderData.getTexture("normWRoughnessMaterialID") ? "1" : "0"
        );
        if (historyProgramChanged)
            mScreenProbes.pScreenRadianceHistoryPass->setVars(nullptr);

        ShaderVar historyVar = mScreenProbes.pScreenRadianceHistoryPass->getRootVar();
        ShaderVar historyCB = historyVar["LumenScreenRadianceHistoryCB"];
        historyCB["gFrameDim"] = mFrameDim;
        historyCB["gHistoryValid"] = mFrameIndex > 0u ? 1u : 0u;
        historyCB["gHistoryGeneration"] = (uint32_t)mHistoryGeneration;
        historyCB["gLightingGeneration"] = (uint32_t)mLightingGeneration;
        historyCB["gResetReason"] = (uint32_t)mLastHistoryResetReason;
        historyCB["gResetActive"] = mHistoryResetThisFrame || mScreenProbes.historyResetPending ? 1u : 0u;
        historyVar["gCurrentRadiance"] = renderData.getTexture("diffuseRadianceHitDist");
        historyVar["gPreviousRadiance"] = mScreenProbes.pScreenRadianceHistory[readIndex];
        historyVar["gCurrentLinearZ"] = pLinearZ;
        historyVar["gPreviousLinearZ"] = mScreenProbes.pScreenRadianceDepthHistory[readIndex];
        historyVar["gPreviousGuide"] = mScreenProbes.pScreenRadianceGuideHistory[readIndex];
        historyVar["gPreviousMoments"] = mScreenProbes.pScreenRadianceMoments[readIndex];
        historyVar["gPreviousLightingGeneration"] = mScreenProbes.pScreenRadianceLightingGeneration[readIndex];
        historyVar["gPreviousAge"] = mScreenProbes.pScreenRadianceAge[readIndex];
        historyVar["gPreviousValidity"] = mScreenProbes.pScreenRadianceValidity[readIndex];
        // Reuse the screen-probe counter UAV for typed A2 history rejection
        // telemetry. The buffer is cleared before this pass and copied after
        // the complete probe chain, so history and trace counters share one
        // frame-scoped readback without an alias.
        historyVar["gProbeCounters"] = mScreenProbes.pCounters;
        if (const ref<Texture> pMotion = renderData.getTexture("mvec"))
            historyVar["gMotionVector"] = pMotion;
        if (const ref<Texture> pGuide = renderData.getTexture("normWRoughnessMaterialID"))
            historyVar["gCurrentGuide"] = pGuide;
        historyVar["gOutputRadiance"].setUav(mScreenProbes.pScreenRadianceHistory[writeIndex]->getUAV());
        historyVar["gOutputLinearZ"].setUav(mScreenProbes.pScreenRadianceDepthHistory[writeIndex]->getUAV());
        historyVar["gOutputGuide"].setUav(mScreenProbes.pScreenRadianceGuideHistory[writeIndex]->getUAV());
        historyVar["gOutputMoments"].setUav(mScreenProbes.pScreenRadianceMoments[writeIndex]->getUAV());
        historyVar["gOutputLightingGeneration"].setUav(mScreenProbes.pScreenRadianceLightingGeneration[writeIndex]->getUAV());
        historyVar["gOutputAge"].setUav(mScreenProbes.pScreenRadianceAge[writeIndex]->getUAV());
        historyVar["gOutputValidity"].setUav(mScreenProbes.pScreenRadianceValidity[writeIndex]->getUAV());
        mScreenProbes.pScreenRadianceHistoryPass->execute(
            pRenderContext, ((mFrameDim.x + 7u) / 8u) * 8u, ((mFrameDim.y + 7u) / 8u) * 8u, 1
        );
        if (pScreenRadianceLightingGenerationOutput)
            pRenderContext->copyResource(
                pScreenRadianceLightingGenerationOutput.get(),
                mScreenProbes.pScreenRadianceLightingGeneration[writeIndex].get()
            );
        if (pScreenRadianceHistoryAgeOutput)
            pRenderContext->copyResource(
                pScreenRadianceHistoryAgeOutput.get(), mScreenProbes.pScreenRadianceAge[writeIndex].get()
            );
        if (pScreenRadianceHistoryValidityOutput)
            pRenderContext->copyResource(
                pScreenRadianceHistoryValidityOutput.get(), mScreenProbes.pScreenRadianceValidity[writeIndex].get()
            );
    }

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
    // The production chain always computes an internal interpolated resource. The
    // graph probeInterpolated output is an optional mirror for diagnostics/capture;
    // filters no longer depend on markOutput() to execute.
    if (hasNormal)
    {
        const ref<Texture> pGIOutput = mScreenProbes.pInterpolated;

        // Shared CB + shared inputs for the S4.3 passes (mirror the trace bindings; gProbeRadiance
        // is the internal integrate output). No scene block: these passes have no scene imports.
        const auto bindProbeCompute = [&](const ref<ComputePass>& pPass)
        {
            ShaderVar var = pPass->getRootVar();
            ShaderVar cb = var["LumenScreenProbeCB"];
            cb["gFrameDim"] = mFrameDim;
            cb["gFrameIndex"] = mFrameIndex;
            cb["gSurfaceCacheFrameIndex"] = mSurfaceCacheFrameIndex;
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
            var["gProbeCounters"] = mScreenProbes.pCounters;
            if (pPass.get() == mScreenProbes.pIntegrate.get())
            {
                var["gProbeRadianceHistory"] = mScreenProbes.pRadianceHistory;
                if (hasSurfaceCacheLookup)
                {
                    var["LumenProbeCacheCB"]["gProbeCacheCardCount"] = mpCardScene->getCardCount();
                    var["LumenProbeCacheCB"]["gProbeCachePagesPerSide"] = mCapturePagesPerSide;
                    var["LumenProbeCacheCB"]["gProbeCacheAtlasSize"] = uint2(mAtlasSizeTexels, mAtlasSizeTexels);
                    var["LumenProbeCacheCB"]["gProbeCacheGridDim"] = uint3(
                        LumenScreenProbe::kCacheCardGridDim,
                        LumenScreenProbe::kCacheCardGridDim,
                        LumenScreenProbe::kCacheCardGridDim
                    );
                    var["LumenProbeCacheCB"]["gProbeCacheGridMaxCandidates"] =
                        LumenScreenProbe::kCacheCardGridMaxCandidates;
                    var["LumenProbeCacheCB"]["gProbeCacheGridMin"] = mCardGridMin;
                    var["LumenProbeCacheCB"]["gProbeCacheGridInvCellSize"] = mCardGridInvCellSize;
                    var["LumenProbeCacheCB"]["gProbeCacheGridCellStride"] =
                        LumenScreenProbe::kCacheCardGridCellStride;
                    var["LumenProbeCacheCB"]["gProbeCacheUseCardGrid"] = mUseCacheCardGrid ? 1u : 0u;
                    var["gProbeCards"] = mCapture.pCards;
                    var["gProbeCardPageTable"] = mCapture.pPageTable;
                    var["gProbeCardPageGeneration"] = mCapture.pPageGeneration;
                    var["gProbePageMetadata"] = mCacheLighting.pPageMetadata;
                    var["gProbeCardGrid"] = mCacheLighting.pCardGrid;
                    var["gProbeRadianceAtlas"] = mCapture.pRadianceAtlas;
                    var["gProbeMetadataAtlas"] = mCapture.pMetadataAtlas;
                    var["gProbeVisibilityAtlas"] = mCacheLighting.pVisibilityAtlas;
                    if (mScreenProbes.pCacheFeedback)
                        var["gProbeCacheFeedback"] = mScreenProbes.pCacheFeedback;
                    if (mScreenProbes.pCacheRequests)
                        var["gProbeCacheRequests"] = mScreenProbes.pCacheRequests;
                }
            }
        };

        if (hasSurfaceCacheLookup && mScreenProbes.pCacheFeedback)
            pRenderContext->clearUAV(mScreenProbes.pCacheFeedback->getUAV().get(), uint4(0));
        if (hasSurfaceCacheLookup && mScreenProbes.pCacheRequests)
            pRenderContext->clearUAV(mScreenProbes.pCacheRequests->getUAV().get(), uint4(0));
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
        mScreenProbes.producedThisFrame = true;
        if (hasProbeInterpolated)
            pRenderContext->copyResource(renderData.getTexture("probeInterpolated").get(), pGIOutput.get());
        if (hasProbeHistory)
            pRenderContext->copyResource(renderData.getTexture(kProbeHistory).get(), mScreenProbes.pRadianceHistory.get());
    }

    // The history pass wrote the slot selected above; make it the read slot
    // for the next frame only after this frame's probe consumers are done.
    if (hasDiffuseRadiance && mScreenProbes.pScreenRadianceHistoryPass)
        mScreenProbes.screenRadianceHistoryCurrIndex = 1u - mScreenProbes.screenRadianceHistoryCurrIndex;

    if (mScreenProbes.pCounters && mScreenProbes.pCountersReadback)
    {
        pRenderContext->copyResource(mScreenProbes.pCountersReadback.get(), mScreenProbes.pCounters.get());
        // Counter readback is consumed at the start of the next execute. Stamp
        // the scheduler domain used by Surface Cache ready fences, not the
        // resettable GI history frame.
        mScreenProbeCountersSubmittedFrame = mSurfaceCacheFrameIndex;
        mScreenProbes.counterReadbackPending = true;
    }
    if (hasSurfaceCacheLookup && mScreenProbes.pCacheFeedback && mScreenProbes.pCacheFeedbackReadback)
    {
        // Consume on the next frame before Surface Cache scheduling. The submitted epoch is
        // carried alongside the GPU generation and checked in readbackScreenProbeCounters().
        pRenderContext->copyResource(mScreenProbes.pCacheFeedbackReadback.get(), mScreenProbes.pCacheFeedback.get());
        mScreenProbes.cacheFeedbackReadbackPending = true;
        mScreenProbes.cacheFeedbackSceneGeneration = mSurfaceCacheSceneGeneration;
        mScreenProbes.cacheFeedbackSubmittedFrame = mSurfaceCacheFrameIndex;
    }
    if (hasSurfaceCacheLookup && mScreenProbes.pCacheRequests && mScreenProbes.pCacheRequestsReadback)
    {
        pRenderContext->copyResource(mScreenProbes.pCacheRequestsReadback.get(), mScreenProbes.pCacheRequests.get());
        mScreenProbes.cacheRequestReadbackPending = true;
        mScreenProbes.cacheRequestSceneGeneration = mSurfaceCacheSceneGeneration;
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
    // (gTemporalAlpha / gTemporalConfidence) and GBuffer normal validation are specialized per
    // frame in runTemporalFilter based on graph allocation. Hit-distance and previous-confidence
    // inputs remain unbound until their producer-generation ABI is frozen.
    DefineList defines;
    defines.add("is_valid_gLinearZ", "1");
    defines.add("is_valid_gCurrent", "1");
    defines.add("is_valid_gMotionVector", "1");
    defines.add("is_valid_gPrevDepth", "1");
    defines.add("is_valid_gPrevGI", "1");
    defines.add("is_valid_gTemporalOutput", "1");
    defines.add("is_valid_gNormalRoughnessMaterialID", "0");
    defines.add("is_valid_gPrevNormalRoughnessMaterialID", "0");
    defines.add("is_valid_gHitDistance", "0");
    defines.add("is_valid_gPrevHitDistance", "0");
    defines.add("is_valid_gPrevConfidence", "0");
    defines.add("is_valid_gTemporalAlpha", "0");
    defines.add("is_valid_gTemporalConfidence", "0");
    defines.add("is_valid_gMoments", "1");
    mTemporalFilter.pFilter = ComputePass::create(mpDevice, kTemporalFilterShaderFile, "main", defines);
}

void LumenGIPass::ensureTemporalFilterResources(RenderContext* pRenderContext)
{
    if (any(mFrameDim == uint2(0u, 0u)))
        return;

    if (!mTemporalFilter.pFilter)
        createTemporalFilterProgram();

    const bool sizeChanged = any(mTemporalFilter.resourceDim != mFrameDim);
    if (!mTemporalFilter.pHistory[0] || !mTemporalFilter.pHistory[1] || !mTemporalFilter.pPrevDepth ||
        !mTemporalFilter.pMoments || !mTemporalFilter.pPrevNormal || sizeChanged)
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

        // Previous-frame packed normal/material.  The current GBuffer is RGB10A2, while this
        // history is RGBA16F so it can be updated through a normalized blit and sampled by the
        // temporal validator without aliasing the graph input.
        mTemporalFilter.pPrevNormal = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget
        );
        mTemporalFilter.pPrevNormal->setName("LumenGIPass::TemporalFilter::PrevNormal");
        pRenderContext->clearUAV(mTemporalFilter.pPrevNormal->getUAV().get(), float4(0.f));

        mTemporalFilter.pMoments = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RG32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mTemporalFilter.pMoments->setName("LumenGIPass::TemporalFilter::Moments");
        pRenderContext->clearUAV(mTemporalFilter.pMoments->getUAV().get(), float4(0.f));

        mTemporalFilter.historyCurrIndex = 0;
        mTemporalFilter.historyResetPending = true;
        mTemporalFilter.resourceDim = mFrameDim;
        mTemporalFilter.historyResetPending = true; // fresh zeroed buffers == reset state.
    }
}

void LumenGIPass::runTemporalFilter(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Allocation gates: gCurrent is the S4.3 interpolate graph output (only produced when the
    // probe path ran), gTemporalOutput is the S5 graph output this pass feeds. The graph channel
    // names mirror kOutputChannels, so renderData.getTexture() resolves them by name.
    // The internal probe interpolation is the producer consumed by the filter
    // chain. The graph channel is only a diagnostic mirror and may be allocated
    // but unwritten when the caller does not markOutput() it.
    // A non-null internal texture is not evidence that this frame produced a new sample. Do not
    // feed a stale interpolation into temporal history when Screen Probes are disabled or an
    // earlier probe stage returned before dispatching.
    if (!mScreenProbes.producedThisFrame)
        return;
    const ref<Texture> pCurrent = mScreenProbes.pInterpolated
        ? mScreenProbes.pInterpolated
        : renderData.getTexture(kProbeInterpolated);
    const ref<Texture> pOutput = renderData.getTexture(kTemporalFiltered);
    if (!pCurrent)
        return;

    const ref<Texture> pLinearZ = renderData.getTexture("linearZ");
    const ref<Texture> pMotionVector = renderData.getTexture("mvec");
    if (!pLinearZ || !pMotionVector)
        return;

    ensureTemporalFilterResources(pRenderContext);
    if (!mTemporalFilter.pFilter || !mTemporalFilter.pHistory[0] || !mTemporalFilter.pHistory[1] ||
        !mTemporalFilter.pPrevDepth || !mTemporalFilter.pPrevNormal || !mTemporalFilter.pMoments)
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
        pRenderContext->clearUAV(mTemporalFilter.pMoments->getUAV().get(), float4(0.f));
        mTemporalFilter.historyResetPending = false;
    }

    const bool hasAlpha = renderData.getTexture(kTemporalAlpha) != nullptr;
    const bool hasConfidence = renderData.getTexture(kTemporalConfidence) != nullptr;
    const bool hasNormal = renderData.getTexture("normWRoughnessMaterialID") != nullptr;

    // Per-frame program specialization for the optional UAVs (graph allocation is fixed per graph,
    // so this changes only once per graph build).
    ref<Program> pProgram = mTemporalFilter.pFilter->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("is_valid_gNormalRoughnessMaterialID", hasNormal ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gPrevNormalRoughnessMaterialID", hasNormal ? "1" : "0");
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
    var["gMoments"] = mTemporalFilter.pMoments;
    if (hasNormal)
    {
        var["gNormalRoughnessMaterialID"] = renderData.getTexture("normWRoughnessMaterialID");
        var["gPrevNormalRoughnessMaterialID"] = mTemporalFilter.pPrevNormal;
    }
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
    mTemporalFilter.producedThisFrame = true;

    // S5-A1 frame-end updates: copy the freshly filtered history into the graph "temporalFiltered"
    // output (same RGBA16F format -> full-resource copy) and blit the current linear depth into
    // pPrevDepth so the NEXT frame validates against THIS frame. Then flip the ping-pong: the slot
    // written this frame becomes the previous frame's input next frame.
    if (pOutput)
        pRenderContext->copyResource(pOutput.get(), mTemporalFilter.pHistory[mTemporalFilter.historyCurrIndex].get());
    if (const ref<Texture> pMomentsOutput = renderData.getTexture(kTemporalMoments))
        pRenderContext->copyResource(pMomentsOutput.get(), mTemporalFilter.pMoments.get());
    pRenderContext->blit(pLinearZ->getSRV(), mTemporalFilter.pPrevDepth->getRTV());
    if (hasNormal)
        pRenderContext->blit(renderData.getTexture("normWRoughnessMaterialID")->getSRV(), mTemporalFilter.pPrevNormal->getRTV());
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
    if (any(mSpatialFilter.resourceDim != mFrameDim) || !mSpatialFilter.pOutput || !mSpatialFilter.pScratch || !mSpatialFilter.pVariance)
    {
        mSpatialFilter.pOutput = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mSpatialFilter.pOutput->setName("LumenGIPass::SpatialFilter::OutputInternal");
        pRenderContext->clearUAV(mSpatialFilter.pOutput->getUAV().get(), float4(0.f));
        mSpatialFilter.pScratch = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mSpatialFilter.pScratch->setName("LumenGIPass::SpatialFilter::ScratchInternal");
        pRenderContext->clearUAV(mSpatialFilter.pScratch->getUAV().get(), float4(0.f));
        mSpatialFilter.pVariance = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::R32Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mSpatialFilter.pVariance->setName("LumenGIPass::SpatialFilter::VarianceInternal");
        pRenderContext->clearUAV(mSpatialFilter.pVariance->getUAV().get(), float4(0.f));
        mSpatialFilter.resourceDim = mFrameDim;
    }
}

void LumenGIPass::runSpatialFilter(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Allocation gates: gGIInput is the S5-B1 temporalFiltered graph output (the S5 main output),
    // gFilteredOutput is the S5-B2 spatialFiltered graph output this pass feeds. The graph channel
    // names mirror kOutputChannels, so renderData.getTexture() resolves them by name.
    ref<Texture> pInput;
    if (mTemporalFilter.producedThisFrame && mTemporalFilter.pHistory[0] && mTemporalFilter.pHistory[1])
        pInput = mTemporalFilter.pHistory[1 - mTemporalFilter.historyCurrIndex];
    if (!pInput && mScreenProbes.producedThisFrame)
        pInput = renderData.getTexture(kProbeInterpolated) ? renderData.getTexture(kProbeInterpolated) : mScreenProbes.pInterpolated;
    const ref<Texture> pGraphOutput = renderData.getTexture(kSpatialFiltered);
    const ref<Texture> pGraphVariance = renderData.getTexture(kFilteredVariance);
    if (!pInput)
        return;

    const ref<Texture> pLinearZ = renderData.getTexture("linearZ");
    if (!pLinearZ)
        return;

    ensureSpatialFilterResources(pRenderContext);
    if (!mSpatialFilter.pFilter || !mSpatialFilter.pOutput || !mSpatialFilter.pScratch)
        return;
    // Always write the internal output. The graph output is an optional mirror
    // and must not become the producer merely because it is allocated.
    const ref<Texture> pOutput = mSpatialFilter.pOutput;

    // S5-B2 confidence source: the S5-A1 temporalConfidence R32F graph channel. This is the
    // chosen confidence source -- temporalFiltered.a carries the S5-B1 HISTORY LENGTH, not a
    // confidence, so it cannot be passed through gGIInput.a. When the graph allocates the
    // channel the host binds it as the pass's optional gConfidenceInput (overriding gGIInput.a);
    // otherwise the pass falls back to gGIInput.a (degraded proxy).
    const bool hasConfidence = renderData.getTexture(kTemporalConfidence) != nullptr;
    const bool hasNormal = renderData.getTexture("normWRoughnessMaterialID") != nullptr;
    const bool hasMoments = mTemporalFilter.producedThisFrame && mTemporalFilter.pMoments != nullptr;

    // Per-frame program specialization for the optional resources (graph allocation is fixed per
    // graph, so this changes only once per graph build).
    ref<Program> pProgram = mSpatialFilter.pFilter->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("is_valid_gNormalRoughnessMaterialID", hasNormal ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gConfidenceInput", hasConfidence ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gMoments", hasMoments ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gFilteredVariance", "1");
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
    cb.fireflyMaxRadiance = 10.f;     // UE Lumen-style max ray intensity guard.
    cb.fireflyStdDevFactor = 4.0f;     // Frozen with LumenSpatialFilterData.slang.
    cb.varianceThresholdLow = mSpatialVarianceThresholdLow;
    cb.varianceThresholdHigh = mSpatialVarianceThresholdHigh;
    cb.radiusMin = mSpatialRadiusMin;
    cb.radiusMax = mSpatialRadiusMax;
    cb.spatialSigmaScale = 0.75f;      // UE-style bilateral footprint; covers the 8px probe grid over 3 passes.
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
    if (hasConfidence)
        var["gConfidenceInput"] = renderData.getTexture(kTemporalConfidence);
    if (hasMoments)
        var["gMoments"] = mTemporalFilter.pMoments;
    var["gLinearZ"] = pLinearZ;
    if (hasNormal)
        var["gNormalRoughnessMaterialID"] = renderData.getTexture("normWRoughnessMaterialID");
    // Always produce the internal variance first. The graph channel is an optional mirror; binding
    // it as the UAV and then copying the still-unwritten internal texture back would erase the
    // diagnostic result and make export topology affect the spatial filter's observable state.
    var["gFilteredVariance"] = mSpatialFilter.pVariance;

    // UE5.8 uses multiple small bilateral passes (three by default) after temporal accumulation.
    // Keep the same pass shader and ping-pong two RGBA16F targets; this removes residual tile
    // structure without introducing a new ABI or a large-radius single blur.
    // UE's production path uses multiple small bilateral passes instead of one
    // wide blur. Five passes are needed here because probe interpolation is
    // currently an 8px grid and the adaptive radius is capped at four pixels.
    constexpr uint32_t kSpatialFilterPasses = 5u;
    ref<Texture> passInput = pInput;
    for (uint32_t pass = 0; pass < kSpatialFilterPasses; ++pass)
    {
        // Alternate the two internal targets for every pass. The previous
        // implementation special-cased pass 1, which was safe for exactly
        // three passes but created an in-place UAV read/write alias as soon
        // as the filter budget was raised to five passes.
        ref<Texture> passOutput = (pass % 2u == 0u) ? pOutput : mSpatialFilter.pScratch;
        var["gGIInput"] = passInput;
        var["gFilteredOutput"] = passOutput;
        mSpatialFilter.pFilter->execute(
            pRenderContext,
            ((mFrameDim.x + 7u) / 8u) * 8u,
            ((mFrameDim.y + 7u) / 8u) * 8u,
            1
        );
        passInput = passOutput;
    }
    if (pGraphOutput && pGraphOutput != pOutput)
        pRenderContext->copyResource(pGraphOutput.get(), pOutput.get());
    if (pGraphVariance && pGraphVariance != mSpatialFilter.pVariance)
        pRenderContext->copyResource(pGraphVariance.get(), mSpatialFilter.pVariance.get());
    mSpatialFilter.producedThisFrame = true;
}

void LumenGIPass::runFinalResolve(RenderContext* pRenderContext, const RenderData& renderData)
{
    const ref<Texture> pDiffuseGI = renderData.getTexture("diffuseGI");
    if (!pDiffuseGI || any(mFrameDim == uint2(0u, 0u)))
        return;

    // Prefer the most reconstructed incident signal. The internal resources are
    // always produced once the probe/filter chain is active, even when callers do
    // not mark diagnostic graph outputs. If no reconstruction signal exists, the
    // HWRT output is already modulated radiance and must pass through unchanged
    // (otherwise albedo would be applied twice).
    ref<Texture> pSource;
    if (mSpatialFilter.producedThisFrame)
        pSource = mSpatialFilter.pOutput;
    if (!pSource && mSpatialFilter.producedThisFrame)
        pSource = renderData.getTexture(kSpatialFiltered);
    bool sourceIsIncident = pSource != nullptr;
    if (!pSource && mTemporalFilter.producedThisFrame)
    {
        if (mTemporalFilter.pHistory[0] && mTemporalFilter.pHistory[1])
            pSource = mTemporalFilter.pHistory[1 - mTemporalFilter.historyCurrIndex];
        if (!pSource)
            pSource = renderData.getTexture(kTemporalFiltered);
        sourceIsIncident = pSource != nullptr;
    }
    if (!pSource && mScreenProbes.producedThisFrame)
    {
        pSource = mScreenProbes.pInterpolated;
        if (!pSource)
            pSource = renderData.getTexture(kProbeInterpolated);
        sourceIsIncident = pSource != nullptr;
    }
    if (!pSource)
    {
        pSource = pDiffuseGI;
        sourceIsIncident = false;
    }
    if (!pSource)
        return;

    if (!mFinalResolve.pPass)
    {
        DefineList defines;
        defines.add("LUMEN_GI_RESOLVE_INCIDENT", "0");
        defines.add("is_valid_gDiffuseOpacity", "0");
        defines.add("is_valid_gFallbackGI", "0");
        defines.add("is_valid_gLinearZ", "0");
        defines.add("is_valid_gRadianceCache", "0");
        mFinalResolve.pPass = ComputePass::create(mpDevice, kFinalResolveShaderFile, "main", defines);
    }

    const bool hasDiffuseOpacity = renderData.getTexture("diffuseOpacity") != nullptr;
    ref<Program> pProgram = mFinalResolve.pPass->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("LUMEN_GI_RESOLVE_INCIDENT", sourceIsIncident ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gDiffuseOpacity", hasDiffuseOpacity ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gFallbackGI", pDiffuseGI ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gLinearZ", renderData.getTexture("linearZ") ? "1" : "0");
    const bool hasRadianceCache = mUseRadianceCache && mRadianceCacheGpu.producedThisFrame &&
        mRadianceCacheGpu.pOutput && mRadianceCacheGpu.pValidityOutput;
    programChanged |= pProgram->addDefine("is_valid_gRadianceCache", hasRadianceCache ? "1" : "0");
    if (programChanged)
        mFinalResolve.pPass->setVars(nullptr);

    const bool sizeChanged = any(mFinalResolve.resourceDim != mFrameDim);
    if (!mFinalResolve.pResolved || sizeChanged)
    {
        mFinalResolve.pResolved = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mFinalResolve.pResolved->setName("LumenGIPass::FinalResolve::ResolvedDiffuseGI");
        mFinalResolve.resourceDim = mFrameDim;
    }

    ShaderVar var = mFinalResolve.pPass->getRootVar();
    var["CB"]["gFrameDim"] = mFrameDim;
    var["gSourceGI"] = pSource;
    var["gFallbackGI"] = pDiffuseGI;
    if (renderData.getTexture("linearZ"))
        var["gLinearZ"] = renderData.getTexture("linearZ");
    if (hasDiffuseOpacity)
        var["gDiffuseOpacity"] = renderData.getTexture("diffuseOpacity");
    if (hasRadianceCache)
    {
        var["gRadianceCacheGI"] = mRadianceCacheGpu.pOutput;
        var["gRadianceCacheValidity"] = mRadianceCacheGpu.pValidityOutput;
    }
    var["gResolvedGI"] = mFinalResolve.pResolved;
    mFinalResolve.pPass->execute(
        pRenderContext,
        ((mFrameDim.x + 7u) / 8u) * 8u,
        ((mFrameDim.y + 7u) / 8u) * 8u,
        1
    );

    // The public diffuseGI output is always the resolved production value. An
    // optional graph output mirrors the same texture for capture/validation.
    pRenderContext->copyResource(pDiffuseGI.get(), mFinalResolve.pResolved.get());
    if (const ref<Texture> pResolvedOutput = renderData.getTexture("resolvedDiffuseGI"))
        pRenderContext->copyResource(pResolvedOutput.get(), mFinalResolve.pResolved.get());
}

// =====================================================================================
// C10: bounded GPU Radiance Cache seed/interpolate path
// =====================================================================================

void LumenGIPass::createRadianceCachePrograms()
{
    if (!mRadianceCacheGpu.pBuild)
    {
        // The producer is scene-backed: unlike the diagnostic screen-projection
        // seed, it needs the scene shader modules, type conformances and TLAS
        // bindings so every cache probe can issue a real inline ray query.
        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kRadianceCacheTraceShaderFile).csEntry("buildMain");
        desc.addTypeConformances(mpScene->getTypeConformances());
        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        // Keep the cache probe producer on the same sampling ABI as the HWRT
        // secondary-hit path.  The C10 pass currently binds no emissive-light
        // sampler, so its bounded NEE contribution is analytic lights plus
        // material emission; a later wave may add the sampler as a matching
        // root-layout variant.
        defines.add(mpSampleGenerator->getDefines());
        defines.add("USE_ANALYTIC_LIGHTS", "1");
        defines.add("USE_EMISSIVE_LIGHTS", "1");
        defines.add("LUMEN_GI_HAS_EMISSIVE_SAMPLER", mpEmissiveLightSampler ? "1" : "0");
        if (mpEmissiveLightSampler)
            defines.add(mpEmissiveLightSampler->getDefines());
        else
            defines.add("_EMISSIVE_LIGHT_SAMPLER_TYPE", "255");
        mRadianceCacheGpu.pBuild = ComputePass::create(mpDevice, desc, defines, /*createVars=*/true);
    }
    if (!mRadianceCacheGpu.pInterpolate)
        mRadianceCacheGpu.pInterpolate = ComputePass::create(mpDevice, kRadianceCacheShaderFile, "main");
}

void LumenGIPass::ensureRadianceCacheResources(RenderContext* pRenderContext)
{
    if (!mUseRadianceCache || !mRadianceCache || any(mFrameDim == uint2(0u, 0u)))
        return;

    createRadianceCachePrograms();
    const uint32_t capacity = mRadianceCache->getMaxSlots() + 1u;
    const uint32_t levelCounterCount = mRadianceCache->getLevelCount() * kLumenRadianceCacheLevelStride;
    if (mRadianceCacheGpu.pProbeMeta[0] && mRadianceCacheGpu.slotCapacity == capacity &&
        mRadianceCacheGpu.pOutput && mRadianceCacheGpu.pHitDistOutput &&
        mRadianceCacheGpu.pQueryCounters && mRadianceCacheGpu.pQueryCountersReadback &&
        mRadianceCacheGpu.pLevelQueryCounters && mRadianceCacheGpu.pLevelQueryCountersReadback &&
        mRadianceCacheGpu.levelCounterCount == levelCounterCount &&
        mRadianceCacheGpu.pValidityOutput)
        return;

    for (uint32_t i = 0; i < 2u; ++i)
    {
        mRadianceCacheGpu.pProbeMeta[i] = mpDevice->createStructuredBuffer(
            sizeof(uint4), capacity, ResourceBindFlags::ShaderResource
        );
        mRadianceCacheGpu.pProbeMeta[i]->setName(
            "LumenGI::RadianceCache::ProbeMeta" + std::to_string(i)
        );
    }
    mRadianceCacheGpu.pProbeWorldPos = mpDevice->createStructuredBuffer(
        sizeof(float4), capacity, ResourceBindFlags::ShaderResource
    );
    mRadianceCacheGpu.pProbeWorldPos->setName("LumenGI::RadianceCache::ProbeWorldPos");
    mRadianceCacheGpu.pProbeScreenPos = mpDevice->createStructuredBuffer(
        sizeof(float4), capacity, ResourceBindFlags::ShaderResource
    );
    mRadianceCacheGpu.pProbeScreenPos->setName("LumenGI::RadianceCache::ProbeScreenPos");
    for (uint32_t i = 0; i < 2u; ++i)
    {
        mRadianceCacheGpu.pProbeRadiance[i] = mpDevice->createStructuredBuffer(
            sizeof(float4), capacity, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mRadianceCacheGpu.pProbeRadiance[i]->setName(
            "LumenGI::RadianceCache::ProbeRadiance" + std::to_string(i)
        );
        pRenderContext->clearUAV(mRadianceCacheGpu.pProbeRadiance[i]->getUAV().get(), float4(0.f));
        mRadianceCacheGpu.pProbeValidity[i] = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), capacity, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mRadianceCacheGpu.pProbeValidity[i]->setName(
            "LumenGI::RadianceCache::ProbeValidity" + std::to_string(i)
        );
        pRenderContext->clearUAV(mRadianceCacheGpu.pProbeValidity[i]->getUAV().get(), uint4(0u));
    }
    mRadianceCacheGpu.pQueryCounters = mpDevice->createStructuredBuffer(
        sizeof(uint32_t), 4u, ResourceBindFlags::UnorderedAccess
    );
    mRadianceCacheGpu.pQueryCounters->setName("LumenGI::RadianceCache::QueryCounters");
    mRadianceCacheGpu.pQueryCountersReadback = mpDevice->createStructuredBuffer(
        sizeof(uint32_t), 4u, ResourceBindFlags::None, MemoryType::ReadBack
    );
    mRadianceCacheGpu.pQueryCountersReadback->setName("LumenGI::RadianceCache::QueryCountersReadback");
    pRenderContext->clearUAV(mRadianceCacheGpu.pQueryCounters->getUAV().get(), uint4(0u));
    mRadianceCacheGpu.queryCountersSubmittedFrame = 0u;
    mRadianceCacheGpu.queryCountersFrame = 0u;
    mRadianceCacheGpu.queryCountersReadbackPending = false;
    mRadianceCacheGpu.pLevelQueryCounters = mpDevice->createStructuredBuffer(
        sizeof(uint32_t), levelCounterCount, ResourceBindFlags::UnorderedAccess
    );
    mRadianceCacheGpu.pLevelQueryCounters->setName("LumenGI::RadianceCache::LevelQueryCounters");
    mRadianceCacheGpu.pLevelQueryCountersReadback = mpDevice->createStructuredBuffer(
        sizeof(uint32_t), levelCounterCount, ResourceBindFlags::None, MemoryType::ReadBack
    );
    mRadianceCacheGpu.pLevelQueryCountersReadback->setName("LumenGI::RadianceCache::LevelQueryCountersReadback");
    pRenderContext->clearUAV(mRadianceCacheGpu.pLevelQueryCounters->getUAV().get(), uint4(0u));
    mRadianceCacheGpu.levelQueryCountersSubmittedFrame = 0u;
    mRadianceCacheGpu.levelCounterCount = levelCounterCount;
    mRadianceCacheGpu.levelQueryCountersFrame = 0u;
    mRadianceCacheGpu.levelQueryCountersReadbackPending = false;

    const uint32_t resolution = mRadianceCache->getResolution();
    const uint32_t levels = mRadianceCache->getLevelCount();
    mRadianceCacheGpu.pIndirection = mpDevice->createTexture3D(
        resolution * levels, resolution, resolution, ResourceFormat::R32Uint, 1, nullptr,
        ResourceBindFlags::ShaderResource
    );
    mRadianceCacheGpu.pIndirection->setName("LumenGI::RadianceCache::Indirection");
    mRadianceCacheGpu.pOutput = mpDevice->createTexture2D(
        mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
    );
    mRadianceCacheGpu.pOutput->setName("LumenGI::RadianceCache::Output");
    mRadianceCacheGpu.pHitDistOutput = mpDevice->createTexture2D(
        mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
    );
    mRadianceCacheGpu.pHitDistOutput->setName("LumenGI::RadianceCache::HitDistOutput");
    mRadianceCacheGpu.pValidityOutput = mpDevice->createTexture2D(
        mFrameDim.x, mFrameDim.y, ResourceFormat::R32Uint, 1, 1, nullptr,
        ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
    );
    mRadianceCacheGpu.pValidityOutput->setName("LumenGI::RadianceCache::ValidityOutput");
    mRadianceCacheGpu.slotCapacity = capacity;
    mRadianceCacheGpu.currIndex = 0u;
    mRadianceCacheGpu.lastReadyFrame = 0u;
    mRadianceCacheGpu.producedThisFrame = false;
}

void LumenGIPass::runRadianceCache(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mUseRadianceCache || !mpScene || !mRadianceCache)
        return;
    ensureRadianceCacheResources(pRenderContext);
    if (!mRadianceCacheGpu.pBuild || !mRadianceCacheGpu.pInterpolate)
        return;

    // Consume the previous interpolation dispatch before submitting the next
    // one.  Unlike the old host-side frameDim estimate, these counters are
    // produced by the exact shader validity path and therefore reconcile
    // attempts, valid cache hits, and misses.
    if (mRadianceCacheGpu.queryCountersReadbackPending && mRadianceCacheGpu.pQueryCountersReadback)
    {
        const uint32_t* pCounters = static_cast<const uint32_t*>(mRadianceCacheGpu.pQueryCountersReadback->map());
        if (pCounters)
        {
            mRadianceCacheGpu.queryAttempts += pCounters[0];
            mRadianceCacheGpu.queryHits += pCounters[1];
            mRadianceCacheGpu.queryMisses += pCounters[2];
            // Radiance Cache has its own CPU clipmap clock. Surface Cache may be
            // disabled in this graph, and its scheduler clock is therefore not
            // a valid provenance source for query readback frames.
            // Use the frame captured when this exact dispatch was submitted. The
            // CPU clipmap advances before the next run, so deriving provenance from
            // the current cache frame is ambiguous after reset/scene transitions.
            mRadianceCacheGpu.queryCountersFrame = mRadianceCacheGpu.queryCountersSubmittedFrame;
            mRadianceCacheGpu.pQueryCountersReadback->unmap();
        }
        mRadianceCacheGpu.queryCountersReadbackPending = false;
    }
    if (mRadianceCacheGpu.levelQueryCountersReadbackPending && mRadianceCacheGpu.pLevelQueryCountersReadback)
    {
        const uint32_t* pCounters = static_cast<const uint32_t*>(mRadianceCacheGpu.pLevelQueryCountersReadback->map());
        if (pCounters)
        {
            const uint32_t levelCount = std::min<uint32_t>(mRadianceCache->getLevelCount(), 8u);
            for (uint32_t level = 0u; level < levelCount; ++level)
            {
                const uint32_t base = level * kLumenRadianceCacheLevelStride;
                mRadianceCacheGpu.levelQueryAttempts[level] += pCounters[base + 0u];
                mRadianceCacheGpu.levelQueryHits[level] += pCounters[base + 1u];
                mRadianceCacheGpu.levelQueryMisses[level] += pCounters[base + 2u];
                mRadianceCacheGpu.levelSampleCount[level] += pCounters[base + 3u];
                mRadianceCacheGpu.levelValidHitDistanceCount[level] += pCounters[base + 4u];
                mRadianceCacheGpu.levelFallbackSampleCount[level] += pCounters[base + 5u];
            }
            mRadianceCacheGpu.levelQueryCountersFrame = mRadianceCacheGpu.levelQueryCountersSubmittedFrame;
            mRadianceCacheGpu.pLevelQueryCountersReadback->unmap();
        }
        mRadianceCacheGpu.levelQueryCountersReadbackPending = false;
    }

    const uint32_t capacity = mRadianceCacheGpu.slotCapacity;
    std::vector<uint4> meta(capacity, uint4(0u));
    std::vector<float4> worldPos(capacity, float4(0.f));
    std::vector<float4> screenPos(capacity, float4(-1.f));
    const uint32_t cacheResolution = mRadianceCache->getResolution();
    const uint32_t cacheLevels = mRadianceCache->getLevelCount();
    const uint32_t cacheWidth = cacheResolution * cacheLevels;
    std::vector<uint32_t> indirection(
        size_t(cacheWidth) * size_t(cacheResolution) * size_t(cacheResolution), 0u
    );
    uint64_t activeProbeCount = 0u;
    const ref<Camera>& pCamera = mpScene->getCamera();
    const float3 cameraPos = pCamera->getPosition();
    const float3 cameraForward = normalize(cameraPos - pCamera->getTarget());
    const float3 cameraRight = normalize(cross(pCamera->getUpVector(), cameraForward));
    const float3 cameraUp = cross(cameraForward, cameraRight);
    const float focalLengthPx = pCamera->getFocalLength() * (float)mFrameDim.y / pCamera->getFrameHeight();
    const float2 principalPoint = float2(0.5f * (float)mFrameDim.x, 0.5f * (float)mFrameDim.y);
    const uint32_t readyFrame = mFrameIndex > 0u ? mFrameIndex : 1u;
    mRadianceCacheGpu.projectedProbeCount = 0;
    mRadianceCacheGpu.inBoundsProbeCount = 0;
    for (uint32_t slotID = 1u; slotID < capacity; ++slotID)
    {
        const auto& slot = mRadianceCache->getSlot(slotID);
        if (!slot.allocated)
            continue;
        ++activeProbeCount;
        const auto p = mRadianceCache->probeWorldPosition(slot.level, slot.cell);
        worldPos[slotID] = float4(p.x, p.y, p.z, 1.f);
        const float3 rel = float3(p.x, p.y, p.z) - cameraPos;
        const float3 viewPos = float3(
            dot(rel, cameraRight), -dot(rel, cameraUp), -dot(rel, cameraForward)
        );
        if (viewPos.z > 1e-4f)
        {
            ++mRadianceCacheGpu.projectedProbeCount;
            if (slot.level < 8u)
                ++mRadianceCacheGpu.levelProjectedProbeCount[slot.level];
            screenPos[slotID] = float4(
                viewPos.x / viewPos.z * focalLengthPx + principalPoint.x,
                viewPos.y / viewPos.z * focalLengthPx + principalPoint.y,
                viewPos.z, 1.f
            );
            if (screenPos[slotID].x >= 0.f && screenPos[slotID].x < float(mFrameDim.x) &&
                screenPos[slotID].y >= 0.f && screenPos[slotID].y < float(mFrameDim.y))
            {
                ++mRadianceCacheGpu.inBoundsProbeCount;
                if (slot.level < 8u)
                    ++mRadianceCacheGpu.levelInBoundsProbeCount[slot.level];
            }
        }
        // Existing slots become visible one frame after their first GPU build. The
        // payload itself is ping-ponged; this metadata fence prevents a same-frame
        // read when a slot is first allocated.
        // The GPU metadata uses one epoch domain.  CPU slot generations are
        // allocator-local and are reset with the clipmap; mixing them with the
        // pass-wide epoch would reject every fresh slot after a scene reset.
        // Payload written into `writeIndex` becomes visible on the following
        // frame, independently of CPU lastUpdateFrame (the GPU producer owns
        // that timestamp in this Wave).
        const uint32_t visible = mFrameIndex == 0u ? 1u : mFrameIndex;
        meta[slotID] = uint4(slotID, mRadianceCacheGpu.generation, visible, 1u);
        if (slot.level < cacheLevels && slot.cell.x >= 0 && slot.cell.y >= 0 && slot.cell.z >= 0 &&
            slot.cell.x < int32_t(cacheResolution) && slot.cell.y < int32_t(cacheResolution) &&
            slot.cell.z < int32_t(cacheResolution))
        {
            const uint32_t x = slot.level * cacheResolution + uint32_t(slot.cell.x);
            const size_t index = size_t(x) + size_t(cacheWidth) *
                (size_t(uint32_t(slot.cell.y)) + size_t(cacheResolution) * size_t(uint32_t(slot.cell.z)));
            indirection[index] = slotID;
        }
    }
    const uint32_t writeIndex = mRadianceCacheGpu.currIndex;
    mRadianceCacheGpu.pProbeMeta[writeIndex]->setBlob(meta.data(), 0, meta.size() * sizeof(uint4));
    mRadianceCacheGpu.pProbeWorldPos->setBlob(worldPos.data(), 0, worldPos.size() * sizeof(float4));
    mRadianceCacheGpu.pProbeScreenPos->setBlob(screenPos.data(), 0, screenPos.size() * sizeof(float4));

    // Upload the CPU clipmap ownership map as an SRV snapshot for this frame.
    // This is intentionally a bounded Wave-1 bridge: later C10 work will replace
    // the recreate-with-data path with a persistent GPU indirection update pass.
    mRadianceCacheGpu.pIndirection = mpDevice->createTexture3D(
        cacheWidth, cacheResolution, cacheResolution, ResourceFormat::R32Uint, 1,
        indirection.data(), ResourceBindFlags::ShaderResource
    );
    mRadianceCacheGpu.pIndirection->setName("LumenGI::RadianceCache::IndirectionFrame");

    const ref<Texture> pSource = renderData.getTexture("diffuseRadianceHitDist");
    const ref<Texture> pViewW = renderData.getTexture("viewW");
    const ref<Texture> pLinearZ = renderData.getTexture("linearZ");
    if (!pSource || !pViewW || !pLinearZ)
        return;

    const auto cacheCenter = mRadianceCache->getCameraCenter();
    const float4 cacheCenterExtent = float4(cacheCenter.x, cacheCenter.y, cacheCenter.z, mRadianceCache->getBaseExtent());
    const float4 cameraPos4 = float4(cameraPos, 1.f);
    const float4 cameraRight4 = float4(cameraRight, 0.f);
    const float4 cameraUp4 = float4(cameraUp, 0.f);
    const float4 cameraForward4 = float4(cameraForward, 0.f);
    const float4 cameraFocalPrincipal = float4(focalLengthPx, principalPoint.x, principalPoint.y, 0.f);

    auto bindCommon = [&](const ref<ComputePass>& pass, const ref<Buffer>& radiance, bool writePayload)
    {
        auto var = pass->getRootVar();
        var["LumenRadianceCacheCB"]["gFrameDim"] = mFrameDim;
        var["LumenRadianceCacheCB"]["gSourceDim"] = uint2(pSource->getWidth(), pSource->getHeight());
        var["LumenRadianceCacheCB"]["gCurrentFrame"] = mFrameIndex;
        var["LumenRadianceCacheCB"]["gGeneration"] = mRadianceCacheGpu.generation;
        var["LumenRadianceCacheCB"]["gViewProj"] = pCamera->getViewProjMatrixNoJitter();
        var["LumenRadianceCacheCB"]["gCacheCameraCenterBaseExtent"] = cacheCenterExtent;
        var["LumenRadianceCacheCB"]["gCameraPosW"] = cameraPos4;
        var["LumenRadianceCacheCB"]["gCameraRightW"] = cameraRight4;
        var["LumenRadianceCacheCB"]["gCameraUpW"] = cameraUp4;
        var["LumenRadianceCacheCB"]["gCameraForwardW"] = cameraForward4;
        var["LumenRadianceCacheCB"]["gCameraFocalPrincipal"] = cameraFocalPrincipal;
        var["gViewW"] = pViewW;
        var["gLinearZ"] = pLinearZ;
        var["gDiffuseRadianceHitDist"] = pSource;
        var["gProbeMeta"] = mRadianceCacheGpu.pProbeMeta[writeIndex];
        var["gProbeWorldPos"] = mRadianceCacheGpu.pProbeWorldPos;
        var["gProbeScreenPos"] = mRadianceCacheGpu.pProbeScreenPos;
        var["gIndirection"] = mRadianceCacheGpu.pIndirection;
        if (writePayload)
        {
            mpScene->bindShaderData(var["gScene"]);
            mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"], 0u);
            if (mpEmissiveLightSampler)
                mpEmissiveLightSampler->bindShaderData(var["emissiveSampler"]);
        }
        if (writePayload)
            var["gProbeRadianceHitDistOut"] = radiance;
        else
            var["gProbeRadianceHitDist"] = radiance;
        if (writePayload)
            var["gProbeValidityOut"] = mRadianceCacheGpu.pProbeValidity[writeIndex];
        else
            var["gProbeValidity"] = mRadianceCacheGpu.pProbeValidity[1u - mRadianceCacheGpu.currIndex];
        return var;
    };

    // Build the current payload with the bounded scene-ray producer. The probe
    // payload has an explicit hit/sky validity sidecar; it is still diagnostic
    // until persistent GPU allocation/commit and final-resolve integration land.
    bindCommon(mRadianceCacheGpu.pBuild, mRadianceCacheGpu.pProbeRadiance[mRadianceCacheGpu.currIndex], true);
    mRadianceCacheGpu.pBuild->execute(pRenderContext, capacity, 1u, 1u);
    mRadianceCacheGpu.requestCount += activeProbeCount;
    mRadianceCacheGpu.rayCount += activeProbeCount * mRadianceCacheGpu.probeDirectionCount;
    mRadianceCacheGpu.traceCount += activeProbeCount;
    mRadianceCacheGpu.probeRayCount += activeProbeCount * mRadianceCacheGpu.probeDirectionCount;
    mRadianceCacheGpu.commitCount += activeProbeCount;

    const ref<Texture> pOutput = renderData.getTexture("radianceCache");
    const ref<Texture> pHitDist = renderData.getTexture("radianceCacheHitDist");
    const ref<Texture> pValidity = renderData.getTexture("radianceCacheValidity");
    const ref<Texture> pCacheOutput = mRadianceCacheGpu.pOutput;
    const ref<Texture> pCacheHitDist = mRadianceCacheGpu.pHitDistOutput;
    const ref<Texture> pCacheValidity = mRadianceCacheGpu.pValidityOutput;
    if (pCacheOutput && pCacheHitDist && pCacheValidity)
    {
        pRenderContext->clearUAV(mRadianceCacheGpu.pQueryCounters->getUAV().get(), uint4(0u));
        pRenderContext->clearUAV(mRadianceCacheGpu.pLevelQueryCounters->getUAV().get(), uint4(0u));
        const uint32_t readIndex = 1u - mRadianceCacheGpu.currIndex;
        auto var = mRadianceCacheGpu.pInterpolate->getRootVar();
        var["LumenRadianceCacheCB"]["gFrameDim"] = mFrameDim;
        var["LumenRadianceCacheCB"]["gSourceDim"] = uint2(pSource->getWidth(), pSource->getHeight());
        var["LumenRadianceCacheCB"]["gCurrentFrame"] = mFrameIndex;
        var["LumenRadianceCacheCB"]["gGeneration"] = mRadianceCacheGpu.generation;
        var["LumenRadianceCacheCB"]["gViewProj"] = pCamera->getViewProjMatrixNoJitter();
        var["LumenRadianceCacheCB"]["gCacheCameraCenterBaseExtent"] = cacheCenterExtent;
        var["LumenRadianceCacheCB"]["gCameraPosW"] = cameraPos4;
        var["LumenRadianceCacheCB"]["gCameraRightW"] = cameraRight4;
        var["LumenRadianceCacheCB"]["gCameraUpW"] = cameraUp4;
        var["LumenRadianceCacheCB"]["gCameraForwardW"] = cameraForward4;
        var["LumenRadianceCacheCB"]["gCameraFocalPrincipal"] = cameraFocalPrincipal;
        var["gViewW"] = pViewW;
        var["gLinearZ"] = pLinearZ;
        var["gDiffuseRadianceHitDist"] = pSource;
        var["gProbeMeta"] = mRadianceCacheGpu.pProbeMeta[readIndex];
        var["gProbeWorldPos"] = mRadianceCacheGpu.pProbeWorldPos;
        var["gProbeScreenPos"] = mRadianceCacheGpu.pProbeScreenPos;
        var["gProbeRadianceHitDist"] = mRadianceCacheGpu.pProbeRadiance[readIndex];
        var["gProbeRadianceHitDistPrev"] = mRadianceCacheGpu.pProbeRadiance[mRadianceCacheGpu.currIndex];
        var["gProbeValidity"] = mRadianceCacheGpu.pProbeValidity[readIndex];
        var["gIndirection"] = mRadianceCacheGpu.pIndirection;
        var["gRadianceCacheOutput"] = pCacheOutput;
        var["gRadianceCacheHitDist"] = pCacheHitDist;
        var["gRadianceCacheValidityOutput"] = pCacheValidity;
        var["gRadianceCacheQueryCounters"] = mRadianceCacheGpu.pQueryCounters;
        var["gRadianceCacheLevelCounters"] = mRadianceCacheGpu.pLevelQueryCounters;
        mRadianceCacheGpu.pInterpolate->execute(
            pRenderContext, ((mFrameDim.x + 7u) / 8u) * 8u, ((mFrameDim.y + 7u) / 8u) * 8u, 1u
        );
        if (pOutput)
            pRenderContext->copyResource(pOutput.get(), pCacheOutput.get());
        if (pHitDist)
            pRenderContext->copyResource(pHitDist.get(), pCacheHitDist.get());
        if (pValidity)
            pRenderContext->copyResource(pValidity.get(), pCacheValidity.get());
        pRenderContext->copyResource(
            mRadianceCacheGpu.pQueryCountersReadback.get(), mRadianceCacheGpu.pQueryCounters.get()
        );
        pRenderContext->copyResource(
            mRadianceCacheGpu.pLevelQueryCountersReadback.get(), mRadianceCacheGpu.pLevelQueryCounters.get()
        );
        const uint32_t submittedCacheFrame = static_cast<uint32_t>(mRadianceCache->getStats().frameIndex);
        mRadianceCacheGpu.queryCountersSubmittedFrame = submittedCacheFrame;
        mRadianceCacheGpu.levelQueryCountersSubmittedFrame = submittedCacheFrame;
        mRadianceCacheGpu.queryCountersReadbackPending = true;
        mRadianceCacheGpu.levelQueryCountersReadbackPending = true;
        mRadianceCacheGpu.producedThisFrame = true;
        mRadianceCacheGpu.readyCount += activeProbeCount;
        // The payload written by this dispatch is visible to interpolation only
        // on the next frame. Keep the fence in the same frame domain as meta.z.
        mRadianceCacheGpu.lastReadyFrame = mFrameIndex + 1u;
    }
    else
    {
        mRadianceCacheGpu.producedThisFrame = false;
        mRadianceCacheGpu.fallbackCount += static_cast<uint64_t>(mFrameDim.x) * mFrameDim.y;
    }
    mRadianceCacheGpu.currIndex = 1u - mRadianceCacheGpu.currIndex;
}

void LumenGIPass::runRoughSpecularDiagnostic(RenderContext* pRenderContext, const RenderData& renderData)
{
    const ref<Texture> pOutput = renderData.getTexture("roughSpecularIndirect");
    const ref<Texture> pValidity = renderData.getTexture("roughSpecularValidity");
    if ((!pOutput && !pValidity) || any(mFrameDim == uint2(0u, 0u)))
        return;

    if (!mRoughSpecularDiagnostic.pPass || any(mRoughSpecularDiagnostic.resourceDim != mFrameDim))
    {
        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(
            "RenderPasses/LumenGI/RoughSpecular/LumenRoughSpecularTrace.cs.slang"
        ).csEntry("main");
        desc.addTypeConformances(mpScene->getTypeConformances());
        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        if (mpSampleGenerator)
            defines.add(mpSampleGenerator->getDefines());
        defines.add("USE_ANALYTIC_LIGHTS", "1");
        defines.add("LUMEN_GI_ROUGH_SPECULAR_ENABLED", "1");
        defines.add("is_valid_gLinearZ", "0");
        defines.add("is_valid_gViewW", "0");
        defines.add("is_valid_gNormalRoughnessMaterialID", "0");
        defines.add("is_valid_gRoughSpecularIndirect", "0");
        defines.add("is_valid_gRoughSpecularValidity", "0");
        mRoughSpecularDiagnostic.pPass = ComputePass::create(
            mpDevice,
            desc,
            defines,
            /*createVars=*/true
        );
        mRoughSpecularDiagnostic.resourceDim = mFrameDim;
    }

    ref<Program> pProgram = mRoughSpecularDiagnostic.pPass->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("LUMEN_GI_ROUGH_SPECULAR_ENABLED", "1");
    programChanged |= pProgram->addDefine("is_valid_gLinearZ", renderData.getTexture("linearZ") ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gViewW", renderData.getTexture("viewW") ? "1" : "0");
    programChanged |= pProgram->addDefine(
        "is_valid_gNormalRoughnessMaterialID",
        renderData.getTexture("normWRoughnessMaterialID") ? "1" : "0"
    );
    programChanged |= pProgram->addDefine("is_valid_gRoughSpecularIndirect", pOutput ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gRoughSpecularValidity", pValidity ? "1" : "0");
    if (programChanged)
        mRoughSpecularDiagnostic.pPass->setVars(nullptr);

    ShaderVar var = mRoughSpecularDiagnostic.pPass->getRootVar();
    var["LumenRoughSpecularTraceCB"]["gFrameDim"] = mFrameDim;
    var["LumenRoughSpecularTraceCB"]["gFrameIndex"] = mFrameIndex;
    var["LumenRoughSpecularTraceCB"]["gMaxRoughness"] = 0.8f;
    var["LumenRoughSpecularTraceCB"]["gMaxRadiance"] = 10.f;
    var["LumenRoughSpecularTraceCB"]["gSampleCount"] = 4u;
    if (mpScene)
    {
        const ref<Camera>& pCamera = mpScene->getCamera();
        const float3 cameraPos = pCamera->getPosition();
        const float3 cameraBackward = normalize(cameraPos - pCamera->getTarget());
        const float3 cameraRight = normalize(cross(pCamera->getUpVector(), cameraBackward));
        const float3 cameraUp = cross(cameraBackward, cameraRight);
        var["LumenRoughSpecularTraceCB"]["gCameraPosW"] = cameraPos;
        var["LumenRoughSpecularTraceCB"]["gCameraRightW"] = cameraRight;
        var["LumenRoughSpecularTraceCB"]["gCameraUpW"] = cameraUp;
        var["LumenRoughSpecularTraceCB"]["gCameraForwardW"] = cameraBackward;
        var["LumenRoughSpecularTraceCB"]["gFocalLengthPx"] =
            pCamera->getFocalLength() * (float)mFrameDim.y / pCamera->getFrameHeight();
        var["LumenRoughSpecularTraceCB"]["gPrincipalX"] = 0.5f * (float)mFrameDim.x;
        var["LumenRoughSpecularTraceCB"]["gPrincipalY"] = 0.5f * (float)mFrameDim.y;
        mpScene->bindShaderData(var["gScene"]);
        mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"], 0u);
    }
    if (renderData.getTexture("linearZ"))
        var["gLinearZ"] = renderData.getTexture("linearZ");
    if (renderData.getTexture("viewW"))
        var["gViewW"] = renderData.getTexture("viewW");
    if (renderData.getTexture("normWRoughnessMaterialID"))
        var["gNormalRoughnessMaterialID"] = renderData.getTexture("normWRoughnessMaterialID");
    if (pOutput)
        var["gRoughSpecularIndirect"] = pOutput;
    if (pValidity)
        var["gRoughSpecularValidity"] = pValidity;
    mRoughSpecularDiagnostic.pPass->execute(
        pRenderContext,
        ((mFrameDim.x + 7u) / 8u) * 8u,
        ((mFrameDim.y + 7u) / 8u) * 8u,
        1u
    );
    mRoughSpecularDiagnostic.producedThisFrame = true;
}

void LumenGIPass::runTransmissionDiagnostic(RenderContext* pRenderContext, const RenderData& renderData)
{
    const ref<Texture> pOutput = renderData.getTexture("transmissionIndirect");
    const ref<Texture> pValidity = renderData.getTexture("transmissionValidity");
    if ((!pOutput && !pValidity) || any(mFrameDim == uint2(0u, 0u)))
        return;

    if (!mTransmissionDiagnostic.pPass || any(mTransmissionDiagnostic.resourceDim != mFrameDim))
    {
        DefineList defines;
        defines.add("LUMEN_GI_TRANSMISSION_ENABLED", "0");
        defines.add("LUMEN_GI_TRANSMISSION_REFERENCE_ONLY", "1");
        defines.add("is_valid_gTransmissionIndirect", "0");
        defines.add("is_valid_gTransmissionValidityOutput", "0");
        mTransmissionDiagnostic.pPass = ComputePass::create(
            mpDevice,
            "RenderPasses/LumenGI/Transmission/LumenTransmissionDiagnostic.cs.slang",
            "main",
            defines
        );
        mTransmissionDiagnostic.resourceDim = mFrameDim;
    }

    ref<Program> pProgram = mTransmissionDiagnostic.pPass->getProgram();
    bool programChanged = false;
    programChanged |= pProgram->addDefine("LUMEN_GI_TRANSMISSION_ENABLED", "0");
    programChanged |= pProgram->addDefine("LUMEN_GI_TRANSMISSION_REFERENCE_ONLY", "1");
    programChanged |= pProgram->addDefine("is_valid_gTransmissionIndirect", pOutput ? "1" : "0");
    programChanged |= pProgram->addDefine("is_valid_gTransmissionValidityOutput", pValidity ? "1" : "0");
    if (programChanged)
        mTransmissionDiagnostic.pPass->setVars(nullptr);

    ShaderVar var = mTransmissionDiagnostic.pPass->getRootVar();
    var["LumenTransmissionCB"]["gFrameDim"] = mFrameDim;
    var["LumenTransmissionCB"]["gFrameIndex"] = mFrameIndex;
    var["LumenTransmissionCB"]["gMaxTransmissionRadiance"] = 10.f;
    if (pOutput)
        var["gTransmissionIndirect"] = pOutput;
    if (pValidity)
        var["gTransmissionValidityOutput"] = pValidity;
    mTransmissionDiagnostic.pPass->execute(
        pRenderContext,
        ((mFrameDim.x + 7u) / 8u) * 8u,
        ((mFrameDim.y + 7u) / 8u) * 8u,
        1u
    );
    mTransmissionDiagnostic.producedThisFrame = true;
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
            sizeof(uint32_t), kGDFTraceStatCount, ResourceBindFlags::None, MemoryType::ReadBack
        );
        pRenderContext->clearUAV(mSDF.pTraceStats->getUAV().get(), uint4(0));
    }

    // Mesh SDF atlas GPU mirror.  The CPU cache keeps the source quality metadata as
    // R16Float/R8Snorm, but the runtime shader declares both atlas SRVs as Texture3D<float>.
    // Use R32Float staging views here so the Slang typed view and D3D12 descriptor format
    // agree; coarse values remain normalized in [-1,1] and are decoded by quantRange.
    const uint32_t P = std::max<uint32_t>(mSDF.atlasPagesPerSide, 1u);
    const uint32_t texels = P * s6::kLumenMeshSDFAtlasPageSize;
    if (!mSDF.pFineAtlas || mSDF.pFineAtlas->getWidth() != texels ||
        mSDF.pFineAtlas->getFormat() != ResourceFormat::R32Float ||
        !mSDF.pCoarseAtlas || mSDF.pCoarseAtlas->getFormat() != ResourceFormat::R32Float)
    {
        mSDF.pFineAtlas = mpDevice->createTexture3D(
            texels, texels, texels, ResourceFormat::R32Float, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mSDF.pFineAtlas->setName("LumenGIPass::MSDFFineAtlas");
        mSDF.pCoarseAtlas = mpDevice->createTexture3D(
            texels, texels, texels, ResourceFormat::R32Float, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mSDF.pCoarseAtlas->setName("LumenGIPass::MSDFCoarseAtlas");
    }
    if (!mSDF.pPageTable)
        mSDF.pPageTable = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), s6::kLumenMeshSDFAtlasMaxInstances * s6::kLumenMeshSDFMaxMipCount,
            ResourceBindFlags::ShaderResource
        );
    {
        // Volume descriptors buffer sized to the deduplicated mesh count (grown lazily so a
        // large scene never overflows the upload). 256 is the fixed minimum.
        const uint32_t volumeCount = std::max<uint32_t>(mSDF.pScene->instanceTable().meshCount(), 256u);
        if (!mSDF.pVolumes || mSDF.pVolumes->getElementCount() < volumeCount)
        {
            mSDF.pVolumes = mpDevice->createStructuredBuffer(
                sizeof(s6::LumenMeshSDFVolumeDescriptor), volumeCount, ResourceBindFlags::ShaderResource
            );
        }
    }
    if (!mSDF.pAtlasInstances)
        mSDF.pAtlasInstances = mpDevice->createStructuredBuffer(
            sizeof(s6::LumenMeshSDFAtlasInstance), s6::kLumenMeshSDFAtlasMaxInstances,
            ResourceBindFlags::ShaderResource
        );

    // Compose pass (LumenGDFCompose.cs.slang, entry "main"). The trace pass is created lazily by
    // runGDFSphereTrace because its is_valid defines depend on the graph channel allocation.
    if (!mSDF.pCompose)
        mSDF.pCompose = ComputePass::create(mpDevice, kGDFComposeShaderFile, "main", DefineList());
    if (mGDFDiagnosticStage == 1 && !mSDF.pComposeDiag)
        mSDF.pComposeDiag = ComputePass::create(mpDevice, kGDFComposeDiagShaderFile, "main", DefineList());
    if (mGDFDiagnosticStage == 2 && !mSDF.pComposeDiagAll)
        mSDF.pComposeDiagAll = ComputePass::create(mpDevice, kGDFComposeDiagAllShaderFile, "main", DefineList());
    if (mGDFDiagnosticStage == 3 && !mSDF.pComposeDiagBuffers)
        mSDF.pComposeDiagBuffers = ComputePass::create(mpDevice, kGDFComposeDiagBuffersShaderFile, "main", DefineList());
    if (mGDFDiagnosticStage == 4 && !mSDF.pComposeDiagAtlas)
        mSDF.pComposeDiagAtlas = ComputePass::create(mpDevice, kGDFComposeDiagAtlasShaderFile, "main", DefineList());
    if (mGDFDiagnosticStage == 5 && !mSDF.pComposeDiagBuffersScalar)
        mSDF.pComposeDiagBuffersScalar = ComputePass::create(mpDevice, kGDFComposeDiagBuffersScalarShaderFile, "main", DefineList());
    if (mGDFDiagnosticStage == 6 && !mSDF.pComposeDiagCBScalar)
        mSDF.pComposeDiagCBScalar = ComputePass::create(mpDevice, kGDFComposeDiagCBScalarShaderFile, "main", DefineList());

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

    // Upload raw float atlas images.  The physical staging resources are R32Float to match
    // Texture3D<float>; coarseImage is already normalized and keeps its quantRange codec.
    if (!mSDF.fineImage.empty())
    {
        pRenderContext->updateTextureData(mSDF.pFineAtlas.get(), mSDF.fineImage.data());
    }
    if (!mSDF.coarseImage.empty())
    {
        pRenderContext->updateTextureData(mSDF.pCoarseAtlas.get(), mSDF.coarseImage.data());
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
    if (regions.empty())
        return; // no dirty voxels this frame (static camera, no instance changes).

    if (mSDF.levels.empty())
    {
        logWarning("C4 GDF diagnostic/compose skipped because no level UAVs are allocated.");
        return;
    }

    // C4 bounded descriptor bisect. This opt-in path intentionally executes only one
    // logical thread against the first dirty level so E1/E2 can distinguish a minimal
    // UAV contract from the full production descriptor set without changing production
    // dispatch dimensions or the Hybrid fallback behavior.
    if (mGDFDiagnosticStage != 0u)
    {
        ref<ComputePass> pDiag;
        switch (mGDFDiagnosticStage)
        {
        case 1u: pDiag = mSDF.pComposeDiag; break;
        case 2u: pDiag = mSDF.pComposeDiagAll; break;
        case 3u: pDiag = mSDF.pComposeDiagBuffers; break;
        case 4u: pDiag = mSDF.pComposeDiagAtlas; break;
        case 5u: pDiag = mSDF.pComposeDiagBuffersScalar; break;
        case 6u: pDiag = mSDF.pComposeDiagCBScalar; break;
        default: break;
        }
        if (!pDiag)
        {
            logWarning("C4 GDF diagnostic stage {} requested but its ComputePass is unavailable.", mGDFDiagnosticStage);
            return;
        }

        // E2 reads gGDFDirtyRegions[0]. Upload the same bounded region used by
        // the diagnostic dispatch before binding the descriptor set; production
        // batches perform this upload inside the per-level loop below.
        pRenderContext->updateBuffer(
            mSDF.pDirtyRegions.get(), &regions.front(), sizeof(LumenGDFDirtyRegionHost)
        );
        const uint32_t level = std::min<uint32_t>(regions.front().level, static_cast<uint32_t>(mSDF.levels.size() - 1u));
        ShaderVar var = pDiag->getRootVar();
        const bool bindClipmap = mGDFDiagnosticStage == 2u || mGDFDiagnosticStage == 3u ||
            mGDFDiagnosticStage == 5u || mGDFDiagnosticStage == 6u;
        const bool bindBuffers = mGDFDiagnosticStage == 2u || mGDFDiagnosticStage == 3u || mGDFDiagnosticStage == 5u;
        const bool bindAtlas = mGDFDiagnosticStage == 2u || mGDFDiagnosticStage == 4u;
        if (bindClipmap)
        {
            ShaderVar cb = var["LumenGDFComposeCB"];
            ShaderVar clip = cb["gClipmap"];
            clip["cameraCenter"] = float3(mSDF.gdf->getCameraCenter().x, mSDF.gdf->getCameraCenter().y, mSDF.gdf->getCameraCenter().z);
            clip["emptyDistance"] = 0.f;
            clip["levelCount"] = levelCount;
            clip["dynamicLevelCount"] = s6gdf::kDynamicLevels;
            clip["resolution"] = R;
            clip["instanceCount"] = static_cast<uint32_t>(gdfInstances.size());
            clip["dirtyRegionCount"] = 1u;
            clip["frameIndex"] = mFrameIndex;
            const s6gdf::index3 scroll = mSDF.gdf->scrollFromCameraMove();
            clip["scroll"] = int3(scroll.x, scroll.y, scroll.z);
        }
        if (bindBuffers)
        {
            var["gGDFLevelTable"] = mSDF.pLevelTable;
            var["gGDFInstances"] = mSDF.pGDFInstances;
            var["gGDFDirtyRegions"] = mSDF.pDirtyRegions;
        }
        if (bindAtlas)
        {
            var["gFineAtlas"] = mSDF.pFineAtlas;
            var["gCoarseAtlas"] = mSDF.pCoarseAtlas;
            var["gPageTable"] = mSDF.pPageTable;
            var["gVolumes"] = mSDF.pVolumes;
            var["gInstances"] = mSDF.pAtlasInstances;
            var["gAtlasInstanceCount"] = mSDF.pScene->instanceTable().instanceCount();
            var["gAtlasVolumeCount"] = mSDF.pScene->instanceTable().meshCount();
            var["gAtlasPagesPerSide"] = mSDF.atlasPagesPerSide;
        }
        if (mGDFDiagnosticStage == 5u || mGDFDiagnosticStage == 6u)
            var["gAtlasPagesPerSide"] = mSDF.atlasPagesPerSide;
        var["gGDFLevel"].setUav(mSDF.levels[level]->getUAV(0));
        logInfo(
            "C4 GDF diagnostic dispatch: stage={} level={} logicalThreads=(1,1,1) allDescriptors={}",
            mGDFDiagnosticStage, level, mGDFDiagnosticStage == 2u
        );
        pDiag->execute(pRenderContext, 1u, 1u, 1u);
        return;
    }

    // Bind the compose pass.
    logInfo(
        "S6 compose bind: P={} texels={} levelCount={} R={} inst={} vol={} fine={} coarse={} pt={} volBuf={} instBuf={} levels={}",
        mSDF.atlasPagesPerSide, mSDF.atlasPagesPerSide, levelCount, R, mSDF.pScene ? mSDF.pScene->instanceTable().instanceCount() : 0u,
        mSDF.pScene ? mSDF.pScene->instanceTable().meshCount() : 0u, mSDF.pFineAtlas ? 1u : 0u,
        mSDF.pCoarseAtlas ? 1u : 0u, mSDF.pPageTable ? 1u : 0u, mSDF.pVolumes ? 1u : 0u,
        mSDF.pAtlasInstances ? 1u : 0u, (uint32_t)mSDF.levels.size()
    );
    ShaderVar var = mSDF.pCompose->getRootVar();
    ShaderVar cb = var["LumenGDFComposeCB"];
    ShaderVar clip = cb["gClipmap"];
    clip["cameraCenter"] = float3(mSDF.gdf->getCameraCenter().x, mSDF.gdf->getCameraCenter().y, mSDF.gdf->getCameraCenter().z);
    clip["emptyDistance"] = 0.f; // levels set their own emptyDistance.
    clip["levelCount"] = levelCount;
    clip["dynamicLevelCount"] = s6gdf::kDynamicLevels;
    clip["resolution"] = R;
    clip["instanceCount"] = static_cast<uint32_t>(gdfInstances.size());
    clip["frameIndex"] = mFrameIndex;
    const s6gdf::index3 scroll = mSDF.gdf->scrollFromCameraMove();
    clip["scroll"] = int3(scroll.x, scroll.y, scroll.z);

    var["gGDFLevelTable"] = mSDF.pLevelTable;
    var["gGDFInstances"] = mSDF.pGDFInstances;
    var["gGDFDirtyRegions"] = mSDF.pDirtyRegions;
    // S6-B2 atlas bindings (SRV): fine/coarse atlas textures, page table, volume descriptors
    // and the instance table, plus the atlas-geometry scalars. Created + uploaded every frame
    // the atlas data is dirty in ensureGDFResources()/uploadMeshSDFAtlas().
    var["gFineAtlas"] = mSDF.pFineAtlas;
    var["gCoarseAtlas"] = mSDF.pCoarseAtlas;
    var["gPageTable"] = mSDF.pPageTable;
    var["gVolumes"] = mSDF.pVolumes;
    var["gInstances"] = mSDF.pAtlasInstances;
    cb["gAtlasInstanceCount"] = mSDF.pScene->instanceTable().instanceCount();
    cb["gAtlasVolumeCount"] = mSDF.pScene->instanceTable().meshCount();
    cb["gAtlasPagesPerSide"] = mSDF.atlasPagesPerSide;

    // A single RWTexture3D UAV is bound per dispatch. This avoids D3D12 rejecting
    // a partially populated/mixed-format UAV descriptor array; regions are batched
    // by clipmap level, and the shader still uses region.level for table lookup.
    for (uint32_t level = 0; level < levelCount; ++level)
    {
        std::vector<LumenGDFDirtyRegionHost> batch;
        for (const LumenGDFDirtyRegionHost& region : regions)
            if (region.level == level)
                batch.push_back(region);
        if (batch.empty())
            continue;

        pRenderContext->updateBuffer(
            mSDF.pDirtyRegions.get(), batch.data(), 0, batch.size() * sizeof(LumenGDFDirtyRegionHost)
        );
        clip["dirtyRegionCount"] = static_cast<uint32_t>(batch.size());
        var["gGDFLevel"].setUav(mSDF.levels[level]->getUAV(0));

        uint32_t maxDx = 1u, maxDyz = 1u;
        for (const LumenGDFDirtyRegionHost& region : batch)
        {
            const uint32_t dx = static_cast<uint32_t>(std::max(0, region.max[0] - region.min[0] + 1));
            const uint32_t dyz = static_cast<uint32_t>(std::max(0, region.max[1] - region.min[1] + 1)) *
                                 static_cast<uint32_t>(std::max(0, region.max[2] - region.min[2] + 1));
            maxDx = std::max(maxDx, std::max(dx, 1u));
            maxDyz = std::max(maxDyz, std::max(dyz, 1u));
        }
        logInfo(
            "S6 compose dispatch: level={} threads=({}, {}, {}) groups=({}, {}, {}) regions={}",
            level, maxDx, static_cast<uint32_t>(batch.size()), maxDyz,
            (maxDx + 7u) / 8u, static_cast<uint32_t>(batch.size()), (maxDyz + 7u) / 8u,
            batch.size()
        );
        // ComputePass::execute takes logical thread counts and performs the
        // ceil-divide using the reflected [numthreads(8, 1, 8)] declaration.
        mSDF.pCompose->execute(pRenderContext, maxDx, static_cast<uint32_t>(batch.size()), maxDyz);
    }

    mSDF.sceneStats = mSDF.pScene->getStats();
}

void LumenGIPass::runGDFSphereTrace(RenderContext* pRenderContext, const RenderData& renderData)
{
    namespace s6 = LumenGI::MeshSDF;
    if (!mSDF.pScene || !mSDF.gdf || !mSDF.pCompose || mSDF.levels.empty())
        return;

    // The current GDF payload is a distance/hit diagnostic. It must not overwrite
    // the already-produced HWRT diffuse radiance until a material-lighting router
    // supplies a physically meaningful replacement.
    constexpr bool primary = false;
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

    // ComputePass::execute takes logical thread counts and performs the
    // ceil-divide using the reflected [numthreads(8, 8, 1)] declaration. Do
    // not pass pre-divided group counts here or the GDF trace covers only an
    // eighth of the intended image in each axis.
    mSDF.pTrace->execute(pRenderContext, mFrameDim.x, mFrameDim.y, 1);

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
    const bool gdfExecuted = mUseGDF && mSDF.pScene && mSDF.gdf && mSDF.pCompose && mSDF.pTrace;
    stats["active"] = gdfExecuted ? 1.0 : 0.0;
    stats["requestedActive"] = (mUseGDF || mTraceMode != TraceMode::HardwareRT) ? 1.0 : 0.0;
    stats["gdfExecuted"] = gdfExecuted ? 1.0 : 0.0;
    stats["traceMode"] = static_cast<double>(static_cast<uint32_t>(mTraceMode));
    stats["useGDF"] = mUseGDF ? 1.0 : 0.0;
    // C4/C5 routing telemetry. The current distance-field payload is diagnostic
    // only; HWRT remains the authoritative material-lighting source until a
    // shared hit-record router is implemented. Exposing this explicitly prevents
    // a gdfTrace texture from being mistaken for selected GI radiance.
    stats["hwrtPrimary"] = 1.0;
    stats["gdfRadianceSelected"] = 0.0;
    stats["hybridFallbackToHWRT"] = (mTraceMode == TraceMode::Hybrid || mTraceMode == TraceMode::MeshSDF) ? 1.0 : 0.0;
    stats["probeFallbackAttempts"] = (double)mScreenProbeStats.fallbackAttempts;
    stats["probeFallbackHits"] = (double)mScreenProbeStats.fallbackHits;
    stats["probeFallbackMisses"] = (double)mScreenProbeStats.fallbackMisses;
    stats["probeFallbackUnavailable"] = (double)mScreenProbeStats.fallbackUnavailable;

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

std::map<std::string, double> LumenGIPass::getScreenProbeStats() const
{
    std::map<std::string, double> stats;
    const uint2 grid = LumenScreenProbe::probeGridDims(mFrameDim);
    stats["probeCount"] = (double)mScreenProbeStats.probeCount;
    stats["resourceDimX"] = (double)mScreenProbes.resourceDim.x;
    stats["resourceDimY"] = (double)mScreenProbes.resourceDim.y;
    stats["probeGridX"] = (double)grid.x;
    stats["probeGridY"] = (double)grid.y;
    stats["directionsPerProbe"] = (double)mScreenProbeStats.directionsPerProbe;
    stats["updateInterval"] = (double)mScreenProbeStats.updateInterval;
    stats["expectedProbesPerFrame"] = (double)mScreenProbeStats.expectedProbesPerFrame;
    stats["screenHits"] = (double)mScreenProbeStats.screenHits;
    stats["fallbackAttempts"] = (double)mScreenProbeStats.fallbackAttempts;
    stats["fallbackHits"] = (double)mScreenProbeStats.fallbackHits;
    stats["fallbackMisses"] = (double)mScreenProbeStats.fallbackMisses;
    stats["fallbackUnavailable"] = (double)mScreenProbeStats.fallbackUnavailable;
    stats["inactiveProbes"] = (double)mScreenProbeStats.inactiveProbes;
    stats["budgetSkipped"] = (double)mScreenProbeStats.budgetSkipped;
    stats["directionsTraced"] = (double)mScreenProbeStats.directionsTraced;
    stats["gdfHits"] = (double)mScreenProbeStats.gdfHits;
    stats["gdfMisses"] = (double)mScreenProbeStats.gdfMisses;
    stats["cacheLookupHits"] = (double)mScreenProbeStats.cacheLookupHits;
    stats["cacheLookupAttempts"] = (double)mScreenProbeStats.cacheLookupAttempts;
    stats["cacheLookupHitsThisFrame"] = (double)mScreenProbeStats.cacheLookupHits;
    stats["cacheLookupAttemptsThisFrame"] = (double)mScreenProbeStats.cacheLookupAttempts;
    stats["cacheLookupStatsFrame"] = (double)mScreenProbeStatsFrame;
    stats["cachePageRejects"] = (double)mScreenProbeStats.cachePageRejects;
    stats["cacheCoverageRejects"] = (double)mScreenProbeStats.cacheCoverageRejects;
    stats["cacheMetadataRejects"] = (double)mScreenProbeStats.cacheMetadataRejects;
    stats["cacheVisibilityRejects"] = (double)mScreenProbeStats.cacheVisibilityRejects;
    stats["cacheDepthRejects"] = (double)mScreenProbeStats.cacheDepthRejects;
    stats["cacheAxisRejects"] = (double)mScreenProbeStats.cacheAxisRejects;
    stats["cacheFacingRejects"] = (double)mScreenProbeStats.cacheFacingRejects;
    stats["cacheOwnerValid"] = (double)mScreenProbeStats.cacheOwnerValid;
    stats["historyAccepted"] = (double)mScreenProbeStats.historyAccepted;
    stats["historyRejectDepth"] = (double)mScreenProbeStats.historyRejectDepth;
    stats["historyRejectGuide"] = (double)mScreenProbeStats.historyRejectGuide;
    stats["historyRejectMotion"] = (double)mScreenProbeStats.historyRejectMotion;
    stats["historyRejectLighting"] = (double)mScreenProbeStats.historyRejectLighting;
    stats["historyRejectCurrentInvalid"] = (double)mScreenProbeStats.historyRejectCurrentInvalid;
    stats["historyRejectPreviousInvalid"] = (double)mScreenProbeStats.historyRejectPreviousInvalid;
    stats["historyReset"] = (double)mScreenProbeStats.historyReset;
    stats["gdfRouteEnabled"] = (mUseGDF && mSDF.gdf && mSDF.pLevelTable && !mSDF.levels.empty()) ? 1.0 : 0.0;
    stats["screenHitRate"] = (double)mScreenProbeStats.screenHitRate();
    stats["fallbackHitRate"] = (double)mScreenProbeStats.fallbackHitRate();
    // A1 producer validity/epoch telemetry. These fields are host-side until the
    // per-direction backend/age sidecar ABI is added; exposing them now lets the
    // convergence/reset harness distinguish a real reset from stale texture data.
    stats["historyGeneration"] = (double)mHistoryGeneration;
    stats["lightingGeneration"] = (double)mLightingGeneration;
    stats["historyResetCount"] = (double)mHistoryResetCount;
    stats["lastHistoryResetReason"] = (double)static_cast<uint32_t>(mLastHistoryResetReason);
    stats["historyResetPending"] = mScreenProbes.historyResetPending ? 1.0 : 0.0;
    stats["historyResetThisFrame"] = mHistoryResetThisFrame ? 1.0 : 0.0;
    stats["historyReadIndex"] = (double)(1u - mScreenProbes.screenRadianceHistoryCurrIndex);
    stats["historyWriteIndex"] = (double)mScreenProbes.screenRadianceHistoryCurrIndex;
    return stats;
}

std::map<std::string, double> LumenGIPass::getRadianceCacheStats() const
{
    std::map<std::string, double> stats;
    stats["enabled"] = mUseRadianceCache ? 1.0 : 0.0;
    // GPU fields are deliberately separate from the CPU clipmap counters below.
    // A non-zero CPU resident page count must never be mistaken for a dispatched
    // producer/interpolator or a ready-frame fence.
    stats["gpuProducerEnabled"] = mRadianceCacheGpu.pBuild ? 1.0 : 0.0;
    stats["gpuInterpolationEnabled"] = mRadianceCacheGpu.pInterpolate ? 1.0 : 0.0;
    stats["readyNextFrame"] = mRadianceCacheGpu.producedThisFrame ? 1.0 : 0.0;
    stats["staleWriteRejects"] = (double)mRadianceCacheGpu.staleWriteRejects;
    stats["traceCount"] = (double)mRadianceCacheGpu.traceCount;
    stats["probeRayCount"] = (double)mRadianceCacheGpu.probeRayCount;
    stats["probeDirectionCount"] = (double)mRadianceCacheGpu.probeDirectionCount;
    stats["requestCount"] = (double)mRadianceCacheGpu.requestCount;
    stats["rayCount"] = (double)mRadianceCacheGpu.rayCount;
    stats["commitCount"] = (double)mRadianceCacheGpu.commitCount;
    stats["readyCount"] = (double)mRadianceCacheGpu.readyCount;
    stats["queryHits"] = (double)mRadianceCacheGpu.queryHits;
    stats["queryMisses"] = (double)mRadianceCacheGpu.queryMisses;
    stats["queryAttempts"] = (double)mRadianceCacheGpu.queryAttempts;
    stats["queryCountersFrame"] = (double)mRadianceCacheGpu.queryCountersFrame;
    stats["queryCountersSubmittedFrame"] = (double)mRadianceCacheGpu.queryCountersSubmittedFrame;
    stats["queryCountersReadbackPending"] = mRadianceCacheGpu.queryCountersReadbackPending ? 1.0 : 0.0;
    stats["fallbackCount"] = (double)mRadianceCacheGpu.fallbackCount;
    stats["projectedProbeCount"] = (double)mRadianceCacheGpu.projectedProbeCount;
    stats["inBoundsProbeCount"] = (double)mRadianceCacheGpu.inBoundsProbeCount;
    stats["levelQueryCountersFrame"] = (double)mRadianceCacheGpu.levelQueryCountersFrame;
    stats["levelQueryCountersSubmittedFrame"] = (double)mRadianceCacheGpu.levelQueryCountersSubmittedFrame;
    stats["levelQueryCountersReadbackPending"] = mRadianceCacheGpu.levelQueryCountersReadbackPending ? 1.0 : 0.0;
    // True only after the final resolve has actually received the cache
    // fallback resources for the current frame. This is intentionally distinct
    // from producer/interpolator creation and from a diagnostic output copy.
    stats["finalResolveConnected"] =
        (mUseRadianceCache && mRadianceCacheGpu.producedThisFrame && mFinalResolve.pPass) ? 1.0 : 0.0;
    if (!mRadianceCache)
    {
        stats["contractStatus"] = 0.0; // disabled/uninitialized
        return stats;
    }

    const auto s = mRadianceCache->getStats();
    stats["contractStatus"] = mRadianceCacheGpu.producedThisFrame ? 2.0 : 1.0;
    stats["levelCount"] = (double)s.levelCount;
    stats["resolution"] = (double)s.resolution;
    stats["maxSlots"] = (double)s.maxSlots;
    stats["allocatedSlotCount"] = (double)s.allocatedSlotCount;
    stats["allocatedSlots"] = (double)s.allocatedSlotCount;
    stats["residentPages"] = (double)s.allocatedSlotCount;
    stats["freeSlotCount"] = (double)s.freeSlotCount;
    stats["emptyCellCount"] = (double)s.emptyCellCount;
    stats["dirtyCells"] = (double)s.emptyCellCount;
    stats["refreshBudgetPerFrame"] = (double)s.refreshBudgetPerFrame;
    stats["lastRefreshCount"] = (double)s.lastRefreshCount;
    stats["refreshedProbes"] = (double)s.lastRefreshCount;
    stats["frameIndex"] = (double)s.frameIndex;
    stats["memoryBudgetBytes"] = (double)s.memoryBudgetBytes;
    stats["estimateMemoryBytes"] = (double)s.estimateMemoryBytes;
    stats["residentBytes"] = (double)s.estimateMemoryBytes;
    stats["allocationCount"] = (double)s.allocationCount;
    stats["evictionCount"] = (double)s.evictionCount;
    stats["evictions"] = (double)s.evictionCount;
    stats["releaseCount"] = (double)s.releaseCount;
    stats["updateCount"] = (double)s.updateCount;
    stats["dropCount"] = (double)s.dropCount;
    stats["queryCount"] = (double)s.queryCount;
    stats["refreshCount"] = (double)s.refreshCount;
    stats["lastReadyFrame"] = (double)mRadianceCacheGpu.lastReadyFrame;
    const uint32_t levelCount = std::min<uint32_t>(s.levelCount, 8u);
    stats["coverageLevelCount"] = (double)levelCount;
    for (uint32_t level = 0u; level < levelCount; ++level)
    {
        const std::string prefix = "coverageLevel" + std::to_string(level);
        stats[prefix + "ProjectedProbeCount"] = (double)mRadianceCacheGpu.levelProjectedProbeCount[level];
        stats[prefix + "InBoundsProbeCount"] = (double)mRadianceCacheGpu.levelInBoundsProbeCount[level];
        stats[prefix + "QueryAttempts"] = (double)mRadianceCacheGpu.levelQueryAttempts[level];
        stats[prefix + "QueryHits"] = (double)mRadianceCacheGpu.levelQueryHits[level];
        stats[prefix + "QueryMisses"] = (double)mRadianceCacheGpu.levelQueryMisses[level];
        stats[prefix + "SampleCount"] = (double)mRadianceCacheGpu.levelSampleCount[level];
        stats[prefix + "ValidHitDistanceCount"] = (double)mRadianceCacheGpu.levelValidHitDistanceCount[level];
        stats[prefix + "FallbackSampleCount"] = (double)mRadianceCacheGpu.levelFallbackSampleCount[level];
    }
    return stats;
}

std::map<std::string, double> LumenGIPass::getQualityPresetStats() const
{
    std::map<std::string, double> stats;
    stats["qualityPreset"] = static_cast<double>(static_cast<uint32_t>(mQualityPreset));
    stats["probeDirectionsPerProbe"] = static_cast<double>(mProbeDirectionsPerProbe);
    stats["captureMaxPagesPerFrame"] = static_cast<double>(mCaptureMaxPagesPerFrame);
    stats["cacheLightingFeedbackMaxBounces"] = static_cast<double>(mCacheLightingFeedbackMaxBounces);
    stats["spatialRadiusMin"] = static_cast<double>(mSpatialRadiusMin);
    stats["spatialRadiusMax"] = static_cast<double>(mSpatialRadiusMax);
    stats["spatialNeighborhoodRadius"] = static_cast<double>(mSpatialNeighborhoodRadius);
    stats["temporalHistoryLengthCap"] = static_cast<double>(mTemporalHistoryLengthCap);
    stats["gdfTraceMaxSteps"] = static_cast<double>(mGDFTraceMaxSteps);
    stats["gdfTraceMaxDistance"] = static_cast<double>(mGDFTraceMaxDistance);
    stats["meshSDFResolution"] = static_cast<double>(mMeshSDFResolution);
    stats["meshSDFQuality"] = static_cast<double>(mMeshSDFQuality);
    return stats;
}
