#include "ECS/SocketSystem.h"
#include "ECS/AnimationComponents.h"
#include "ECS/HierarchyComponents.h"
#include "Graphics/SkinningBuffers.h"
#include "Resource/SkeletonAsset.h"
#include "System/Log.h"

#include <DirectXMath.h>

#include <cstring>

using namespace DirectX;

namespace
{
    // modelBone = bindPose * skin (row vectors: bindPose first, then skin cancels the invBind it
    // embeds, leaving the bone's current model-space transform).
    inline XMMATRIX RecoverModelBone(const XMFLOAT4X4& bindPose, const XMFLOAT4X4& skin)
    {
        return XMLoadFloat4x4(&bindPose) * XMLoadFloat4x4(&skin);
    }

    bool BoneIs(const SkeletonAsset& asset, uint32_t b, const char* name)
    {
        return std::strncmp(asset.boneNames[b], name, sizeof(asset.boneNames[b])) == 0;
    }

    // A socket names its bone; the index caches that name. Re-resolved when the index no longer names
    // the bone (a re-import reordered the skeleton); a socket without a name gets it from a valid
    // index, so the next save names the bone. A name the skeleton lacks: the socket is not placed.
    void ResolveSocketBones(SocketComponent& sc, const SkeletonAsset& asset, Entity e)
    {
        constexpr size_t kName = sizeof(SocketComponent::Socket::boneName);
        for (uint32_t i = 0; i < sc.count; ++i)
        {
            SocketComponent::Socket& s = sc.sockets[i];
            s.flags &= static_cast<uint8_t>(~SocketComponent::kBoneMissing);
            if (s.boneName[0] == '\0')
            {
                if (s.boneIndex < asset.boneCount)
                {
                    const size_t n = strnlen(asset.boneNames[s.boneIndex], kName - 1);
                    std::memcpy(s.boneName, asset.boneNames[s.boneIndex], n);
                    s.boneName[n] = '\0';
                }
                continue;
            }
            if (s.boneIndex < asset.boneCount && BoneIs(asset, s.boneIndex, s.boneName)) continue;
            uint32_t found = ~0u;
            for (uint32_t b = 0; b < asset.boneCount && found == ~0u; ++b)
                if (BoneIs(asset, b, s.boneName)) found = b;
            if (found != ~0u)
            {
                s.boneIndex = found;
                continue;
            }
            s.flags |= SocketComponent::kBoneMissing;
            LOG_WARNING("SocketSystem: entity %u socket '%.64s' names bone '%.64s', which its skeleton does not "
                        "have - the socket is not placed", e, s.name, s.boneName);
        }
    }
}

void SocketSystem::Update(World& world)
{
    UpdateSkeletonOwners(world);
    UpdateOwnersWithoutSkeleton(world);
}

// Sockets of an entity without a skeleton sit at offset x its world. Callers skip an entity the
// skeleton pass owns (its sockets are on bones).
namespace
{
    void PlaceOnEntity(World& world, Entity e, SocketComponent& sc)
    {
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        const XMMATRIX entWorld = gt ? XMLoadFloat4x4(&gt->matrix) : XMMatrixIdentity();
        for (uint32_t k = 0; k < sc.count; ++k)
        {
            SocketComponent::Socket& s = sc.sockets[k];
            XMStoreFloat4x4(&s.worldTransform, XMLoadFloat4x4(&s.localOffset) * entWorld);
        }
    }
}

void SocketSystem::UpdateOwnersWithoutSkeleton(World& world)
{
    auto* sockPool = world.GetPool<SocketComponent>();
    if (!sockPool) return;
    auto*       skelPool = world.GetPool<SkeletonComponent>();
    const auto& ents     = sockPool->Entities();
    auto&       data     = sockPool->Data();
    for (std::size_t i = 0; i < ents.size(); ++i)
    {
        const Entity e = ents[i];
        if (skelPool && skelPool->Has(e)) continue;
        if (!world.IsAlive(e)) continue;
        PlaceOnEntity(world, e, data[i]);
    }
}

void SocketSystem::PlaceWithoutSkeleton(World& world, Entity e)
{
    SocketComponent* sc = world.GetComponent<SocketComponent>(e);
    if (!sc || sc->count == 0 || world.HasComponent<SkeletonComponent>(e)) return;
    PlaceOnEntity(world, e, *sc);
}

void SocketSystem::UpdateSkeletonOwners(World& world)
{
    // Walk the SkeletonComponent pool, not the whole world: GetEntities() + GetComponent costs one
    // unordered_map lookup per entity (15,000+ per frame on an imported city scene). Nothing below
    // mutates the skeleton pool, so indexing it directly is safe.
    auto* skelPool = world.GetPool<SkeletonComponent>();
    if (!skelPool) return;
    const auto& skelEnts = skelPool->Entities();
    const auto& skelData = skelPool->Data();

    for (std::size_t si = 0; si < skelEnts.size(); ++si)
    {
        const Entity e = skelEnts[si];
        if (!world.IsAlive(e)) continue;

        const SkeletonComponent* skel = &skelData[si];
        if (skel->assetIndex == kInvalidAnimHandle) continue;
        // ~0u is the only sentinel for "no pose this frame"; 0 IS a valid byte offset.
        if (skel->poseByteOffset == ~0u) continue;

        const XMFLOAT4X4* skinMatrices = m_poseBuffer.ReadMapped(skel->poseByteOffset);
        if (!skinMatrices) continue;

        const SkeletonAsset& asset = m_skeletons.Get(skel->assetIndex);

        const GlobalTransform* rootGT = world.GetComponent<GlobalTransform>(e);
        const XMMATRIX rootWorld = rootGT
            ? XMLoadFloat4x4(&rootGT->matrix)
            : XMMatrixIdentity();

        // Socket definitions live on the entity (not the asset): iterate whatever SocketComponent::Add or
        // the editor published. Each socket caches its world transform in place, so FollowSystem only
        // reads the cache (no pose-buffer traversal on the follower side).
        SocketComponent* sc = world.GetComponent<SocketComponent>(e);
        if (!sc) continue;

        if (sc->boneResolvedSkel != skel->assetIndex || sc->boneResolvedGen != sc->generation)
        {
            ResolveSocketBones(*sc, asset, e);
            sc->boneResolvedSkel = skel->assetIndex;
            sc->boneResolvedGen  = sc->generation;
        }

        for (uint32_t i = 0; i < sc->count; ++i)
        {
            SocketComponent::Socket& s = sc->sockets[i];
            if (s.boneIndex >= skel->boneCount || (s.flags & SocketComponent::kBoneMissing))
            {
                // Its bone is gone (a re-import renamed it): place it on the character (offset x its world), not
                // at the identity, which would put an equipped weapon and a hitbox at the world origin. Lua
                // Skeleton.SocketWorld answers nil for it; the warning named it.
                XMStoreFloat4x4(&s.worldTransform, XMLoadFloat4x4(&s.localOffset) * rootWorld);
                continue;
            }

            const XMMATRIX modelBone = RecoverModelBone(
                asset.bindPose[s.boneIndex], skinMatrices[s.boneIndex]);

            const XMMATRIX offset     = XMLoadFloat4x4(&s.localOffset);
            const XMMATRIX worldMat   = offset * modelBone * rootWorld;
            XMStoreFloat4x4(&s.worldTransform, worldMat);
        }
    }
}
