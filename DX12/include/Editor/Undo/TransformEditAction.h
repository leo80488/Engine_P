#pragma once

// TransformEditAction - undo/redo for a single entity's LocalTransform.
//
// Typed inverse: LocalTransform is small POD (10 floats), so the action stores
// the before/after values and swaps them. Covers gizmo translate/rotate/scale and
// any other path that writes LocalTransform.
//
// EditorLayer captures the pre-drag LocalTransform on the rising edge of
// ImGuizmo::IsUsing() and pushes one action on release, so a whole drag is one
// undo step. TryMerge still collapses two back-to-back programmatic edits of the
// same entity.

#include "Editor/Undo/EditAction.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform

namespace Editor::Undo
{
class TransformEditAction : public IEditAction
{
public:
    // `handle` carries the entity's generation so a delete+id-recycle can't make
    // this action silently mutate a different entity that reused the slot.
    TransformEditAction(EntityHandle handle, const LocalTransform& before, const LocalTransform& after)
        : m_handle(handle), m_before(before), m_after(after) {}

    void        Apply(EditContext& c) override  { Set(c, m_after); }   // redo
    void        Revert(EditContext& c) override { Set(c, m_before); }  // undo
    const char* Label() const override          { return "Transform"; }

    bool TryMerge(const IEditAction& incoming) override
    {
        const auto* t = dynamic_cast<const TransformEditAction*>(&incoming);
        if (!t || t->m_handle != m_handle) return false;
        m_after = t->m_after;   // extend this action's end state
        return true;
    }

    EntityHandle TargetHandle() const { return m_handle; }

private:
    void Set(EditContext& c, const LocalTransform& v)
    {
        if (!c.world || !c.world->IsHandleValid(m_handle)) return;
        if (LocalTransform* lt = c.world->GetComponent<LocalTransform>(m_handle.entity))
            *lt = v;
        // GlobalTransform is recomputed by TransformSystem's Propagate (runs in
        // Edit mode), so we only need to set the authoritative LocalTransform.
    }

    EntityHandle   m_handle;
    LocalTransform m_before;
    LocalTransform m_after;
};

} // namespace Editor::Undo
