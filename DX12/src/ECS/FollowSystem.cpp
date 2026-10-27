#include "ECS/FollowSystem.h"
#include "ECS/FollowComponents.h"
#include "ECS/FollowEvents.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/SocketSystem.h"   // SocketSystem::PlaceWithoutSkeleton: a moved follower's own sockets
#include "Scene/TransformSystem.h"
#include "System/EventBus.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <algorithm>
#include <vector>

using namespace DirectX;

namespace
{
    // follower.Global = follower.Local * attachParentWorld: the same rule TransformSystem::Propagate
    // applies to a real child.
    //
    // LocalTransform is read, never written: writing the composed world matrix back into it would
    // overwrite the author's scale with the target chain's and fight Propagate and the editor gizmo.
    inline void ApplyFollow(World& world, Entity follower, FXMMATRIX attachParentWorld)
    {
        GlobalTransform* gt = world.GetComponent<GlobalTransform>(follower);
        if (!gt) return;

        const LocalTransform* lt = world.GetComponent<LocalTransform>(follower);
        const XMMATRIX local = lt ? lt->ToMatrix() : XMMatrixIdentity();
        XMStoreFloat4x4(&gt->matrix, local * attachParentWorld);

        // The frame's Propagate ran back in PhysicsInterpolation, so a follower's
        // own children still hold a world matrix composed against the follower's
        // pre-follow GlobalTransform. Refresh them here, the way
        // PhysicsSystem::ApplyRenderInterpolation does after it writes out of band.
        if (world.HasComponent<Children>(follower))
            TransformSystem::PropagateSubtree(world, static_cast<unsigned int>(follower));
    }

    // Re-resolve the follower's socket index from its name hash whenever the target's socket list
    // changed shape. Returns SocketComponent::kNotFound if the named socket is gone; the caller then
    // skips the follower rather than snapping it onto whatever entry inherited the old index.
    uint32_t ResolveSocketIndex(const SocketComponent& sc, FollowSocketComponent& fs)
    {
        // Index-addressed (the VFX bone path, an asset without a hash): the entity's own sockets only.
        // They come first; the model's derived sockets after them move with every model edit, so a
        // notify's default index 0 would otherwise take the first derived socket.
        if (fs.socketNameHash == 0u)
            return (fs.socketIndex < sc.AuthoredCount()) ? fs.socketIndex : SocketComponent::kNotFound;

        if (fs.cachedSocketGen == sc.generation && fs.socketIndex < sc.count)
            return fs.socketIndex;

        const uint32_t idx = sc.FindByName(fs.socketNameHash);
        if (idx != SocketComponent::kNotFound)
        {
            fs.socketIndex     = idx;
            fs.cachedSocketGen = sc.generation;
        }
        return idx;
    }

    // Resolve the target, publishing FollowTargetLostEvent once on the valid -> invalid transition
    // and clearing the ref so the next frame does not re-fire. Returns NullEntity when the follower
    // should be skipped. One transition event, then the follower stays orphaned until a subscriber
    // (e.g. the equipment manager) re-assigns or deletes it.
    //
    // `cachedOwner` before Resolve tells the two failures apart: never resolved (NullEntity) means
    // the target is not spawned yet - a scene may deserialize a follower before its target - so the
    // ref survives untouched. Only a ref that resolved before is a lost target.
    Entity ResolveTarget(Entity follower, AttachmentRef& target, World& world)
    {
        const EntityHandle prior{ target.cachedOwner, target.cachedGeneration };

        if (const Entity e = target.Resolve(world); e != NullEntity)
            return e;

        if (prior.entity != NullEntity)
        {
            EventBus::Get().Publish(FollowTargetLostEvent{ follower, prior });
            target.Clear();
        }
        return NullEntity;
    }

    // SocketSystem placed the sockets of an entity without a skeleton (a StaticMesh owner's mesh
    // sockets) from its GlobalTransform before this system ran: for a follower, or anything under
    // one, that was the pre-follow matrix. After ApplyFollow moved @p root and propagated its
    // subtree, place them again. Bounded: a held prop is a handful of entities.
    void RefreshSocketsUnder(World& world, Entity root)
    {
        auto* sockPool = world.GetPool<SocketComponent>();
        if (!sockPool || sockPool->Entities().empty()) return;
        thread_local std::vector<Entity> t_stack;
        t_stack.clear();
        t_stack.push_back(root);
        for (int visited = 0; !t_stack.empty() && visited < 4096; ++visited)
        {
            const Entity e = t_stack.back();
            t_stack.pop_back();
            if (sockPool->Has(e)) SocketSystem::PlaceWithoutSkeleton(world, e);
            if (const Children* ch = world.GetComponent<Children>(e))
                t_stack.insert(t_stack.end(), ch->entities.begin(), ch->entities.end());
        }
    }

