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

#include "LumenCardScene.h"

#include "Core/Error.h"
#include <algorithm>

namespace
{
/** Capture priority of a card face: the world-space area of the box face the card
    covers, i.e. the product of the two AABB extents perpendicular to the face normal.
    Clamped to a positive floor so every supported card has a usable priority.
*/
float computeCardPriority(uint32_t faceIndex, const Falcor::float3& extent)
{
    const uint32_t axis = faceIndex >> 1;
    const uint32_t tangentAxis0 = (axis + 1) % 3;
    const uint32_t tangentAxis1 = (axis + 2) % 3;
    const float area = extent[(int)tangentAxis0] * extent[(int)tangentAxis1];
    return std::max(area, kLumenCardMinExtent * kLumenCardMinExtent);
}

bool isTriangleMesh(Falcor::Scene::GeometryType type)
{
    return type == Falcor::Scene::GeometryType::TriangleMesh || type == Falcor::Scene::GeometryType::DisplacedTriangleMesh;
}
} // namespace

LumenCardScene::LumenCardScene(const Falcor::ref<Falcor::Scene>& pScene) : mpScene(pScene)
{
    rebuild();
}

LumenCardScene::~LumenCardScene() = default;

void LumenCardScene::update(Falcor::RenderContext* /*pRenderContext*/, Falcor::IScene::UpdateFlags updateFlags)
{
    if (!mpScene)
        return;

    mSceneUpdates |= updateFlags;

    if (!mInitialized ||
        is_set(mSceneUpdates, Falcor::IScene::UpdateFlags::GeometryChanged) ||
        is_set(mSceneUpdates, Falcor::IScene::UpdateFlags::MeshesChanged))
    {
        rebuild();
        mSceneUpdates = Falcor::IScene::UpdateFlags::None;
        rebuildDirtyList();
        return;
    }

    if (is_set(mSceneUpdates, Falcor::IScene::UpdateFlags::MaterialsChanged) ||
        is_set(mSceneUpdates, Falcor::IScene::UpdateFlags::EmissiveMaterialsChanged) ||
        is_set(mSceneUpdates, Falcor::IScene::UpdateFlags::DisplacementChanged))
    {
        markAllCardsDirty(LumenCardDirtyFlags::Material);
    }

    if (is_set(mSceneUpdates, Falcor::IScene::UpdateFlags::SceneGraphChanged) ||
        is_set(mSceneUpdates, Falcor::IScene::UpdateFlags::GeometryMoved))
    {
        recomputeInstanceBounds();
    }

    mSceneUpdates = Falcor::IScene::UpdateFlags::None;
    rebuildDirtyList();
}

const LumenCard& LumenCardScene::getCard(uint32_t cardIndex) const
{
    FALCOR_ASSERT(cardIndex < mCards.size());
    return mCards[cardIndex];
}

uint32_t LumenCardScene::getCardIndex(uint32_t instanceIndex, uint32_t faceIndex) const
{
    if (instanceIndex >= mInstances.size() || faceIndex >= kLumenCardFaceCount)
        return kLumenCardInvalidID;
    return mInstances[instanceIndex].firstCardIndex == kLumenCardInvalidID
        ? kLumenCardInvalidID
        : mInstances[instanceIndex].firstCardIndex + faceIndex;
}

const LumenCardInstance& LumenCardScene::getInstance(uint32_t instanceIndex) const
{
    FALCOR_ASSERT(instanceIndex < mInstances.size());
    return mInstances[instanceIndex];
}

int32_t LumenCardScene::getInstanceIndex(uint32_t sceneInstanceID) const
{
    if (sceneInstanceID >= mInstanceIndexBySceneID.size())
        return -1;
    return mInstanceIndexBySceneID[sceneInstanceID];
}

uint32_t LumenCardScene::getSupportedInstanceCount() const
{
    uint32_t count = 0;
    for (const auto& instance : mInstances)
    {
        if (instance.unsupportedReasons == 0)
            ++count;
    }
    return count;
}

uint32_t LumenCardScene::getUnsupportedInstanceCount() const
{
    return (uint32_t)mInstances.size() - getSupportedInstanceCount();
}

void LumenCardScene::clearDirty(uint32_t cardIndex)
{
    if (cardIndex >= mCards.size())
        return;
    mCards[cardIndex].dirtyFlags = 0;
    rebuildDirtyList();
}

