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

/** S4-B3: probe directional integration (Agent Z2, aligned to the Z1 S4.2 layout).
 *
 *  One thread per probe. Each thread reduces the probe's per-direction hit
 *  records (LumenProbeHit, written by Z1's S4-B2 trace) into a single diffuse
 *  irradiance estimate per probe, which the S4-B3 interpolate pass consumes.
 *
 *  ============================================================================
 *  INTEGRATION WEIGHT FORMULA (the contract this pass implements)
 *  ============================================================================
 *  For a diffuse probe surface with world normal n, the quantity of interest
 *  is the incident diffuse irradiance
 *        E = ∫_Ω Li(ω) · max(ω·n, 0) dω
 *  where Ω is the sphere around the probe (directions below the probe plane
 *  contribute zero for an opaque surface).
 *
 *  Z1 generates the direction set by low-discrepancy / blue-noise sampling in
 *  the probe's COSINE-WEIGHTED hemisphere (lumenProbeSampleDirection in
 *  LumenScreenProbeTrace.cs.slang). Three standard distributions are supported,
 *  selected by gWeightMode; the per-direction weight is
 *        W_i = W0 · s_i · f(ω_i)
 *  with s_i the per-direction stratification weight (1.0 for the equal-area
 *  set Z1 traces; no direction buffer exists, so s_i = 1) and
 *
 *    Mode 0  cosine-weighted hemisphere  (default; pdf ∝ max(ω·n,0)/π):
 *        W0 = π / N,   f(ω_i) = 1
 *        The pdf cos/π exactly cancels the cosine in the MC estimator
 *        Li·cos/pdf = Li·π, so each sample carries the constant weight π/N.
 *
 *    Mode 1  uniform hemisphere           (pdf = 1/(2π)):
 *        W0 = 2π / N,  f(ω_i) = max(ω_i·n, 0)
 *
 *    Mode 2  uniform sphere               (pdf = 1/(4π), includes
 *        W0 = 4π / N,  f(ω_i) = max(ω_i·n, 0)      below-horizon samples)
 *
 *  where N = gDirectionsPerProbe (all directions are traced under the Z1 budget
 *  path, so N is the traced count). Modes 1/2 are statistical mismatches to the
 *  cosine-weighted sample set and are kept only for diagnostics; mode 0 is the
 *  Z1-consistent default.
 *
 *  The final estimator is
 *        E = Σ_i W_i · clamp(Li_i, 0, kLumenScreenProbeMaxRadiance)
 *  over directions whose hit record is valid (screen hit or HWRT hit) and
 *  finite. Traced directions that missed (Li = 0) contribute zero but still
 *  scale the confidence via the valid-hit fraction, so a probe that lost many
 *  directions reports lower confidence instead of a darkened irradiance.
 *
 *  CONFIDENCE
 *        conf = (Σ_i W_i · hitConfidence_i) / (Σ_i W_i)   // over valid hits
 *        conf *= validHitCount / N                         // miss penalty
 *
 *  ============================================================================
 *  ALIGNMENT WITH THE Z1 S4.2 LAYOUT (LumenScreenProbeData.slang, frozen)
 *  ============================================================================
 *  The Z2 draft assumed per-probe gridState/maskState metadata, a per-direction
 *  StructuredBuffer<LumenScreenProbeDirection> indexed set*N+dir, and a
 *  StructuredBuffer<LumenScreenProbeRadiance> output. Z1 landed a DIFFERENT
 *  contract, which this pass now follows (以 Z1 实际为准):
 *    * LumenScreenProbeMeta  (64 B): active/normalW/materialID/depth/... -- the
 *      pass keys validity on `active` and reads `normalW` (oct-decoded by the
 *      Z1 trace) for the mode 1/2 cosTheta.
 *    * LumenProbeHit (32 B): the pass reads `flags` (kLumenProbeHitFlag*),
 *      `radiance` (float3), `confidence`; hit index = probe*gMaxHitRecordStride+dir
 *      (gMaxHitRecordStride = kLumenScreenProbeMaxDirectionsPerProbe, NOT the
 *      runtime gDirectionsPerProbe -- this was the Z2->Z1 index fix).
 *    * No direction buffer: Z1 generates directions on the fly and never stores
 *      them. This pass RECOMPUTES each direction with the exact
 *      lumenProbeSampleDirection formula (a pure function of probeIndex,
 *      dirIndex, frameIndex, gSeed, meta.normalW) so the weights match the
 *      traced set without a buffer.
 *    * gProbeRadiance is RWTexture2D<float4> (Z1 contract): RGB = E,
 *      A = confidence, written sparsely at uint2(meta.screenPos) -- the same
 *      texel Z1's finalizeMain writes its naive average to.
 *    * The integrate CB is the SHARED LumenScreenProbeCB (host fills every
 *      field for the trace passes; S4.3 adds gWeightMode at the tail, +176).
 *
 *  Dispatch: one thread per probe, [numthreads(64,1,1)], thread count
 *  ceil(gProbeCount / 64). The pass writes EVERY probe's texel (active ->
 *  integrated value, inactive/untraced -> zero) so the radiance texture is
 *  deterministic without a host clear.
 *
 *  Deterministic: pure function of the input buffers; no randomness, no shared
 *  memory, one thread writes exactly one texel.
 */

