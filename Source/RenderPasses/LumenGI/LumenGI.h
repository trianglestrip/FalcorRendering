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
#include "Spatial/LumenReconstruction.h" // S5-A2 reconstruction host component (full/half/quarter resolution, upscale, spatial-filter CB mirror).
#include "MeshSDF/LumenMeshSDFScene.h"       // S6-A/A2: scene -> cache -> builder -> volume -> atlas -> instance table (CPU, header-only).
#include "MeshSDF/LumenGlobalDistanceField.h" // S6-A3: camera-centered GDF clipmap (CPU, header-only).
#include "RadianceCache/LumenRadianceCache.h" // C10 CPU ownership/usage-feedback contract.

#include <unordered_map>
#include <unordered_set>

using namespace Falcor;

/** Real-time diffuse global illumination pass inspired by the public Lumen architecture.

    The implementation is intentionally modular. Every optional subsystem can be disabled
    independently so that a validated earlier stage remains available as a fallback.
*/
class LumenGIPass : public RenderPass
{public:
    FALCOR_PLUGIN_CLASS(LumenGIPass, "LumenGI", "Real-time diffuse global illumination.");

    static ref<LumenGIPass> create(ref<Device> pDevice, const Properties& props) { return make_ref<LumenGIPass>(pDevice, props); }

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

    LumenGIPass(ref<Device> pDevice, const Properties& props);

    void setProperties(const Properties& props) override;
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

    ///< Scriptable S6 gate snapshot (read through the Python binding "gdfStats"): Mesh SDF
    ///< scene pipeline counters + GDF clipmap parameters + the last sphere-trace dispatch
    ///< counters. Values are doubles so the map converts losslessly to a Python dict.
    std::map<std::string, double> getGDFStats() const;

    ///< Scriptable S4/C2/C7 probe-resource and counter snapshot (screenProbeStats).
    std::map<std::string, double> getScreenProbeStats() const;

    ///< Scriptable C10 preparation snapshot. This reports the CPU clipmap scheduler
    ///< only; GPU producer/interpolation fields remain zero until C10 resources exist.
    std::map<std::string, double> getRadianceCacheStats() const;

    ///< Scriptable C11 snapshot of the currently effective quality preset and derived values.
    std::map<std::string, double> getQualityPresetStats() const;

    ///< Card-specific Surface Cache request lifecycle events.  Unlike the aggregate
    ///< surfaceCacheStats counters, each record keeps the request/capture/ready/first-hit
    ///< frame relation and the page generation used for the validation.
    std::vector<std::map<std::string, double>> getSurfaceCacheEvents() const;

private:
    void parseProperties(const Properties& props, bool applyPresetDefaults = false);
    enum class HistoryResetReason : uint32_t
    {
        Unknown = 0,
        Resize = 1,
        SceneChange = 2,
        CameraCut = 3,
        SetScene = 4,
        HotReload = 5,
    };

    void resetHistory(HistoryResetReason reason = HistoryResetReason::Unknown);
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
    void exportCacheCaptureRadiance(RenderContext* pRenderContext, const RenderData& renderData);

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

    // ------------------------------------------------------------------------------------------
    // S5: spatial filter host (S5-B2 pass wiring + S5-A2 reconstruction CB)
    // ------------------------------------------------------------------------------------------
    void createSpatialFilterProgram();
    void ensureSpatialFilterResources(RenderContext* pRenderContext);
    void runSpatialFilter(RenderContext* pRenderContext, const RenderData& renderData);
    void runFinalResolve(RenderContext* pRenderContext, const RenderData& renderData);

    // ------------------------------------------------------------------------------------------
    // C10: bounded GPU Radiance Cache seed/interpolate path. This first producer is intentionally
    // source-backed (current HWRT diffuse radiance) while the full UE-style scene-ray request
    // tracer is added in the next wave. It still owns explicit generation/ready fencing and never
    // substitutes CPU bookkeeping for a GPU output.
    // ------------------------------------------------------------------------------------------
    void createRadianceCachePrograms();
    void ensureRadianceCacheResources(RenderContext* pRenderContext);
    void runRadianceCache(RenderContext* pRenderContext, const RenderData& renderData);

    // E1 diagnostics. These passes are graph-visible but hard-disabled until
    // directional rough-specular and medium-aware transmission producers own
    // their inputs; they never alias diffuseGI or finalColor.
    void runRoughSpecularDiagnostic(RenderContext* pRenderContext, const RenderData& renderData);
    void runTransmissionDiagnostic(RenderContext* pRenderContext, const RenderData& renderData);

    // ------------------------------------------------------------------------------------------
    // S6: Mesh SDF + Global Distance Field host (S6-A data pipeline, S6-B3 compose, S6-B4 sphere
    // trace). The CPU components (LumenMeshSDFScene + LumenGlobalDistanceField) are header-only
    // and std-lib; this pass owns all GPU resources and dispatches the two S6 shaders
    // (LumenGDFCompose.cs.slang, LumenGDFTrace.cs.slang). See the S6 section comment in the .cpp.
    // ------------------------------------------------------------------------------------------
    void invalidateMeshSDF();                     ///< Drop the CPU scene + GPU resources (scene change / teardown).
    void ensureMeshSDFScene();                    ///< (Re)create the CPU scene and register the scene's static instances.
    void ensureGDFResources(RenderContext* pRenderContext); ///< Create GDF clipmap textures + buffers + passes.
    void rebuildMeshSDFAtlasImages();             ///< Re-tile the CPU atlas page data into the host fine/coarse images.
    void uploadMeshSDFAtlas(RenderContext* pRenderContext); ///< Upload the host atlas images to the GPU textures.
    void uploadGDFHostData(RenderContext* pRenderContext, bool forceFullCompose); ///< Level table / instances / dirty regions / CB.
    void runGDFCompose(RenderContext* pRenderContext);      ///< Dispatch LumenGDFCompose.cs.slang over the dirty regions.
    void runGDFSphereTrace(RenderContext* pRenderContext, const RenderData& renderData); ///< Dispatch LumenGDFTrace.cs.slang.
    void readbackGDFTraceStats(RenderContext* pRenderContext);

