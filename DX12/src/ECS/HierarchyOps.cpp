#include "ECS/HierarchyOps.h"

#include "ECS/HierarchyComponents.h"   // Parent, Children, PrefabInstanceMember, PrefabSourcePath
#include "UI/UICanvas.h"               // UI::UIParent - Canvas widgets do not use Children
#include "UI/UICanvasLayout.h"         // UI::ResolveUIParent - the one edge rule
#include "System/Log.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Hierarchy
{
namespace
{

// A malformed Parent/Children pair (a cycle introduced by a bad reparent, or a
// snapshot restored with stale ids) must not turn a delete into a hang. The
// visited set already breaks true cycles; this is the belt for the case where
// the graph is not a cycle but is absurdly deep.
constexpr size_t kMaxSubtreeEntities = 1u << 20;

} // namespace

void CollectSubtree(const World& world, Entity root, std::vector<Entity>& out)
{
    out.clear();
    if (root == NullEntity || !world.IsAlive(root)) return;

    // PrefabInstanceMember is ownership only while the root is still an undetached instance (the
    // component's contract). Resolved once rather than per candidate, which keeps the walk
    // O(subtree).
    const bool rootOwnsPrefabMembers =
        world.GetComponent<PrefabSourcePath>(root) != nullptr;

    // The member back-pointers are not indexed by root, so one pool scan runs up front, for the root
    // only; nested prefab instances under it are reached through Children like everything else.
    std::vector<Entity> prefabMembers;
    if (rootOwnsPrefabMembers)
    {
        if (auto* pool = world.GetPool<PrefabInstanceMember>())
        {
            const auto& ents = pool->Entities();
            const auto& data = pool->Data();
            for (size_t i = 0; i < data.size(); ++i)
                if (data[i].root == root && ents[i] != root)
                    prefabMembers.push_back(ents[i]);
        }
    }

    // UIParent is likewise not indexed by parent. Scanned once and bucketed, so
    // a deep Canvas subtree costs one pass rather than one pass per node.
    std::unordered_map<Entity, std::vector<Entity>> uiChildren;
    if (auto* pool = world.GetPool<UI::UIParent>())
    {
        const auto& ents = pool->Entities();
        const auto& data = pool->Data();
        for (size_t i = 0; i < data.size(); ++i)
        {
            // UI::ResolveUIParent's rule: an edge spawned this frame has only its Guid until layout resolves
            // it, and a subtree deleted in that frame must not leave it behind.
            const Entity p = UI::ResolveUIParent(world, data[i]);
            if (p != NullEntity && ents[i] != p)
                uiChildren[p].push_back(ents[i]);
        }
    }

    std::unordered_set<Entity> visited;
    visited.insert(root);
    out.push_back(root);

    // Breadth-first over `out` itself: everything appended is walked in turn, so
    // the result comes out shallowest-first and the caller can destroy in
    // reverse to get deepest-first.
    for (size_t i = 0; i < out.size(); ++i)
    {
        // Truncating here silently leaks everything past the cap, the failure this file exists to
        // prevent, so warn.
        if (out.size() >= kMaxSubtreeEntities)
        {
            LOG_WARNING("Hierarchy::CollectSubtree: subtree of entity %u exceeds "
                        "%zu entities; the remainder is NOT destroyed and its GPU "
                        "slots will leak. This means a malformed Parent/Children "
                        "graph, not a legitimately large scene.",
                        root, kMaxSubtreeEntities);
            break;
        }

        const Entity cur = out[i];

        auto push = [&](Entity c)
        {
            if (c == NullEntity || !world.IsAlive(c)) return;
            if (visited.insert(c).second) out.push_back(c);
        };

        if (const Children* ch = world.GetComponent<Children>(cur))
            for (Entity c : ch->entities) push(c);

        if (const auto it = uiChildren.find(cur); it != uiChildren.end())
            for (Entity c : it->second) push(c);

        if (cur == root)
            for (Entity c : prefabMembers) push(c);
    }
}

uint32_t DestroySubtree(World& world, Entity root)
{
    if (root == NullEntity || !world.IsAlive(root)) return 0;

    std::vector<Entity> order;
    CollectSubtree(world, root, order);
    if (order.empty()) return 0;

    // Unlink from the parent BEFORE destroying: a Children list that still names
    // a dead id outlives this call (the parent is not being destroyed), and the
    // next walk over it would hand a recycled entity to whatever reads it.
    if (const Parent* p = world.GetComponent<Parent>(root); p && p->entity != NullEntity)
    {
        if (Children* pc = world.GetComponent<Children>(p->entity))
        {
            auto& vec = pc->entities;
            vec.erase(std::remove(vec.begin(), vec.end(), root), vec.end());
        }
    }

    // Reverse = deepest-first. Each destroy fires the World destroy listeners
    // synchronously (that is how SkinnedMeshSubsystem::OnEntityDestroyed gets to
    // recycle the mesh-descriptor + bindless slots), and a listener is free to
    // destroy further entities, so every id is re-checked for liveness.
    uint32_t destroyed = 0;
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        if (!world.IsAlive(*it)) continue;
        world.DestroyEntity(*it);
        ++destroyed;
    }
    return destroyed;
}

} // namespace Hierarchy