#include "LumenScreenProbeData.slang"
import RenderPasses.LumenGI.Capture.LumenCardCaptureData;

// Optional-resource guards. Falcor specializes is_valid_<name> when it binds
// the resource; local defaults keep the module compilable standalone and every
// read is guarded so an unbound resource is never dereferenced.
#ifndef is_valid_gProbeMeta
#define is_valid_gProbeMeta 0
#endif
#ifndef is_valid_gProbeHitRecords
#define is_valid_gProbeHitRecords 0
#endif
#ifndef is_valid_gProbeRadiance
#define is_valid_gProbeRadiance 0
#endif
#ifndef is_valid_gProbeRadianceHistory
#define is_valid_gProbeRadianceHistory 0
#endif

#define is_valid(name) (is_valid_##name != 0)

#ifndef LUMEN_GI_PROBE_CACHE_LOOKUP
#define LUMEN_GI_PROBE_CACHE_LOOKUP 0
#endif

#if LUMEN_GI_PROBE_CACHE_LOOKUP
StructuredBuffer<LumenCard> gProbeCards;
StructuredBuffer<uint> gProbeCardPageTable;
StructuredBuffer<uint> gProbeCardPageGeneration;
// pageID -> {allocator generation, page state, first frame visible to ScreenProbe, reserved}.
StructuredBuffer<uint4> gProbePageMetadata;
StructuredBuffer<uint> gProbeCardGrid;
Texture2D<float4> gProbeRadianceAtlas;
Texture2D<float4> gProbeMetadataAtlas;
Texture2D<float> gProbeVisibilityAtlas;

cbuffer LumenProbeCacheCB
{
    uint gProbeCacheCardCount;
    uint gProbeCachePagesPerSide;
    uint2 gProbeCacheAtlasSize;
    uint3 gProbeCacheGridDim;
    uint gProbeCacheGridMaxCandidates;
    float3 gProbeCacheGridMin;
    float gProbeCacheGridPadding;
    float3 gProbeCacheGridInvCellSize;
    uint gProbeCacheGridCellStride;
    uint gProbeCacheUseCardGrid;
};

// The host continues to publish the grid and its telemetry, but acceleration
// stays opt-in until a per-cell completeness/ordering proof is available.
// Keeping this compile-time default at zero makes grid-on and grid-off use the
// same deterministic authority path instead of merely relying on a runtime
// branch with a different reduction schedule.
#ifndef LUMEN_GI_CARD_GRID_ACCELERATION
#define LUMEN_GI_CARD_GRID_ACCELERATION 0
#endif