void LumenCardScene::rebuild()
{
    mCards.clear();
    mInstances.clear();
    mInstanceIndexBySceneID.clear();

    if (!mpScene)
    {
        mInitialized = true;
        return;
    }

    const Falcor::Scene* pScene = mpScene.get();
    const Falcor::AnimationController* pAnimationController = pScene->getAnimationController();
    std::vector<Falcor::float4x4> emptyMatrices;
    const std::vector<Falcor::float4x4>& globalMatrices = pAnimationController ? pAnimationController->getGlobalMatrices() : emptyMatrices;

    const uint32_t instanceCount = pScene->getGeometryInstanceCount();
    mInstances.reserve(instanceCount);
    mInstanceIndexBySceneID.assign(instanceCount, -1);

    for (uint32_t sceneInstanceID = 0; sceneInstanceID < instanceCount; ++sceneInstanceID)
    {
        const Falcor::GeometryInstanceData& instance = pScene->getGeometryInstance(sceneInstanceID);
        mInstanceIndexBySceneID[sceneInstanceID] = (int32_t)mInstances.size();

        LumenCardInstance record;
        record.sceneInstanceID = sceneInstanceID;
        record.meshID = instance.geometryID;
        record.worldBounds.invalidate();

        if (!isTriangleMesh(instance.getType()))
        {
            record.unsupportedReasons |= (uint32_t)LumenCardUnsupportedReason::NotTriangleMesh;
        }
        else if (instance.globalMatrixID >= globalMatrices.size())
        {
            record.unsupportedReasons |= (uint32_t)LumenCardUnsupportedReason::MissingTransform;
        }
        else
        {
            const Falcor::float4x4& transform = globalMatrices[instance.globalMatrixID];
            const Falcor::MeshDesc& mesh = pScene->getMesh(Falcor::MeshID::fromSlang(instance.geometryID));
            if (instance.isDynamic() || mesh.isDynamic())
            {
                record.unsupportedReasons |= (uint32_t)LumenCardUnsupportedReason::Dynamic;
            }
            else
            {
                const Falcor::AABB worldBounds = pScene->getMeshBounds(instance.geometryID).transform(transform);
                const Falcor::float3 extent = worldBounds.extent();
                const bool degenerate = !worldBounds.valid() ||
                    extent.x < kLumenCardMinExtent || extent.y < kLumenCardMinExtent || extent.z < kLumenCardMinExtent;
                if (degenerate)
                {
                    record.unsupportedReasons |= (uint32_t)LumenCardUnsupportedReason::DegenerateBounds;
                }
                else
                {
                    record.worldBounds = worldBounds;
                    record.worldTransform = transform;
                    record.firstCardIndex = (uint32_t)mCards.size();
                    for (uint32_t faceIndex = 0; faceIndex < kLumenCardFaceCount; ++faceIndex)
                    {
                        LumenCard card;
                        card.meshID = instance.geometryID;
                        card.instanceID = sceneInstanceID;
                        card.faceIndex = faceIndex;
                        card.dirtyFlags = (uint32_t)LumenCardDirtyFlags::Bounds | (uint32_t)LumenCardDirtyFlags::Material;
                        card.boundsMin = Falcor::float4(worldBounds.minPoint, 0.f);
                        card.boundsMax = Falcor::float4(worldBounds.maxPoint, 0.f);
                        card.center = Falcor::float4(worldBounds.center(), 0.f);
                        card.extent = Falcor::float4(extent, 0.f);
                        card.priority = computeCardPriority(faceIndex, extent);
                        mCards.push_back(card);
                    }
                }
            }
        }
        mInstances.push_back(record);
    }

    mInitialized = true;
}

void LumenCardScene::markAllCardsDirty(LumenCardDirtyFlags flags)
{
    for (auto& card : mCards)
    {
        card.dirtyFlags |= (uint32_t)flags;
    }
}

void LumenCardScene::recomputeInstanceBounds()
{
    if (!mpScene)
        return;
    const Falcor::AnimationController* pAnimationController = mpScene->getAnimationController();
    if (!pAnimationController)
        return;
    const std::vector<Falcor::float4x4>& globalMatrices = pAnimationController->getGlobalMatrices();

    for (size_t i = 0; i < mInstances.size(); ++i)
    {
        LumenCardInstance& record = mInstances[i];
        if (record.firstCardIndex == kLumenCardInvalidID)
            continue;

        const Falcor::GeometryInstanceData& instance = mpScene->getGeometryInstance(record.sceneInstanceID);
        if (instance.globalMatrixID >= globalMatrices.size())
            continue;
        const Falcor::float4x4& transform = globalMatrices[instance.globalMatrixID];
        const Falcor::AABB worldBounds = mpScene->getMeshBounds(record.meshID).transform(transform);
        const bool matrixChanged = pAnimationController->isMatrixChanged(Falcor::NodeID{ instance.globalMatrixID });
        if (!matrixChanged && worldBounds == record.worldBounds)
            continue;

        record.worldBounds = worldBounds;
        record.worldTransform = transform;
        const Falcor::float3 extent = worldBounds.extent();
        for (uint32_t faceIndex = 0; faceIndex < kLumenCardFaceCount; ++faceIndex)
        {
            LumenCard& card = mCards[record.firstCardIndex + faceIndex];
            card.boundsMin = Falcor::float4(worldBounds.minPoint, 0.f);
            card.boundsMax = Falcor::float4(worldBounds.maxPoint, 0.f);
            card.center = Falcor::float4(worldBounds.center(), 0.f);
            card.extent = Falcor::float4(extent, 0.f);
            card.priority = computeCardPriority(faceIndex, extent);
            card.dirtyFlags |= (uint32_t)LumenCardDirtyFlags::Bounds;
        }
    }
}

void LumenCardScene::rebuildDirtyList()
{
    mDirtyCardIndices.clear();
    for (uint32_t i = 0; i < (uint32_t)mCards.size(); ++i)
    {
        if (mCards[i].dirtyFlags != 0)
            mDirtyCardIndices.push_back(i);
    }
    std::sort(
        mDirtyCardIndices.begin(),
        mDirtyCardIndices.end(),
        [this](uint32_t lhs, uint32_t rhs)
        {
            const float lhsPriority = mCards[lhs].priority;
            const float rhsPriority = mCards[rhs].priority;
            if (lhsPriority != rhsPriority)
                return lhsPriority > rhsPriority;
            return lhs < rhs;
        }
    );
}
