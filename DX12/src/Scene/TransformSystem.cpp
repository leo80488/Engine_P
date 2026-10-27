#include "Scene/TransformSystem.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "System/TaskSystem.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    // ==== Dirty tracking ====
    // Propagate runs every frame, idle editor included (RunMask::Always), so recomposing every
    // GlobalTransform and re-transforming every WorldAabb's 8 corners when nothing moved is
    // pure waste (~57k XMVector3TransformCoord per frame on a ~13k-entity imported city).
    // The cache stores, per entity, the exact bytes of every input of the last computation
    // plus the bytes written; a visit is skipped only when all still match (see UpdateNode).
    // Value-based on purpose: LocalTransform has ~40 writers and GlobalTransform a handful
    // more, and one writer forgetting a dirty flag would be a silent stale-transform bug.
    // Comparing bytes cannot miss a writer.
    struct XformCacheEntry
    {
        LocalTransform      lt{};                    // input:  local TRS
        DirectX::XMFLOAT4X4 parentGlobal{};          // input:  matrix we composed AGAINST
        DirectX::XMFLOAT4X4 global{};                // output: what WE last wrote
        LocalAabb           laabb{};                 // input:  model-space bounds
        WorldAabb           waabb{};                 // output: what WE last wrote
        Entity              parent     = NullEntity; // input:  topology (reparent)
        uint32_t            generation = 0;          // guard:  entity-id recycling
        uint8_t             valid      = 0;
        uint8_t             hasLocal   = 0;
        uint8_t             hasAabb    = 0;
        uint8_t             _pad       = 0;
    };
    // Members are compared individually, never as a whole struct, so the padding above never
    // enters a memcmp.

    struct XformCache
    {
        const World* owner = nullptr;
        uint32_t     epoch = 0;                 // World::Epoch(), bumped by Clear()
        std::vector<XformCacheEntry> entries;   // indexed by entity id
    };

    // Main-thread owned. Sized (and cleared) in Propagate's serial prologue and never resized
    // once the fan-out starts: workers write the entries of their own disjoint subtree, which
    // needs a stable buffer. Not thread_local (the BFS scratch vectors are): per-thread copies
    // would make the parallel path compare against an empty baseline.
    XformCache g_xformCache;

    // Pool pointers resolved once per Propagate: World::GetComponent<T> is an unordered_map
    // lookup by type_index per call (~14% of CPU in the inner loop).
    struct XformPools
    {
        World*                              world    = nullptr;
        ComponentPool<GlobalTransform>*     gt       = nullptr;
        ComponentPool<LocalTransform>*      lt       = nullptr;
        ComponentPool<Children>*            children = nullptr;
        ComponentPool<VisibilityComponent>* vis      = nullptr;
        ComponentPool<LocalAabb>*           laabb    = nullptr;
        ComponentPool<WorldAabb>*           waabb    = nullptr;
    };

    enum class NodeResult : uint8_t
    {
        Ok,
        NeedsAdd,   // no GlobalTransform entry yet; caller must use Add()
    };

    inline XformCacheEntry* CacheFor(Entity e)
    {
        return (static_cast<size_t>(e) < g_xformCache.entries.size())
            ? &g_xformCache.entries[e]
            : nullptr;   // out of range -> always recompute (fail-safe direction)
    }

    // Transform a LocalAabb by worldMatrix into worldAabb. Shared by the serial and
    // parallel paths so the math can never diverge between them.
    inline void UpdateWorldAabb(const LocalAabb& localAabb, WorldAabb& worldAabb,
                                const DirectX::XMMATRIX& worldMatrix)
    {
        using namespace DirectX;
        const float corners[8][3] =
        {
            {localAabb.min.x, localAabb.min.y, localAabb.min.z},
            {localAabb.max.x, localAabb.min.y, localAabb.min.z},
            {localAabb.min.x, localAabb.max.y, localAabb.min.z},
            {localAabb.max.x, localAabb.max.y, localAabb.min.z},
            {localAabb.min.x, localAabb.min.y, localAabb.max.z},
            {localAabb.max.x, localAabb.min.y, localAabb.max.z},
            {localAabb.min.x, localAabb.max.y, localAabb.max.z},
            {localAabb.max.x, localAabb.max.y, localAabb.max.z},
        };

        XMVECTOR vmin = XMVectorSet(FLT_MAX,  FLT_MAX,  FLT_MAX,  0.f);
        XMVECTOR vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0.f);
        for (const auto& c : corners)
        {
            XMVECTOR v = XMVector3TransformCoord(
                XMVectorSet(c[0], c[1], c[2], 1.f), worldMatrix);
            vmin = XMVectorMin(vmin, v);
            vmax = XMVectorMax(vmax, v);
        }
        XMStoreFloat3(&worldAabb.min, vmin);
        XMStoreFloat3(&worldAabb.max, vmax);
    }

    // The one place a node is brought up to date; every propagation path calls it so the math
    // and the skip rule cannot diverge.
    // @p parentMat == nullptr: @p node is a root; global = local, no inherited hidden state,
    // and Propagate does not own its WorldAabb. Otherwise it points at a caller-owned copy of
    // the parent's matrix, never into the GlobalTransform pool (allowAdd can realloc it).
    // @p allowAdd is true only on serial paths, where growing a ComponentPool is legal (a
    // realloc while workers read the pool is UB).
    //
    // The skip is taken only when every input is bit-identical to the one that produced the
    // cached output and the stored outputs are still exactly the bytes written:
    //   (a) parent matrix == c->parentGlobal, compared by value
    //   (b) same parent entity: catches reparents that keep LocalTransform
    //   (c) LocalTransform bit-identical: catches all ~40 LT writers
    //   (d) stored GlobalTransform == cache: catches out-of-band GT writers (FollowSystem,
    //       physics interpolation, AimLightEntity)
    //   (e) LocalAabb identical and stored WorldAabb == cache: Renderer::TickAnimationChain's
    //       bone-AABB merge writes WorldAabb out of band
    //   (f) same entity generation: entity ids are recycled
    // All are re-read from live component bytes on every visit, never assuming the entity was
    // visited last frame; under all of them the recompose reproduces the stored bytes, and a
    // miss falls back to the full recompute. The cache can cost time, never correctness.
    // (a) is a cached matrix, not a "parent moved" flag: a flag only holds if this entity was
    // visited every frame its parent was. Undo of a delete breaks that (CreateEntityWithId
    // restores the original generation and RestoreEntityComponents the exact bytes), as does a
    // detach / re-attach while the parent moves; the child would keep a stale matrix forever.
    // Do not prune a subtree under a clean parent: ApplyRenderInterpolation and FollowSystem
    // write a parent's GT and call PropagateSubtree, which bypasses this cache; next frame only
    // the descendant's own check (d) notices.
    // inheritedHidden is outside the skip: its inputs (the parent's flags and inheritedHidden)
    // do not depend on transforms, and gating it would latch a subtree's visibility forever
    // (Renderer's SceneItemKey carries inheritedHidden).
    inline NodeResult UpdateNode(Entity node, Entity parentEntity,
                                 const DirectX::XMFLOAT4X4* parentMat,
                                 bool ancestorHidden,
                                 const XformPools& p, bool allowAdd)
    {
        using namespace DirectX;

        // Propagate writes WorldAabb only for children: a root's bounds belong to whoever writes
        // them (e.g. the bone-merged box Renderer::TickAnimationChain stamps on a skeleton root).
        const bool ownsBounds = (parentMat != nullptr);

        // Visibility: always reconciled, never skipped (see above).
        if (p.vis)
        {
            if (VisibilityComponent* v = p.vis->Get(node))
            {
                if (v->inheritedHidden != ancestorHidden)
                    v->inheritedHidden = ancestorHidden;
            }
        }

        const LocalTransform* lt = p.lt ? p.lt->Get(node) : nullptr;
        GlobalTransform*      gt = p.gt->Get(node);
        if (!gt)
        {
            // Nothing to compose into. With a LocalTransform this is an entity
            // spawned since the last Propagate and needs a pool grow.
            if (!lt)       return NodeResult::Ok;
            if (!allowAdd) return NodeResult::NeedsAdd;
            GlobalTransform fresh;
            XMStoreFloat4x4(&fresh.matrix,
                            parentMat ? (lt->ToMatrix() * XMLoadFloat4x4(parentMat))
                                      : lt->ToMatrix());
            // Add() may realloc the pool's dense array. parentMat points at a
            // caller-owned copy, not into the pool, so it survives; `gt` is
            // re-taken from Add's return.
            gt = p.gt->Add(node, fresh);
            if (!gt) return NodeResult::Ok;
        }

        XformCacheEntry* c = CacheFor(node);
        const uint32_t generation = p.world->GetGeneration(node);

        // Skip the visit when it is provably a no-op.
        if (c && c->valid &&
            c->generation == generation &&
            c->parent     == parentEntity &&
            c->hasLocal   == (lt ? 1u : 0u) &&
            // A node with no LocalTransform is not composed from its parent at
            // all (its global belongs to whoever writes it out of band), so the
            // parent matrix is not one of its inputs.
            (!lt || !parentMat ||
             std::memcmp(parentMat, &c->parentGlobal, sizeof(XMFLOAT4X4)) == 0) &&
            (!lt || std::memcmp(lt, &c->lt, sizeof(LocalTransform)) == 0) &&
            std::memcmp(&gt->matrix, &c->global, sizeof(XMFLOAT4X4)) == 0)
        {
            // Transform is stable. Bounds may still owe a rebuild: probe the live pools every time,
            // never a cached membership fact, so a component added while this entity missed a visit
            // cannot strand its box.
            bool boundsStable = true;
            if (ownsBounds)
            {
                const LocalAabb* la = p.laabb ? p.laabb->Get(node) : nullptr;
                const WorldAabb* wa = p.waabb ? p.waabb->Get(node) : nullptr;
                boundsStable = (la && wa)
                    ? (c->hasAabb != 0 &&
                       std::memcmp(la, &c->laabb, sizeof(LocalAabb)) == 0 &&
                       std::memcmp(wa, &c->waabb, sizeof(WorldAabb)) == 0)
                    : (c->hasAabb == 0);
            }

            if (boundsStable)
                return NodeResult::Ok;
        }

        // Full recompute.
        if (lt)
        {
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, parentMat ? (lt->ToMatrix() * XMLoadFloat4x4(parentMat))
                                          : lt->ToMatrix());
            // Skip the store when the bytes already match so we don't dirty a
            // cache line shared with another worker's entity.
            if (std::memcmp(&m, &gt->matrix, sizeof(XMFLOAT4X4)) != 0)
                gt->matrix = m;
            if (c) { c->global = m; c->lt = *lt; }
        }
        else if (c)
        {
            // Not composed here: someone else owns this global (e.g. the camera entity, patched by
            // CameraFollowResolveSystem). Re-baseline so a later out-of-band write still fails
            // check (d).
            c->global = gt->matrix;
        }

        if (ownsBounds)
        {
            const LocalAabb* la = p.laabb ? p.laabb->Get(node) : nullptr;
            WorldAabb*       wa = p.waabb ? p.waabb->Get(node) : nullptr;
            if (la && wa)
            {
                UpdateWorldAabb(*la, *wa, XMLoadFloat4x4(&gt->matrix));
                if (c) { c->laabb = *la; c->waabb = *wa; }
            }
            if (c) c->hasAabb = (la && wa) ? 1u : 0u;
        }
        else if (c)
        {
            c->hasAabb = 0u;   // roots: bounds are not ours, never compared
        }

        if (c)
        {
            if (parentMat) c->parentGlobal = *parentMat;
            c->parent     = parentEntity;
            c->generation = generation;
            c->hasLocal   = lt ? 1u : 0u;
            c->valid      = 1u;
        }
        return NodeResult::Ok;
    }

    // The hidden-state a node's children inherit. Kept as one helper so the
    // call sites can't drift apart.
    inline bool ChildAncestorHidden(const VisibilityComponent* parentVis)
    {
        const bool parentHidden = parentVis && !parentVis->IsEffectivelyVisible();
        return parentHidden || (parentVis && parentVis->inheritedHidden);
    }

    // Worker-side subtree propagation for Propagate's parallel path: the serial BFS's
    // traversal and math, but it writes components in place only, never through
    // ComponentPool::Add (a pool realloc while other workers read it is UB). Returns false
    // when a child needs a GlobalTransform entry created (spawned since the last Propagate);
    // the caller re-runs that subtree serially.
    // Safe concurrently for different roots: the hierarchy is a tree, so subtrees share no
    // entity, no component instance and no XformCache entry.
    bool PropagateSubtreeInPlace(Entity root, const XformPools& p)
    {
        using namespace DirectX;

        // Per-worker BFS scratch, reused across roots: zero steady-state allocation.
        thread_local std::vector<Entity> t_bfs;
        t_bfs.clear();
        size_t head = 0;
        t_bfs.push_back(root);

        while (head < t_bfs.size())
        {
            const Entity cur = t_bfs[head++];

            const GlobalTransform*     parentGT  = p.gt->Get(cur);
            const VisibilityComponent* parentVis = p.vis ? p.vis->Get(cur) : nullptr;
            const Children*            children  = p.children ? p.children->Get(cur) : nullptr;

            if (!children || !parentGT)
                continue;

            // COPY, not a pool pointer: UpdateNode may Add() on serial paths.
            const XMFLOAT4X4 parentMat      = parentGT->matrix;
            const bool       ancestorHidden = ChildAncestorHidden(parentVis);

            for (Entity child : children->entities)
            {
                if (!p.world->IsAlive(child))
                    continue;

                if (UpdateNode(child, cur, &parentMat, ancestorHidden,
                               p, /*allowAdd*/ false) == NodeResult::NeedsAdd)
                    return false;   // would grow the pool -> serial fallback

                // Only descend into entities that have Children: about half of an imported scene is leaf
                // meshes, each costing a queue slot and three pool probes for nothing.
                if (p.children && p.children->Get(child))
                    t_bfs.push_back(child);
            }
        }
        return true;
    }
}