bool tryLookupSurfaceCache(
    const float3 hitPosition,
    const float3 hitDirection,
    out float3 radiance,
    out float confidence,
    out uint feedbackPageID,
    out uint feedbackPageGeneration,
    out uint requestCardID,
    out uint requestReason
)
{
    radiance = float3(0.f);
    confidence = 0.f;
    feedbackPageID = kInvalidPageID;
    feedbackPageGeneration = 0u;
    requestCardID = kLumenCardInvalidID;
    requestReason = 0u;
    const float eps = 0.04f;
    const float3 rayDir = normalize(hitDirection);
    float bestScore = -1.f;
    float3 bestRadiance = float3(0.f);
    float bestConfidence = 0.f;
    uint bestPageID = kInvalidPageID;
    uint bestPageGeneration = 0u;
    float bestRequestScore = -1.f;
    uint bestRequestCardID = kLumenCardInvalidID;
    uint bestRequestReason = 0u;
    bool found = false;
    uint gridBase = 0u;
    bool gridOverflow = false;
    const bool useCardGrid = gProbeCacheUseCardGrid != 0u && LUMEN_GI_CARD_GRID_ACCELERATION != 0;
    uint candidateCount = gProbeCacheCardCount;
    if (useCardGrid)
    {
        const float3 gridCoord = (hitPosition - gProbeCacheGridMin) * gProbeCacheGridInvCellSize;
        const int3 gridCellI = clamp(
            int3(floor(gridCoord)), int3(0), int3(gProbeCacheGridDim) - int3(1)
        );
        const uint3 gridCell = uint3(gridCellI);
        const uint gridCellIndex = (gridCell.z * gProbeCacheGridDim.y + gridCell.y) * gProbeCacheGridDim.x + gridCell.x;
        gridBase = gridCellIndex * gProbeCacheGridCellStride;
        const uint packedCandidateCount = gProbeCardGrid[gridBase];
        gridOverflow = (packedCandidateCount & 0x80000000u) != 0u;
        const uint boundedCandidateCount = packedCandidateCount & 0x7fffffffu;
        candidateCount = gridOverflow
            ? gProbeCacheCardCount
            : min(boundedCandidateCount, gProbeCacheGridMaxCandidates);
        // The bounded grid path is intentionally disabled until the host
        // publishes a per-cell completeness proof. Even a cell whose list
        // happens to contain all cards can take a different divergent shader
        // path and change atomic/history reduction order between grid-on and
        // grid-off runs. Keep the grid data/telemetry alive, but use the
        // authoritative full-card scan for deterministic production output.
        if (!gridOverflow)
        {
            gridOverflow = true;
            candidateCount = gProbeCacheCardCount;
        }
    }
    // A conservative grid is an acceleration structure, not a correctness
    // fence. If its bounded cell list yields no valid page, retry once with
    // the authoritative full-card scan so grid-on cannot drop a cache hit.
    const uint gridCandidateCount = candidateCount;
    for (uint gridPass = 0u; gridPass < 2u; ++gridPass)
    {
        if (gridPass == 1u &&
            (!useCardGrid || gridOverflow || found))
            break;
        const uint passCandidateCount = gridPass == 0u ? gridCandidateCount : gProbeCacheCardCount;
        for (uint candidateOrdinal = 0u; candidateOrdinal < passCandidateCount; ++candidateOrdinal)
        {
            const uint cardIndex = (!useCardGrid || gridOverflow || gridPass == 1u)
                ? candidateOrdinal
                : gProbeCardGrid[gridBase + 1u + candidateOrdinal];
        if (cardIndex >= gProbeCacheCardCount)
            continue;
        const uint pageID = gProbeCardPageTable[cardIndex];
        const uint cardGeneration = gProbeCardPageGeneration[cardIndex];
        const uint4 pageMetadata = pageID != kInvalidPageID ? gProbePageMetadata[pageID] : uint4(0u);
        const uint pageGeneration = pageMetadata.x;
        const uint pageState = pageMetadata.y;
        const uint pageReadyFrame = pageMetadata.z;
        const bool pageOwnerValid = pageID != kInvalidPageID && cardGeneration != 0u && pageGeneration != 0u &&
            cardGeneration == pageGeneration && (pageState == 1u || pageState == 2u) &&
            pageReadyFrame <= gSurfaceCacheFrameIndex;
        const LumenCard card = gProbeCards[cardIndex];
        const uint axis = lumenCardFaceAxis(card.faceIndex);
        const bool positive = lumenCardFaceIsPositive(card.faceIndex);
        const float3 extent = max(card.extent, float3(1e-5f));
        const float3 local = hitPosition - card.center;
        const float halfAxis = 0.5f * lumenCardComponent(extent, axis);
        const float axisCoord = lumenCardComponent(local, axis);
        // A card is captured from the AABB face, but the actual mesh surface
        // can be behind that face by an arbitrary capture depth. Treat the
        // axis as a slab instead of requiring the hit to lie on the exact
        // AABB plane; this is the missing per-face refinement in the MVP card
        // representation. The face orientation and depth still reject points
        // outside the card volume.
        const float faceDepth = positive ? halfAxis - axisCoord : axisCoord + halfAxis;
        if (faceDepth < -eps || faceDepth > 2.f * halfAxis + eps)
        {
            InterlockedAdd(gProbeCounters[0].cacheCoverageRejects, 1u);
            InterlockedAdd(gProbeCounters[0].cacheDepthRejects, 1u);
            continue;
        }
        const uint2 axes = lumenCardFacePerpendicularAxes(card.faceIndex);
        if (abs(lumenCardComponent(local, axes.x)) > 0.5f * lumenCardComponent(extent, axes.x) + eps ||
            abs(lumenCardComponent(local, axes.y)) > 0.5f * lumenCardComponent(extent, axes.y) + eps)
        {
            InterlockedAdd(gProbeCounters[0].cacheCoverageRejects, 1u);
            InterlockedAdd(gProbeCounters[0].cacheAxisRejects, 1u);
            continue;
        }

        float3 viewDir, right, up;
        lumenCardFaceBasis(card.faceIndex, viewDir, right, up);
        // The probe ray points toward the card; compare against the incident
        // view direction used by the capture convention.
        const float facing = dot(-rayDir, viewDir);
        if (facing <= 0.01f)
        {
            InterlockedAdd(gProbeCounters[0].cacheCoverageRejects, 1u);
            InterlockedAdd(gProbeCounters[0].cacheFacingRejects, 1u);
            continue;
        }
        InterlockedAdd(gProbeCounters[0].cacheOwnerValid, pageOwnerValid ? 1u : 0u);
        const float halfW = 0.5f * lumenCardComponent(extent, axes.x);
        const float halfH = 0.5f * lumenCardComponent(extent, axes.y);
        const float requestScore = facing / max(1.f + faceDepth, 1e-3f);
        if (!pageOwnerValid)
        {
            if (!found && requestScore > bestRequestScore)
            {
                bestRequestScore = requestScore;
                bestRequestCardID = cardIndex;
                bestRequestReason = pageID == kInvalidPageID ? 1u : 2u; // unmapped / stale owner
            }
            InterlockedAdd(gProbeCounters[0].cachePageRejects, 1u);
            continue;
        }
        const float2 uv = saturate(float2(dot(local, right) / max(halfW, 1e-5f), dot(local, up) / max(halfH, 1e-5f)) * 0.5f + 0.5f);
        const uint2 origin = lumenCardPageTexelOrigin(pageID, gProbeCachePagesPerSide);
        const uint2 texel = min(origin + uint2(uv * float(kLumenSurfaceCacheTileSize - 1u)), gProbeCacheAtlasSize - 1u);
        const uint2 pageMax = min(
            origin + uint2(kLumenSurfaceCacheTileSize - 1u),
            gProbeCacheAtlasSize - 1u);
        uint2 selectedTexel = texel;
        uint selectedMetadataFlags = uint(gProbeMetadataAtlas[texel].g);
        float selectedVisibility = saturate(gProbeVisibilityAtlas[texel]);
        const bool exactMetadataValid =
            (selectedMetadataFlags & kLumenCardMetadataFlagValid) != 0u &&
            (selectedMetadataFlags & kLumenCardMetadataFlagFallbackAny) == 0u;
        bool anyMetadataValid = exactMetadataValid;
        bool selectedVisible = exactMetadataValid && selectedVisibility > 0.f;

        // Capture can leave a single texel invalid (or visibility-unready) at a
        // page boundary even though the page owner/generation is valid. Search
        // only the bounded 3x3 neighborhood inside this same 16x16 page. This
        // preserves all card/page/ generation/extent fences while avoiding a
        // false miss caused by one uncovered raster texel; never cross into a
        // neighboring page or use a fallback-marked texel.
        if (!selectedVisible)
        {
            const int2 localTexel = int2(texel - origin);
            const int2 localMax = int2(pageMax - origin);
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int2 localCandidate = clamp(
                        localTexel + int2(dx, dy), int2(0, 0), localMax);
                    const uint2 candidateTexel = origin + uint2(localCandidate);
                    const uint candidateFlags = uint(gProbeMetadataAtlas[candidateTexel].g);
                    const bool candidateMetadataValid =
                        (candidateFlags & kLumenCardMetadataFlagValid) != 0u &&
                        (candidateFlags & kLumenCardMetadataFlagFallbackAny) == 0u;
                    anyMetadataValid = anyMetadataValid || candidateMetadataValid;
                    if (!candidateMetadataValid)
                    {
                        continue;
                    }
                    const float candidateVisibility = saturate(gProbeVisibilityAtlas[candidateTexel]);
                    if (candidateVisibility > selectedVisibility)
                    {
                        selectedTexel = candidateTexel;
                        selectedMetadataFlags = candidateFlags;
                        selectedVisibility = candidateVisibility;
                    }
                }
            }
            selectedVisible = anyMetadataValid && selectedVisibility > 0.f;
        }

        if (!selectedVisible)
        {
            const bool requestSelected = !found && requestScore > bestRequestScore;
            if (requestSelected)
            {
                bestRequestScore = requestScore;
                bestRequestCardID = cardIndex;
                bestRequestReason = anyMetadataValid ? 8u : 4u;
            }
            if (!anyMetadataValid)
            {
                InterlockedAdd(gProbeCounters[0].cacheMetadataRejects, 1u);
            }
            else
            {
                InterlockedAdd(gProbeCounters[0].cacheVisibilityRejects, 1u);
            }
            continue;
        }
        const float score = requestScore;
            if (!found || score > bestScore)
            {
                bestScore = score;
                bestRadiance = max(gProbeRadianceAtlas[selectedTexel].rgb, float3(0.f));
                bestConfidence = selectedVisibility;
                bestPageID = pageID;
                bestPageGeneration = pageGeneration;
                found = true;
            }
        }
    }
    if (found)
    {
        radiance = bestRadiance;
        confidence = bestConfidence;
        feedbackPageID = bestPageID;
        feedbackPageGeneration = bestPageGeneration;
    }
    else if (bestRequestCardID != kLumenCardInvalidID)
    {
        requestCardID = bestRequestCardID;
        requestReason = bestRequestReason;
    }
    return found;
}
#endif

