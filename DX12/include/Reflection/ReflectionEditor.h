#pragma once

// ReflectionEditor - generic ImGui drawer for Reflect::TypeDescriptor.
// Emits one ImGui control per field (Drag / Color / Combo / Checkbox / InputText) from each
// FieldDescriptor's FieldKind + Widget hint. EditorLayer's RegisterReflectedComponent<T>() wraps
// DrawObject, so most pure-data components need no hand-written ImGui code.

#include "Reflection/Reflect.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Reflect
{

// Edit lifecycle aggregated over the fields drawn in one DrawObject call. Lets the editor bracket
// a continuous edit (a drag scrub) into one undo entry: capture "before" on activated, keep the
// bracket open while active, commit on the first idle frame after. changed marks a value write
// this frame (instant widgets: checkbox / combo / drop).
struct EditSignal
{
    bool activated = false;   // a field's IsItemActivated() fired this frame
    bool active    = false;   // a field's IsItemActive() (still being held)
    bool changed   = false;   // a field value changed this frame
};

// Draws every field of desc for the object at obj. Returns true if any field changed this frame
// (mark dirty); a non-null sig receives the aggregated edit lifecycle.
bool DrawObject(void* obj, const TypeDescriptor& desc, EditSignal* sig);

// Draws one field, so callers can interleave reflected fields with hand-written widgets.
bool DrawField(void* obj, const FieldDescriptor& f, EditSignal* sig);

// Two-argument forms: no edit-signal tracking.
inline bool DrawObject(void* obj, const TypeDescriptor& desc) { return DrawObject(obj, desc, nullptr); }
inline bool DrawField (void* obj, const FieldDescriptor& f)   { return DrawField (obj, f, nullptr); }

// Chain Physics Preview layout: while the push depth is > 0, InfoText / InfoTextFn fields
// (REFLECT_INFO) draw as TextDisabled wrapped at the visible right edge instead of one line. A
// width-aware caller (the preview's parameter tree) brackets its DrawObject with Push/Pop. UI
// thread only.
void PushInfoWrap();
void PopInfoWrap();
// Chain Physics Preview layout (harness only): while set, every REFLECT_COLLAPSE section is forced
// open, so the layout pack measures the expanded tree.
void SetForceOpenSections(bool on);

// Inspector switch: FieldFlag::Advanced fields are drawn only while it is on. UI thread.
void SetShowAdvanced(bool on);
bool ShowAdvanced();

// EntityRef fields: the editor names an entity id of the edited world ("" when it has no name);
// nullptr until the editor installs one. UI thread.
using EntityNameFn = const char* (*)(uint32_t entity);
void SetEntityNameResolver(EntityNameFn fn);

// A multi-selection: the fields whose value differs across it are drawn mixed, and a NoMultiEdit
// field says it applies to this entity only. Set around one DrawObject, cleared after (nullptr, 1).
void SetMultiSelection(const std::vector<const FieldDescriptor*>* mixedFields, int selected);

// The component being drawn, for each field's row chrome (EditorUI::PropertyRowChrome): its
// serializer tag ("RigidBody"; "" = paths without a tag) and its entity's Lua id (0 = no Lua call
// offered). Set around one DrawObject, cleared after ("", 0).
void SetDrawContext(const char* tag, double luaEntity);

// An asset reference's "..." picker: the asset paths that accept a browser payload ("ITEX_PATH"),
// installed by the editor; nullptr = no picker.
using AssetListFn = std::vector<std::string> (*)(const char* payload);
void SetAssetLister(AssetListFn fn);

// True once after a row's right-click Paste / Reset wrote a field (a menu item holds no active id, so
// an undo bracket keyed on the active widget cannot see it); the Inspector records it then.
bool ConsumeRowActionEdit();

// True when the active row filter (EditorUI::ActiveRowFilter) shows a field of @p desc on @p obj, or
// no filter is active: the Inspector hides a component it would show nothing of.
bool AnyFieldShown(const void* obj, const TypeDescriptor& desc);

// The display suffix of a field's unit ("kg", "/s", "m", ...); nullptr when it shows none.
const char* UnitSuffix(const char* unit);

template <typename T>
inline bool DrawObject(T* obj)
{
    return DrawObject(static_cast<void*>(obj), Describe<T>(), nullptr);
}

} // namespace Reflect
