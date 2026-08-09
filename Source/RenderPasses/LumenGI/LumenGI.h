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
#pragma once

#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Utils/Sampling/SampleGenerator.h"
#include "Rendering/Lights/LightBVHSampler.h"
#include "Rendering/Lights/EnvMapSampler.h"
#include "LumenGIStats.h"
#include "Capture/LumenCaptureScheduler.h" // Brings in Cards/LumenCardScene.h and SurfaceCache/LumenSurfaceCache.h.
#include "ScreenTrace/LumenHZB.h" // S4-A1 HZB chain host component (mip dims / dispatch params).
#include "ScreenProbe/LumenScreenProbe.h" // S4.2 screen probe grid host component (grid math / budget / stats).

using namespace Falcor;

/** Real-time diffuse global illumination pass inspired by the public Lumen architecture.

    The implementation is intentionally modular. Every optional subsystem can be disabled
    independently so that a validated earlier stage remains available as a fallback.
*/
class LumenGI : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(LumenGI, "LumenGI", "Real-time diffuse global illumination.");

    static ref<LumenGI> create(ref<Device> pDevice, const Properties& props) { return make_ref<LumenGI>(pDevice, props); }

    enum class TraceMode : uint32_t
    {
        HardwareRT,
        MeshSDF,
        Hybrid,
    };

    FALCOR_ENUM_INFO(
        TraceMode,
        {
            {TraceMode::HardwareRT, "HardwareRT"},
            {TraceMode::MeshSDF, "MeshSDF"},
            {TraceMode::Hybrid, "Hybrid"},
        }
    );

    enum class QualityPreset : uint32_t
    {
        Low,
        Medium,
        High,
        Reference,
    };

    FALCOR_ENUM_INFO(
        QualityPreset,
        {
            {QualityPreset::Low, "Low"},
            {QualityPreset::Medium, "Medium"},
            {QualityPreset::High, "High"},
            {QualityPreset::Reference, "Reference"},
        }
    );

    enum class DebugMode : uint32_t
    {
        None,
        Normal,
        LinearDepth,
        Motion,
        MaterialID,
        Confidence,
        DirectLighting,
        EnvironmentOnly,
        EmissiveOnly,
        AnalyticOnly,
        IndirectOnly,
        FireflyMask,
        CardsOverlay,
    };

    FALCOR_ENUM_INFO(
        DebugMode,
        {
            {DebugMode::None, "None"},
            {DebugMode::Normal, "Normal"},
            {DebugMode::LinearDepth, "LinearDepth"},
            {DebugMode::Motion, "Motion"},
            {DebugMode::MaterialID, "MaterialID"},
            {DebugMode::Confidence, "Confidence"},
            {DebugMode::DirectLighting, "DirectLighting"},
            {DebugMode::EnvironmentOnly, "EnvironmentOnly"},
            {DebugMode::EmissiveOnly, "EmissiveOnly"},
            {DebugMode::AnalyticOnly, "AnalyticOnly"},
            {DebugMode::IndirectOnly, "IndirectOnly"},
            {DebugMode::FireflyMask, "FireflyMask"},
            {DebugMode::CardsOverlay, "CardsOverlay"},
        }
    );

    LumenGI(ref<Device> pDevice, const Properties& props);

    Properties getProperties() const override;
    RenderPassReflection reflect(const CompileData& compileData) override;
    void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    void renderUI(Gui::Widgets& widget) override;
    void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    void onHotReload(HotReloadFlags reloaded) override;

    ///< Scriptable snapshot of the Surface Cache / Cards capture state for the S2 gate
    ///< (read through the Python binding "surfaceCacheStats"). Values are doubles so the
    ///< map converts losslessly to a Python dict. Keys are documented in the .cpp.
    std::map<std::string, double> getSurfaceCacheStats() const;

