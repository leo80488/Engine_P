// EditorLayer_Properties.cpp - reflected property edits (Reflection/Property.h): the Inspector's
// field undo by path, a multi-selection edit applied field by field, the mixed fields of a
// selection, the Inspector's toolbar (field search, Modified only, Advanced, lock), the drawers'
// editor hooks (entity names, the asset picker), and the Editor.GetProperty / SetProperty /
// ResetProperty Lua calls.

#include "Editor/EditorLayer.h"

#include "Editor/Undo/PropertyEditAction.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform: the "Transform" tag
#include "Reflection/Property.h"
#include "Reflection/ReflectRegistry.h"
#include "Resource/AssetTypes.generated.h"
#include "Resource/ComponentSerializers.h"
#include "Scripting/LuaDoc.h"
#include "Scripting/LuaEntityId.h"
#include "System/Log.h"

#include "imgui/imgui.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    [[noreturn]] void Fail(const char* fn, const char* code, const std::string& what)
    {
        throw sol::error(std::string("Editor.") + fn + ": " + code + ": " + what);
    }

    const Reflect::TypeDescriptor* DescriptorOf(std::type_index type)
    {
        const Reflect::RuntimeEntry* e = Reflect::FindRuntime(type);
        return e ? e->desc : nullptr;
    }

    sol::object ToLua(sol::state_view lua, const Reflect::PropertyValue& v)
    {
        if (const bool* b = std::get_if<bool>(&v)) return sol::make_object(lua, *b);
        if (const int64_t* i = std::get_if<int64_t>(&v)) return sol::make_object(lua, static_cast<double>(*i));
        if (const double* d = std::get_if<double>(&v)) return sol::make_object(lua, *d);
        if (const std::string* s = std::get_if<std::string>(&v)) return sol::make_object(lua, *s);
        if (const Reflect::FloatVec* fv = std::get_if<Reflect::FloatVec>(&v))
        {
            sol::table t = lua.create_table();
            for (int i = 0; i < fv->n; ++i) t[i + 1] = fv->v[i];
            return t;
        }
        return sol::lua_nil;
    }

    // A Lua value as a property value of @p path's kind (numbers convert; a table is a float vector).
    bool FromLua(const sol::object& o, const Reflect::PropertyPath& path, Reflect::PropertyValue& out)
    {
        if (o.is<bool>()) { out = o.as<bool>(); return true; }
        if (o.is<double>())
        {
            const double d = o.as<double>();
            const Reflect::FieldDescriptor* f = path.Leaf();
            const bool integer = f && (f->kind == Reflect::FieldKind::I32 || f->kind == Reflect::FieldKind::U32 ||
                                       f->kind == Reflect::FieldKind::U8 || f->kind == Reflect::FieldKind::U16 ||
                                       f->kind == Reflect::FieldKind::I64 || f->kind == Reflect::FieldKind::Enum ||
                                       f->kind == Reflect::FieldKind::Flags || f->kind == Reflect::FieldKind::EntityRef);
            if (integer && d == static_cast<double>(static_cast<int64_t>(d))) out = static_cast<int64_t>(d);
            else out = d;
            return true;
        }
        if (o.is<std::string>()) { out = o.as<std::string>(); return true; }
        if (o.is<sol::table>())
        {
            Reflect::FloatVec fv;
            sol::table t = o.as<sol::table>();
            for (int i = 1; i <= 4; ++i)
            {
                const sol::object e = t[i];
                if (!e.valid() || e == sol::lua_nil) break;
                if (!e.is<double>()) return false;
                fv.v[fv.n++] = static_cast<float>(e.as<double>());
            }
            if (fv.n == 0) return false;
            out = fv;
            return true;
        }
        return false;
    }

    // ==== Drawer hooks ====

    // The editor's world member: the drawers name entities of whichever world it holds now.
    World* const* g_hookWorld = nullptr;

    const char* EntityName(uint32_t entity)
    {
        World* w = g_hookWorld ? *g_hookWorld : nullptr;
        if (!w || !w->IsAlive(entity)) return nullptr;
        return w->GetName(entity).c_str();
    }

    // The extensions an asset browser payload carries (the AssetTypes rows that name it).
    std::vector<const char*> PayloadExtensions(const char* payload)
    {
        std::vector<const char*> out;
        for (const Resource::AssetTypes::Row& row : Resource::AssetTypes::kRows)
            if (row.payload && std::strcmp(row.payload, payload) == 0) out.push_back(row.ext);
        return out;
    }

    // The assets under asset/ that a payload accepts, scanned at most every few seconds per payload
    // (the picker asks every frame it is open).
    std::vector<std::string> ListAssets(const char* payload)
    {
        struct Cached
        {
            std::chrono::steady_clock::time_point at;
            std::vector<std::string> paths;
        };
        static std::unordered_map<std::string, Cached> s_cache;
        const auto now = std::chrono::steady_clock::now();
        Cached& c = s_cache[payload];
        if (!c.paths.empty() && now - c.at < std::chrono::seconds(5)) return c.paths;
        c.at = now;
        c.paths.clear();
        const std::vector<const char*> exts = PayloadExtensions(payload);
        if (exts.empty()) return c.paths;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator("asset", ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (std::find_if(exts.begin(), exts.end(), [&](const char* e) { return ext == e; }) == exts.end()) continue;
            std::string path = it->path().generic_string();
            c.paths.push_back(std::move(path));
            if (c.paths.size() >= 4000) break;   // a picker, not an index
        }
        std::sort(c.paths.begin(), c.paths.end());
        return c.paths;
    }
}

