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

#include "Core/Macros.h"
#include "Scene/IScene.h"
#include "Utils/Math/AABB.h"
#include "Utils/Math/Matrix.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Falcor
{
    class Scene;
    class RenderContext;
} // namespace Falcor

/** Number of cards generated per supported static mesh instance (one per axis direction).
    cardIndex = instanceIndex * kLumenCardFaceCount + faceIndex, where instanceIndex is the
    LumenCardScene-internal instance ordinal (NOT the scene geometry instance ID).
*/
static constexpr uint32_t kLumenCardFaceCount = 6;

/** Sentinel for invalid mesh, instance, and card indices.
*/
static constexpr uint32_t kLumenCardInvalidID = 0xFFFFFFFFu;

/** Minimum world-space extent per AABB axis (meters) for a mesh to be card-izable.
    Meshes with an axis extent below this produce degenerate card projections and are
    marked unsupported (LumenCardUnsupportedReason::DegenerateBounds).
*/
static constexpr float kLumenCardMinExtent = 1e-4f;

// Card face index convention, consumed by the capture shader (Agent C) and the
// placement shader. Lumen convention: 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z.
// The face normal axis is faceIndex >> 1 (0=x, 1=y, 2=z) and the positive direction
// of the axis is encoded as (faceIndex & 1) == 0.

/** Per-card dirty bits. The host uploads metadata and re-captures a card while any bit
    is set, then clears the flags through clearDirty().
*/
enum class LumenCardDirtyFlags : uint32_t
{
    None = 0,
    Bounds = 1 << 0,   ///< World bounds/center/extent changed (new card or instance transform); re-upload metadata and re-capture.
    Material = 1 << 1, ///< Material, emissive, or displacement changed; re-capture. Bounds are unchanged.
};
FALCOR_ENUM_CLASS_OPERATORS(LumenCardDirtyFlags);

/** Reasons an instance is not card-ized. Stored as a bit mask in
    LumenCardInstance::unsupportedReasons; such instances must be routed to HWRT hit
    lighting by the host and never appear in the card buffer.
*/
enum class LumenCardUnsupportedReason : uint32_t
{
    None = 0,
    NotTriangleMesh = 1 << 0,  ///< Geometry type is not TriangleMesh or DisplacedTriangleMesh.
    Dynamic = 1 << 1,          ///< Instance or mesh is skinned or vertex-animated.
    DegenerateBounds = 1 << 2, ///< Local AABB is invalid or has an axis extent below kLumenCardMinExtent.
    MissingTransform = 1 << 3, ///< globalMatrixID is out of range of the global matrices.
};
FALCOR_ENUM_CLASS_OPERATORS(LumenCardUnsupportedReason);

/** One six-axis AABB card.

    The GPU layout is fixed at 96 bytes and must match the LumenCard struct in
    LumenCardPlacement.cs.slang byte for byte (offsets asserted below):
      +0   meshID (uint32), instanceID (uint32), faceIndex (uint32), dirtyFlags (uint32)
      +16  boundsMin.xyz, +32 boundsMax.xyz, +48 center.xyz, +64 extent.xyz
      +80  priority (float), +84 padding
    All float4 components except xyz are zero and unused.
*/
struct LumenCard
{
    uint32_t meshID = kLumenCardInvalidID;     ///< Scene mesh ID (GeometryInstanceData::geometryID for triangle meshes).
    uint32_t instanceID = kLumenCardInvalidID; ///< Scene geometry instance ID, equals the TLAS instance index.
    uint32_t faceIndex = kLumenCardInvalidID;  ///< Card face, 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z.
    uint32_t dirtyFlags = 0;                   ///< LumenCardDirtyFlags bits awaiting capture/upload.
    Falcor::float4 boundsMin = Falcor::float4(0.f); ///< World AABB min (xyz, meters; w unused).
    Falcor::float4 boundsMax = Falcor::float4(0.f); ///< World AABB max (xyz, meters; w unused).
    Falcor::float4 center = Falcor::float4(0.f);    ///< World AABB center (xyz, meters; w unused).
    Falcor::float4 extent = Falcor::float4(0.f);    ///< World AABB size (xyz, meters; w unused).
    float priority = 0.f;       ///< Capture priority, see the class documentation for the formula.
    float padding0 = 0.f;
    float padding1 = 0.f;
    float padding2 = 0.f;
};

