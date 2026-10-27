#pragma once

// PropertyEditAction - one reflected field of one entity's component, before and after
// (Reflection/Property.h). Undo and redo write only that field back through Reflect::WriteProperty,
// so the component's other fields - run-time state systems keep beside the authored values - stay
// as they are, and the type's change hooks run as they do for every other writer. A gesture that
// changes several fields is a macro of these.

#include "Editor/Undo/EditAction.h"
#include "Reflection/Property.h"

#include <functional>
#include <string>
#include <utility>

namespace Editor::Undo
{
class PropertyEditAction : public IEditAction
{
public:
    // @p fetch returns the component on the entity (nullptr when it is gone).
    using FetchFn = std::function<void*(World&, Entity)>;

    PropertyEditAction(EntityHandle handle, FetchFn fetch, Reflect::PropertyPath path, Reflect::PropertyValue before,
                       Reflect::PropertyValue after, std::string label)
        : m_handle(handle), m_fetch(std::move(fetch)), m_path(std::move(path)), m_before(std::move(before)),
          m_after(std::move(after)), m_label("Edit " + std::move(label))
    {
    }

    void        Apply(EditContext& c) override  { Write(c, m_after); }
    void        Revert(EditContext& c) override { Write(c, m_before); }
    const char* Label() const override          { return m_label.c_str(); }

    // A later edit of the same field of the same entity keeps this one's before.
    bool TryMerge(const IEditAction& incoming) override
    {
        const auto* other = dynamic_cast<const PropertyEditAction*>(&incoming);
        if (!other || other->m_handle.entity != m_handle.entity || other->m_path.text != m_path.text ||
            other->m_path.type != m_path.type)
            return false;
        m_after = other->m_after;
        return true;
    }

    const Reflect::PropertyPath& Path() const { return m_path; }

private:
    void Write(EditContext& c, const Reflect::PropertyValue& value)
    {
        if (!c.world || !c.world->IsHandleValid(m_handle) || !m_fetch) return;
        if (void* obj = m_fetch(*c.world, m_handle.entity))
            Reflect::WriteProperty(obj, m_path, value, Reflect::WriteOrigin::Undo);
    }

    EntityHandle           m_handle;
    FetchFn                m_fetch;
    Reflect::PropertyPath  m_path;
    Reflect::PropertyValue m_before, m_after;
    std::string            m_label;
};

} // namespace Editor::Undo