void EditorLayer::InstallReflectionHooks()
{
    g_hookWorld = &m_world;
    Reflect::SetEntityNameResolver(&EntityName);
    Reflect::SetAssetLister(&ListAssets);
}

const char* EditorLayer::PropertyTagOf(std::type_index type)
{
    if (type == std::type_index(typeid(LocalTransform))) return "Transform";
    const Reflect::RuntimeEntry* e = Reflect::FindRuntime(type);
    return e && e->tag ? e->tag : "";
}

void EditorLayer::DrawInspectorToolbar()
{
    const float lockW = ImGui::CalcTextSize("Locked").x + ImGui::GetStyle().FramePadding.x * 2.f;
    const float checksW = ImGui::CalcTextSize("Modified").x + ImGui::CalcTextSize("Adv").x +
                          ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.x * 4.f +
                          ImGui::GetStyle().ItemInnerSpacing.x * 2.f;
    ImGui::SetNextItemWidth((std::max)(80.f, ImGui::GetContentRegionAvail().x - lockW - checksW));
    ImGui::InputTextWithHint("##inspectorSearch", "Search fields", m_inspectorSearch, sizeof(m_inspectorSearch));
    m_inspectorFilter.text = m_inspectorSearch;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Shows only the fields whose label, key or tooltip contains the text.");
    ImGui::SameLine();
    ImGui::Checkbox("Modified", &m_inspectorFilter.modifiedOnly);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Shows only the fields that differ from their component's default.");
    ImGui::SameLine();
    bool advanced = Reflect::ShowAdvanced();
    if (ImGui::Checkbox("Adv", &advanced)) Reflect::SetShowAdvanced(advanced);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Shows the fields marked Advanced.");
    ImGui::SameLine();
    const bool locked = m_inspectorLocked;
    if (locked) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(locked ? "Locked" : "Lock"))
    {
        m_inspectorLocked = !locked;
        if (m_inspectorLocked && m_world) m_inspectorLockHandle = m_world->MakeHandle(m_selectedEntity);
    }
    if (locked) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip(locked ? "The Inspector keeps this entity while the selection moves on. Click to follow the selection."
                                 : "Keep this entity in the Inspector while the selection moves on.");
}

std::shared_ptr<void> EditorLayer::SnapshotReflected(std::type_index type, const void* comp) const
{
    const Reflect::TypeDescriptor* desc = DescriptorOf(type);
    if (!comp || !desc || !desc->construct || !desc->destroy || !desc->copy || desc->size == 0) return {};
    const std::size_t align = desc->align ? desc->align : alignof(std::max_align_t);
    void* mem = ::operator new(desc->size, std::align_val_t(align));
    desc->construct(mem);
    // A trivially copyable type is copied byte for byte, padding included, so the commit can memcmp it.
    if (desc->trivial) std::memcpy(mem, comp, desc->size);
    else desc->copy(mem, comp);
    return std::shared_ptr<void>(mem, [desc, align](void* p) {
        desc->destroy(p);
        ::operator delete(p, std::align_val_t(align));
    });
}