static_assert(sizeof(LumenCard) == 96, "LumenCard must be 96 bytes to match the GPU layout in LumenCardPlacement.cs.slang");
static_assert(alignof(LumenCard) == 4, "LumenCard must stay at natural 4-byte alignment");
static_assert(offsetof(LumenCard, boundsMin) == 16, "LumenCard::boundsMin offset mismatch");
static_assert(offsetof(LumenCard, boundsMax) == 32, "LumenCard::boundsMax offset mismatch");
static_assert(offsetof(LumenCard, center) == 48, "LumenCard::center offset mismatch");
static_assert(offsetof(LumenCard, extent) == 64, "LumenCard::extent offset mismatch");
static_assert(offsetof(LumenCard, priority) == 80, "LumenCard::priority offset mismatch");

/** Per-scene-instance record forming the mesh/instance/card mapping table.
    The instance ordinal is the index into LumenCardScene::getInstance() and is
    unrelated to the scene geometry instance ID, which is stored in sceneInstanceID.
*/
struct LumenCardInstance
{
    uint32_t sceneInstanceID = kLumenCardInvalidID; ///< Scene geometry instance ID.
    uint32_t meshID = kLumenCardInvalidID;          ///< Scene mesh ID.
    uint32_t firstCardIndex = kLumenCardInvalidID;  ///< Index of the first card; valid only when supported.
    uint32_t unsupportedReasons = 0;                ///< LumenCardUnsupportedReason bits; nonzero when not card-ized.
    Falcor::AABB worldBounds;                       ///< Last known world AABB; invalid when unsupported.
    Falcor::float4x4 worldTransform = Falcor::float4x4::identity(); ///< Last known object-to-world transform.
};

/** CPU-side Lumen-style card placement for static triangle meshes.

    World conventions (shared with agents B and C, fixed here):
    - World space, Y-up, meters, matching the Falcor Scene conventions.
    - Card bounds are world-space AABBs stored as float3 min/max
      (boundsMin/boundsMax), matching Falcor's AABB (minPoint/maxPoint).
    - Card face orientation: faceIndex 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z. The card
      captures the mesh from the face normal direction.
    - In the MVP every static mesh instance generates exactly kLumenCardFaceCount
      cards; all six cards of an instance share the same world AABB of the instance
      (per-face slab refinement is future work).

    Indexing: cardIndex = instanceIndex * kLumenCardFaceCount + faceIndex, where
    instanceIndex is the ordinal of the LumenCardInstance record. The ordinal is the
    index of the instance in mInstances, which contains one record per scene geometry
    instance (supported and unsupported alike) in scene instance ID order, so
    getCardIndex() is deterministic and stable for a given scene. A meshID-based card
    lookup is intentionally not provided: a mesh can be instanced multiple times, so
    (meshID, faceIndex) does not identify a single card.

    Capture priority: priority = extent[t0] * extent[t1], where t0/t1 are the two AABB
    axes perpendicular to the card face normal (t0 = (axis + 1) % 3, t1 = (axis + 2) % 3).
    This is the world-space area (m^2) of the box face the card covers; cards with
    higher priority are captured first when the per-frame capture budget is limited.
    The value is clamped to a positive floor of kLumenCardMinExtent^2.

    Scene update semantics (update()):
    | UpdateFlags                                   | Action                                        |
    |-----------------------------------------------|-----------------------------------------------|
    | GeometryChanged                               | Full rebuild of the instance table and all cards; new cards are dirty Bounds+Material. |
    | MeshesChanged                                 | Full rebuild (conservative; skinning/vertex animations can flip the dynamic classification). |
    | MaterialsChanged / EmissiveMaterialsChanged / DisplacementChanged | Mark all cards dirty Material; bounds unchanged. |
    | SceneGraphChanged / GeometryMoved             | Recompute world bounds of instances whose AnimationController matrix changed or whose recomputed bounds differ; mark those cards dirty Bounds. |
    | CurvesMoved / CustomPrimitivesMoved / SDFGeometryChanged / SDFGridConfigChanged / Camera* / Lights* / EnvMap* / GridVolume* / RenderSettingsChanged / LightCollectionChanged / RecompileNeeded | Ignored (no cards for these geometry types; this class owns no GPU programs or screen resources). |

    update() must be called every frame with the IScene::UpdateFlags accumulated since
    the previous call (the same value the RenderPass receives from the scene update
    signal). Passing UpdateFlags::All on the first call is supported. The RenderContext
    parameter is currently unused (no GPU resources are created by this class) and may
    be null; it is part of the API for future GPU readback/resource work.

    This class creates no GPU resources. GPU buffers are created by the host from
    getCard()/getCardCount() using the 96-byte layout documented on LumenCard.
*/
class LumenCardScene
{
public:
    explicit LumenCardScene(const Falcor::ref<Falcor::Scene>& pScene);
    ~LumenCardScene();
    LumenCardScene(const LumenCardScene&) = delete;
    LumenCardScene& operator=(const LumenCardScene&) = delete;