private:
    void parseProperties(const Properties& props);
    void resetHistory();
    void clearOutputs(RenderContext* pRenderContext, const RenderData& renderData) const;
    void createDebugPass(const DefineList& defines = {});
    void createTraceProgram();
    void prepareTraceVars();
    void ensureTraceResources();
    void readbackCounters(RenderContext* pRenderContext);

    // ------------------------------------------------------------------------------------------
    // S2: Surface Cache / Cards capture host
    // ------------------------------------------------------------------------------------------
    void invalidateCaptureResources();
    void ensureCaptureResources(RenderContext* pRenderContext);
    void createCaptureProgram();
    void runSurfaceCacheCapture(RenderContext* pRenderContext, IScene::UpdateFlags updateFlags);
    void runCapturePass(RenderContext* pRenderContext, const LumenCaptureFrame& frame);

    // ------------------------------------------------------------------------------------------
    // S3: Surface Cache direct lighting host (S3-B1)
    // ------------------------------------------------------------------------------------------
    void createCacheLightingProgram();
    void ensureCacheLightingResources(RenderContext* pRenderContext);
    void runCacheLighting(RenderContext* pRenderContext);
    void exportCacheDirectRadiance(RenderContext* pRenderContext, const RenderData& renderData);

    ///< Rebuild pageID -> cardIndex and the per-frame lighting render list from the host
    ///< card->page mirror (mCardPageTable/mCardPageGeneration, maintained from the scheduler
    ///< command stream) plus the page cache residency/generation. Returns the render list size.
    uint32_t buildCacheLightingRenderData();

    // ------------------------------------------------------------------------------------------
    // S4: hierarchical screen-space trace host (S4-A1: HZB build + screen trace dispatch)
    // ------------------------------------------------------------------------------------------
    void createHZBBuildProgram();
    void createScreenTraceProgram();
    void ensureScreenTraceResources(RenderContext* pRenderContext);
    void runScreenTrace(RenderContext* pRenderContext, const RenderData& renderData);

    // ------------------------------------------------------------------------------------------
    // S4.2: screen probe grid host (S4-A2/B2: probe placement + direction sampling + screen-first
    // trace + HWRT fallback). Runs after the screen trace (consumes its HZB chain).
    // ------------------------------------------------------------------------------------------
    void createScreenProbePrograms();
    void ensureScreenProbeResources(RenderContext* pRenderContext);
    void runScreenProbeTrace(RenderContext* pRenderContext, const RenderData& renderData);
    void readbackScreenProbeCounters(RenderContext* pRenderContext);

    // ------------------------------------------------------------------------------------------
    // S5: temporal filter host (S5-A1 history double buffer + S5-B1 pass wiring)
    // ------------------------------------------------------------------------------------------
    void createTemporalFilterProgram();
    void ensureTemporalFilterResources(RenderContext* pRenderContext);
    void runTemporalFilter(RenderContext* pRenderContext, const RenderData& renderData);

    ///< gSamplesPerTexel preset mapping: Low/Medium/High/Reference -> 1/2/4/8
    ///< (LumenCacheLightingQuality in LumenSurfaceCacheLightingData.slang).
    uint32_t cacheLightingSamplesPerTexel() const;

    ///< Per-scene card placement (Agent A). Rebuilt on setScene() and on geometry changes.
    std::unique_ptr<LumenCardScene> mpCardScene;

    ///< Tile-atlas page allocator (Agent B). The frame tick (endFrame()) is owned by
    ///< mCaptureScheduler.scheduleFrame(); the host never calls it directly.
    LumenSurfaceCache mPageCache;

    ///< Dirty-card -> page -> capture-command scheduler (Agent H, S2-A2). Re-pointed at
    ///< mpCardScene and mPageCache on setScene().
    LumenCaptureSchedulerForScene mCaptureScheduler;

    ///< Capture pass GPU resources. Created lazily by ensureCaptureResources(), scene-scoped,
    ///< all named. Formats/sizes/lifetimes documented next to their creation in the .cpp.
    struct
    {
        ref<Program> pProgram;       ///< LumenCardCapture.3d.slang (vsMain/psMain) + scene material shader modules.
        ref<ProgramVars> pVars;      ///< Graphics vars with the scene parameter block (gScene) bound.
        ref<GraphicsState> pState;   ///< FBO-less raster state; viewport and VAO are set per capture command.
        ref<Vao> pVao32;             ///< Scene mesh VAO with R32 index buffer format; DRAW_ID = scene geometry instance ID.
        ref<Vao> pVao16;             ///< Same VAO with R16 index buffer format (meshes with 16-bit indices).
        ref<Buffer> pInstanceIDs;    ///< Per-instance identity buffer (element i == i), R32Uint, vertex slot 1.
        ref<Buffer> pCards;          ///< gCards StructuredBuffer<LumenCard> (96 B/card), SRV, full upload per frame.
        ref<Buffer> pPageTable;      ///< cardIndex -> pageID (uint32), SRV, host mirror upload per frame.
        ref<Buffer> pDrawArgs;       ///< Per-command indirect draw arguments (20 B each), IndirectArg bind.
        ref<Texture> pMaterialAtlas; ///< Material atlas, RGBA8 (base color + opacity), UAV + SRV.
        ref<Texture> pRadianceAtlas; ///< Radiance atlas, RGBA16F, UAV + SRV; stale pages cleared by the shader.
        ref<Texture> pMetadataAtlas; ///< Metadata atlas, RGBA16F (depth/flags/normal octahedral), UAV + SRV.
    } mCapture;

    uint32_t mAtlasSizeTexels = kLumenSurfaceCacheDefaultAtlasSize;        ///< Atlas side in texels (normalized to whole tiles).
    uint32_t mCapturePagesPerSide = kLumenSurfaceCacheDefaultPagesPerSide; ///< Tiles per atlas side (bound as gPagesPerSide).
    uint32_t mCaptureMaxPagesPerFrame = kLumenCaptureDefaultMaxPagesPerFrame; ///< Per-frame capture budget in pages.
    std::vector<uint32_t> mCardPageTable; ///< Host mirror cardIndex -> pageID (kLumenCardInvalidID when no page is assigned).
    LumenCaptureFrameStats mLastCaptureFrameStats; ///< Stats of the last scheduleFrame() call, for the UI.

    // ------------------------------------------------------------------------------------------
    // S3: Surface Cache direct lighting host (S3-B1)
    // ------------------------------------------------------------------------------------------
    ///< Cache lighting pass GPU resources. Created lazily by ensureCacheLightingResources();
    ///< pPass is scene-scoped (recreated on setScene/geometry rebuild through
    ///< invalidateCaptureResources()); the buffers and the atlases are atlas-lifetime.
    struct
    {
        ref<ComputePass> pPass;          ///< LumenSurfaceCacheLighting.cs.slang, entry "main".
        ref<Buffer> pPageToCard;         ///< gLumenPageToCard (uint32, pageCount+1), SRV; pageID -> cardIndex.
        ref<Buffer> pRenderList;         ///< gLumenRenderList (uint32, pageCount), SRV; resident pages to light this frame.
        ref<Texture> pVisibilityAtlas;   ///< gLumenVisibilityAtlas (R16F), UAV + SRV; per-texel confidence.
        ///< S3-B2 multi-bounce feedback double buffer (RGBA16F, RGB = indirect radiance).
        ///< Each frame the shader writes gIndirectCurr = pIndirect[indirectCurrIndex] and
        ///< reads gIndirectPrev = pIndirect[1 - indirectCurrIndex]; the host flips the index
        ///< after every dispatch (feedback on or off) so the buffer written this frame becomes
        ///< the previous frame's input next frame.
        ref<Texture> pIndirect[2];
        ref<Texture> pBounceCount;       ///< gBounceCountAtlas (R32Uint), UAV; per-texel bounce cap counter.
        uint32_t indirectCurrIndex = 0;  ///< Double-buffer slot written this frame (flipped after each dispatch).
    } mCacheLighting;

    ///< Host mirror cardIndex -> page generation at the last capture command (size = card
    ///< count). Used to resolve which card currently owns a page when the page->card table is
    ///< rebuilt: only the card whose recorded generation matches the page cache's current page
    ///< generation owns the page (a stale card->page entry has a mismatched generation).
    std::vector<uint32_t> mCardPageGeneration;
    std::vector<uint32_t> mPageToCardData; ///< Host mirror pageID -> cardIndex (pageCount+1), rebuilt per frame.
    std::vector<uint32_t> mRenderListData; ///< Host mirror of the frame's lighting render list.

    ///< Optional cache-lighting GPU counters (LumenGICounterIndex layout). SEPARATE buffer from
    ///< mpLumenGICounters (trace): the trace readback/copy happens earlier in the frame and the
    ///< trace counters must stay S1/S2-identical, so cache lighting cannot share them without
    ///< either double-counting (post-copy writes accumulate into the next frame's trace counts)
    ///< or re-ordering the trace sequence. Independent counters keep both diagnostics clean.
    ref<Buffer> mpCacheLightingCounters;
    ref<Buffer> mpCacheLightingCountersReadback;
    bool mCacheLightingCounterReadbackPending = false;
    LumenGIFrameCounters mCacheLightingCounters; ///< Read back cache-lighting counter values (last completed dispatch).
    uint32_t mLastCacheLightingPageCount = 0;    ///< Pages lit by the last dispatch (0 = none).

    // ------------------------------------------------------------------------------------------
    // S4: hierarchical screen-space trace host (S4-A1: HZB build + screen trace dispatch)
    // ------------------------------------------------------------------------------------------
    ///< Screen-trace pass GPU resources, created lazily by ensureScreenTraceResources().
    ///< pPasses are plain compute (no scene block): the HZB build reads GBufferRT.linearZ,
    ///< the screen trace reads linearZ + the internal HZB chain + the fixed direction texture
    ///< and writes the optional "screenTrace" graph output (RGBA16F).
    struct
    {
        ref<ComputePass> pHZBBuild; ///< LumenHZBBuild.cs.slang, entry "main" (one dispatch per mip).
        ref<ComputePass> pTrace;    ///< LumenScreenTrace.cs.slang, entry "main" (8x8 threads).
        std::vector<ref<Texture>> pHZBMips; ///< Independent R32F textures, one per ceil-halving level (D3D12 mip chains are floor-sized).
        ref<Texture> pRayDirection; ///< RGBA32F view-space ray direction (S4-A1 direction input; see .cpp).
        uint2 resourceDim = {0, 0}; ///< Frame dims the resources were built for; recreated on resize.
    } mScreenTrace;

    // ------------------------------------------------------------------------------------------
    // S4.2: screen probe grid GPU resources (S4-A2/B2, Agent Z1). Created lazily by
    // ensureScreenProbeResources(), frame-dim-scoped (buffers sized to the probe grid; recreated
    // on resize). The passes compile LumenScreenProbeTrace.cs.slang (three entry points).
    // ------------------------------------------------------------------------------------------
    struct
    {
        ref<ComputePass> pUpdate;    ///< LumenScreenProbeTrace.cs.slang, entry "updateMain" (1 thread / probe).
        ref<ComputePass> pTrace;     ///< entry "traceMain" (1 thread / (probe, direction)).
        ref<ComputePass> pFinalize;  ///< entry "finalizeMain" (1 thread / probe).
        ref<ComputePass> pIntegrate; ///< LumenScreenProbeIntegrate.cs.slang, entry "main" (S4.3, 1 thread / probe).
        ref<ComputePass> pInterpolate; ///< LumenScreenProbeInterpolate.cs.slang, entry "main" (S4.3, 8x8 threads).
        ref<Buffer> pMetadata;       ///< gProbeMeta StructuredBuffer<LumenScreenProbe::Meta> (64 B), UAV + SRV.
        ref<Buffer> pHitRecords;     ///< gProbeHitRecords StructuredBuffer<LumenScreenProbe::Hit> (32 B), UAV + SRV.
        ref<Buffer> pCounters;       ///< gProbeCounters StructuredBuffer<LumenScreenProbe::Counters> (32 B), UAV.
        ref<Buffer> pCountersReadback; ///< ReadBack mirror of pCounters for the host stats.
        ref<Texture> pHZBNative;     ///< gHZBMips native floor-halved R32F mip chain (probe march; built per frame).
        ///< S4.3 internal integrated-probe radiance (RGBA16F, full-res, sparse writes at the
        ///< probe tile-center texel): RGB = integrated incident irradiance E, A = confidence.
        ///< Written by pIntegrate, read by pInterpolate. Distinct from the graph "probeRadiance"
        ///< output (Z1's finalize naive average, consumed by run_probe.py).
        ref<Texture> pRadiance;
        uint32_t probeCount = 0;     ///< Probe count the buffers were sized for (0 = not created).
        uint2 resourceDim = {0, 0};  ///< Frame dims the resources were built for.
        bool counterReadbackPending = false;
    } mScreenProbes;

    ///< Scriptable S4.2 probe gate channel: probe grid radiance (RGB avg radiance, A hit
    ///< fraction) at frame resolution, sparse per-probe writes. Exposed for
    ///< tests/lumengi/run_screenprobe.py and the S4-C2 distribution tests (Agent C).
    static constexpr const char* kProbeRadiance = "probeRadiance";

    ///< Scriptable S4.3 interpolate RESULT channel (S5-B1 temporal filter INPUT contract):
    ///< full-res RGBA16F, RGB = incident irradiance E (composite multiplies by albedo/pi),
    ///< A = confidence in [0, 1] (0 = sky / no valid tap). Probed by
    ///< tests/lumengi/run_probe_interp.py (V1/V2) and consumed by the S5-B1 filter.
    static constexpr const char* kProbeInterpolated = "probeInterpolated";

    ///< S4.2 probe configuration. Directions per probe default 16 (fixed hit-record stride
    ///< 32); maxProbesPerFrame default 0 = all probes every frame (updateInterval 1).
    uint32_t mProbeDirectionsPerProbe = LumenScreenProbe::kDefaultDirectionsPerProbe;
    uint32_t mProbeMaxProbesPerFrame = 0u;
    LumenScreenProbe::Stats mScreenProbeStats; ///< Last completed dispatch read-back.

    ///< S4.3 integrate weight mode (gWeightMode in the shared probe CB): 0 = cosine-weighted
    ///< hemisphere (the only Z1-consistent mode; the trace samples a cosine-weighted set).
    uint32_t mProbeIntegrateWeightMode = 0u;

    ///< S4.3 interpolate weight parameters (LumenScreenProbeInterpolateCB). Defaults are the
    ///< Z2 task-spec values: depth dead-zone 0.02 m, depth falloff 4.0 /m, normal exponent 8.0,
    ///< material-mismatch weight 0.05, degraded-sample confidence scale 0.25.
    float mProbeInterpDepthThreshold = 0.02f;
    float mProbeInterpDepthSigmaInv = 4.0f;
    float mProbeInterpNormalExponent = 8.0f;
    float mProbeInterpMaterialMismatchWeight = 0.05f;
    float mProbeInterpFallbackConfidenceScale = 0.25f;

    // ------------------------------------------------------------------------------------------
    // S5: temporal filter host (S5-A1 history double buffer + S5-B1 pass wiring)
    // ------------------------------------------------------------------------------------------
    ///< Scriptable S5 gate channel (temporalFiltered): full-res RGBA16F, RGB = temporally filtered
    ///< incident irradiance E, A = NEW history length (capped). This is the S5 main output; probed
    ///< by tests/lumengi/run_temporal*.py (Z6) and consumed by the S5-B2 spatial filter (Z8).
    static constexpr const char* kTemporalFiltered = "temporalFiltered";
    ///< gTemporalAlpha (R32F): effective EMA alpha (1 = full reject / reset; cross-check channel).
    static constexpr const char* kTemporalAlpha = "temporalAlpha";
    ///< gTemporalConfidence (R32F): updated confidence, the S5-B2 spatial-filter input.
    static constexpr const char* kTemporalConfidence = "temporalConfidence";

    ///< S5-B1 temporal filter GPU resources. Created lazily by ensureTemporalFilterResources(),
    ///< frame-dim-scoped (recreated on resize). The pass compiles LumenTemporalFilter.cs.slang.
    struct
    {
        ref<ComputePass> pFilter;         ///< LumenTemporalFilter.cs.slang, entry "main" (8x8 threads).
        ///< S5-A1 history ping-pong (RGBA16F, .rgb = smoothed irradiance, .a = history length).
        ///< The pass reads gPrevGI from slot [1-historyCurrIndex] while writing gTemporalOutput to
        ///< slot [historyCurrIndex] (the two must be distinct resources); the host flips the index
        ///< after every dispatch so the buffer written this frame is the previous frame's input next.
        ref<Texture> pHistory[2];
        ref<Texture> pPrevDepth;          ///< S5-A1 previous-frame linear depth (R32F, blit of linearZ.x).
        uint32_t historyCurrIndex = 0;    ///< History slot written this frame (flipped after each dispatch).
        uint2 resourceDim = {0, 0};       ///< Frame dims the resources were built for.
        ///< Camera cut / resize / scene-change reset: marks the prev double buffer for a hard clear
        ///< (emitted inside runTemporalFilter where a RenderContext is available). resetHistory()
        ///< only fires on hard invalidations, never on camera-movement-only updates, so smooth
        ///< motion reuses history through the motion-vector reprojection.
        bool historyResetPending = true;
    } mTemporalFilter;

    ///< S5-B1 tuning (LumenTemporalFilterCB; defaults frozen with Z5's LumenTemporalFilterData.slang).
    bool mTemporalClampHistory = false;           ///< gClampHistory: AABB-clamp history to the current 3x3 (TAA anti-ghost; off in the S5 MVP -- see S5 report).
    float mTemporalHistoryAlpha = 0.1f;           ///< gHistoryAlpha: base EMA weight toward the current frame.
    float mTemporalHistoryLengthCap = 255.f;      ///< gHistoryLengthCap: output history length cap (task gate: no overflow).
    float mTemporalDepthThreshold = 0.05f;        ///< gDepthThreshold (m): depthW dead zone below which weight = 1.
    float mTemporalDepthSigmaInv = 8.0f;          ///< gDepthSigmaInv (1/m): depthW exponential falloff beyond the zone.
    float mTemporalDepthRelativeThreshold = 0.05f; ///< gDepthRelativeThreshold: hard reject on relative depth jump.
    float mTemporalMaxRejectAlpha = 1.0f;         ///< gMaxRejectAlpha: blend alpha on disocclusion / soft reject.
    float mMotionLengthThreshold = 0.5f;          ///< gMotionLengthThreshold: hard reject when |mvec| exceeds this (normalized).

    ///< S5-A1 camera-cut detector: when the camera position moved more than this many meters
    ///< between frames (a jump, not a smooth pan/orbit), the history is hard-reset (the S5-B1
    ///< filter alone would still re-use coplanar history that reprojects to a matching depth).
    float mCameraCutDistance = 0.3f;
    float3 mPrevCameraPosition = float3(1e30f); ///< Last frame's camera position (large sentinel = first frame).

    ///< Scriptable S3 gate channel name: exposes the internal radiance atlas (RGB = direct,
    ///< linear) at atlas resolution for tests/lumengi/run_cachelighting.py (Agent N).
    static constexpr const char* kCacheDirectRadiance = "cacheDirectRadiance";

    ref<Scene> mpScene;
    sigs::Connection mUpdateFlagsConnection;
    IScene::UpdateFlags mSceneUpdates = IScene::UpdateFlags::None;
    bool mLightCollectionInitialized = false;

    ///< Emissive light BVH sampler for secondary-hit NEE; scene-scoped (rebuilt on setScene).
    std::unique_ptr<LightBVHSampler> mpEmissiveLightSampler;

    ///< EnvMap importance sampler for the Surface Cache lighting NEE (S3-B1); scene-scoped.
    ///< Created lazily when the scene has an env map, mirroring PathTracer.cpp (EnvMapSampler
    ///< needs only a Device + EnvMap; the importance map is built internally on construction).
    std::unique_ptr<EnvMapSampler> mpEnvMapSampler;

    ref<ComputePass> mpDebugPass;
    ref<SampleGenerator> mpSampleGenerator;

    struct
    {
        ref<Program> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;
    } mTracer;

    /// Optional GPU lighting counters (cleared before each trace dispatch).
    ref<Buffer> mpLumenGICounters;
    ref<Buffer> mpLumenGICountersReadback;
    bool mCounterReadbackPending = false;
    LumenGIFrameCounters mCounters;
    uint64_t mCaptureStatsLogCounter = 0;

    /// Optional per-pixel lighting components written by the trace shader.
    ref<Texture> mpLightingComponents;

    uint2 mFrameDim = {0, 0};
    uint32_t mFrameIndex = 0;
    bool mOptionsChanged = false;
    bool mEnabled = true;

    TraceMode mTraceMode = TraceMode::HardwareRT;
    QualityPreset mQualityPreset = QualityPreset::High;
    DebugMode mDebugMode = DebugMode::None;
    bool mUseSurfaceCache = false;
    bool mUseCacheLighting = false;

    // ------------------------------------------------------------------------------------------
    // S3-B2: multi-bounce feedback configuration (mirrored into the
    // LumenSurfaceCacheLightingCB feedback fields each dispatch; see the shader
    // module header for the frozen recurrence and the LumenGI.cpp CB mirror).
    // ------------------------------------------------------------------------------------------
    bool mCacheLightingFeedbackEnabled = false;  ///< cacheLightingFeedback (default off -> single bounce).
    float mCacheLightingFeedbackStrength = 1.0f; ///< cacheLightingFeedbackStrength (default 1.0).
    uint32_t mCacheLightingFeedbackMaxBounces = 4u; ///< cacheLightingFeedbackMaxBounces (default 4).

    bool mUseScreenTrace = false;
    bool mUseScreenProbes = false;
    bool mUseTemporalFilter = false;
    bool mUseSpatialFilter = false;
    bool mUseRadianceCache = false;
};

FALCOR_ENUM_REGISTER(LumenGI::TraceMode);
FALCOR_ENUM_REGISTER(LumenGI::QualityPreset);
FALCOR_ENUM_REGISTER(LumenGI::DebugMode);
