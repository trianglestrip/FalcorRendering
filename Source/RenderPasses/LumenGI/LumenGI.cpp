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

namespace
{
const char kShaderFile[] = "RenderPasses/LumenGI/LumenGIDebug.cs.slang";
const char kTraceShaderFile[] = "RenderPasses/LumenGI/Tracing/LumenHardwareTrace.rt.slang";
const char kCaptureShaderFile[] = "RenderPasses/LumenGI/Capture/LumenCardCapture.3d.slang";
const char kCacheLightingShaderFile[] = "RenderPasses/LumenGI/Lighting/LumenSurfaceCacheLighting.cs.slang";

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
const char kUseTemporalFilter[] = "useTemporalFilter";
const char kUseSpatialFilter[] = "useSpatialFilter";
const char kUseRadianceCache[] = "useRadianceCache";
const char kSurfaceCacheAtlasSize[] = "surfaceCacheAtlasSize";
const char kCaptureMaxPagesPerFrame[] = "captureMaxPagesPerFrame";

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
    // clang-format on
};

void registerBindings(pybind11::module& m)
{
    pybind11::class_<LumenGI, RenderPass, ref<LumenGI>> pass(m, "LumenGI");
    // Scriptable S2 gate snapshot: read as m.activeGraph.getPass("LumenGI").surfaceCacheStats.
    // std::map<std::string, double> converts to a Python dict losslessly.
    pass.def_property_readonly("surfaceCacheStats", &LumenGI::getSurfaceCacheStats);
}
} // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, LumenGI>();
    ScriptBindings::registerBinding(registerBindings);
}

LumenGI::LumenGI(ref<Device> pDevice, const Properties& props)
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

void LumenGI::parseProperties(const Properties& props)
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
        else if (key == kUseTemporalFilter)
            mUseTemporalFilter = value;
        else if (key == kUseSpatialFilter)
            mUseSpatialFilter = value;
        else if (key == kUseRadianceCache)
            mUseRadianceCache = value;
        else if (key == kSurfaceCacheAtlasSize)
            mAtlasSizeTexels = value;
        else if (key == kCaptureMaxPagesPerFrame)
            mCaptureMaxPagesPerFrame = value;
        else
            logWarning("Unknown property '{}' in LumenGI properties.", key);
    }
}

Properties LumenGI::getProperties() const
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
    props[kUseTemporalFilter] = mUseTemporalFilter;
    props[kUseSpatialFilter] = mUseSpatialFilter;
    props[kUseRadianceCache] = mUseRadianceCache;
    props[kSurfaceCacheAtlasSize] = mAtlasSizeTexels;
    props[kCaptureMaxPagesPerFrame] = mCaptureMaxPagesPerFrame;
    return props;
}

RenderPassReflection LumenGI::reflect(const CompileData& compileData)
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

void LumenGI::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    if (any(mFrameDim != compileData.defaultTexDims))
    {
        mFrameDim = compileData.defaultTexDims;
        resetHistory();
    }
}

void LumenGI::execute(RenderContext* pRenderContext, const RenderData& renderData)
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
        resetHistory();
        mSceneUpdates = IScene::UpdateFlags::None;
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

void LumenGI::renderUI(Gui::Widgets& widget)
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

    if (dirty)
    {
        mOptionsChanged = true;
        resetHistory();
    }
}

void LumenGI::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
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

void LumenGI::onHotReload(HotReloadFlags reloaded)
{
    if (is_set(reloaded, HotReloadFlags::Program))
    {
        mpDebugPass = nullptr;
        mTracer.pProgram = nullptr;
        mTracer.pBindingTable = nullptr;
        mTracer.pVars = nullptr;
        invalidateCaptureResources();
        resetHistory();
    }
}

void LumenGI::resetHistory()
{
    mFrameIndex = 0;
}