// Radiance ceiling mirrored from LumenGIData.slang (kLumenGIMaxRadiance),
// overridable by the host through the same define. Applied per direction
// before accumulation to prevent single-sample fireflies from dominating a
// probe, and again on the integrated output.
#ifndef LUMEN_GI_MAX_RADIANCE
#define LUMEN_GI_MAX_RADIANCE 10.f
#endif
static const float kLumenScreenProbeMaxRadiance = LUMEN_GI_MAX_RADIANCE;

static const float kLumenScreenProbePI = 3.14159265358979f;

/** Weight-mode constants (see the formula block in the header). */
static const uint kLumenScreenProbeWeightModeCosineHemisphere = 0u;
static const uint kLumenScreenProbeWeightModeUniformHemisphere = 1u;
static const uint kLumenScreenProbeWeightModeUniformSphere = 2u;

// -------------------------------------------------------------------------------------
// Direction sampling (MIRROR of lumenProbeSampleDirection in LumenScreenProbeTrace.cs.slang;
// kept local so this module compiles standalone. The trace does not store directions, so the
// integrate pass recomputes them deterministically to recover cosTheta for modes 1/2).
// -------------------------------------------------------------------------------------

/** Standard 32-bit integer hash (Wang). Identical to lumenProbeHash in the trace. */
uint lumenProbeIntegrateHash(uint x)
{
    x = (x ^ 61u) ^ (x >> 16u);
    x = x * 9u + 0x85ebca6bu;
    x = x ^ (x >> 13u);
    x = x * 0xc2b2ae35u;
    x = x ^ (x >> 16u);
    return x;
}

