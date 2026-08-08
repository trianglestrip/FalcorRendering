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

/** Surface Cache lighting shared data (S3-B1, Agent J).
 *
 * Single source of truth for the Surface Cache direct-lighting compute pass
 * (LumenSurfaceCacheLighting.cs.slang): the constant buffer, the resource
 * layout (inputs/outputs), the quality presets and the light-component
 * enumeration. Nothing here is owned by the host; the host binds the
 * resources described below. The cbuffer and the global parameters live in
 * this imported module (same pattern as the Falcor LightBVH module, which
 * also declares its globals in an imported file) and are only pulled into
 * the program by the S3-B1 entry shader, so no other LumenGI program picks
 * them up.
 *
 * Frozen contracts this module builds on (read-only, owned by other agents):
 *   - LumenCardCaptureData.slang (Agent C): atlas channel layouts, the
 *     LumenCard struct, the pageID formula, the face basis, the metadata
 *     validity flags.
 *   - LumenSurfaceCache.h (Agent B): page/tile/budget semantics.
 *   - LumenGIData.slang (Agent B): kLumenGIMaxRadiance, the GPU counter
 *     layout, the debug mode values. This module reads them, never changes
 *     them.
 *
 * Radiance atlas channel layout (frozen by Agent C):
 *   R,G,B = direct radiance (written by this pass)
 *   A     = indirect radiance (written by S3-B2, multi-bounce feedback).
 * This pass therefore PRESERVES the existing A channel (read-modify-write).
 * Confidence/visibility is written to the separate gLumenVisibilityAtlas
 * (R16F), because the radiance atlas A channel is not free.
 */

__exported import RenderPasses.LumenGI.Capture.LumenCardCaptureData;
__exported import RenderPasses.LumenGI.LumenGIData;
import Rendering.Lights.EnvMapSampler;
import Rendering.Lights.EmissiveLightSampler;

// ---------------------------------------------------------------------------
// Light source toggles. The host defines these on the lighting program; the
// defaults must match LumenGILighting.slang / LumenGILightSampling.slang so
// that this module is statically compilable before the host wires anything.
// ---------------------------------------------------------------------------

#ifndef USE_ENV_LIGHT
#define USE_ENV_LIGHT 0
#endif
#ifndef USE_ANALYTIC_LIGHTS
#define USE_ANALYTIC_LIGHTS 1
#endif
#ifndef USE_EMISSIVE_LIGHTS
#define USE_EMISSIVE_LIGHTS 1
#endif

/** Set to 1 by the host only when the EnvMapSampler object was created and
    bound to the `envMapSampler` parameter. When 0 the lighting shader falls
    back to uniform-sphere sampling of gScene.envMap (unbiased, higher
    variance). See LumenGILightSampling.slang for the same convention.
*/
#ifndef LUMEN_GI_HAS_ENVIRONMENT_SAMPLER
#define LUMEN_GI_HAS_ENVIRONMENT_SAMPLER 0
#endif

/** Set to 1 by the host only when the EmissiveLightSampler object
    (LightBVHSampler in S3) was created and bound to the `emissiveSampler`
    parameter. When 0 the emissive next-event term degrades to a miss
    constant (emissive triangle lighting is omitted; texel emission from the
    params atlas is still added). See LumenGILightSampling.slang.
*/
#ifndef LUMEN_GI_HAS_EMISSIVE_SAMPLER
#define LUMEN_GI_HAS_EMISSIVE_SAMPLER 0
#endif

/** Balance-heuristic MIS toggle with the exact semantics of
    LUMEN_GI_ANALYTIC_LIGHT_MIS in LumenGILighting.slang: weights the
    analytic light-sampling technique against the BSDF-sampling technique
    evaluated at the light direction (here the Lambert pdf cos/pi). Default 0
    (weight 1, unbiased single-technique) and only set to 1 together with a
    scatter technique executed from the cache texel (S3-B2 feedback), where
    both strategies run and the balance weight is unbiased.
*/
#ifndef LUMEN_GI_ANALYTIC_LIGHT_MIS
#define LUMEN_GI_ANALYTIC_LIGHT_MIS 0
#endif

/** Visibility test toggle (owned by this module). Default 1: every NEE
    sample is shadow-tested with an inline scene ray query, which requires
    the host to call Scene::bindShaderDataForRaytracing() on the lighting
    program (DXR). Set to 0 by hosts that cannot bind ray tracing data:
    lighting then degrades to unshadowed direct radiance (documented
    degradation, not a Lumen-correct path).
*/
#ifndef LUMEN_GI_CACHE_LIGHTING_SHADOWS
#define LUMEN_GI_CACHE_LIGHTING_SHADOWS 1
#endif