void TransformSystem::Propagate(World& world)
{
    // Static-named zone (csvexport-friendly) so captures can track this function's wall time.
    ZoneScopedN("TransformPropagate");

    using namespace DirectX;

    // Pass 0: bind the dirty-tracking cache to this World. Keyed on the World and its Epoch():
    // Clear() resets entity ids and generations, so a newly loaded scene would otherwise
    // inherit the previous scene's entries.
    if (g_xformCache.owner != &world || g_xformCache.epoch != world.Epoch())
    {
        g_xformCache.owner = &world;
        g_xformCache.epoch = world.Epoch();
        g_xformCache.entries.clear();
    }

    // Persistent vector + head index as the queue: zero steady-state allocation (a
    // std::deque-backed queue spent >40% of this function in operator new).
    thread_local std::vector<Entity> t_queue;
    t_queue.clear();
    size_t head = 0;

    // Pools are long-lived (created at scene load): one lookup per call is enough.
    XformPools pools;
    pools.world    = &world;
    pools.gt       = world.EnsurePool<GlobalTransform>();
    pools.lt       = world.GetPool<LocalTransform>();
    pools.children = world.GetPool<Children>();
    pools.vis      = world.GetPool<VisibilityComponent>();
    pools.laabb    = world.GetPool<LocalAabb>();
    pools.waabb    = world.GetPool<WorldAabb>();
    auto* poolParent = world.GetPool<Parent>();

    // Walk the GlobalTransform pool's dense array so only owners of the component are
    // visited (a 22k-entity scene can have ~2k).
    const auto&  gtEntities = pools.gt->Entities();
    const size_t gtCount    = gtEntities.size();

    // The cache is indexed by entity id and must be sized before any fan-out: a resize while
    // workers hold entry pointers is UB. Every entity Propagate touches owns a GlobalTransform
    // (a child that needs one created goes to the serial fixup, which does not touch the
    // cache), so this roster bounds the ids. CacheFor() still bounds-checks and forces a
    // recompute past the end.
    {
        size_t maxId = 0;
        for (size_t gi = 0; gi < gtCount; ++gi)
            maxId = std::max<size_t>(maxId, static_cast<size_t>(gtEntities[gi]));
        if (gtCount > 0 && g_xformCache.entries.size() < maxId + 1)
            g_xformCache.entries.resize(maxId + 1);
    }

    // Pass 1: seed the queue with the roots: a GlobalTransform and no Parent, or a Parent
    // whose entity is NullEntity.
    for (size_t gi = 0; gi < gtCount; ++gi)
    {
        const Entity e = gtEntities[gi];
        const Parent* pc = poolParent ? poolParent->Get(e) : nullptr;
        if (pc && pc->entity != NullEntity)
            continue;

        // Roots have no ancestor to inherit hidden-state from, and no parent
        // matrix: global = local. Passing NullEntity as the parent means an
        // entity reparented to or from root fails the c->parent check and
        // recomputes.
        UpdateNode(e, NullEntity, /*parentMat*/ nullptr,
                   /*ancestorHidden*/ false, pools, /*allowAdd*/ false);

        t_queue.push_back(e);
    }

    // Pass 2: propagate each root's subtree, parent before children.
    // Parallel path: subtrees are disjoint (tree hierarchy), so they fan out to TaskSystem
    // workers writing in place. A child that needs a GlobalTransform entry (spawned since the
    // last Propagate) would grow a pool: its subtree aborts into the overflow list and re-runs
    // serially, where ComponentPool::Add is safe. Small scenes stay serial: dispatch overhead
    // would exceed the win.
    const size_t rootCount = t_queue.size();

    constexpr size_t kMinEntitiesForParallel = 512;
    constexpr size_t kMinUnitsForParallel    = 4;
    TaskSystem& tasks = TaskSystem::Get();
    const bool runParallel = gtCount >= kMinEntitiesForParallel
                          && rootCount >= 1
                          && tasks.GetWorkerCount() > 1;

    if (runParallel)
    {
        // Separate zone so a capture shows which path ran + the fan-out cost.
        ZoneScopedN("Propagate.ParallelRoots");

        // Fan-out unit is (root, depth-1 child), not the root itself: an
        // imported world parents thousands of nodes under ONE mega-root, and
        // per-root fan-out would hand that entire tree to a single worker.
        // Depth-1 subtrees are still disjoint (tree hierarchy), and every
        // root's own GlobalTransform was finalized in Pass 1, so workers only
        // ever READ parent state and write inside their own subtree.
        struct Depth1Unit { Entity parent; Entity child; };
        thread_local std::vector<Depth1Unit> t_units;
        t_units.clear();
        for (size_t ri = 0; ri < rootCount; ++ri)
        {
            const Entity root = t_queue[ri];
            const Children* children = pools.children ? pools.children->Get(root) : nullptr;
            if (!children)
                continue;
            for (Entity child : children->entities)
                if (world.IsAlive(child))
                    t_units.push_back({ root, child });
        }

        if (t_units.size() >= kMinUnitsForParallel)
        {
            std::mutex               overflowMtx;
            std::vector<Depth1Unit>  overflow;
            const Depth1Unit* units = t_units.data();   // stable while ParallelFor blocks

            tasks.ParallelFor(0, static_cast<uint32_t>(t_units.size()), [&](uint32_t ui)
            {
                const Depth1Unit& u = units[ui];

                // Seed: bring the unit's child up to date from its finalized root parent, with the BFS
                // inner loop's helper, in place only.
                const GlobalTransform* parentGT = pools.gt->Get(u.parent);
                if (!parentGT)
                    return;
                const VisibilityComponent* parentVis = pools.vis ? pools.vis->Get(u.parent) : nullptr;
                const XMFLOAT4X4 parentMat      = parentGT->matrix;
                const bool       ancestorHidden = ChildAncestorHidden(parentVis);

                if (UpdateNode(u.child, u.parent, &parentMat, ancestorHidden,
                               pools, /*allowAdd*/ false) == NodeResult::NeedsAdd)
                {
                    // Would grow the pool -> serial fixup below.
                    std::lock_guard<std::mutex> lk(overflowMtx);
                    overflow.push_back(u);
                    return;
                }

                if (!PropagateSubtreeInPlace(u.child, pools))
                {
                    std::lock_guard<std::mutex> lk(overflowMtx);
                    overflow.push_back(u);
                }
            });

            // Serial fixup for subtrees that need pool growth (only the frame an entity spawns). Do not
            // stamp the XformCache here: PropagateSubtree recomputes the subtree with the identical
            // expression, so entries an aborted worker stamped stay true and the new entity has none
            // and recomputes next frame. A stamp without a matching write would strand the entity.
            for (const Depth1Unit& u : overflow)
            {
                const GlobalTransform*     parentGT  = pools.gt->Get(u.parent);
                const VisibilityComponent* parentVis = pools.vis ? pools.vis->Get(u.parent) : nullptr;
                if (!parentGT)
                    continue;
                const XMMATRIX parentMat = XMLoadFloat4x4(&parentGT->matrix);

                if (const LocalTransform* lt = pools.lt ? pools.lt->Get(u.child) : nullptr)
                {
                    GlobalTransform gt;
                    XMStoreFloat4x4(&gt.matrix, lt->ToMatrix() * parentMat);
                    pools.gt->Add(u.child, gt);
                }
                if (pools.vis)
                {
                    if (VisibilityComponent* v = pools.vis->Get(u.child))
                        v->inheritedHidden = ChildAncestorHidden(parentVis);
                }
                const LocalAabb* localAabb = pools.laabb ? pools.laabb->Get(u.child) : nullptr;
                WorldAabb*       worldAabb = pools.waabb ? pools.waabb->Get(u.child) : nullptr;
                if (localAabb && worldAabb)
                {
                    if (const GlobalTransform* childGT = pools.gt->Get(u.child))
                        UpdateWorldAabb(*localAabb, *worldAabb,
                                        XMLoadFloat4x4(&childGT->matrix));
                }
                PropagateSubtree(world, static_cast<unsigned int>(u.child));
            }
            return;
        }
        // Too few depth-1 units for a fan-out: fall through to the serial BFS (s_queue / head are
        // still untouched).
    }

    // Serial BFS over all roots (s_queue doubles as the BFS queue).
    while (head < t_queue.size())
    {
        const Entity cur = t_queue[head++];

        const GlobalTransform*     parentGT  = pools.gt->Get(cur);
        const VisibilityComponent* parentVis = pools.vis ? pools.vis->Get(cur) : nullptr;
        const Children*            children  = pools.children ? pools.children->Get(cur) : nullptr;

        if (!children || !parentGT)
            continue;

        // COPY, not a pool pointer: UpdateNode Adds on this path and that can
        // realloc the GlobalTransform pool's dense array.
        const XMFLOAT4X4 parentMat      = parentGT->matrix;
        const bool       ancestorHidden = ChildAncestorHidden(parentVis);

        for (Entity child : children->entities)
        {
            if (!world.IsAlive(child))
                continue;

            // child.GlobalTransform = child.LocalTransform x parent.GlobalTransform, plus
            // inheritedHidden and WorldAabb, all inside UpdateNode. Add is legal here, so a freshly
            // spawned child is handled inline.
            UpdateNode(child, cur, &parentMat, ancestorHidden,
                       pools, /*allowAdd*/ true);

            if (pools.children && pools.children->Get(child))
                t_queue.push_back(child);
        }
    }
}