bool EditorLayer::CommitReflectedEdit(Entity e, std::type_index type, const void* before, bool propagate)
{
    const Reflect::TypeDescriptor* desc = DescriptorOf(type);
    const auto entryIt = m_componentEditors.find(type);
    if (!m_world || !before || !desc || !desc->trivial || entryIt == m_componentEditors.end() || !entryIt->second.fetch)
        return false;
    const auto fetch = entryIt->second.fetch;
    void* comp = fetch(*m_world, e);
    if (!comp) return false;

    // Every changed field must be a writable path, and writing them onto the snapshot must give the
    // live component byte for byte: then the fields are the whole edit.
    std::vector<Reflect::PropertyPath> paths;
    for (const std::string& text : Reflect::Diff(*desc, before, comp, /*intoElements*/ true))
    {
        Reflect::PropertyPath path;
        std::string error;
        if (!Reflect::ParsePath(*desc, text, path, error) || !Reflect::Writable(path)) return false;
        paths.push_back(std::move(path));
    }
    if (paths.empty()) return false;
    std::vector<unsigned char> check(desc->size);
    std::memcpy(check.data(), before, desc->size);
    for (const Reflect::PropertyPath& path : paths)
        if (!Reflect::WriteProperty(check.data(), path, Reflect::Get(comp, path), Reflect::WriteOrigin::Undo).ok) return false;
    if (std::memcmp(check.data(), comp, desc->size) != 0) return false;

    const std::string label = ComponentLabelFor(type);
    for (const Reflect::PropertyPath& path : paths)
        PushEdit(std::make_unique<Editor::Undo::PropertyEditAction>(
            m_world->MakeHandle(e), fetch, path, Reflect::Get(before, path), Reflect::Get(comp, path), label));
    if (!propagate) return true;

    // The rest of the selection: only the fields this gesture changed, each written in place through
    // the same writer (the primary was edited in place too), each one undo step inside the caller's macro.
    std::size_t entities = 0, skipped = 0;
    std::string skippedFields;
    for (Entity other : m_selectedEntities)
    {
        if (other == e || !m_world->IsAlive(other)) continue;
        void* target = fetch(*m_world, other);
        if (!target) continue;   // the field is not common: the entity lacks the component
        bool any = false;
        for (const Reflect::PropertyPath& path : paths)
        {
            if (Reflect::Has(*path.Leaf(), Reflect::FieldFlag::NoMultiEdit)) continue;
            const Reflect::PropertyValue value = Reflect::Get(comp, path);
            if (!Reflect::Address(target, path))
            {
                // An element the other entity does not have (its vector is shorter).
                ++skipped;
                if (skippedFields.find(path.text) == std::string::npos) skippedFields += (skippedFields.empty() ? "" : ", ") + path.text;
                continue;
            }
            if (Reflect::Equal(Reflect::Get(target, path), value)) continue;
            const Reflect::WriteResult r = Reflect::WriteProperty(target, path, value, Reflect::WriteOrigin::Inspector);
            if (!r.ok) continue;
            PushEdit(std::make_unique<Editor::Undo::PropertyEditAction>(
                m_world->MakeHandle(other), fetch, path, r.before, r.written, label));
            any = true;
        }
        if (any) ++entities;
    }
    if (entities > 0)
        LOG_INFO("Inspector: applied %s edit to %zu more selected entit%s (by property: %zu field%s)",
                 label.c_str(), entities, entities == 1 ? "y" : "ies", paths.size(), paths.size() == 1 ? "" : "s");
    if (skipped > 0)
        LOG_INFO("Inspector: %s edit not applied where the element is missing (%s)", label.c_str(), skippedFields.c_str());
    return true;
}