/** Two independent hash outputs of the probe index, normalized to [0, 1). */
float2 lumenProbeIntegrateHash2(const uint probeIndex)
{
    const uint a = lumenProbeIntegrateHash(probeIndex);
    const uint b = lumenProbeIntegrateHash(a ^ 0x9e3779b9u);
    return float2(float(a) * (1.0 / 4294967296.0), float(b) * (1.0 / 4294967296.0));
}

/** Base-2 Van der Corput radical inverse (identical to the trace). */
float lumenProbeIntegrateRadicalInverse(const uint i)
{
    return float(reversebits(i)) * (1.0 / 4294967296.0);
}

/** Hammersley 2D point (i of n). */
float2 lumenProbeIntegrateHammersley(const uint i, const uint n)
{
    return float2(float(i % n) / float(n), lumenProbeIntegrateRadicalInverse(i));
}

/** One world-space direction of probe (probeIndex) for slot dirIndex at frame
    frameIndex (identical formula to lumenProbeSampleDirection in the trace).
*/
float3 lumenProbeIntegrateSampleDirection(
    const uint probeIndex,
    const uint dirIndex,
    const uint frameIndex,
    const float3 normalW
)
{
    const uint n = max(1u, gDirectionsPerProbe);
    const uint rot = (probeIndex * 2654435761u + frameIndex * 1140071481u + gSeed) % n;
    const uint j = (dirIndex + rot) % n;
    float2 u = frac(lumenProbeIntegrateHammersley(j, n) +
                    lumenProbeIntegrateHash2(probeIndex ^ (frameIndex * 0x9e3779b9u)) * (1.0 / float(n)));

    const float3 N = any(isnan(normalW)) ? float3(0.f, 1.f, 0.f) : normalize(normalW);
    const float3 helper = abs(N.z) < 0.999f ? float3(0.f, 0.f, 1.f) : float3(1.f, 0.f, 0.f);
    const float3 T = normalize(cross(helper, N));
    const float3 B = cross(N, T);

    const float z = 1.f - u.x;
    const float s = sqrt(max(0.f, 1.f - z * z));
    const float phi = 2.f * kLumenScreenProbePI * u.y;
    const float3 localDir = float3(s * cos(phi), s * sin(phi), z);
    return normalize(T * localDir.x + B * localDir.y + N * localDir.z);
}