// Does not stamp the dirty cache: its callers (ApplyRenderInterpolation,
// FollowSystem::ApplyFollow) write a GlobalTransform the next Propagate replaces, so a stamp
// would record a stale baseline. Left alone, the divergence fails check (d) and the subtree
// recomputes.
void TransformSystem::PropagateSubtree(World& world, unsigned int rootU)
{
    using namespace DirectX;
    const Entity root = static_cast<Entity>(rootU);

    auto* poolGT       = world.GetPool<GlobalTransform>();
    auto* poolChildren = world.GetPool<Children>();
    if (!poolGT || !poolChildren)
        return;

    // Nothing to do unless the root actually has descendants. The common case
    // (a dynamic prop with no children) early-outs here after two lookups.
    const Children* rootChildren = poolChildren->Get(root);
    if (!rootChildren || rootChildren->entities.empty())
        return;
    if (!poolGT->Get(root))   // root must already have a current GlobalTransform
        return;

    auto* poolLT    = world.GetPool<LocalTransform>();
    auto* poolVis   = world.GetPool<VisibilityComponent>();
    auto* poolLAabb = world.GetPool<LocalAabb>();
    auto* poolWAabb = world.GetPool<WorldAabb>();

    // BFS over descendants only; the root's GlobalTransform (just written by the caller) is
    // authoritative. Mirrors Pass 2 of Propagate().
    thread_local std::vector<Entity> t_subQueue;
    t_subQueue.clear();
    size_t head = 0;
    t_subQueue.push_back(root);

    while (head < t_subQueue.size())
    {
        const Entity current = t_subQueue[head++];

        const GlobalTransform*     parentGT  = poolGT->Get(current);
        const VisibilityComponent* parentVis = poolVis ? poolVis->Get(current) : nullptr;
        const Children*            children  = poolChildren->Get(current);

        if (!children || !parentGT)
            continue;

        const XMMATRIX parentMat      = XMLoadFloat4x4(&parentGT->matrix);
        const bool     ancestorHidden = ChildAncestorHidden(parentVis);

        for (Entity child : children->entities)
        {
            if (!world.IsAlive(child))
                continue;

            const LocalTransform* lt = poolLT ? poolLT->Get(child) : nullptr;
            if (lt)
            {
                GlobalTransform gt;
                XMStoreFloat4x4(&gt.matrix, lt->ToMatrix() * parentMat);
                poolGT->Add(child, gt);
            }

            if (poolVis)
            {
                if (VisibilityComponent* v = poolVis->Get(child))
                    v->inheritedHidden = ancestorHidden;
            }

            const LocalAabb* localAabb = poolLAabb ? poolLAabb->Get(child) : nullptr;
            WorldAabb*       worldAabb = poolWAabb ? poolWAabb->Get(child) : nullptr;
            if (localAabb && worldAabb)
            {
                if (const GlobalTransform* childGT = poolGT->Get(child))
                    UpdateWorldAabb(*localAabb, *worldAabb,
                                    XMLoadFloat4x4(&childGT->matrix));
            }

            t_subQueue.push_back(child);
        }
    }
}