std::vector<const Reflect::FieldDescriptor*> EditorLayer::MixedReflectedFields(Entity primary, std::type_index type) const
{
    std::vector<const Reflect::FieldDescriptor*> out;
    const Reflect::TypeDescriptor* desc = DescriptorOf(type);
    const auto entryIt = m_componentEditors.find(type);
    if (!m_world || !desc || m_selectedEntities.size() < 2 || entryIt == m_componentEditors.end() || !entryIt->second.fetch)
        return out;
    const void* comp = entryIt->second.fetch(*m_world, primary);
    if (!comp) return out;
    for (Entity other : m_selectedEntities)
    {
        if (other == primary || !m_world->IsAlive(other)) continue;
        const void* target = entryIt->second.fetch(*m_world, other);
        if (!target) continue;
        for (const std::string& text : Reflect::Diff(*desc, comp, target))
        {
            Reflect::PropertyPath path;
            std::string error;
            if (!Reflect::ParsePath(*desc, text, path, error)) continue;
            const Reflect::FieldDescriptor* top = path.steps.front().field;
            if (std::find(out.begin(), out.end(), top) == out.end()) out.push_back(top);
        }
    }
    return out;
}

void EditorLayer::RegisterPropertyLua(lua_State* L)
{
    sol::state_view lua(L);
    sol::table ed = lua["Editor"].get_or_create<sol::table>();

    // "Tag.path" -> the component's type, its descriptor and the parsed path; Fail()s with the reason.
    struct Resolved
    {
        std::type_index type = std::type_index(typeid(void));
        Reflect::PropertyPath path;
    };
    auto resolve = [this](const char* fn, const std::string& text) -> Resolved
    {
        const std::size_t dot = text.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == text.size())
            Fail(fn, "bad_value", "a property is \"Tag.path\" (\"RigidBody.linDamp\", \"Transform.position.y\")");
        const std::string tag = text.substr(0, dot);
        Resolved r;
        bool found = false;
        if (tag == "Transform") { r.type = std::type_index(typeid(LocalTransform)); found = true; }
        else
            for (const auto& [type, serializer] : GetComponentRegistry().All())
                if (serializer.tag == tag) { r.type = type; found = true; break; }
        if (!found) Fail(fn, "unknown_component", "no component '" + tag + "'");
        const Reflect::TypeDescriptor* desc = DescriptorOf(r.type);
        if (!desc) Fail(fn, "not_reflected", "'" + tag + "' has no field descriptor (its inspector is hand-written)");
        std::string error;
        if (!Reflect::ParsePath(*desc, text.substr(dot + 1), r.path, error)) Fail(fn, "bad_path", error);
        r.path.text = text.substr(dot + 1);
        return r;
    };
    auto target = [this](const char* fn, const LuaEntity& id, const Resolved& r) -> std::pair<Entity, void*>
    {
        if (!m_world) Fail(fn, "no_world", "the editor has no world");
        const Entity e = LuaId::ResolveLive(*m_world, id);
        if (e == NullEntity) Fail(fn, "stale_handle", "the entity is not alive (destroyed, or its id was reused)");
        const auto it = m_componentEditors.find(r.type);
        void* comp = it != m_componentEditors.end() && it->second.fetch ? it->second.fetch(*m_world, e) : nullptr;
        if (!comp) Fail(fn, "missing_component", "the entity has no " + ComponentLabelFor(r.type) + " component");
        return { e, comp };
    };
    // One undoable write, as an Inspector edit of that field.
    auto write = [this](const char* fn, Entity e, void* comp, const Resolved& r, const Reflect::PropertyValue& value,
                        Reflect::WriteOrigin origin, sol::this_state ts) -> sol::table
    {
        if (GetPlayState() != ViewportPlayState::Stopped)
            Fail(fn, "requires_edit_mode", "property edits are Edit-mode only (Stop Play first)");
        const Reflect::WriteResult w = Reflect::WriteProperty(comp, r.path, value, origin);
        if (!w.ok) Fail(fn, "refused", w.error);
        if (UndoEnabled() && !Reflect::Equal(w.before, w.written))
        {
            SyncedEditContext();
            PushEdit(std::make_unique<Editor::Undo::PropertyEditAction>(
                m_world->MakeHandle(e), m_componentEditors.at(r.type).fetch, r.path, w.before, w.written,
                ComponentLabelFor(r.type)));
        }
        sol::state_view view(ts);
        sol::table t = view.create_table();
        t["before"] = ToLua(view, w.before);
        t["value"] = ToLua(view, w.written);
        t["clamped"] = w.clamped;
        return t;
    };

    LuaDoc::Fn(ed, "Editor", "GetProperty",
        [resolve, target](sol::this_state ts, const LuaEntity& id, const std::string& text) -> sol::object
        {
            const Resolved r = resolve("GetProperty", text);
            const auto [e, comp] = target("GetProperty", id, r);
            if (!Reflect::Writable(r.path)) Fail("GetProperty", "bad_path", "'" + text + "' has no single value");
            return ToLua(sol::state_view(ts), Reflect::Get(comp, r.path));
        },
        { .doc = "Reads one reflected field of an entity's component by \"Tag.path\": the field's key or C++ name, "
                 "then a component (Transform.position.y), a split-key part (CvRect.aMinX) or an element "
                 "(Beam.controlPoints[1].radius).",
          .params = { { "id", "an entity id", "integer" }, { "path", "\"Tag.path\", e.g. \"RigidBody.linDamp\"", "string" } },
          .returns = "any", .returnsDoc = "a number, boolean, string or {x, y, z[, w]}",
          .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor,
          .errors = { "stale_handle", "unknown_component", "not_reflected", "missing_component", "bad_path" } });
    LuaDoc::Fn(ed, "Editor", "SetProperty",
        [resolve, target, write](sol::this_state ts, const LuaEntity& id, const std::string& text, sol::object value,
                                 sol::optional<sol::table> options) -> sol::table
        {
            const Resolved r = resolve("SetProperty", text);
            const auto [e, comp] = target("SetProperty", id, r);
            Reflect::PropertyValue v;
            if (!FromLua(value, r.path, v)) Fail("SetProperty", "bad_value", "a number, boolean, string or {x, y, z[, w]}");
            const bool clamp = options && options->get_or("clamp", false);
            return write("SetProperty", e, comp, r, v, clamp ? Reflect::WriteOrigin::Inspector : Reflect::WriteOrigin::Tool, ts);
        },
        { .doc = "Writes one reflected field through the Inspector's writer (Reflect::WriteProperty), one undo step "
                 "that puts back only that field. Refuses a read-only field, a value past the hard range "
                 "({clamp = true} clamps it, as the Inspector does), an enum value that is not an option and a "
                 "value of the wrong kind.",
          .params = { { "id", "an entity id", "integer" }, { "path", "\"Tag.path\", e.g. \"RigidBody.mass\"", "string" },
                      { "value", "a number, boolean, string or {x, y, z[, w]}", "any" },
                      { "opts", "{clamp = true}: clamp to the hard range instead of refusing", "table", true } },
          .returns = "table", .returnsDoc = "{before, value, clamped}",
          .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "stale_handle", "unknown_component", "not_reflected", "missing_component", "bad_path",
                      "bad_value", "refused", "requires_edit_mode" } });
    LuaDoc::Fn(ed, "Editor", "ResetProperty",
        [resolve, target, write](sol::this_state ts, const LuaEntity& id, const std::string& text) -> sol::table
        {
            const Resolved r = resolve("ResetProperty", text);
            const auto [e, comp] = target("ResetProperty", id, r);
            const void* defaults = Reflect::DefaultInstance(*r.path.type);
            if (!defaults) Fail("ResetProperty", "refused", "the component has no default instance");
            return write("ResetProperty", e, comp, r, Reflect::Get(defaults, r.path), Reflect::WriteOrigin::Tool, ts);
        },
        { .doc = "Writes a reflected field's default (the component's T{} value) as Editor.SetProperty does, one "
                 "undo step.",
          .params = { { "id", "an entity id", "integer" }, { "path", "\"Tag.path\"", "string" } },
          .returns = "table", .returnsDoc = "{before, value, clamped}",
          .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "stale_handle", "unknown_component", "not_reflected", "missing_component", "bad_path",
                      "refused", "requires_edit_mode" } });
}
