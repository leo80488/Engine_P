#pragma once

// Structural undo actions: create / delete / duplicate / reparent.
//   * StructuralAction - wraps an EntitySubtreeSnapshot with a polarity:
//       delete -> Apply detaches (destroy + park ids), Revert re-attaches;
//       create -> Apply re-attaches,                  Revert detaches.
//     While the subtree is detached its ids stay parked; OnDiscard frees them
//     when the action leaves history without being resurrected.
//   * ReparentAction - restores the child's Parent / LocalTransform (or UIParent)
//     on both sides of a re-parent, fixing the affected Children lists.

#include "Editor/Undo/EditAction.h"
#include "Editor/Undo/EntitySnapshot.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace Editor::Undo
{
class StructuralAction : public IEditAction
{
public:
    StructuralAction(EntitySubtreeSnapshot snap, bool applyAttaches, bool attached, std::string label)
        : m_snap(std::move(snap)), m_applyAttaches(applyAttaches)
        , m_attached(attached), m_label(std::move(label)) {}

    void Apply(EditContext& c) override  { m_applyAttaches ? Attach(c) : Detach(c); }
    void Revert(EditContext& c) override { m_applyAttaches ? Detach(c) : Attach(c); }
    const char* Label() const override   { return m_label.c_str(); }

    // While detached, the subtree's ids are parked in the World and its GPU slots in
    // the renderer - release both, since this action can never resurrect it.
    void OnDiscard(EditContext& c) override
    {
        if (m_attached) return;
        if (c.world)
            for (const auto& n : m_snap.nodes)
                c.world->UnreserveId(n.id);
        if (c.onEntitiesDiscarded)
            c.onEntitiesDiscarded(NodeIds());
    }

private:
    std::vector<Entity> NodeIds() const
    {
        std::vector<Entity> ids;
        ids.reserve(m_snap.nodes.size());
        for (const auto& n : m_snap.nodes) ids.push_back(n.id);
        return ids;
    }

    void Attach(EditContext& c)
    {
        if (!c.world) return;
        m_snap.Attach(*c.world);
        m_attached = true;
        // Un-park / re-resolve GPU state for the restored entities.
        if (c.onEntitiesRestored) c.onEntitiesRestored(NodeIds());
        // Select the resurrected root so undo-of-delete re-focuses it.
        if (c.setSelection) c.setSelection(m_snap.rootId);
    }
    void Detach(EditContext& c)
    {
        if (!c.world) return;
        // Park GPU slots BEFORE destroying so OnEntityDestroyed keeps them alive.
        if (c.onEntitiesWillDetach) c.onEntitiesWillDetach(NodeIds());
        m_snap.Detach(*c.world);
        m_attached = false;
        // The subtree is gone: drop the selection.
        if (c.setSelection) c.setSelection(NullEntity);
    }

    EntitySubtreeSnapshot m_snap;
    bool                  m_applyAttaches;
    bool                  m_attached;
    std::string           m_label;
};

// Delete: the entity is alive at construction; Execute() runs Apply -> Detach.
inline std::unique_ptr<StructuralAction> MakeDeleteAction(EntitySubtreeSnapshot snap, std::string label)
{
    return std::make_unique<StructuralAction>(std::move(snap), /*applyAttaches*/false,
                                              /*attached*/true, "Delete " + std::move(label));
}
// Create/Duplicate: the entity already exists; Push() records without applying.
inline std::unique_ptr<StructuralAction> MakeCreateAction(EntitySubtreeSnapshot snap, std::string label)
{
    return std::make_unique<StructuralAction>(std::move(snap), /*applyAttaches*/true,
                                              /*attached*/true, std::move(label));
}

// Undo / redo of a re-parent: restores the hierarchy edge and the preserved
// LocalTransform.
class ReparentAction : public IEditAction
{
public:
    // Scene-graph reparent: capture the child's parent + LocalTransform before
    // and after the move (ReparentEntity recomputes the LT to preserve world
    // pose, so we restore the exact value rather than recompute).
    ReparentAction(EntityHandle child,
                   Entity oldParent, LocalTransform oldLocal, bool hasOldLocal,
                   Entity newParent, LocalTransform newLocal, bool hasNewLocal,
                   std::string label)
        : m_child(child), m_isUI(false)
        , m_oldParent(oldParent), m_oldLocal(oldLocal), m_hasOldLocal(hasOldLocal)
        , m_newParent(newParent), m_newLocal(newLocal), m_hasNewLocal(hasNewLocal)
        , m_label("Reparent " + std::move(label)) {}

    // Canvas-UI reparent (UIParent edge; no transform recompute).
    ReparentAction(EntityHandle child, Entity oldUIParent, Entity newUIParent, std::string label)
        : m_child(child), m_isUI(true)
        , m_oldParent(oldUIParent), m_newParent(newUIParent)
        , m_label("Reparent " + std::move(label)) {}

    void Apply(EditContext& c) override   // redo -> new parent
    {
        if (!c.world || !c.world->IsAlive(m_child.entity)) return;
        if (m_isUI) SetUIParent(c, m_newParent);
        else        ApplyParentEdge(c, m_child.entity, m_newParent, m_newLocal, m_hasNewLocal);
    }
    void Revert(EditContext& c) override  // undo -> old parent
    {
        if (!c.world || !c.world->IsAlive(m_child.entity)) return;
        if (m_isUI) SetUIParent(c, m_oldParent);
        else        ApplyParentEdge(c, m_child.entity, m_oldParent, m_oldLocal, m_hasOldLocal);
    }
    const char* Label() const override { return m_label.c_str(); }

private:
    static void ApplyParentEdge(EditContext& c, Entity child, Entity parent,
                                const LocalTransform& local, bool hasLocal)
    {
        World& w = *c.world;
        if (!c.world || !w.IsAlive(child)) return;

        // Detach from the current parent's Children list.
        if (Parent* cur = w.GetComponent<Parent>(child); cur && cur->entity != NullEntity)
            if (Children* ch = w.GetComponent<Children>(cur->entity))
            {
                auto& v = ch->entities;
                v.erase(std::remove(v.begin(), v.end(), child), v.end());
            }

        if (parent == NullEntity)
        {
            if (w.HasComponent<Parent>(child)) w.RemoveComponent<Parent>(child);
        }
        else
        {
            if (Parent* p = w.GetComponent<Parent>(child)) p->entity = parent;
            else                                           w.AddComponent<Parent>(child, Parent{ parent });

            if (Children* ch = w.GetComponent<Children>(parent))
            {
                if (std::find(ch->entities.begin(), ch->entities.end(), child) == ch->entities.end())
                    ch->entities.push_back(child);
            }
            else
            {
                Children nc; nc.entities.push_back(child);
                w.AddComponent<Children>(parent, std::move(nc));
            }
        }

        if (hasLocal)
            if (LocalTransform* lt = w.GetComponent<LocalTransform>(child)) *lt = local;
    }

    void SetUIParent(EditContext& c, Entity parent)
    {
        World& w = *c.world;
        if (!c.world || !w.IsAlive(m_child.entity)) return;
        if (UI::UIParent* up = w.GetComponent<UI::UIParent>(m_child.entity))
        {
            up->parent     = parent;
            up->parentGuid = (parent != NullEntity && w.IsAlive(parent))
                           ? ECS::EnsureGuidOn(w, parent) : ECS::Guid{};
        }
    }

    EntityHandle   m_child;
    bool           m_isUI;
    Entity         m_oldParent = NullEntity;
    Entity         m_newParent = NullEntity;
    LocalTransform m_oldLocal{}, m_newLocal{};
    bool           m_hasOldLocal = false, m_hasNewLocal = false;
    std::string    m_label;
};

} // namespace Editor::Undo
