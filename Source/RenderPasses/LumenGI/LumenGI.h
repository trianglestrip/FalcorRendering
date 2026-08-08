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
#include "LumenGIStats.h"
#include "Capture/LumenCaptureScheduler.h" // Brings in Cards/LumenCardScene.h and SurfaceCache/LumenSurfaceCache.h.

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

    ref<Scene> mpScene;
    sigs::Connection mUpdateFlagsConnection;
    IScene::UpdateFlags mSceneUpdates = IScene::UpdateFlags::None;
    bool mLightCollectionInitialized = false;

    ///< Emissive light BVH sampler for secondary-hit NEE; scene-scoped (rebuilt on setScene).
    std::unique_ptr<LightBVHSampler> mpEmissiveLightSampler;

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
    bool mUseScreenTrace = false;
    bool mUseScreenProbes = false;
    bool mUseTemporalFilter = false;
    bool mUseSpatialFilter = false;
    bool mUseRadianceCache = false;
};

FALCOR_ENUM_REGISTER(LumenGI::TraceMode);
FALCOR_ENUM_REGISTER(LumenGI::QualityPreset);
FALCOR_ENUM_REGISTER(LumenGI::DebugMode);