    /** Apply scene updates since the last call. See the class documentation for the
        UpdateFlags mapping. Safe to call with null RenderContext.
    */
    void update(Falcor::RenderContext* pRenderContext, Falcor::IScene::UpdateFlags updateFlags);

    /** Total number of cards (kLumenCardFaceCount per supported instance). */
    uint32_t getCardCount() const { return (uint32_t)mCards.size(); }

    /** Get a card. Precondition: cardIndex < getCardCount(). */
    const LumenCard& getCard(uint32_t cardIndex) const;

    /** Map an instance ordinal and face index to a card index. Returns
        kLumenCardInvalidID when the instance is unsupported or faceIndex is invalid.
    */
    uint32_t getCardIndex(uint32_t instanceIndex, uint32_t faceIndex) const;

    /** Number of cards generated per static mesh (always kLumenCardFaceCount). */
    uint32_t getMeshCardCount() const { return kLumenCardFaceCount; }

    /** Number of instance records, always equal to Scene::getGeometryInstanceCount(). */
    uint32_t getInstanceCount() const { return (uint32_t)mInstances.size(); }

    /** Get an instance record. Precondition: instanceIndex < getInstanceCount(). */
    const LumenCardInstance& getInstance(uint32_t instanceIndex) const;

    /** Map a scene geometry instance ID to an instance ordinal, or -1 when unknown. */
    int32_t getInstanceIndex(uint32_t sceneInstanceID) const;

    /** Number of instances that are card-ized. */
    uint32_t getSupportedInstanceCount() const;

    /** Number of instances excluded from card placement. */
    uint32_t getUnsupportedInstanceCount() const;

    /** True when any card has pending dirty flags. */
    bool isDirty() const { return !mDirtyCardIndices.empty(); }

    /** Number of cards with pending dirty flags. */
    uint32_t getDirtyCardCount() const { return (uint32_t)mDirtyCardIndices.size(); }

    /** Card indices with pending dirty flags, sorted by priority descending
        (ties broken by card index). Consumed by the per-frame capture budget.
    */
    const std::vector<uint32_t>& getDirtyCardIndices() const { return mDirtyCardIndices; }

    /** Clear the dirty flags of a card after the host has captured/uploaded it.
        Rebuilds the dirty list; O(card count) per call in the MVP.
    */
    void clearDirty(uint32_t cardIndex);

private:
    void rebuild();
    void markAllCardsDirty(LumenCardDirtyFlags flags);
    void recomputeInstanceBounds();
    void rebuildDirtyList();

    Falcor::ref<Falcor::Scene> mpScene;
    std::vector<LumenCard> mCards;
    std::vector<LumenCardInstance> mInstances;
    std::vector<int32_t> mInstanceIndexBySceneID; ///< Scene geometry instance ID -> instance ordinal or -1.
    std::vector<uint32_t> mDirtyCardIndices;      ///< Sorted by priority descending.
    Falcor::IScene::UpdateFlags mSceneUpdates = Falcor::IScene::UpdateFlags::None;
    bool mInitialized = false;
};