    // Per-entity scratch marks for the dependency-ordered walk below.
    enum FollowMark : uint8_t
    {
        kIsFollower = 1u << 0,   // owns FollowEntity and/or FollowSocket
        kApplied    = 1u << 1,   // ApplyFollow already ran for it this frame
        kOnPath     = 1u << 2,   // sits on the chain currently being walked
    };

    // How far up a Parent chain to look for the follower that owns a target's world matrix, and how
    // long a follow chain may get before it counts as a cycle. Sanity bounds, not real limits:
    // attachment chains are 2-3 links in practice (character -> weapon -> trail).
    constexpr int kMaxAttachDepth = 32;
}

// Followers are resolved in dependency order, not pool order.
//
// TransformSystem::Propagate (PhysicsInterpolation, the phase before this one) writes
// Global = Local on every parentless entity, followers included, so a follower's GlobalTransform
// is its raw LocalTransform until its own ApplyFollow has run. In pool order a chained follower
// (a trail bound by FollowEntity to a weapon root that follows a hand socket) would compose
// against that constant raw matrix and never move.
//
// Each follower has at most one attach dependency (a functional graph, out-degree <= 1): walk each
// chain to its deepest unresolved link and apply back up. O(followers), no sort, no recursion.
void FollowSystem::Update(World& world)
{
    auto* socketPool = world.GetPool<FollowSocketComponent>();
    auto* entityPool = world.GetPool<FollowEntityComponent>();
    auto* parentPool = world.GetPool<Parent>();

    const size_t socketN = socketPool ? socketPool->Entities().size() : 0;
    const size_t entityN = entityPool ? entityPool->Entities().size() : 0;
    if (socketN == 0 && entityN == 0)
        return;

    // Main-thread only (FollowSystem::Update is serial), reused across frames
    // so the steady state allocates nothing.
    thread_local std::vector<Entity>  t_roster;
    thread_local std::vector<uint8_t> t_mark;    // indexed by entity id
    thread_local std::vector<Entity>  t_path;

    t_roster.clear();
    t_roster.reserve(socketN + entityN);
    for (size_t i = 0; i < socketN; ++i) t_roster.push_back(socketPool->Entities()[i]);
    for (size_t i = 0; i < entityN; ++i) t_roster.push_back(entityPool->Entities()[i]);

    t_mark.assign(world.EntityIdBound(), 0u);
    for (const Entity e : t_roster)
        if (static_cast<size_t>(e) < t_mark.size())
            t_mark[e] |= kIsFollower;

    // The follower that must be applied before `e`: whoever owns the world matrix `e` composes
    // against.
    //
    // A FollowEntity follower depends on the nearest follower at or above its target: the target
    // itself when it follows, otherwise the follower ancestor whose ApplyFollow -> PropagateSubtree
    // writes the target's matrix. A FollowSocket follower of a skinned owner has none: it reads
    // SocketSystem's per-frame cache, computed from the pose before this system ran. One of an owner
    // without a skeleton (a StaticMesh owner's mesh sockets: a muzzle flash on a held weapon) depends
    // like a FollowEntity one: those sockets are the owner's world x offset, final only once the
    // owner (or the follower above it) has moved and RefreshSocketsUnder placed them.
    auto dependencyOf = [&](Entity e) -> Entity
    {
        // Probe only: plain Resolve, never ResolveTarget - an ordering query must not publish
        // FollowTargetLostEvent or clear the ref.
        Entity target = NullEntity;
        if (FollowEntityComponent* fe = entityPool ? entityPool->Get(e) : nullptr)
            target = fe->target.Resolve(world);
        else if (FollowSocketComponent* fs = socketPool ? socketPool->Get(e) : nullptr)
        {
            const Entity owner = fs->target.Resolve(world);
            if (owner != NullEntity && !world.HasComponent<SkeletonComponent>(owner)) target = owner;
        }
        if (target == NullEntity) return NullEntity;

        Entity a = target;
        for (int depth = 0; a != NullEntity && depth < kMaxAttachDepth; ++depth)
        {
            if (static_cast<size_t>(a) < t_mark.size() && (t_mark[a] & kIsFollower))
                return (a == e) ? NullEntity : a;   // self-follow: no dependency
            const Parent* p = parentPool ? parentPool->Get(a) : nullptr;
            a = p ? p->entity : NullEntity;
        }
        return NullEntity;
    };

    // An entity with both kinds applies FollowEntity first, then FollowSocket (the socket write lands
    // last and wins): the precedence Follow::ResolveAttachParentWorld reports to the gizmo.
    auto applyOne = [&](Entity follower)
    {
        bool moved = false;
        if (FollowEntityComponent* fe = entityPool ? entityPool->Get(follower) : nullptr)
        {
            const Entity target = ResolveTarget(follower, fe->target, world);
            if (target != NullEntity)
            {
                if (const GlobalTransform* targetGT = world.GetComponent<GlobalTransform>(target))
                {
                    ApplyFollow(world, follower, XMLoadFloat4x4(&targetGT->matrix));
                    moved = true;
                }
            }
        }

        if (FollowSocketComponent* fs = socketPool ? socketPool->Get(follower) : nullptr)
        {
            const Entity target = ResolveTarget(follower, fs->target, world);
            const SocketComponent* sc = target != NullEntity ? world.GetComponent<SocketComponent>(target) : nullptr;
            const uint32_t idx = sc ? ResolveSocketIndex(*sc, *fs) : SocketComponent::kNotFound;
            if (idx != SocketComponent::kNotFound)
            {
                ApplyFollow(world, follower, XMLoadFloat4x4(&sc->sockets[idx].worldTransform));
                moved = true;
            }
        }
        // The moved subtree's own no-skeleton sockets, before anything following them.
        if (moved) RefreshSocketsUnder(world, follower);
    };

    bool warnedCycle = false;

    for (const Entity seed : t_roster)
    {
        if (static_cast<size_t>(seed) >= t_mark.size()) continue;
        if (t_mark[seed] & kApplied)                    continue;   // second pool hit

        t_path.clear();
        Entity cur = seed;
        while (cur != NullEntity && static_cast<size_t>(cur) < t_mark.size() &&
               (t_mark[cur] & kIsFollower) && !(t_mark[cur] & kApplied))
        {
            if (t_mark[cur] & kOnPath)
            {
                // A follows B follows A. Applying what we have still leaves
                // both entities somewhere sane (one frame stale); the point is
                // to not spin forever.
                if (!warnedCycle)
                {
                    LOG_WARNING("FollowSystem: attachment cycle through entity %u - "
                                "breaking the chain, one link will lag a frame", cur);
                    warnedCycle = true;
                }
                break;
            }
            t_mark[cur] |= kOnPath;
            t_path.push_back(cur);
            cur = dependencyOf(cur);
        }

        // Deepest dependency first.
        for (size_t i = t_path.size(); i-- > 0; )
        {
            const Entity f = t_path[i];
            applyOne(f);
            t_mark[f] = static_cast<uint8_t>((t_mark[f] & ~kOnPath) | kApplied);
        }
    }
}

namespace Follow
{
    // Twin of FollowSystem::Update's resolution minus the target-lost event and ref clearing: an
    // editor query must never orphan an attachment just by looking at it.
    bool ResolveAttachParentWorld(World& world, Entity e, XMMATRIX& out)
    {
        if (FollowSocketComponent* fs = world.GetComponent<FollowSocketComponent>(e))
        {
            const Entity target = fs->target.Resolve(world);
            if (target == NullEntity) return false;
            const SocketComponent* sc = world.GetComponent<SocketComponent>(target);
            if (!sc) return false;
            const uint32_t idx = ResolveSocketIndex(*sc, *fs);
            if (idx == SocketComponent::kNotFound) return false;
            out = XMLoadFloat4x4(&sc->sockets[idx].worldTransform);
            return true;
        }

        if (const FollowEntityComponent* fe = world.GetComponent<FollowEntityComponent>(e))
        {
            const Entity target = fe->target.Resolve(world);
            if (target == NullEntity) return false;
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(target);
            if (!gt) return false;
            out = XMLoadFloat4x4(&gt->matrix);
            return true;
        }

        return false;
    }
}