// ---------------------------------------------------------------------------
// Quality presets (frozen)
// ---------------------------------------------------------------------------

/** samplesPerTexel presets. The host passes one of these as the CB field
    gSamplesPerTexel and may change it per frame without recompiling.

    Sample budget per texel (each "sample" draws one analytic + one
    environment + one emissive NEE sample when the corresponding technique
    is enabled):

      preset    gSamplesPerTexel   NEE draws/texel   shadow rays/texel (max)
      ------    ----------------   ----------------  -----------------------
      Low       1                  3                 3
      Medium    2                  6                 6
      High      4                  12                12
      Reference 8                  24                24

    Shadow rays are only cast when the hemisphere test (NdotL > 0) passes,
    so the worst case equals the NEE draw count. The environment technique
    never casts a shadow ray in the default path when the ray ends in a miss
    beyond kLumenGIInfiniteLightDistance -- the visibility query still
    executes once per environment sample. LumenGICounterIndex::TracedRays
    counts the shadow queries actually issued.
*/
enum class LumenCacheLightingQuality : uint32_t
{
    Low = 1,
    Medium = 2,
    High = 4,
    Reference = 8,
};

// ---------------------------------------------------------------------------
// Light components of the direct radiance (debug views and stats)
// ---------------------------------------------------------------------------

/** Direct-radiance components accumulated per texel. Emitted is the texel's
    own material emission (params atlas), which is not sampled and not
    firefly-clamped.
*/
enum class LumenCacheDirectComponent : uint32_t
{
    Analytic = 0,
    Emissive = 1,
    Environment = 2,
    Emitted = 3,
    Count = 4,
};

/** Golden-ratio constant used to stride the RNG seed per frame, so adjacent
    frames sample different points while the sequence stays fully
    reproducible for a fixed gSeed.
*/
static const uint kLumenGICacheSeedFrameStride = 0x9E3779B9u;

// ---------------------------------------------------------------------------
// Per-dispatch lighting parameters (frozen layout, 48 bytes)
//
// The host binds this cbuffer by name and fills every field every dispatch.
// Field offsets are fixed below so a C++ struct mirror (added by the root
// integrator) can be static_asserted against the GPU layout.
// ---------------------------------------------------------------------------
cbuffer LumenSurfaceCacheLightingCB
{
    uint  gPagesPerSide;    // +0  N of the pageID formula (tiles per atlas side). Must equal the allocator grid.
    uint  gRenderPageCount; // +4  Pages to light this dispatch (one threadgroup per page); must equal the dispatch X size.
    uint  gFrameIndex;      // +8  Frame counter; rotates the per-frame RNG seed.
    uint  gSeed;            // +12 Fixed base seed. Task rule 5: every stochastic process must support a fixed seed.
    uint  gSamplesPerTexel; // +16 1/2/4/8, see LumenCacheLightingQuality.
    uint  gDebugMode;       // +20 kLumenDebug* value from LumenGIData.slang; 0 = final output.
    uint2 gAtlasSize;       // +24 Atlas texture size in texels; every page write is bounds-checked against it.
    float gNearMargin;      // +32 Same value the capture pass used for its card cameras (meters); <= 0 means the frozen default kLumenCardCaptureDefaultNearMargin.
    uint2 gReserved;        // +36 Padding to 48 bytes (multiple of 16).
};

// ---------------------------------------------------------------------------
// Resource layout (frozen)
//
// is_valid_<name> defines are emitted by Falcor for optional parameters and
// are defaulted here (required -> 1, optional -> 0) so the module compiles
// standalone and degrades at runtime. Note: preprocessor macros do NOT cross
// Slang module boundaries, so the entry shader re-declares these defaults and
// the `is_valid(name)` macro locally (same pattern as LumenCardCapture.3d.slang).
// ---------------------------------------------------------------------------
#ifndef is_valid_gCards
#define is_valid_gCards 1
#endif
#ifndef is_valid_gLumenPageToCard
#define is_valid_gLumenPageToCard 1
#endif
#ifndef is_valid_gLumenRenderList
#define is_valid_gLumenRenderList 1
#endif
#ifndef is_valid_gMaterialAtlas
#define is_valid_gMaterialAtlas 1
#endif
#ifndef is_valid_gMetadataAtlas
#define is_valid_gMetadataAtlas 1
#endif
#ifndef is_valid_gMaterialParamsAtlas
#define is_valid_gMaterialParamsAtlas 0
#endif
#ifndef is_valid_gRadianceAtlas
#define is_valid_gRadianceAtlas 1
#endif
#ifndef is_valid_gLumenVisibilityAtlas
#define is_valid_gLumenVisibilityAtlas 1
#endif
#ifndef is_valid_gLumenGICounters
#define is_valid_gLumenGICounters 0
#endif
#ifndef is_valid_gDebugTexture
#define is_valid_gDebugTexture 0
#endif