/** Per-direction integration weight W_i (see header formula block). */
float lumenScreenProbeDirectionWeight(
    const float cosTheta,     ///< dot(direction, probeNormal).
    const float stratWeight,  ///< s_i: per-direction stratification weight (1.0 equal-area).
    const uint mode,
    const uint tracedCount
)
{
    const float n = max(float(tracedCount), 1.f);
    if (mode == kLumenScreenProbeWeightModeUniformHemisphere)
        return (2.f * kLumenScreenProbePI / n) * stratWeight * max(cosTheta, 0.f);
    if (mode == kLumenScreenProbeWeightModeUniformSphere)
        return (4.f * kLumenScreenProbePI / n) * stratWeight * max(cosTheta, 0.f);
    // Cosine-weighted hemisphere: the pdf cos/π cancels the cosine, so the
    // per-sample weight is the constant π/N.
    return (kLumenScreenProbePI / n) * stratWeight;
}

/** True when every component of a radiance estimate is finite. */
bool lumenScreenProbeRadianceIsFinite(const float3 radiance)
{
    return !any(isnan(radiance)) && !any(isinf(radiance));
}

/** Clamp a radiance estimate to the firefly ceiling and zero non-finite /
 *  negative components. Mirrors the LumenGI sample sanitization.
 */