void LumenGI::clearOutputs(RenderContext* pRenderContext, const RenderData& renderData) const
{
    for (const auto& channel : kOutputChannels)
    {
        if (const auto& pTexture = renderData.getTexture(channel.name))
            pRenderContext->clearUAV(pTexture->getUAV().get(), float4(0.f));
    }
}

void LumenGI::createDebugPass(const DefineList& defines)
{
    mpDebugPass = ComputePass::create(mpDevice, kShaderFile, "main", defines);
}

void LumenGI::createTraceProgram()
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

void LumenGI::prepareTraceVars()
{
    FALCOR_ASSERT(mpScene && mTracer.pProgram && mTracer.pBindingTable);
    mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());
    mTracer.pVars = RtProgramVars::create(mpDevice, mTracer.pProgram, mTracer.pBindingTable);
    mpSampleGenerator->bindShaderData(mTracer.pVars->getRootVar());
}

void LumenGI::ensureTraceResources()
{
    // One uint4 per LumenGICounterIndex entry (NaN/Inf, firefly, negative,
    // traced rays). The shader defines the same layout in LumenGIData.slang.
    const uint32_t kCounterCount = 4u;
    if (!mpLumenGICounters)
    {
        mpLumenGICounters = mpDevice->createStructuredBuffer(
            sizeof(uint4), kCounterCount, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpLumenGICounters->setName("LumenGI::Counters");
    }
    if (!mpLumenGICountersReadback)
    {
        mpLumenGICountersReadback = mpDevice->createStructuredBuffer(
            sizeof(uint4), kCounterCount, ResourceBindFlags::None, MemoryType::ReadBack
        );
        mpLumenGICountersReadback->setName("LumenGI::CountersReadback");
    }

    if (!mpLightingComponents && any(mFrameDim > 0u))
    {
        mpLightingComponents = mpDevice->createTexture2D(
            mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpLightingComponents->setName("LumenGI::LightingComponents");
    }
}

void LumenGI::readbackCounters(RenderContext* pRenderContext)
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

std::map<std::string, double> LumenGI::getSurfaceCacheStats() const
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

void LumenGI::invalidateCaptureResources()
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

void LumenGI::ensureCaptureResources(RenderContext* pRenderContext)
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
        mCapture.pMaterialAtlas->setName("LumenGI::Capture::MaterialAtlas"); // RGBA8, 4 B/texel, atlas lifetime.
        pRenderContext->clearUAV(mCapture.pMaterialAtlas->getUAV().get(), float4(0.f));
    }
    if (!mCapture.pRadianceAtlas)
    {
        mCapture.pRadianceAtlas = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCapture.pRadianceAtlas->setName("LumenGI::Capture::RadianceAtlas"); // RGBA16F, 8 B/texel, atlas lifetime.
        pRenderContext->clearUAV(mCapture.pRadianceAtlas->getUAV().get(), float4(0.f));
    }
    if (!mCapture.pMetadataAtlas)
    {
        mCapture.pMetadataAtlas = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::RGBA16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCapture.pMetadataAtlas->setName("LumenGI::Capture::MetadataAtlas"); // RGBA16F, 8 B/texel, atlas lifetime.
        pRenderContext->clearUAV(mCapture.pMetadataAtlas->getUAV().get(), float4(0.f));
    }

    const uint32_t cardCount = mpCardScene->getCardCount();

    // gCards: StructuredBuffer<LumenCard>, 96 B/card, full upload every capture frame. The
    // buffer (and the host page-table mirror) is recreated when the card count changes
    // (geometry rebuild), which also resets the page table to invalid.
    if (cardCount > 0 && (!mCapture.pCards || mCapture.pCards->getElementCount() != cardCount))
    {
        mCapture.pCards = mpDevice->createStructuredBuffer(sizeof(LumenCard), cardCount, ResourceBindFlags::ShaderResource);
        mCapture.pCards->setName("LumenGI::Capture::Cards"); // StructuredBuffer<LumenCard>, 96 B stride, scene-scoped.
        mCardPageTable.assign(cardCount, kLumenCardInvalidID);
        mCardPageGeneration.assign(cardCount, 0u);
        mCapture.pPageTable = mpDevice->createStructuredBuffer(sizeof(uint32_t), cardCount, ResourceBindFlags::ShaderResource);
        mCapture.pPageTable->setName("LumenGI::Capture::PageTable"); // cardIndex -> pageID, uint32, scene-scoped.
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
        mCapture.pDrawArgs->setName("LumenGI::Capture::DrawArgs"); // DrawIndexedArguments/DrawArguments blob, scene-scoped.
    }

    if (!mCapture.pProgram)
        createCaptureProgram();
}