// --- Inputs (SRV / sampler objects) ----------------------------------------

/** Card table, one LumenCard (96 bytes) per card index; layout frozen in
    LumenCardCaptureData.slang (must match LumenCardScene.h). Host: upload
    from LumenCardScene::getCard()/getCardCount(). Required.
*/
StructuredBuffer<LumenCard> gCards;

/** Page -> card reverse lookup: gLumenPageToCard[pageID] is the index into
    gCards of the card currently occupying the page (kLumenCardInvalidID for
    free pages). The lighting shader reconstructs world positions from the
    card AABB/face, so this table is required: without it a page cannot be
    lit. Host: one uint per allocator page ID (size getPageCount()+1), filled
    from the card -> page mapping owned by the root pass, updated whenever a
    page is allocated or evicted.
*/
StructuredBuffer<uint> gLumenPageToCard;

/** Render list of pages to light this frame (S3-A1 scheduler output, sorted
    by priority/dirty/age): gLumenRenderList[i] is a page ID. Optional: when
    unbound the shader runs in full-atlas mode where threadgroup x lights
    page (x+1) and the host dispatches (pagesPerSide*pagesPerSide, 1, 1)
    threadgroups with gRenderPageCount set accordingly.
*/
StructuredBuffer<uint> gLumenRenderList;

/** Material atlas (RGBA8 SRV): RGB = base color (linear), A = opacity.
    Layout frozen in LumenCardCaptureData.slang. Required.
*/
Texture2D<float4> gMaterialAtlas;

/** Metadata atlas (RGBA16F SRV): R = linear depth (meters), G = validity
    flags, BA = shading normal octahedral. Required.
*/
Texture2D<float4> gMetadataAtlas;

/** Material params atlas (RGBA16F SRV, Agent C extension): R = roughness
    (unused by the Lambert direct term; consumed by later stages), GBA =
    emissive (linear). Optional: when unbound, emissive = 0.
*/
Texture2D<float4> gMaterialParamsAtlas;

/** Environment sampler. Host: EnvMapSampler::bindShaderData(var["envMapSampler"])
    and LUMEN_GI_HAS_ENVIRONMENT_SAMPLER = 1 when created. Unbound ->
    uniform-sphere fallback over gScene.envMap.
*/
EnvMapSampler envMapSampler;

/** Emissive triangle sampler (LightBVHSampler in S3). Host:
    mpEmissiveSampler->bindShaderData(var["emissiveSampler"]) and
    LUMEN_GI_HAS_EMISSIVE_SAMPLER = 1, with the _EMISSIVE_LIGHT_SAMPLER_TYPE
    define from EmissiveLightSampler::getDefines(). Unbound (or the NULL
    sampler type) -> emissive next-event term is a miss constant.
*/
EmissiveLightSampler emissiveSampler;

// --- Outputs (UAV) ---------------------------------------------------------

/** Radiance atlas (RGBA16F UAV): RGB = average direct radiance (linear),
    written by this pass; A = indirect radiance, PRESERVED from the previous
    contents (S3-B2 owns it). Required.
*/
RWTexture2D<float4> gRadianceAtlas;

/** Visibility/confidence atlas (R16F UAV): fraction of samples per texel
    whose next-event estimate survived the hemisphere test AND the shadow
    test, scaled by the material opacity, in [0,1]. Written by this pass.
    Required. (A separate buffer is used because the radiance atlas A
    channel is frozen to indirect radiance.)
*/
RWTexture2D<float> gLumenVisibilityAtlas;

/** Optional GPU counters (LumenGICounterIndex layout in LumenGIData.slang).
    Host clears the buffer before each dispatch and reads it back into
    LumenGIStats. Writes are skipped when unbound.
*/
RWStructuredBuffer<uint4> gLumenGICounters;

/** Optional debug output (RGBA16F UAV) for the gDebugMode views of the
    direct lighting. Unbound -> no debug writes.
*/
RWTexture2D<float4> gDebugTexture;