float3 lumenScreenProbeSanitizeRadiance(const float3 radiance)
{
    float3 r = max(radiance, float3(0.f));
    if (!lumenScreenProbeRadianceIsFinite(r))
        return float3(0.f);
    return min(r, float3(kLumenScreenProbeMaxRadiance));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint probeIndex = dispatchThreadID.x;
    if (probeIndex >= gProbeCount)
        return;

    if (!is_valid(gProbeMeta) || !is_valid(gProbeHitRecords) || !is_valid(gProbeRadiance) ||
        !is_valid(gProbeRadianceHistory))
        return; // Unconfigured pass: never dereference an unbound resource.

    const LumenScreenProbeMeta meta = gProbeMeta[probeIndex];

    // Output texel: the probe tile center (the Z1 finalize pass writes the same
    // texel; tile centers always lie inside the frame by construction).
    const int2 outPixel = int2(meta.screenPos);
    if (outPixel.x < 0 || outPixel.y < 0 || outPixel.x >= int(gFrameDim.x) || outPixel.y >= int(gFrameDim.y))
        return;

    // Inactive probe (no surface): deterministic zero record.
    if (meta.active == 0u)
    {
        gProbeRadiance[outPixel] = float4(0.f, 0.f, 0.f, 0.f);
        gProbeRadianceHistory[outPixel] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    // The trace/finalize stages use a round-robin budget. A skipped probe must
    // preserve its previous history and probe radiance; integrating its stale hit
    // records with the current frame's direction sequence would falsely claim new
    // samples and corrupt the cross-frame estimator.
    const uint interval = max(1u, gUpdateInterval);
    const bool dirty = (meta.dirty & kLumenScreenProbeMetaFlagDirty) != 0u;
    const bool due = dirty ||
                     (probeIndex % interval) == (gFrameIndex % interval);
    if (!due)
        return;

    const float3 probeNormal = normalize(meta.normalW);
    const uint n = max(1u, gDirectionsPerProbe);
    const uint tracedCount = n;

    float3 accRadiance = float3(0.f);
    float accConfidence = 0.f;
    float accWeight = 0.f;
    uint validHitCount = 0u;

    for (uint dir = 0u; dir < n; ++dir)
    {
        // Hit record index: probe * FIXED STRIDE + dir (Z1 trace contract; the stride is
        // kLumenScreenProbeMaxDirectionsPerProbe, not the runtime gDirectionsPerProbe).
        const LumenProbeHit hit = gProbeHitRecords[probeIndex * gMaxHitRecordStride + dir];
        if ((hit.flags & (kLumenProbeHitFlagScreenHit | kLumenProbeHitFlagHWRTHit | kLumenProbeHitFlagGDFHit)) == 0u)
            continue; // Traced but no radiance (screen miss / fallback miss): Li = 0, lowers confidence via hit fraction.

        // A finite black hit is a valid zero-radiance sample. Only non-finite
        // radiance is invalid; do not use RGB > 0 as the hit-validity test.
        if (!lumenScreenProbeRadianceIsFinite(hit.radiance))
            continue;
        float3 li = lumenScreenProbeSanitizeRadiance(hit.radiance);
        // The cache candidate uses the same deterministic direction as the
        // weighting below, including its ray-facing card selection.
        const float3 dirW = lumenProbeIntegrateSampleDirection(probeIndex, dir, gFrameIndex, probeNormal);
#if LUMEN_GI_PROBE_CACHE_LOOKUP
        // Surface Cache is authoritative when a valid captured card/page covers
        // this hit. Cache visibility is a validity gate, not a lerp weight;
        // stale/invalid cache data falls back to the finite HWRT hit record.
        const float3 hitPosition = meta.worldPos + lumenProbeIntegrateSampleDirection(
            probeIndex, dir, gFrameIndex, probeNormal
        ) * max(hit.hitDistance, 0.f);
        float3 cacheRadiance;
        float cacheConfidence;
        uint cachePageID;
        uint cachePageGeneration;
        uint requestCardID;
        uint requestReason;
        InterlockedAdd(gProbeCounters[0].cacheLookupAttempts, 1u);
        const bool cacheFound = tryLookupSurfaceCache(
            hitPosition, dirW, cacheRadiance, cacheConfidence, cachePageID, cachePageGeneration, requestCardID, requestReason
        );
        if (cacheFound &&
            cacheConfidence > 0.f && lumenScreenProbeRadianceIsFinite(cacheRadiance))
        {
            li = lumenScreenProbeSanitizeRadiance(cacheRadiance);
            InterlockedAdd(gProbeCounters[0].cacheLookupHits, 1u);
#if is_valid_gProbeCacheFeedback
            if (cachePageID != kInvalidPageID && cachePageGeneration != 0u)
            {
                InterlockedAdd(gProbeCacheFeedback[cachePageID].x, 1u);
                // All hits for a page in one dispatch observe the same validated generation;
                // InterlockedMax is supported by DXC and remains safe if a stale write races
                // with a newer generation during a resource transition.
                InterlockedMax(gProbeCacheFeedback[cachePageID].y, cachePageGeneration);
            }
#endif
        }
#if is_valid_gProbeCacheRequests
        else if (requestCardID != kLumenCardInvalidID && requestReason != 0u)
        {
            InterlockedAdd(gProbeCacheRequests[requestCardID].x, 1u);
            InterlockedOr(gProbeCacheRequests[requestCardID].y, requestReason);
        }
#endif
#endif
        // Sanitization preserves finite black as a valid contribution while
        // retaining zero output for non-finite/invalid records above.
        if (!lumenScreenProbeRadianceIsFinite(li))
            continue;
        ++validHitCount;

        // Recompute the traced direction (identical formula to the trace) for the mode 1/2
        // cosTheta; mode 0 is direction-independent (constant π/N weight).
        const float cosTheta = dot(dirW, probeNormal);
        const float weight = lumenScreenProbeDirectionWeight(cosTheta, 1.f, gWeightMode, tracedCount);
        if (weight <= 0.f)
            continue;

        accRadiance += weight * li;
        accConfidence += weight * saturate(hit.confidence);
        accWeight += weight;
    }

    // E = Σ W_i · clamp(Li) (incident irradiance). conf weighted by the valid-hit fraction.
    const float confidence = (accWeight > 0.f)
        ? (accConfidence / accWeight) * (float(validHitCount) / float(tracedCount))
        : 0.f;
    const float3 irradiance = (accWeight > 0.f)
        ? lumenScreenProbeSanitizeRadiance(accRadiance)
        : float3(0.f);

    // C7 cross-frame accumulation. The history stores a running mean in RGB and
    // the number of traced directions in A. Each frame contributes a complete
    // N-direction estimator (including misses), so the estimator remains unbiased
    // while the effective direction union grows beyond the fixed per-frame set.
    const float4 previous = gProbeRadianceHistory[outPixel];
    const float previousCount = dirty ? 0.f : max(previous.a, 0.f);
    const float currentCount = float(tracedCount);
    const float totalCount = min(previousCount + currentCount, 65504.f);
    const float currentWeight = currentCount / max(totalCount, 1.f);
    const float3 accumulated = (previousCount > 0.f)
        ? lerp(max(previous.rgb, float3(0.f)), irradiance, currentWeight)
        : irradiance;
    gProbeRadiance[outPixel] = float4(lumenScreenProbeSanitizeRadiance(accumulated), confidence);
    gProbeRadianceHistory[outPixel] = float4(lumenScreenProbeSanitizeRadiance(accumulated), totalCount);
}