void LumenGI::runSurfaceCacheCapture(RenderContext* pRenderContext, IScene::UpdateFlags updateFlags)
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

void LumenGI::runCapturePass(RenderContext* pRenderContext, const LumenCaptureFrame& frame)
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

void LumenGI::createCaptureProgram()
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
        mCapture.pInstanceIDs->setName("LumenGI::Capture::InstanceIDs"); // R32Uint identity map, scene-scoped.

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

uint32_t LumenGI::cacheLightingSamplesPerTexel() const
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

uint32_t LumenGI::buildCacheLightingRenderData()
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

void LumenGI::createCacheLightingProgram()
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

void LumenGI::ensureCacheLightingResources(RenderContext* pRenderContext)
{
    // Visibility/confidence atlas: R16F, atlas-lifetime (kept across scene changes like the
    // capture atlases; contents are re-written every dispatch).
    if (!mCacheLighting.pVisibilityAtlas)
    {
        mCacheLighting.pVisibilityAtlas = mpDevice->createTexture2D(
            mAtlasSizeTexels, mAtlasSizeTexels, ResourceFormat::R16Float, 1, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mCacheLighting.pVisibilityAtlas->setName("LumenGI::CacheLighting::VisibilityAtlas"); // R16F, atlas lifetime.
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
        mCacheLighting.pPageToCard->setName("LumenGI::CacheLighting::PageToCard"); // gLumenPageToCard, uint32, atlas lifetime.
    }
    if (!mCacheLighting.pRenderList || mCacheLighting.pRenderList->getElementCount() != pageCount)
    {
        mCacheLighting.pRenderList = mpDevice->createStructuredBuffer(
            sizeof(uint32_t), pageCount, ResourceBindFlags::ShaderResource
        );
        mCacheLighting.pRenderList->setName("LumenGI::CacheLighting::RenderList"); // gLumenRenderList, uint32, atlas lifetime.
    }

    // Independent cache-lighting counters (see the header comment: separate from the trace
    // counters so the S1/S2 counter statistics stay unchanged). LumenGICounterIndex layout.
    constexpr uint32_t kCounterCount = 4u;
    if (!mpCacheLightingCounters)
    {
        mpCacheLightingCounters = mpDevice->createStructuredBuffer(
            sizeof(uint4), kCounterCount, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpCacheLightingCounters->setName("LumenGI::CacheLighting::Counters");
    }
    if (!mpCacheLightingCountersReadback)
    {
        mpCacheLightingCountersReadback = mpDevice->createStructuredBuffer(
            sizeof(uint4), kCounterCount, ResourceBindFlags::None, MemoryType::ReadBack
        );
        mpCacheLightingCountersReadback->setName("LumenGI::CacheLighting::CountersReadback");
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
                std::string("LumenGI::CacheLighting::Indirect") + (i == 0 ? "A" : "B")
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
        mCacheLighting.pBounceCount->setName("LumenGI::CacheLighting::BounceCount"); // R32Uint, atlas lifetime, S3-B2.
        pRenderContext->clearUAV(mCacheLighting.pBounceCount->getUAV().get(), uint4(0));
    }

    if (!mCacheLighting.pPass)
        createCacheLightingProgram();
}

void LumenGI::runCacheLighting(RenderContext* pRenderContext)
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

void LumenGI::exportCacheDirectRadiance(RenderContext* pRenderContext, const RenderData& renderData)
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