    ///< S6-A per-mesh volume resolution (voxel count along the longest grid axis).
    uint32_t meshSDFResolution() const { return std::max<uint32_t>(mMeshSDFResolution, 8u); }
    ///< S6-A per-mesh volume quality (LumenGI::MeshSDF::Quality enum value: 0 = High, 1 = Low).
    LumenGI::MeshSDF::Quality meshSDFQuality() const
    {
        return mMeshSDFQuality == 0 ? LumenGI::MeshSDF::Quality::High : LumenGI::MeshSDF::Quality::Low;
    }

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
        ref<Buffer> pPageGeneration; ///< cardIndex -> capture generation (uint32), SRV, stale-owner guard.
        ref<Buffer> pDrawArgs;       ///< Per-command indirect draw arguments (20 B each), IndirectArg bind.
        ref<ComputePass> pPageClear; ///< Page-local clear before raster capture (capture-owned atlases).
        ref<Buffer> pPageIDs;        ///< Unique page IDs for the page-local clear dispatch.
        ref<Texture> pMaterialAtlas; ///< Material atlas, RGBA8 (base color + opacity), UAV + SRV.
        ref<Texture> pRadianceAtlas; ///< Radiance atlas, RGBA16F, UAV + SRV; stale pages cleared by the shader.
        ref<Texture> pMetadataAtlas; ///< Metadata atlas, RGBA16F (depth/flags/normal octahedral), UAV + SRV.
        ref<Texture> pCaptureOrderAtlas; ///< Deterministic raster winner key, R32Uint, UAV + SRV.
    } mCapture;

    uint32_t mAtlasSizeTexels = kLumenSurfaceCacheDefaultAtlasSize;        ///< Atlas side in texels (normalized to whole tiles).
    uint32_t mCapturePagesPerSide = kLumenSurfaceCacheDefaultPagesPerSide; ///< Tiles per atlas side (bound as gPagesPerSide).
    uint32_t mCaptureMaxPagesPerFrame = kLumenCaptureDefaultMaxPagesPerFrame; ///< Per-frame capture budget in pages.
    uint32_t mSurfaceCachePageClearCommands = 0; ///< Unique pages cleared before the last capture batch.
    uint64_t mSurfaceCachePageClearTexels = 0; ///< Capture atlas texels cleared before the last batch.
    uint64_t mSurfaceCachePageClearCommandsTotal = 0; ///< Capture-page clears in the current scene/history epoch.
    uint64_t mSurfaceCachePageClearTexelsTotal = 0; ///< Cleared capture texels in the current scene/history epoch.
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
        ref<Buffer> pPageMetadata;      ///< pageID -> {allocator generation, page state, ready frame, reserved}, SRV.
        ref<Buffer> pCardGrid;           ///< world-space cell -> bounded card candidate IDs, SRV.
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
        bool envSamplerVariant = false;  ///< Root-signature shape used by pPass; rebuild on toggle.
    } mCacheLighting;

    ///< Host mirror cardIndex -> page generation at the last capture command (size = card
    ///< count). Used to resolve which card currently owns a page when the page->card table is
    ///< rebuilt: only the card whose recorded generation matches the page cache's current page
    ///< generation owns the page (a stale card->page entry has a mismatched generation).
    std::vector<uint32_t> mCardPageGeneration;
    std::vector<uint32_t> mPageToCardData; ///< Host mirror pageID -> cardIndex (pageCount+1), rebuilt per frame.
    std::vector<uint4> mPageMetadataData; ///< Host mirror pageID -> {generation, state, first valid frame, reserved}.
    std::vector<uint32_t> mCardGridData; ///< Host mirror: [cell count|overflow, bounded card IDs...].
    float3 mCardGridMin = float3(0.f); ///< World-space grid origin for the current card set.
    float3 mCardGridInvCellSize = float3(1.f); ///< Per-axis inverse cell size.
    uint32_t mCardGridOverflowCells = 0; ///< Cells that require the correctness full-scan fallback.
    uint32_t mCardGridCandidateCount = 0; ///< Number of card IDs stored in bounded cells.
    uint32_t mCardGridCardsIndexed = 0; ///< Active card IDs included, including non-resident pages.
    std::vector<uint32_t> mRenderListData; ///< Host mirror of the frame's lighting render list.
    uint32_t mSurfaceCacheGenerationRejects = 0; ///< Stale card/page ownership entries rejected this frame.
    uint32_t mSurfaceCacheStateRejects = 0; ///< Allocated pages in a non-readable lifecycle state rejected this frame.
    uint32_t mSurfaceCacheStaleOwnerRejects = 0; ///< Card/page generation mismatches rejected this frame.
    // C6.1 GPU demand feedback, accumulated from validated probe cache hits.
    uint64_t mSurfaceCacheFeedbackHits = 0;
    uint64_t mSurfaceCacheFeedbackPages = 0;
    uint64_t mSurfaceCacheFeedbackDedup = 0;
    uint64_t mSurfaceCacheFeedbackStaleRejects = 0;
    // C6.2 per-card miss/request telemetry. Requests are submitted only for a card whose
    // geometry covers a probe hit but whose page/metadata is unavailable or stale.
    uint64_t mSurfaceCacheRequestRaw = 0;
    uint64_t mSurfaceCacheRequestCards = 0;
    uint64_t mSurfaceCacheRequestDedup = 0;
    uint64_t mSurfaceCacheRequestStaleRejects = 0;
    uint64_t mSurfaceCacheRequestCaptureCompleted = 0;
    // Explicit frame provenance for the strict C6 request -> next-frame
    // publication contract.  The counters above are cumulative; these frame
    // stamps prevent a readback/scheduler handoff from being misread as a
    // same-frame GPU publication.
    uint32_t mSurfaceCacheRequestObservedFrame = 0;
    uint32_t mSurfaceCacheRequestCaptureFrame = 0;
    // Surface Cache owns a monotonic scheduler clock. It must not reuse
    // mFrameIndex because history resets intentionally rewind that counter.
    uint32_t mSurfaceCacheFrameIndex = 0;
    // OR-ed reason bits from the GPU per-card request ring. Keep these as
    // separate raw-request counters so C6 priority/coverage diagnostics do
    // not infer a cause from aggregate requestCards alone.
    uint64_t mSurfaceCacheRequestUnmapped = 0;
    uint64_t mSurfaceCacheRequestStaleOwner = 0;
    uint64_t mSurfaceCacheRequestMetadataInvalid = 0;
    uint64_t mSurfaceCacheRequestVisibilityInvalid = 0;
    // Per-host-frame event counters. Unlike the cumulative counters above these are
    // reset at execute() entry and are used to prove request -> capture -> ready
    // ordering without reconstructing events from asynchronous readback deltas.
    uint64_t mSurfaceCacheRequestRawThisFrame = 0;
    uint64_t mSurfaceCacheRequestCardsThisFrame = 0;
    uint64_t mSurfaceCacheRequestCaptureCompletedThisFrame = 0;
    uint32_t mSurfaceCachePageMetadataPendingThisFrame = 0;
    uint32_t mSurfaceCachePageMetadataReadyThisFrame = 0;
    std::unordered_set<uint32_t> mSurfaceCachePendingReadyPages;
    std::unordered_set<uint32_t> mSurfaceCacheRequestedCards;
    // GPU miss requests are observed from the previous integrate dispatch. Keep
    // them for one host frame before enqueueing so publication cannot occur on
    // the same frame as request readback.
    std::unordered_set<uint32_t> mSurfaceCacheDeferredRequestCards;
    std::unordered_map<uint32_t, uint32_t> mSurfaceCacheDeferredRequestFrameByCard;
    uint32_t mSurfaceCacheDeferredRequestFrame = 0;

    struct SurfaceCacheRequestEvent
    {
        uint64_t sequence = 0;
        uint32_t sceneGeneration = 0;
        uint32_t cardIndex = kLumenCardInvalidID;
        uint32_t pageID = kInvalidPageID;
        uint32_t generation = 0;
        uint32_t requestFrame = 0;
        uint32_t captureFrame = 0;
        uint32_t readyFrame = 0;
        uint32_t firstHitFrame = 0;
        uint32_t reasonBits = 0;
        uint32_t requestCount = 0;
        uint32_t lookupHits = 0;
        uint32_t state = 0; ///< 1=requested, 2=captured, 3=ready, 4=hit, 5=stale.
    };
    // Telemetry-only event ledger capacity. This does not change scheduler admission, capture
    // budget, residency, or any correctness threshold; it prevents long tiny-atlas pressure
    // runs from losing identity records before the strict validator can drain them.
    static constexpr size_t kMaxSurfaceCacheRequestEvents = 65536u;
    std::vector<SurfaceCacheRequestEvent> mSurfaceCacheRequestEvents;
    uint64_t mSurfaceCacheRequestEventSequence = 0;
    uint64_t mSurfaceCacheRequestEventDropped = 0;

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
        ref<ComputePass> pScreenRadianceHistoryPass; ///< UE-style per-pixel radiance history update.
        ref<ComputePass> pIntegrate; ///< LumenScreenProbeIntegrate.cs.slang, entry "main" (S4.3, 1 thread / probe).
        ref<ComputePass> pInterpolate; ///< LumenScreenProbeInterpolate.cs.slang, entry "main" (S4.3, 8x8 threads).
        ref<Buffer> pMetadata;       ///< gProbeMeta StructuredBuffer<LumenScreenProbe::Meta> (64 B), UAV + SRV.
        ref<Buffer> pHitRecords;     ///< gProbeHitRecords StructuredBuffer<LumenScreenProbe::Hit> (32 B), UAV + SRV.
        ref<Buffer> pCounters;       ///< gProbeCounters StructuredBuffer<LumenScreenProbe::Counters> (96 B), UAV.
        ref<Buffer> pCountersReadback; ///< ReadBack mirror of pCounters for the host stats.
        ///< Optional C6.1 per-page demand feedback: uint2{hit count, page generation}.
        ref<Buffer> pCacheFeedback;
        ref<Buffer> pCacheFeedbackReadback;
        uint32_t cacheFeedbackPageCount = 0;
        uint32_t cacheFeedbackSceneGeneration = 0;
        // Surface Cache scheduler frame at which the GPU feedback UAV was
        // submitted. Readback is consumed one host frame later; event
        // firstHitFrame must retain dispatch provenance rather than the
        // current consumer frame.
        uint32_t cacheFeedbackSubmittedFrame = 0;
        bool cacheFeedbackReadbackPending = false;
        ///< Optional C6.2 per-card miss requests: uint2{raw request count, miss reason bits}.
        ref<Buffer> pCacheRequests;
        ref<Buffer> pCacheRequestsReadback;
        uint32_t cacheRequestCardCount = 0;
        uint32_t cacheRequestSceneGeneration = 0;
        bool cacheRequestReadbackPending = false;
        ref<Texture> pHZBNative;     ///< gHZBMips native floor-halved R32F mip chain (probe march; built per frame).
        ///< S4.3 internal integrated-probe radiance (RGBA16F, full-res, sparse writes at the
        ///< probe tile-center texel): RGB = integrated incident irradiance E, A = confidence.
        ///< Written by pIntegrate, read by pInterpolate. Distinct from the graph "probeRadiance"
        ///< output (Z1's finalize naive average, consumed by run_probe.py).
        ref<Texture> pRadiance;
        ///< C8 internal full-resolution interpolated incident irradiance. The graph
        ///< probeInterpolated output is only an optional mirror of this resource.
        ref<Texture> pInterpolated;
        ///< C7 cross-frame accumulated probe estimate. RGB stores the running mean incident
        ///< irradiance and A stores the accumulated traced-direction count (up to RGBA16F max).
        ref<Texture> pRadianceHistory;
        ///< Full-resolution ping-pong raw screen-radiance history. RGB is the
        ///< unmodulated HWRT radiance mean; A is the secondary hit distance.
        ref<Texture> pScreenRadianceHistory[2];
        ///< Full-resolution ping-pong linear-depth history used for reprojection rejection.
        ref<Texture> pScreenRadianceDepthHistory[2];
        ///< Full-resolution ping-pong normal/material guide history used to reject
        ///< coplanar-but-different surfaces in the screen-radiance producer.
        ref<Texture> pScreenRadianceGuideHistory[2];
        ///< Full-resolution ping-pong source luminance moments. RG stores mean and mean-square;
        ///< this is distinct from downstream probe-irradiance moments.
        ref<Texture> pScreenRadianceMoments[2];
        ///< Full-resolution ping-pong lighting-generation fence. R32Uint stores the
        ///< mLightingGeneration epoch associated with each raw radiance sample.
        ref<Texture> pScreenRadianceLightingGeneration[2];
        ///< Full-resolution ping-pong raw-radiance history age. R32Uint stores the number
        ///< of consecutive accepted reprojections (0 means invalid/reset). This is separate
        ///< from RGBA16F alpha, which remains the secondary hit distance.
        ref<Texture> pScreenRadianceAge[2];
        ///< Full-resolution ping-pong per-pixel history validity sidecar. R32Uint: 1 means
        ///< the corresponding RGB/hit-distance sample is valid for reprojection; 0 means
        ///< invalid/reset. This is deliberately separate from hit-distance alpha and age.
        ref<Texture> pScreenRadianceValidity[2];
        uint32_t screenRadianceHistoryCurrIndex = 0;
        uint32_t probeCount = 0;     ///< Probe count the buffers were sized for (0 = not created).
        uint2 resourceDim = {0, 0};  ///< Frame dims the resources were built for.
        bool counterReadbackPending = false;
        bool historyResetPending = true;
        bool producedThisFrame = false; ///< Interpolate completed for the current frame.
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

    ///< C7 diagnostic mirror of the internal cross-frame probe history. RGB is the running
    ///< incident-irradiance mean and A is the accumulated traced-direction count. This channel
    ///< is never consumed by the production resolve; it exists for the 8/32/96-frame gate.
    static constexpr const char* kProbeHistory = "probeHistory";

    ///< S4.2 probe configuration. Directions per probe default 16 (fixed hit-record stride
    ///< 32); maxProbesPerFrame default 0 = all probes every frame (updateInterval 1).
    uint32_t mProbeDirectionsPerProbe = LumenScreenProbe::kDefaultDirectionsPerProbe;
    uint32_t mProbeMaxProbesPerFrame = 0u;
    LumenScreenProbe::Stats mScreenProbeStats; ///< Last completed dispatch read-back.
    // Host frame associated with mScreenProbeStats.  This prevents consumers
    // from treating the cumulative lookup counters as current-request events.
    uint32_t mScreenProbeStatsFrame = 0u;
    // Scheduler-frame stamp attached when the GPU counter readback is submitted.
    // The readback is consumed one execute later, so using mFrameIndex at consume
    // time would misattribute cache lookup events across history resets.
    uint32_t mScreenProbeCountersSubmittedFrame = 0u;

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
    ///< C8 diagnostic running luminance moments (.x = mean, .y = mean square).
    static constexpr const char* kTemporalMoments = "temporalMoments";
    ///< Scriptable S5-B2 gate channel (spatialFiltered): full-res RGBA16F, RGB = variance-guided
    ///< spatially filtered incident irradiance, A = FILTERED confidence in [0,1] (carried from
    ///< gConfidenceInput / the S5-B1 temporalConfidence, blended by the pass). This is the S5
    ///< final filtered output; probed by tests/lumengi/run_spatial_gate.py / run_spatial_ghost.py.
    static constexpr const char* kSpatialFiltered = "spatialFiltered";
    ///< C8 diagnostic combined variance written by the spatial filter (R32F).
    static constexpr const char* kFilteredVariance = "filteredVariance";

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
        ///< C8 running luminance moments (RG32F: mean and mean square), updated alongside
        ///< the temporal history so the spatial filter can use a true temporal variance.
        ref<Texture> pMoments;
        ref<Texture> pPrevDepth;          ///< S5-A1 previous-frame linear depth (R32F, blit of linearZ.x).
        ref<Texture> pPrevNormal;         ///< UE-style previous-frame packed normal/material (RGBA16F blit).
        uint32_t historyCurrIndex = 0;    ///< History slot written this frame (flipped after each dispatch).
        uint2 resourceDim = {0, 0};       ///< Frame dims the resources were built for.
        ///< Camera cut / resize / scene-change reset: marks the prev double buffer for a hard clear
        ///< (emitted inside runTemporalFilter where a RenderContext is available). resetHistory()
        ///< only fires on hard invalidations, never on camera-movement-only updates, so smooth
        ///< motion reuses history through the motion-vector reprojection.
        bool historyResetPending = true;
        bool producedThisFrame = false;   ///< Temporal dispatch completed for the current frame.
    } mTemporalFilter;

    ///< S5-B1 tuning (LumenTemporalFilterCB; defaults frozen with Z5's LumenTemporalFilterData.slang).
    bool mTemporalClampHistory = true;            ///< gClampHistory: UE-style AABB clamp history to the current 3x3 neighborhood.
    float mTemporalHistoryAlpha = 0.1f;           ///< gHistoryAlpha: base EMA weight toward the current frame.
    float mTemporalHistoryLengthCap = 10.f;       ///< gHistoryLengthCap: UE-style MaxFramesAccumulated cap.
    float mTemporalDepthThreshold = 0.05f;        ///< gDepthThreshold (m): depthW dead zone below which weight = 1.
    float mTemporalDepthSigmaInv = 8.0f;          ///< gDepthSigmaInv (1/m): depthW exponential falloff beyond the zone.
    float mTemporalDepthRelativeThreshold = 0.05f; ///< gDepthRelativeThreshold: hard reject on relative depth jump.
    float mTemporalMaxRejectAlpha = 1.0f;         ///< gMaxRejectAlpha: blend alpha on disocclusion / soft reject.
    float mMotionLengthThreshold = 0.5f;          ///< gMotionLengthThreshold: hard reject when |mvec| exceeds this (normalized).

    // ------------------------------------------------------------------------------------------
    // S5: spatial filter host (S5-B2 pass wiring). The pass is a pure GPU compute filter: all
    // resources come from the graph (gGIInput = temporalFiltered, gConfidenceInput =
    // temporalConfidence, gLinearZ / gNormalRoughnessMaterialID from the GBuffer, gFilteredOutput
    // = the "spatialFiltered" graph channel), so the host owns only the ComputePass. It runs at
    // full frame resolution in the S5 MVP; half/quarter GI (S5-A2, LumenReconstruction) is an S8
    // quality-preset item (the CB is still built through LumenReconstruction::makeSpatialFilterCB).
    // ------------------------------------------------------------------------------------------
    struct
    {
        ///< LumenSpatialFilter.cs.slang, entry "main" (8x8 threads, variance-guided bilateral).
        ref<ComputePass> pFilter;
        ref<Texture> pOutput; ///< C8 internal spatial result; graph output is an optional mirror.
        ref<Texture> pScratch; ///< Ping-pong target for the UE-style multi-pass spatial filter.
        ref<Texture> pVariance; ///< C8 internal combined variance; graph output is an optional mirror.
        uint2 resourceDim = {0, 0};
        bool producedThisFrame = false; ///< Spatial dispatch completed for the current frame.
    } mSpatialFilter;

    // C9: production Final Resolve. Probe/temporal/spatial stages store incident
    // irradiance, while the public diffuseGI contract is modulated linear diffuse
    // radiance. Resolve is written into an internal texture first so the source and
    // destination may legally alias the graph's diffuseGI output.
    struct
    {
        ref<ComputePass> pPass;
        ref<Texture> pResolved;
        uint2 resourceDim = {0, 0};
    } mFinalResolve;

    ///< S5-B2 tuning (LumenSpatialFilterCB; defaults frozen with LumenSpatialFilterData.slang and
    ///< mirrored by LumenReconstruction::SpatialFilterConstantBuffer). Every field below is set on
    ///< the CB each dispatch; values match the frozen shader defaults so the pass is deterministic
    ///< and the radius / threshold / variance fields can be retuned per preset (S8).
    // The probe interpolation grid is currently 8x8 pixels. Keep a non-zero
    // bilateral floor and allow the adaptive pass to cover one probe cell;
    // depth/normal/material gates still prevent cross-surface bleeding.
    float mSpatialRadiusMin = 2.0f;               ///< gRadiusMin: UE-style realtime denoise floor (pixels).
    float mSpatialRadiusMax = 4.0f;               ///< gRadiusMax: adaptive radius ceiling (pixels).
    float mSpatialVarianceThresholdLow = 0.0f;   ///< gVarianceThresholdLow: rel-var below which radius = gRadiusMin.
    float mSpatialVarianceThresholdHigh = 0.20f; ///< gVarianceThresholdHigh: rel-var above which radius = gRadiusMax.
    bool mSpatialFireflyClamp = true;             ///< gFireflyClamp: firefly replace + clamp.
    uint32_t mSpatialNeighborhoodRadius = 2u;     ///< gNeighborhoodRadius: variance window radius (1 = 3x3, 2 = 5x5).
    float mSpatialTemporalVarianceWeight = 1.0f;  ///< gTemporalVarianceWeight: scale on the S5-A1 temporal variance (0 = spatial only).
    float mSpatialDepthThreshold = 0.05f;         ///< gDepthThreshold (m): depthW dead zone.
    float mSpatialDepthSigmaInv = 8.0f;           ///< gDepthSigmaInv (1/m): depthW falloff beyond the zone.
    float mSpatialNormalExponent = 8.0f;          ///< gNormalExponent: pow(saturate(dot)) on the normal affinity.
    float mSpatialMaterialMismatchWeight = 0.05f; ///< gMaterialMismatchWeight: material-ID mismatch residual.

    ///< S5-A1 camera-cut detector: when the camera position moved more than this many meters
    ///< between frames (a jump, not a smooth pan/orbit), the history is hard-reset (the S5-B1
    ///< filter alone would still re-use coplanar history that reprojects to a matching depth).
    float mCameraCutDistance = 0.3f;
    float3 mPrevCameraPosition = float3(1e30f); ///< Last frame's camera position (large sentinel = first frame).

    ///< Scriptable S3 gate channel name: exposes the internal radiance atlas (RGB = direct,
    ///< linear) at atlas resolution for tests/lumengi/run_cachelighting.py (Agent N).
    static constexpr const char* kCacheDirectRadiance = "cacheDirectRadiance";
    ///< Diagnostic snapshot of the capture-owned radiance atlas before S3 cache lighting.
    static constexpr const char* kCacheCaptureRadiance = "cacheCaptureRadiance";

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
    uint64_t mHistoryGeneration = 0;
    uint64_t mLightingGeneration = 1;
    // Surface-cache page generations are allocator-local and intentionally reset when a
    // scene is reloaded. Keep a separate monotonic scene epoch so reload invalidation is
    // observable without reusing stale page IDs/generations across scenes.
    uint32_t mSurfaceCacheSceneGeneration = 0;
    uint32_t mSurfaceCacheResetCount = 0;
    uint32_t mHistoryResetCount = 0;
    HistoryResetReason mLastHistoryResetReason = HistoryResetReason::Unknown;
    bool mHistoryResetThisFrame = false;
    bool mOptionsChanged = false;
    bool mEnabled = true;

    TraceMode mTraceMode = TraceMode::HardwareRT;
    QualityPreset mQualityPreset = QualityPreset::High;
    DebugMode mDebugMode = DebugMode::None;
    bool mUseSurfaceCache = false;
    bool mUseCacheLighting = false;
    bool mUseCacheCardGrid = false; ///< Experimental candidate grid; keep full scan as the production default until A/B equivalence closes.

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
    bool mUseScreenRadianceMoments = true;
    bool mUseSpatialFilter = false;
    bool mUseRadianceCache = false;
    std::unique_ptr<LumenRadianceCache> mRadianceCache;
    // Scene/hot-reload boundaries can occur before execute() clears the per-frame
    // history flag. Keep an explicit RC reset request so the GPU epoch, payload
    // ping-pong buffers, and query readback counters cannot cross that boundary.
    bool mRadianceCacheResetPending = false;

    struct
    {
        ref<ComputePass> pBuild;       ///< C10 source-backed probe payload producer.
        ref<ComputePass> pInterpolate; ///< C10 generation/ready-checked pixel output.
        ref<Buffer> pProbeMeta[2];     ///< uint4 {world-position index, generation, readyFrame, valid}, ping-pong fence.
        ref<Buffer> pProbeWorldPos;    ///< float4 world position per slot.
        ref<Buffer> pProbeScreenPos;   ///< float4 projected pixel x/y, view depth, valid.
        ref<Buffer> pProbeRadiance[2]; ///< float4 RGB radiance + hit distance, ping-pong payload.
        ref<Buffer> pProbeValidity[2]; ///< uint hit/sky/radiance/producer validity bitmask, ping-pong.
        ref<Buffer> pQueryCounters;     ///< uint4 {attempts, valid hits, invalid/miss, reserved} UAV.
        ref<Buffer> pQueryCountersReadback; ///< Previous dispatch query counters.
        ref<Buffer> pLevelQueryCounters; ///< uint[levels * 8] per-level query/sample counters.
        ref<Buffer> pLevelQueryCountersReadback; ///< Previous dispatch per-level counters.
        ref<Texture> pIndirection;     ///< R32Uint compact slot indirection (diagnostic/ABI fence).
        ref<Texture> pOutput;           ///< Internal full-resolution cache output.
        ref<Texture> pHitDistOutput;    ///< Internal full-resolution cache hit-distance output.
        ref<Texture> pValidityOutput;   ///< Internal full-resolution hit/sky validity bitmask.
        uint32_t slotCapacity = 0;
        uint32_t levelCounterCount = 0;
        uint32_t currIndex = 0;
        uint32_t generation = 1;
        uint32_t lastReadyFrame = 0;
        uint64_t traceCount = 0;
        uint64_t probeRayCount = 0;
        uint32_t probeDirectionCount = 8;
        uint64_t requestCount = 0; ///< Allocated probe requests submitted to the bounded producer.
        uint64_t rayCount = 0;     ///< Deterministic probe rays submitted (requestCount * directions).
        uint64_t commitCount = 0;
        uint64_t readyCount = 0;   ///< Probe payload slots published behind the next-frame fence.
        uint64_t staleWriteRejects = 0;
        uint64_t queryAttempts = 0; ///< Pixels dispatched to interpolation; valid hits require a GPU readback counter.
        uint64_t queryHits = 0;
        uint64_t queryMisses = 0;
        uint32_t queryCountersSubmittedFrame = 0; ///< Cache clock of the dispatch copied to readback.
        uint32_t queryCountersFrame = 0;
        bool queryCountersReadbackPending = false;
        uint32_t levelQueryCountersSubmittedFrame = 0; ///< Cache clock of the per-level dispatch.
        uint32_t levelQueryCountersFrame = 0;
        bool levelQueryCountersReadbackPending = false;
        std::array<uint64_t, 8> levelQueryAttempts = {};
        std::array<uint64_t, 8> levelQueryHits = {};
        std::array<uint64_t, 8> levelQueryMisses = {};
        std::array<uint64_t, 8> levelSampleCount = {};
        std::array<uint64_t, 8> levelValidHitDistanceCount = {};
        std::array<uint64_t, 8> levelFallbackSampleCount = {};
        std::array<uint64_t, 8> levelProjectedProbeCount = {};
        std::array<uint64_t, 8> levelInBoundsProbeCount = {};
        uint64_t fallbackCount = 0;
        uint64_t projectedProbeCount = 0;
        uint64_t inBoundsProbeCount = 0;
        bool producedThisFrame = false;
    } mRadianceCacheGpu;

    struct
    {
        ref<ComputePass> pPass;
        uint2 resourceDim = {0, 0};
        bool producedThisFrame = false;
    } mRoughSpecularDiagnostic;

    struct
    {
        ref<ComputePass> pPass;
        uint2 resourceDim = {0, 0};
        bool producedThisFrame = false;
    } mTransmissionDiagnostic;

    // ------------------------------------------------------------------------------------------
    // S6: Mesh SDF / Global Distance Field state (see the S6 section comment in the .cpp).
    // ------------------------------------------------------------------------------------------
    ///< Scriptable S6 gate channel: the GDF sphere-trace output (RGBA16F). RGB = (t / tMax,
    ///< |SDF| at surface / voxel, t), A = hit. Written every frame the GDF pipeline is active;
    ///< used by tests/lumengi/run_s6_gdf.py. In TraceMode::MeshSDF the same data also replaces
    ///< the S1 outputs (diffuseGI / diffuseRadianceHitDist / confidence).
    static constexpr const char* kGDFTrace = "gdfTrace";

    ///< S6-A: master switch. The GDF data pipeline (scene -> MeshSDFScene -> GDF compose) runs
    ///< when this is true OR TraceMode is MeshSDF / Hybrid.
    bool mUseGDF = false;
    ///< S6-A: optional MeshSDFBuilder.exe path. When empty (or the file is missing) the built-in
    ///< analytic box-SDF builder is used (the "placeholder / built-in generation" fallback).
    std::filesystem::path mMeshSDFBuilderPath;
    ///< S6-A: disk cache directory override (default: LumenMeshSDFCache default).
    std::filesystem::path mMeshSDFCacheDir;
    ///< S6-A: per-mesh volume voxel count along the longest axis.
    uint32_t mMeshSDFResolution = 48u;
    ///< S6-A: 0 = High (R16Float mip0), 1 = Low (R8Snorm mip0).
    uint32_t mMeshSDFQuality = 0u;
    ///< S6-A: grid padding around the mesh AABB, fraction of the largest mesh extent.
    float mMeshSDFPadding = 0.1f;
    ///< S6-A: atlas + volume budget in GPU bytes (0 = unlimited).
    uint64_t mMeshSDFBudgetBytes = 0u;
    ///< S6-A3: GDF clipmap level count (>= kDynamicLevels; default 2 = dynamic near + static far).
    uint32_t mGDFLevelCount = 2u;
    ///< S6-A3: GDF voxels per side (all levels share one resolution).
    uint32_t mGDFResolution = 64u;
    ///< S6-A3: GDF base level extent (meters); level m spans baseExtent * 2^m.
    float mGDFBaseExtent = 4.f;
    ///< S6-B4: sphere-trace step budget (0 -> shader default).
    uint32_t mGDFTraceMaxSteps = 64u;
    ///< S6-B4: sphere-trace max distance (world m; 0 -> shader default).
    float mGDFTraceMaxDistance = 20.f;
    ///< S6-B3: empty-distance scale in voxels (empty GDF voxels store this * voxelSize).
    float mGDFEmptyDistanceScale = 8.f;
    ///< C4 diagnostic stage: 0=production compose, 1=single-UAV E1, 2=all-descriptor E2,
    ///< 3=CB+GDF buffers E2a, 4=atlas descriptors E2b, 5=CB+GDF buffers+uniform,
    ///< 6=CB+uniform only E2d.
    ///< This is intentionally opt-in and never changes the default production path.
    uint32_t mGDFDiagnosticStage = 0u;

    ///< S6 host resources + CPU components. pScene and gdf are CPU-only; the rest are GPU.
    struct
    {
        ///< S6-A2/A: scene -> cache -> builder -> volume -> atlas -> instance table (CPU).
        std::unique_ptr<LumenGI::MeshSDF::Scene::LumenMeshSDFScene> pScene;
        ///< S6-A3: camera-centered GDF clipmap (CPU). Rebuilt when the GDF config changes.
        std::unique_ptr<LumenGI::GlobalDistanceField::LumenGlobalDistanceField> gdf;
        ///< Scene-instance -> MeshSDFScene instance handle (parallel to pScene->instances).
        std::vector<uint32_t> sceneInstanceHandles;
        ///< Per-handle scene mesh descriptors (parallel to sceneInstanceHandles; used to re-parse
        ///< the cached volumes when building the GPU atlas images).
        std::vector<LumenGI::MeshSDF::Scene::LumenMeshSDFSceneMeshDesc> sceneMeshDescs;
        ///< True once the GDF levels have been fully composed at least once (first frame /
        ///< after a resize / after the instance list changes issue a full-region compose).
        bool needsFullCompose = true;
        ///< Host-tracked atlas upload generation; bump => re-upload the GPU atlas textures.
        uint64_t atlasUploadVersion = 0;
        ///< Last uploaded atlas upload generation (== atlasUploadVersion when the GPU is current).
        uint64_t uploadedAtlasVersion = 0;
        ///< Host-tracked GDF instance-list generation; bump => force a full recompose.
        uint64_t gdfInstanceVersion = 0;

        ///< S6-B3 compose pass (LumenGDFCompose.cs.slang).
        ref<ComputePass> pCompose;
        ///< C4 E1 diagnostic compose pass (single level UAV).
        ref<ComputePass> pComposeDiag;
        ///< C4 E2 diagnostic compose pass (all production descriptors).
        ref<ComputePass> pComposeDiagAll;
        ///< C4 E2a diagnostic compose pass (CB + GDF buffers).
        ref<ComputePass> pComposeDiagBuffers;
        ///< C4 E2b diagnostic compose pass (atlas SRVs + scalar uniforms).
        ref<ComputePass> pComposeDiagAtlas;
        ///< C4 E2c diagnostic pass (CB + GDF buffers + one global uniform).
        ref<ComputePass> pComposeDiagBuffersScalar;
        ///< C4 E2d diagnostic pass (CB + one global uniform).
        ref<ComputePass> pComposeDiagCBScalar;
        ///< S6-B4 sphere-trace pass (LumenGDFTrace.cs.slang).
        ref<ComputePass> pTrace;

        ///< GDF clipmap textures: runtime staging uses R32Float for all levels; the logical
        ///< level-table format remains the identity codec for compatibility (levelCount entries).
        std::vector<ref<Texture>> levels;
        ///< StructuredBuffer<LumenGDFLevelParams> (levelCount entries).
        ref<Buffer> pLevelTable;
        ///< StructuredBuffer<LumenGDFInstance> (resident instances this frame).
        ref<Buffer> pGDFInstances;
        ///< StructuredBuffer<LumenGDFDirtyRegion> (dirty/removed regions this frame).
        ref<Buffer> pDirtyRegions;
        ///< Sphere-trace counters (kGDFTraceStatCount uints) + readback mirror.
        ref<Buffer> pTraceStats;
        ref<Buffer> pTraceStatsReadback;
        bool traceStatsReadbackPending = false;
        ///< Last completed sphere-trace dispatch counters [traced, hit, miss, maxSteps, noGrid].
        std::array<uint32_t, 5> traceStats = {};

        ///< Mesh SDF atlas GPU mirror (S6-B2): both physical SRVs are R32Float staging views;
        ///< source quality metadata remains fine R16Float/coarse R8Snorm.
        ref<Texture> pFineAtlas;
        ref<Texture> pCoarseAtlas;
        ///< StructuredBuffer<uint> page table (atlas-instance * kLumenMeshSDFMaxMipCount).
        ref<Buffer> pPageTable;
        ///< StructuredBuffer<LumenMeshSDFVolumeDescriptor> (deduplicated meshes).
        ref<Buffer> pVolumes;
        ///< StructuredBuffer<LumenMeshSDFAtlasInstance> (atlas instance table).
        ref<Buffer> pAtlasInstances;
        ///< Host mirror of the fine/coarse atlas images (capacity * pageSize^3 floats each).
        std::vector<float> fineImage;
        std::vector<float> coarseImage;
        ///< [meshID][mip] floats, re-encoded exactly as the CPU atlas tiles them (for GPU upload).
        std::vector<std::vector<std::vector<float>>> meshMipFloats;
        ///< Atlas geometry: pages per side of each 3D texture (== pScene->instanceTable() atlas).
        uint32_t atlasPagesPerSide = 0;
        ///< Last scene stats snapshot (read on demand for getGDFStats / UI).
        LumenGI::MeshSDF::Scene::LumenMeshSDFSceneStats sceneStats;
    } mSDF;
};

FALCOR_ENUM_REGISTER(LumenGIPass::TraceMode);
FALCOR_ENUM_REGISTER(LumenGIPass::QualityPreset);
FALCOR_ENUM_REGISTER(LumenGIPass::DebugMode);
