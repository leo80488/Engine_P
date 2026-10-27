#pragma once

// RenameAction - undo/redo for an entity name change (World::SetName).

#include "Editor/Undo/EditAction.h"

#include <string>
#include <utility>

namespace Editor::Undo
{
class RenameAction : public IEditAction
{
public:
    RenameAction(EntityHandle h, std::string before, std::string after)
        : m_handle(h), m_before(std::move(before)), m_after(std::move(after)) {}

    void        Apply(EditContext& c) override  { Set(c, m_after); }   // redo
    void        Revert(EditContext& c) override { Set(c, m_before); }  // undo
    const char* Label() const override          { return "Rename"; }

private:
    void Set(EditContext& c, const std::string& name)
    {
        if (c.world && c.world->IsHandleValid(m_handle))
            c.world->SetName(m_handle.entity, name);
    }
    EntityHandle m_handle;
    std::string  m_before, m_after;
};

} // namespace Editor::Undo
