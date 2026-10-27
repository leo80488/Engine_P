#pragma once

// Component-level undo actions:
//   * ComponentEditAction   - inspector field edit: whole-component before/after
//                             text-blob swap (safe for string/vector/conditional
//                             fields where a raw memcpy-by-offset is not).
//   * AddComponentAction    - Add Component: redo = add(default), undo = remove.
//   * RemoveComponentAction - Remove Component: redo = remove, undo = re-add and
//                             restore the captured data blob.
//
// All use EntityHandle (generation-checked) so a delete+id-recycle cannot make
// a stale action mutate the wrong entity, and route component data through the
// ComponentSerializerRegistry (see ComponentBlob.h).

#include "Editor/Undo/EditAction.h"
#include "Editor/Undo/ComponentBlob.h"
#include "Resource/ComponentWrite.h"   // ComponentEditAction restores by rewrite

#include <functional>
#include <string>
#include <typeindex>

namespace Editor::Undo
{
// Whole-component before/after blob swap - one inspector edit gesture.
class ComponentEditAction : public IEditAction
{
public:
    ComponentEditAction(EntityHandle h, std::type_index type,
                        std::string before, std::string after, std::string label)
        : m_handle(h), m_type(type)
        , m_before(std::move(before)), m_after(std::move(after))
        , m_label("Edit " + std::move(label)) {}

    void        Apply(EditContext& c) override  { Restore(c, m_after); }   // redo
    void        Revert(EditContext& c) override { Restore(c, m_before); }  // undo
    const char* Label() const override          { return m_label.c_str(); }
    size_t      HeapBytes() const override       { return m_before.size() + m_after.size() + m_label.size(); }

private:
    void Restore(EditContext& c, const std::string& blob)
    {
        if (!c.world || !c.reg || !c.world->IsHandleValid(m_handle)) return;
        // A REWRITE of a live component, not a plain deserialize: that would
        // reset its runtime state (a light's gather order - Ctrl+Z could hand
        // the sun to another light - an AudioSource's voice) and append a
        // Script slot per line instead of restoring the slot list.
        ComponentWrite::RewriteFromBlob(*c.reg, *c.world, m_handle.entity, m_type, blob, c.assets);
    }

    EntityHandle    m_handle;
    std::type_index m_type;
    std::string     m_before, m_after, m_label;
};

// Add Component. The initial add is done by the editor; this records how to
// undo (remove) and redo (add default) it.
class AddComponentAction : public IEditAction
{
public:
    using CompFn = std::function<void(World&, Entity)>;
    AddComponentAction(EntityHandle h, CompFn addFn, CompFn removeFn, std::string label)
        : m_handle(h), m_add(std::move(addFn)), m_remove(std::move(removeFn))
        , m_label("Add " + std::move(label)) {}

    void        Apply(EditContext& c) override  { if (Valid(c) && m_add)    m_add(*c.world, m_handle.entity); }
    void        Revert(EditContext& c) override { if (Valid(c) && m_remove) m_remove(*c.world, m_handle.entity); }
    const char* Label() const override          { return m_label.c_str(); }

private:
    bool Valid(EditContext& c) const { return c.world && c.world->IsHandleValid(m_handle); }
    EntityHandle m_handle;
    CompFn       m_add, m_remove;
    std::string  m_label;
};

// Remove Component. Captures the component's data blob BEFORE removal so undo
// can restore the exact prior state; falls back to a default re-add when the
// type has no serializer.
class RemoveComponentAction : public IEditAction
{
public:
    using CompFn = std::function<void(World&, Entity)>;
    RemoveComponentAction(EntityHandle h, std::type_index type,
                          CompFn addFn, CompFn removeFn,
                          std::string beforeBlob, std::string label)
        : m_handle(h), m_type(type)
        , m_add(std::move(addFn)), m_remove(std::move(removeFn))
        , m_blob(std::move(beforeBlob)), m_label("Remove " + std::move(label)) {}

    void Apply(EditContext& c) override   // redo = remove
    {
        if (Valid(c) && m_remove) m_remove(*c.world, m_handle.entity);
    }
    void Revert(EditContext& c) override  // undo = re-add + restore data
    {
        if (!Valid(c)) return;
        if (m_add) m_add(*c.world, m_handle.entity);   // ensure the slot exists
        if (c.reg && !m_blob.empty())
            DeserializeComponentBlob(*c.reg, *c.world, m_handle.entity, m_type, m_blob, c.assets);
    }
    const char* Label() const override { return m_label.c_str(); }
    size_t      HeapBytes() const override { return m_blob.size() + m_label.size(); }

private:
    bool Valid(EditContext& c) const { return c.world && c.world->IsHandleValid(m_handle); }
    EntityHandle    m_handle;
    std::type_index m_type;
    CompFn          m_add, m_remove;
    std::string     m_blob, m_label;
};

} // namespace Editor::Undo
