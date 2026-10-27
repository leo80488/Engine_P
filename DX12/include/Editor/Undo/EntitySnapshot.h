#pragma once

// EntitySubtreeSnapshot - capture / detach / re-attach a whole entity subtree with
// EXACT id + generation preservation, for structural undo (delete / create /
// duplicate). Parent/Children/UIParent hold raw (non-generation-checked) Entity
// ids, so resurrecting a deleted entity under the SAME id keeps every inbound
// reference valid.
//
// Component data is captured by TYPE-ERASED VALUE-COPY
// (World::CloneEntityComponents -> ComponentCloner<T>), NOT serializer text: the
// same deep copy Edit<->Play uses, so it captures EVERY component, including the
// skinned-mesh runtime ones (Skeleton / Morph / Animation / MeshLibRef /
// SkeletonRef) that have no serializer. GPU handles inside them are shared by
// refcount, so the snapshot keeps those resources alive while a delete sits on
// the undo stack.
//
// Hierarchy is captured too (Parent/Children/UIParent are ordinary components), so
// internal links restore under the preserved ids; only the root's link into its
// EXTERNAL parent's Children list needs explicit fixup.

#include "ECS/HierarchyComponents.h"   // Parent, Children (subtree walk + relink)
#include "ECS/GuidRegistry.h"          // GuidRegistry::Get().RebuildFromWorld
#include "UI/UICanvas.h"               // UI::UIParent (Canvas subtree walk)
#include "UI/UICanvasLayout.h"         // UI::ResolveUIParent (an edge spawned this frame)

#include <algorithm>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace Editor::Undo
{
struct EntitySubtreeSnapshot
{
    struct Node
    {
        Entity      id  = NullEntity;
        uint32_t    gen = 0;
        std::string name;
        std::vector<std::pair<std::type_index, std::shared_ptr<void>>> comps; // ALL components, by value
    };

    bool Empty() const { return nodes.empty(); }

    // ---- Capture the subtree rooted at `root` (entity must be alive) ----
    static EntitySubtreeSnapshot Capture(World& w, Entity root)
    {
        EntitySubtreeSnapshot snap;
        if (!w.IsAlive(root)) return snap;
        snap.rootId = root;
        if (const Parent* p = w.GetComponent<Parent>(root)) snap.rootParent = p->entity;

        std::vector<Entity> order;
        CollectSubtree(w, root, order);

        for (Entity e : order)
        {
            Node n;
            n.id    = e;
            n.gen   = w.GetGeneration(e);
            n.name  = w.GetName(e);
            n.comps = w.CloneEntityComponents(e);   // deep value-copy of every component
            snap.nodes.push_back(std::move(n));
        }
        return snap;
    }

    // ---- Destroy the subtree + park its ids (redo-of-create / do-of-delete) ----
    void Detach(World& w) const
    {
        // Unlink root from its external parent's Children list.
        if (rootParent != NullEntity && w.IsAlive(rootParent))
            if (Children* pc = w.GetComponent<Children>(rootParent))
            {
                auto& v = pc->entities;
                v.erase(std::remove(v.begin(), v.end(), rootId), v.end());
            }

        // Destroy every node (fires destroy listeners: GuidRegistry unregister,
        // renderer / physics per-entity cache teardown).
        for (const Node& n : nodes)
            if (w.IsAlive(n.id)) w.DestroyEntity(n.id);

        // Park the ids so CreateEntity can't reissue them while we hold them.
        for (const Node& n : nodes)
            w.ReserveId(n.id);
    }

    // ---- Recreate the subtree under its exact ids (undo-of-delete) ----
    void Attach(World& w) const
    {
        // Pass 1: recreate every entity with its exact id so raw references
        // between subtree members resolve once components are restored.
        for (const Node& n : nodes)
        {
            if (w.IsAlive(n.id)) continue;
            const Entity e = w.CreateEntityWithId(n.id, n.gen);
            if (e == NullEntity) continue;
            w.SetName(e, n.name);
        }

        // Pass 2: restore all component values (hierarchy included).
        for (const Node& n : nodes)
            if (w.IsAlive(n.id))
                w.RestoreEntityComponents(n.id, n.comps);

        // Re-insert the root into its external parent's Children list.
        if (rootParent != NullEntity && w.IsAlive(rootParent))
        {
            Children* pc = w.GetComponent<Children>(rootParent);
            if (!pc)
            {
                w.AddComponent<Children>(rootParent, Children{});
                pc = w.GetComponent<Children>(rootParent);
            }
            if (pc && std::find(pc->entities.begin(), pc->entities.end(), rootId) == pc->entities.end())
                pc->entities.push_back(rootId);
        }

        // Rebuild the GUID map so AttachmentRef / UIParent-by-guid references
        // re-resolve against the restored entities.
        ECS::GuidRegistry::Get().RebuildFromWorld(w);
    }

private:
    static void CollectSubtree(World& w, Entity e, std::vector<Entity>& out)
    {
        out.push_back(e);

        std::vector<Entity> kids;
        if (const Children* ch = w.GetComponent<Children>(e)) kids = ch->entities;
        // Canvas-UI children link via UIParent, not scene-graph Children.
        if (auto* pp = w.GetPool<UI::UIParent>())
        {
            const auto& ents = pp->Entities();
            const auto& data = pp->Data();
            for (size_t i = 0; i < data.size(); ++i)
                if (ents[i] != e && UI::ResolveUIParent(w, data[i]) == e) kids.push_back(ents[i]);
        }
        for (Entity k : kids)
            if (w.IsAlive(k) && std::find(out.begin(), out.end(), k) == out.end())
                CollectSubtree(w, k, out);
    }

public:
    std::vector<Node> nodes;
    Entity            rootId     = NullEntity;
    Entity            rootParent = NullEntity;   // root's external parent (for re-link)
};

} // namespace Editor::Undo
