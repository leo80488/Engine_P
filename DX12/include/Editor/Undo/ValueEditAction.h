#pragma once

// ValueEditAction - inspector field-edit undo via value-copy snapshots.
//
// The primary field-edit action: it stores two RestoreFn closures (each holding a
// deep value-copy of the component captured before / after the edit) and swaps
// them. Works for ANY copy-assignable component - POD (LocalTransform) and
// string / map-holding types (MaterialComponent) alike - without a serializer.
// ComponentEditAction (a serializer blob) is for cases where a text blob is
// preferred; Add / RemoveComponentAction live in ComponentEditAction.h.

#include "Editor/Undo/EditAction.h"

#include <string>
#include <utility>

namespace Editor::Undo
{
class ValueEditAction : public IEditAction
{
public:
    ValueEditAction(EntityHandle h, RestoreFn before, RestoreFn after, std::string label)
        : m_handle(h), m_before(std::move(before)), m_after(std::move(after))
        , m_label("Edit " + std::move(label)) {}

    void        Apply(EditContext& c) override  { Run(c, m_after); }   // redo
    void        Revert(EditContext& c) override { Run(c, m_before); }  // undo
    const char* Label() const override          { return m_label.c_str(); }

private:
    void Run(EditContext& c, const RestoreFn& fn)
    {
        if (c.world && c.world->IsHandleValid(m_handle) && fn)
            fn(*c.world, m_handle.entity);
    }

    EntityHandle m_handle;
    RestoreFn    m_before, m_after;
    std::string  m_label;
};

} // namespace Editor::Undo
