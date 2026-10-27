// The Lua `Editor` table: scripted authoring through the editor's own undo,
// selection and dirty state.
//
// Every function is an ordinary editor edit - the same helpers the menus, the
// Outliner and the viewport drops call (RecordSpawn, DeleteEntities,
// DuplicateEntity, ReparentWithUndo, AssignMaterial) - so a scripted edit is undone
// with Ctrl+Z, dirties the scene and saves exactly like one made by hand. Targets
// are always named: nothing depends on the current selection. Edit mode only
// (Play-mode edits are runtime state that Stop discards).
//
//   Editor.Batch(label, fn, ...)   fn's results; ONE undo entry, all or nothing:
//                                  an error anywhere reverts every edit fn made
//   Editor.Create{name, parent, position, rotation, scale, components = {Tag = {k = v}}}
//   Editor.CreatePrimitive(kind, {name, parent, position, rotation, scale})
//   Editor.InstantiatePrefab(path, {name, parent, position, rotation, scale})   .ipfb, or an .iscn model
//   Editor.DetachPrefab(root)
//   Editor.Delete(id | {ids})  Editor.Duplicate(id)  Editor.Reparent(id, parent|nil)
//   Editor.Rename(id, name)    Editor.AssignMaterial(id, path)
//   Editor.AddComponent(id, tag, {k = v})  Editor.RemoveComponent(id, tag)
//   Editor.Components()        -> {{tag, label, category}}: what those two take
//   Editor.SavePrefab(root, path[, {link = true}]) -> {path, guid, bytes, materials, linked, members}:
//                                  Save as Prefab; link: the source becomes its instance (undoable)
//   Editor.CreateMaterialInstance(parent, {dest, params = {name = value},
//                                  textures = {SLOT = path | false}, meta = {key = value},
//                                  custom = {name = value}, customTextures = {name = path | false}})
//                                  -> {path, parent, guid, overrides}: an .imatinst
//   Editor.Undo()  Editor.Redo()  Editor.State()
//   Editor.SaveScene([path])  Editor.NewScene{discard}  Editor.OpenScene(path, {discard})
//   Editor.Import(source, {dest = "<.itex>", format})   a texture; a model: {dest = "<directory>"}
//                                  -> job ids (`job wait`): they run in the deferred
//                                  frame-boundary slot, like File > ...
//   Editor.Play()  Editor.Pause()  Editor.Step()  Editor.Stop()
//                                  -> job ids: the toolbar's transitions, done when the
//                                  next frame boundary applied them (ProcessScriptedPlay);
//                                  State().mode is edit | playing | paused | stepping
//
// An edit the save would drop is refused, not made: inside a prefab instance (saved
// as a reference to its .ipfb) or under an .iscn placement (its parts come from the
// file) - see SavedByOf - and on a system-owned helper the save omits
// (SceneTransientTag: the editor camera, a preview stage) - see RefuseTransient.
//
// Errors raise "Editor.<Fn>: <code>: <what>" (the Entity.set convention). Codes
// include requires_edit_mode, no_world, stale_handle, transient, bad_value,
// unknown_component, unknown_key, no_material, load_failed, cycle, prefab_instance,
// scene_instance, unsaved_changes, busy, nothing_to_undo, aborted, unsupported_source,
// not_addable, already_present, not_allowed, no_component, write_failed, refused,
// not_found, already_playing, already_paused, not_playing, not_paused; a play job
// fails "superseded".

#include "Editor/EditorLayer.h"
#include "Editor/EditorCommands.h"   // Editor.Commands / Editor.Command
#include "Editor/EditorUITests.h"    // Editor.RunUITests
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/MeshEditorWindow.h"         // Editor.OpenMeshEditor / MeshEditor*
#include "Editor/SkeletonEditorWindow.h"     // Editor.OpenSkeletonEditor / SkeletonEditor*
#include "Renderer/TextureViewService.h"     // Editor.TextureViewer().views
#include "Renderer/MaterialPermutation.h"    // MaterialShaderReflection
#include "Editor/TextureViewerWindow.h"      // Editor.OpenTextureViewer / TextureViewer*
#include "Editor/UIEditorWindow.h"           // Editor.OpenUIEditor / UIEditor*
#include "Editor/UITemplates.h"              // Editor.UIEditorCreate's widget names
#include "Renderer/UIViewService.h"          // Editor.UIEditor().views
#include "Scripting/LuaDoc.h"                // the UI Editor bindings are described (test_lua_doc_ratchet)
#include "ECS/HierarchyOps.h"                // InstantiatePrefab: a refused UI placement leaves nothing
#include "UI/UICanvas.h"                     // Create / Reparent / InstantiatePrefab under a UI node
#include "UI/UICanvasLayout.h"
#include "Editor/MaterialEditorWindow.h"     // Editor.OpenMaterialEditor / MaterialEditor*
#include "MaterialGraph/MaterialGraphNodes.h"  // Editor.MaterialGraph*: node labels
#include "System/Sha256.h"                     // ... the compiled HLSL's hash
#include "imgui/imgui.h"                      // ImGui::GetTime - State().slotFlash's age

#include "Editor/Undo/ComponentEditAction.h"   // Add/Remove/ComponentEditAction (+ ComponentBlob.h)
#include "Editor/Undo/EditHistory.h"
#include "Editor/Undo/RenameAction.h"
#include "Editor/Undo/StructuralAction.h"
#include "Editor/Undo/TransformEditAction.h"   // AttachToSocket zeroes the follower's LocalTransform
#include "Editor/Undo/ValueEditAction.h"
#include "ECS/AnimationComponents.h"           // SocketComponent (AttachToSocket)
#include "ECS/FollowComponents.h"              // FollowSocketComponent
#include "ECS/Components.h"
#include "ECS/GuidRegistry.h"               // SavePrefab {link}: the root's identity is baked first
#include "ECS/HierarchyComponents.h"
#include "ECS/StaticMeshComponents.h"       // AssignMaterial {slot}: a Static Mesh owner's slot
#include "ECS/PlayModeController.h"         // Editor.Play / Pause / Step / Stop
#include "Harness/ControlChannelJobs.h"
#include "ECS/MaterialReflectionSync.h"     // CreateMaterialInstance: the custom shader's parameters
#include "ECS/MaterialSchema.h"             // CreateMaterialInstance: params by schema name
#include "Graphics/ShaderLibrary.h"
#include "Graphics/ShadingModel.h"
#include "Graphics/ShadingModelTable.h"     // the customShadingModel names, one per table row
#include "Renderer/Renderer.h"              // RegisterMaterialShader
#include "Resource/AssetFS.h"               // CreateMaterialInstance: texture paths through the search roots
#include "Resource/AssetGuid.h"             // SavePrefab / CreateMaterialInstance mint identities
#include "Resource/AssetRefs.h"             // Editor.ActivateAsset: the listing's paths compared normalised
#include "Resource/ComponentSerializers.h"
#include "Resource/MaterialDomain.h"
#include "Resource/MaterialSerializer.h"    // LoadMaterial, SaveMaterialInstance, ApplyMaterialTextDelta
#include "Resource/MeshLibrary.h"
#include "Resource/MeshLibIdentity.h"       // AssignMaterial {slot}: SectionKey spelling
#include "Math/MathUtils.h"               // the socket verbs' Euler order (RollPitchYaw)
#include "Resource/PrefabSerializer.h"      // LoadPrefab, SavePrefab
#include "Resource/SkeletonAsset.h"         // Editor.SkeletonEditor*: bone names of the open skeleton
#include "Resource/HumanoidBone.h"          // Editor.SkeletonEditorHumanoid*: role names
#include "Scene/MeshAssetRegistry.h"        // AssignMaterial {slot}: the owner's slots
#include "Resource/TextureImporter.h"       // Editor.Import
#include "Resource/TextureImportRecord.h"   // Editor.Reimport of a texture
#include "Scene/MeshSpawner.h"              // the Create > 3D Object meshes
#include "Scene/SceneInstanceLoader.h"      // InstantiatePrefab of an .iscn model
#include "Scripting/LuaEntityId.h"
#include "Scripting/LuaEntityWrite.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <tuple>
#include <unordered_set>

namespace
{
    [[noreturn]] void Fail(const char* fn, const char* code, const std::string& what)
    {
        throw sol::error(std::string("Editor.") + fn + ": " + code + ": " + what);
    }

    std::uint64_t RequiredJob(std::uint64_t id)
    {
        if (!id) Fail("Job", "busy", "active job admission limit reached; wait for accepted work");
        return id;
    }

    std::uint64_t AdmitJob(const char* kind, std::string detail, std::uint32_t request)
    { return RequiredJob(Harness::Jobs::TryCreate(kind, std::move(detail), request)); }

    // A humanoid role by its name (HumanoidRoleName: "hips", "leftHand", ...);
    // -1 = none of them.
    int SkeHumanoidRole(const std::string& name)
    {
        for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r)
            if (name == HumanoidRoleName(static_cast<HumanoidBone>(r))) return static_cast<int>(r);
        return -1;
    }

    // A json value as Lua: objects -> tables, arrays -> 1-based tables.
    sol::object JsonToLua(sol::state_view& view, const nlohmann::json& j)
    {
        switch (j.type())
        {
        case nlohmann::json::value_t::object:
        {
            sol::table t = view.create_table();
            for (auto it = j.begin(); it != j.end(); ++it) t[it.key()] = JsonToLua(view, it.value());
            return t;
        }
        case nlohmann::json::value_t::array:
        {
            sol::table t = view.create_table();
            for (size_t i = 0; i < j.size(); ++i) t[i + 1] = JsonToLua(view, j[i]);
            return t;
        }
        case nlohmann::json::value_t::string:          return sol::make_object(view, j.get<std::string>());
        case nlohmann::json::value_t::boolean:         return sol::make_object(view, j.get<bool>());
        case nlohmann::json::value_t::number_integer:  return sol::make_object(view, j.get<int64_t>());
        case nlohmann::json::value_t::number_unsigned: return sol::make_object(view, j.get<uint64_t>());
        case nlohmann::json::value_t::number_float:    return sol::make_object(view, j.get<double>());
        default:                                       return sol::make_object(view, sol::lua_nil);
        }
    }

    // A Lua value as json (the inverse of JsonToLua): a table whose keys are exactly
    // 1..n is an array, any other table an object; an empty table is an empty array.
    nlohmann::json LuaToJson(const sol::object& o, int depth = 0)
    {
        if (depth > 32) return nullptr;
        switch (o.get_type())
        {
        case sol::type::boolean: return o.as<bool>();
        case sol::type::number:
        {
            const double d = o.as<double>();
            if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9.0e15) return static_cast<long long>(d);
            return d;
        }
        case sol::type::string:  return o.as<std::string>();
        case sol::type::table:
        {
            const sol::table t = o.as<sol::table>();
            std::size_t count = 0;
            bool array = true;
            for (const auto& [k, v] : t)
            {
                ++count;
                if (k.get_type() != sol::type::number) array = false;
            }
            if (count == 0) return nlohmann::json::array();
            if (array && t.size() == count)
            {
                nlohmann::json a = nlohmann::json::array();
                for (std::size_t i = 1; i <= count; ++i) a.push_back(LuaToJson(t[i], depth + 1));
                return a;
            }
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& [k, v] : t)
            {
                const std::string key = k.get_type() == sol::type::string ? k.as<std::string>()
                                                                          : std::to_string(k.as<long long>());
                obj[key] = LuaToJson(v, depth + 1);
            }
            return obj;
        }
        default: return nullptr;
        }
    }

    const char* ModeName(EngineMode m)
    {
        switch (m)
        {
            case EngineMode::Edit:     return "edit";
            case EngineMode::Playing:  return "playing";
            case EngineMode::Paused:   return "paused";
            case EngineMode::Stepping: return "stepping";
        }
        return "?";
    }

    // {x, y, z} / {x, y, z, w} - a Lua array of finite numbers, exactly n long.
    bool ReadNumbers(const sol::object& v, float* out, size_t n)
    {
        if (!v.is<sol::table>()) return false;
        const sol::table t = v.as<sol::table>();
        if (t.size() != n) return false;
        for (size_t i = 0; i < n; ++i)
        {
            const sol::object e = t[i + 1];
            if (!e.is<double>()) return false;
            const double d = e.as<double>();
            if (!std::isfinite(d) || std::fabs(d) > Resource::Authoring::FloatMax) return false;
            out[i] = static_cast<float>(d);
        }
        return true;
    }

    // ---- CreateMaterialInstance: the meta keys ----
    // What an .imatinst may override besides schema parameters, texture slots and the
    // custom shader's own parameters: every key MaterialSerializer's BuildTextImpl
    // writes when it is in the explicit override set (the shaderType tombstone aside).
    // A value is checked by its key's type and spelled the way the file spells it, then
    // applied through the loader's own parser (ApplyMaterialTextDelta), so it means
    // what it will mean on load.
    enum class MetaType { Bool, Int, UInt, Float, Vec2, Enum, Path };
    struct MetaKey
    {
        const char*        key;
        MetaType           type;
        const char* const* names = nullptr;   // Enum: the index is the serialized value
        int                count   = 0;
    };
    constexpr const char* kBlendModeNames[]  = { "Opaque", "Alpha", "Additive", "Premultiplied", "Multiply",
                                                 "Masked", "Dithered", "Transmissive" };
    constexpr const char* kShadowCullNames[] = { "Default", "Front", "None" };
    constexpr const char* kDomainNames[]     = { "Opaque", "Transparent", "Decal", "PostProcess" };
    // The enumerators' own names (ShadingModelName is the Inspector's display text),
    // one per row of the shading-model table, in value order.
#define SMT_ENUM_NAME(name, ...) #name,
    constexpr const char* kShadingModelNames[] = { SHADING_MODEL_TABLE(SMT_ENUM_NAME) };
#undef SMT_ENUM_NAME
    static_assert(std::size(kShadingModelNames) == static_cast<size_t>(ShadingModel::Count),
                  "a ShadingModel without a name");
    static_assert(std::size(kBlendModeNames) == static_cast<size_t>(BlendMode::Count), "a BlendMode without a name");
    static_assert(std::size(kDomainNames) == static_cast<size_t>(Resource::MaterialDomain::Count),
                  "a MaterialDomain without a name");
    static_assert(static_cast<int>(ShadowCullMode::None) + 1 == static_cast<int>(std::size(kShadowCullNames)),
                  "a ShadowCullMode without a name");
    constexpr MetaKey kMetaKeys[] = {
        { "blendMode",      MetaType::Enum, kBlendModeNames,  static_cast<int>(std::size(kBlendModeNames)) },
        { "shadowCullMode", MetaType::Enum, kShadowCullNames, static_cast<int>(std::size(kShadowCullNames)) },
        { "domain",         MetaType::Enum, kDomainNames,     static_cast<int>(std::size(kDomainNames)) },
        { "customShadingModel", MetaType::Enum, kShadingModelNames, static_cast<int>(std::size(kShadingModelNames)) },
        { "castShadow",      MetaType::Bool }, { "receiveShadow",       MetaType::Bool },
        { "doubleSided",     MetaType::Bool }, { "outline",             MetaType::Bool },
        { "outlineScreenSpace", MetaType::Bool },
        { "excludeFromSsao", MetaType::Bool }, { "hairMask",            MetaType::Bool },
        { "nprToonShadowEdge", MetaType::Bool }, { "sourceOutlineExempt", MetaType::Bool },
        { "useVertexColor",  MetaType::Bool }, { "nprClampEnabled",     MetaType::Bool },
        { "useCustomShader", MetaType::Bool }, { "rtUseStandardMaterial", MetaType::Bool },
        { "decalWriteMask",  MetaType::UInt }, { "decalSortLayer",      MetaType::Int },
        { "decalAngleFade",  MetaType::Float }, { "outlinePixels",      MetaType::Float },
        { "texTiling",       MetaType::Vec2 }, { "texOffset",           MetaType::Vec2 },
        { "customShaderPath", MetaType::Path }, { "customVertexShaderPath", MetaType::Path },
    };

    std::string LowerAscii(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // @p v as the file's text for @p m; empty, with @p code / @p why set, when
    // it is not a value of that type.
    std::string MetaValueText(const MetaKey& m, const sol::object& v, const char*& code, std::string& why)
    {
        code = "bad_value";
        char buf[64];
        double d = 0.0;
        const bool num = v.is<double>() && std::isfinite(d = v.as<double>());
        switch (m.type)
        {
        case MetaType::Bool:
            if (v.is<bool>()) return v.as<bool>() ? "1" : "0";
            why = "wants true or false";
            return {};
        case MetaType::Int:
        case MetaType::UInt:
            if (num && d == std::floor(d) && std::fabs(d) <= 2147483647.0 && (m.type == MetaType::Int || d >= 0))
                return std::to_string(static_cast<long long>(d));
            why = m.type == MetaType::Int ? "wants a whole number" : "wants a whole number >= 0";
            return {};
        case MetaType::Float:
            if (num) { std::snprintf(buf, sizeof(buf), "%.6f", d); return buf; }
            why = "wants a number";
            return {};
        case MetaType::Vec2:
        {
            float uv[2]{};
            if (ReadNumbers(v, uv, 2)) { std::snprintf(buf, sizeof(buf), "%.6f %.6f", uv[0], uv[1]); return buf; }
            why = "wants {u, v}";
            return {};
        }
        case MetaType::Enum:
            if (v.is<std::string>())
            {
                const std::string want = LowerAscii(v.as<std::string>());
                for (int i = 0; i < m.count; ++i)
                    if (LowerAscii(m.names[i]) == want) return std::to_string(i);
            }
            else if (num && d == std::floor(d) && d >= 0 && d < m.count)
                return std::to_string(static_cast<int>(d));
            why = "wants one of";
            for (int i = 0; i < m.count; ++i) why += std::string(i ? ", " : " ") + m.names[i];
            why += " (or its number)";
            return {};
        case MetaType::Path:
            if (v.is<bool>() && !v.as<bool>()) return "<none>";   // cleared: the parent's is NOT inherited
            if (v.is<std::string>())
            {
                std::string p = v.as<std::string>();
                for (char& c : p) if (c == '\\') c = '/';
                std::error_code ec;
                if (!p.empty() && std::filesystem::is_regular_file(Sys::U8Path(p), ec)) return p;
                code = "load_failed";
                why = "names no shader file: " + p;
                return {};
            }
            why = "wants a shader path, or false to clear it";
            return {};
        }
        why = "has no type";
        return {};
    }

    // ---- CreateMaterialInstance: custom shader reflection ----
    // The custom shaders' parameters as the Inspector knows them: each stage's
    // reflection Synced onto @p probe, a throwaway material carrying only the shader
    // paths. The id comes from Renderer::RegisterMaterialShader (one registry for every
    // library) and the reflection from the GBuffer pass's library, compiled on first use.
    bool ReflectCustomShaders(Renderer* renderer, MaterialComponent& probe, std::string& why)
    {
        ShaderLibrary* lib = renderer ? renderer->GetMaterialShaderLibrary() : nullptr;
        if (!lib)
        {
            why = "no renderer to reflect the custom shader with";
            return false;
        }
        struct Stage { const std::string* path; RHI::ShaderStage stage; };
        const Stage stages[] = { { &probe.customShaderPath,       RHI::ShaderStage::PS },
                                 { &probe.customVertexShaderPath, RHI::ShaderStage::VS } };
        for (const Stage& s : stages)
        {
            if (s.path->empty()) continue;
            uint32_t id = lib->FindDynamic(s.path->c_str(), s.stage);
            if (id == ShaderLibrary::kInvalidDynShaderID)
                id = renderer->RegisterMaterialShader(s.path->c_str(), s.stage);
            // The renderer's own reflection for this material.
            const ShaderReflect::Reflection* refl =
                id != ShaderLibrary::kInvalidDynShaderID ? MaterialShaderReflection(*lib, id, probe) : nullptr;
            if (!refl)
            {
                why = "the custom shader did not compile, so its parameters are unknown: " + *s.path;
                return false;
            }
            MaterialReflectionSync::Sync(probe, *refl);
        }
        return true;
    }

    // ---- Create / AddComponent: Lua values as serializer text ----
    // Space, '%' and control characters as %XX, as Entity.set writes them: the
    // first two split a serializer token, a raw control character the line.
    // The serializers decode any %XX.
    std::string EncodeText(const std::string& s)
    {
        std::string r;
        for (unsigned char c : s)
        {
            if (c == '%' || c == ' ' || c < 0x20)
            {
                char b[4];
                std::snprintf(b, sizeof(b), "%%%02X", c);
                r += b;
            }
            else r += static_cast<char>(c);
        }
        return r;
    }

    // One Lua value as the text a serializer line carries (what .iscene has).
    bool ValueText(const sol::object& v, std::string& out)
    {
        switch (v.get_type())
        {
        case sol::type::boolean: out = v.as<bool>() ? "1" : "0"; return true;
        case sol::type::number:
        {
            const double d = v.as<double>();
            if (!std::isfinite(d)) return false;
            char buf[40];
            const auto r = (d == std::floor(d) && std::fabs(d) < 1e15)
                ? std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(d))
                : std::to_chars(buf, buf + sizeof(buf), d);
            out.assign(buf, r.ptr);
            return true;
        }
        case sol::type::string: out = EncodeText(v.as<std::string>()); return true;
        case sol::type::table:
        {
            // A vector: numbers joined by '_' ("1_0.5_0").
            const sol::table t = v.as<sol::table>();
            if (t.size() == 0) return false;
            out.clear();
            for (size_t i = 1; i <= t.size(); ++i)
            {
                std::string one;
                const sol::object e = t[i];
                if (e.get_type() != sol::type::number || !ValueText(e, one)) return false;
                if (i > 1) out.push_back('_');
                out += one;
            }
            return true;
        }
        default: return false;
        }
    }

    // The keys a serializer line carries ("  Tag: a=1 b=2" -> {a, b}).
    std::unordered_set<std::string> LineKeys(const std::string& text)
    {
        std::unordered_set<std::string> keys;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
        {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::istringstream tok(line.substr(colon + 1));
            std::string t;
            while (tok >> t)
                if (const auto eq = t.find('='); eq != std::string::npos) keys.insert(t.substr(0, eq));
        }
        return keys;
    }

    // ---- Spawns: transform and parent ----
    struct TransformArgs
    {
        bool     hasPos = false, hasRot = false, hasScale = false;
        float    pos[3]{}, rot[4]{ 0, 0, 0, 1 }, scale[3]{ 1, 1, 1 };
    };

    TransformArgs ReadTransform(const char* fn, const sol::table& spec)
    {
        sol::table fields = sol::state_view(spec.lua_state()).create_table();
        for (const auto& field : Resource::Authoring::Fields("Transform"))
        {
            sol::object value = spec[field.key];
            if (value.valid() && value != sol::lua_nil) fields[field.key] = value;
        }
        LuaAuthoringCreateValues("Transform", fields);
        TransformArgs t;
        if (const sol::object v = spec["position"]; v.valid() && v != sol::lua_nil)
        {
            if (!ReadNumbers(v, t.pos, 3)) Fail(fn, "bad_value", "position wants {x, y, z}");
            t.hasPos = true;
        }
        if (const sol::object v = spec["rotation"]; v.valid() && v != sol::lua_nil)
        {
            if (!ReadNumbers(v, t.rot, 4)) Fail(fn, "bad_value", "rotation wants a quaternion {x, y, z, w}");
            const double len = std::sqrt(double(t.rot[0]) * t.rot[0] + double(t.rot[1]) * t.rot[1] +
                                         double(t.rot[2]) * t.rot[2] + double(t.rot[3]) * t.rot[3]);
            if (len < 1e-6f) Fail(fn, "bad_value", "rotation has zero length");
            for (float& c : t.rot) c = static_cast<float>(c / len);
            t.hasRot = true;
        }
        if (const sol::object v = spec["scale"]; v.valid() && v != sol::lua_nil)
        {
            if (!ReadNumbers(v, t.scale, 3)) Fail(fn, "bad_value", "scale wants {x, y, z}");
            t.hasScale = true;
        }
        return t;
    }

    void ApplyTransform(World& w, Entity e, const TransformArgs& t)
    {
        LocalTransform* lt = w.GetComponent<LocalTransform>(e);
        if (!lt) return;
        if (t.hasPos)   lt->translation = { t.pos[0], t.pos[1], t.pos[2] };
        if (t.hasRot)   lt->rotation    = { t.rot[0], t.rot[1], t.rot[2], t.rot[3] };
        if (t.hasScale) lt->scale       = { t.scale[0], t.scale[1], t.scale[2] };
    }

    // Hook `child` under `parent` (both alive), keeping child's LocalTransform
    // as authored - a spawn places a thing RELATIVE to its new parent.
    void LinkParent(World& w, Entity child, Entity parent)
    {
        if (Parent* p = w.GetComponent<Parent>(child)) p->entity = parent;
        else w.AddComponent<Parent>(child, Parent{ parent });
        if (Children* ch = w.GetComponent<Children>(parent)) ch->entities.push_back(child);
        else
        {
            Children nc;
            nc.entities.push_back(child);
            w.AddComponent<Children>(parent, std::move(nc));
        }
    }

    // ---- Edits the scene save would drop ----
    // Where an entity's edits go when the scene is saved. A prefab instance is
    // written as a REFERENCE to its .ipfb - the root's N line (name, transform,
    // parent) and PrefabRef, nothing under it - and an .iscn placement as its
    // root without the parts under it; both come back from their file on the
    // next load (SceneSerializer's subtree marking). An edit inside one lasts
    // only until then, so the functions below refuse it instead of letting a
    // script believe it saved.
    struct SavedBy
    {
        Entity      root = NullEntity;   // the instance's root; NullEntity = saved as itself
        std::string path;                // its .ipfb / .iscn
        bool        prefab = false;
        bool        isRoot = false;      // the entity asked about IS that root
    };

    SavedBy SavedByOf(World& w, Entity e)
    {
        auto prefabOf = [&w](Entity r) -> const PrefabSourcePath*
        {
            if (r == NullEntity || !w.IsAlive(r)) return nullptr;
            const PrefabSourcePath* p = w.GetComponent<PrefabSourcePath>(r);
            return p && !p->path.empty() ? p : nullptr;
        };
        SavedBy out;
        const Entity start = e;
        for (int depth = 0; e != NullEntity && w.IsAlive(e) && depth < 512; ++depth)
        {
            if (const PrefabSourcePath* p = prefabOf(e)) return { e, p->path, true, e == start };
            if (const PrefabInstanceMember* m = w.GetComponent<PrefabInstanceMember>(e))
                if (const PrefabSourcePath* p = prefabOf(m->root)) return { m->root, p->path, true, m->root == start };
            if (const SceneSourcePath* s = w.GetComponent<SceneSourcePath>(e); s && !s->path.empty())
                out = { e, s->path, false, e == start };   // keep walking: an outer prefab wins
            const Parent* par = w.GetComponent<Parent>(e);
            if (!par) break;
            e = par->entity;
        }
        return out;
    }

    [[noreturn]] void FailSavedBy(const char* fn, World& w, const SavedBy& s, const std::string& what)
    {
        // The id as a script holds it (generation included - sol_lua_push's packing).
        const LuaEntity id = LuaId::Out(w, s.root);
        const std::string root = std::to_string((static_cast<std::uint64_t>(id.gen) << 32) | id.raw());
        Fail(fn, s.prefab ? "prefab_instance" : "scene_instance",
             what + " is inside the instance of '" + s.path + "' (root " + root + "), which the scene saves "
             "as a reference to that file, so the change would be gone after the next load. " +
             (s.prefab ? "Editor.DetachPrefab(" + root + ") first makes it plain scene data; or edit the .ipfb"
                       : std::string("Edit the .iscn instead")));
    }

    // An edit of `e` itself: fine when e saves as itself or is the instance's
    // own root (its N line is saved) - refused for the parts under a root.
    void RefusePart(const char* fn, World& w, Entity e, const char* what)
    {
        const SavedBy s = SavedByOf(w, e);
        if (s.root != NullEntity && !s.isRoot) FailSavedBy(fn, w, s, what);
    }

    // A new child of `parent`: refused anywhere inside an instance, its root
    // included - the child would be under it.
    void RefuseParent(const char* fn, World& w, Entity parent)
    {
        if (parent == NullEntity) return;
        const SavedBy s = SavedByOf(w, parent);
        if (s.root != NullEntity) FailSavedBy(fn, w, s, "the parent");
    }

    // A system-owned helper - the editor camera, the Chain Physics Preview's
    // stage, a game-flow runtime entity - is live ECS state, not scene data:
    // SceneSerializer omits it (SceneTransientTag), so an authoring edit of it
    // or of anything under it would not be saved, and a preview stage must not
    // leak into a scene or a prefab either. Every Editor function's target and
    // parent goes through here (the `live` resolver).
    void RefuseTransient(const char* fn, World& w, Entity e, const char* what)
    {
        for (int depth = 0; e != NullEntity && w.IsAlive(e) && depth < 512; ++depth)
        {
            if (w.HasComponent<SceneTransientTag>(e))
                Fail(fn, "transient", std::string(what) + (depth ? " is under" : " is") + " a system-owned helper "
                     "(the editor camera, a preview stage, ...), not scene data: the scene save omits it");
            const Parent* par = w.GetComponent<Parent>(e);
            if (!par) break;
            e = par->entity;
        }
    }

    // ---- SavePrefab {link = true} ----
    // Editor.SavePrefab {link = true}: the source becomes, in place, the instance of the
    // prefab it was just saved as - the root takes PrefabSourcePath, every other node the
    // save wrote (the subtree and the followers it pulled in) PrefabInstanceMember{root},
    // as LoadPrefab gives an instance of the file. A nested instance's root inside it
    // loses its own PrefabSourcePath: SavePrefab does not write that tag, so after a
    // reload it is a plain part of this instance, and left in place it would still take
    // edits the scene save drops. ONE undo entry; undo puts back what each node had
    // (nothing, or another instance's tag and source). Identities are not touched:
    // LoadPrefab keeps the baked Guids for the seed that equals the file's root Guid,
    // i.e. for this very instance.
    class LinkPrefabAction : public Editor::Undo::IEditAction
    {
    public:
        struct Member
        {
            EntityHandle                        h;
            std::optional<PrefabInstanceMember> before;
            std::optional<PrefabSourcePath>     sourceBefore;   // a nested instance's root
        };
        LinkPrefabAction(EntityHandle root, std::optional<PrefabSourcePath> rootBefore, std::string path,
                         std::vector<Member> members)
            : m_root(root), m_rootBefore(std::move(rootBefore)), m_path(std::move(path)),
              m_members(std::move(members)) {}

        void Apply(Editor::Undo::EditContext& c) override
        {
            if (!c.world || !c.world->IsHandleValid(m_root)) return;
            World& w = *c.world;
            Set(w, m_root.entity, PrefabSourcePath{ m_path });
            for (const Member& m : m_members)
                if (w.IsHandleValid(m.h))
                {
                    Set(w, m.h.entity, PrefabInstanceMember{ m_root.entity });
                    Put(w, m.h.entity, std::optional<PrefabSourcePath>{});
                }
        }
        void Revert(Editor::Undo::EditContext& c) override
        {
            if (!c.world) return;
            World& w = *c.world;
            for (auto it = m_members.rbegin(); it != m_members.rend(); ++it)
                if (w.IsHandleValid(it->h))
                {
                    Put(w, it->h.entity, it->before);
                    Put(w, it->h.entity, it->sourceBefore);
                }
            if (w.IsHandleValid(m_root)) Put(w, m_root.entity, m_rootBefore);
        }
        const char* Label() const override { return "Link to Prefab"; }
        size_t HeapBytes() const override { return m_path.size() + m_members.size() * sizeof(Member); }

    private:
        template <typename T> static void Set(World& w, Entity e, const T& v)
        {
            if (T* c = w.GetComponent<T>(e)) *c = v;
            else w.AddComponent<T>(e, T(v));
        }
        template <typename T> static void Put(World& w, Entity e, const std::optional<T>& v)
        {
            if (v) Set(w, e, *v);
            else if (w.GetComponent<T>(e)) w.RemoveComponent<T>(e);
        }

        EntityHandle                    m_root;
        std::optional<PrefabSourcePath> m_rootBefore;
        std::string                     m_path;
        std::vector<Member>             m_members;
    };
}

void EditorLayer::RegisterAuthoringLua(lua_State* L)
{
    if (!L) return;
    sol::state_view lua(L);
    sol::table ed = lua["Editor"].get_or_create<sol::table>();
    RegisterProjectSettingsLua(L);   // Editor.ProjectSetting (EditorLayer_GraphicsSettings.cpp)
    RegisterPropertyLua(L);          // Editor.GetProperty / SetProperty / ResetProperty (EditorLayer_Properties.cpp)
    RegisterPanelLua(L);             // Editor.Panels / SetPanelOpen (EditorLayer_Panels.cpp)
    RegisterGameViewLua(L);          // Editor.GameView / SetGameView (EditorLayer_GameView.cpp)
    ed.set_function("AnimGraph",[](sol::this_state ts,sol::optional<sol::table> options)->sol::object {
        sol::state_view view(ts);auto command=options?LuaToJson(sol::make_object(view,*options)):nlohmann::json::object();
        return JsonToLua(view,nlohmann::json::parse(Editor::AnimGraphEditor::Control(command.dump())));
    });
    ed.set_function("AnimGraphEdit",[](sol::this_state ts,sol::object ops)->sol::object {
        sol::state_view view(ts);return JsonToLua(view,nlohmann::json::parse(Editor::AnimGraphEditor::Control(nlohmann::json{{"action","edit"},{"ops",LuaToJson(ops)}}.dump())));
    });
    ed.set_function("AnimGraphCompile",[](sol::this_state ts)->sol::object {
        sol::state_view view(ts);return JsonToLua(view,nlohmann::json::parse(Editor::AnimGraphEditor::Control("{\"action\":\"compile\"}")));
    });
    ed.set_function("AnimSet", [](sol::this_state ts, sol::optional<sol::table> options) -> sol::object {
        sol::state_view view(ts);
        auto command = options ? LuaToJson(sol::make_object(view, *options)) : nlohmann::json::object();
        return JsonToLua(view, nlohmann::json::parse(Editor::AnimSetEditor::Control(command.dump())));
    });
    ed.set_function("CreateAnimationAsset", [this](const std::string& kind, sol::optional<std::string> directory) {
        if (kind != "graph" && kind != "set") throw std::runtime_error("Animation asset kind must be graph or set");
        return CreateAnimationAsset(kind == "graph", directory.value_or(std::string()));
    });

    // ---- Guards ----
    auto guard = [this](const char* fn)
    {
        if (!m_world) Fail(fn, "no_world", "the editor has no world");
        if (GetPlayState() != ViewportPlayState::Stopped)
            Fail(fn, "requires_edit_mode", "authoring edits are Edit-mode only (Stop Play first): Play-mode "
                                           "changes are runtime state that Stop discards");
    };
    auto live = [this](const char* fn, const LuaEntity& id, const char* what = "the entity") -> Entity
    {
        const Entity e = LuaId::ResolveLive(*m_world, id);
        if (e == NullEntity) Fail(fn, "stale_handle", std::string(what) + " is not alive (destroyed, or its id was reused)");
        RefuseTransient(fn, *m_world, e, what);
        return e;
    };
    auto out = [this](Entity e) { return LuaId::Out(*m_world, e); };

    // ---- Batch ----
    ed.set_function("Batch", [this, guard](const std::string& label, sol::protected_function fn,
                                           sol::variadic_args args) -> sol::variadic_results
    {
        guard("Batch");
        if (!fn.valid()) Fail("Batch", "bad_value", "Batch(label, function)");
        SyncedEditContext();
        m_editHistory.BeginMacro(label.empty() ? std::string("Batch") : label);
        // An asset tab's ring the batch reaches joins it (EditTarget -> JoinBatch).
        if (m_batchDepth == 0) m_batchLabel = label.empty() ? std::string("Batch") : label;
        sol::protected_function_result r = [&]
        {
            struct Depth
            {
                int& d;
                explicit Depth(int& x) : d(x) { ++d; }
                ~Depth() { --d; }
            } depth(m_batchDepth);
            return fn(args);
        }();
        if (!r.valid())
        {
            const sol::error err = r;
            const size_t reverted = m_editHistory.AbortMacro() +   // everything, even from a nested Batch
                                    AbortBatchSinks();             // ... and in every asset tab it reached
            MarkHierarchyDirty();
            throw sol::error(std::string("Editor.Batch: aborted: ") + err.what() + " - nothing was kept (" +
                             std::to_string(reverted) + " edit(s) reverted)");
        }
        m_editHistory.EndMacro();
        if (m_batchDepth == 0) EndBatchSinks();
        sol::variadic_results results;
        for (int i = 0; i < r.return_count(); ++i) results.push_back(r.get<sol::object>(i));
        return results;
    });

    // ---- Create ----
    ed.set_function("Create", [this, guard, live, out](sol::this_state ts, sol::optional<sol::table> specIn) -> LuaEntity
    {
        guard("Create");
        const sol::table spec = specIn ? *specIn : sol::state_view(ts).create_table();
        const std::string name = spec.get_or<std::string>("name", "Entity");
        Entity parent = NullEntity;
        if (const sol::object p = spec["parent"]; p.valid() && p != sol::lua_nil)
        {
            if (!p.is<LuaEntity>()) Fail("Create", "bad_value", "parent wants an entity id");
            parent = live("Create", p.as<LuaEntity>(), "the parent");
            RefuseParent("Create", *m_world, parent);
        }
        const TransformArgs xf = ReadTransform("Create", spec);

        // Every component resolved and every value converted BEFORE the world
        // changes: a bad request creates nothing.
        struct Comp { const ComponentSerializer* ser; std::string tag; KVMap kv; };
        std::vector<Comp> comps;
        if (const sol::object c = spec["components"]; c.valid() && c != sol::lua_nil)
        {
            if (!c.is<sol::table>()) Fail("Create", "bad_value", "components wants {Tag = {key = value}}");
            ComponentSerializerRegistry& reg = GetComponentRegistry();
            for (auto& [k, v] : c.as<sol::table>())
            {
                if (!k.is<std::string>()) Fail("Create", "bad_value", "components are keyed by tag name");
                const std::string tag = k.as<std::string>();
                const ComponentSerializer* ser = reg.FindByTag(tag);
                if (!ser || !ser->deserialize || !ser->serialize)
                    Fail("Create", "unknown_component", "no serializable component '" + tag +
                                                        "' (Entity.components() lists them)");
                if (!v.is<sol::table>()) Fail("Create", "bad_value", tag + " wants {key = value}");
                Comp comp{ ser, tag, {} };
                if (!Resource::Authoring::Fields(tag).empty())
                {
                    for (const auto& [key, text] : LuaAuthoringCreateValues(tag, v.as<sol::table>())) comp.kv[key] = text;
                    comps.push_back(std::move(comp));
                    continue;
                }
                for (auto& [fk, fv] : v.as<sol::table>())
                {
                    if (!fk.is<std::string>()) Fail("Create", "bad_value", tag + " keys are names");
                    std::string text;
                    if (!ValueText(fv, text))
                        Fail("Create", "bad_value", tag + "." + fk.as<std::string>() +
                                                    " wants a number, a boolean, a string or a list of numbers");
                    comp.kv[fk.as<std::string>()] = std::move(text);
                }
                comps.push_back(std::move(comp));
            }
        }

        // Under a canvas or a UI node the new entity is a UI node - a UIRect (CvRect's
        // fields, when given) on the UIParent edge, last among its siblings unless CvRect
        // gives `sibling` - and has no 3D transform (a transform child there would be
        // neither laid out nor drawn).
        const bool uiChild = parent != NullEntity && UI::IsUINode(*m_world, parent);
        bool siblingGiven = false;
        if (uiChild)
        {
            if (xf.hasPos || xf.hasRot || xf.hasScale)
                Fail("Create", "bad_value", "a UI node has no 3D transform: place it with CvRect (offX, offY, "
                                            "szX, szY, aMinX, aMinY, aMaxX, aMaxY, pivX, pivY)");
            for (const Comp& c : comps)
            {
                if (c.tag == "CvParent") Fail("Create", "bad_value", "the UI parent is `parent`, not a CvParent");
                if (c.tag == "CvRect" && c.kv.count("sibling")) siblingGiven = true;
            }
        }

        // Create, then check every requested key really landed (a serializer
        // ignores keys it does not know). Only a complete entity is recorded.
        Entity e = m_world->CreateEntity();
        m_world->SetName(e, name);
        int nextSibling = 0;
        if (uiChild)
        {
            std::vector<Entity> siblings;
            UI::UIChildrenOf(*m_world, parent, siblings);
            for (Entity s : siblings)
                if (const UI::UIRect* r = m_world->GetComponent<UI::UIRect>(s))
                    nextSibling = (std::max)(nextSibling, r->siblingIndex + 1);
            UI::UIRect r;
            r.siblingIndex = nextSibling;
            m_world->AddComponent<UI::UIRect>(e, r);
            UI::AttachUIParent(*m_world, e, parent);
        }
        else
        {
            LocalTransform lt{};
            lt.translation = parent == NullEntity && !xf.hasPos ? ComputeSpawnPosition() : lt.translation;
            m_world->AddComponent<LocalTransform>(e, lt);
            m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
            ApplyTransform(*m_world, e, xf);
            if (parent != NullEntity) LinkParent(*m_world, e, parent);
        }
        for (const Comp& c : comps)
        {
            c.ser->deserialize(*m_world, e, c.kv, m_assetMgr);
            std::ostringstream ss;
            c.ser->serialize(*m_world, e, ss);
            const auto have = LineKeys(ss.str());
            for (const auto& [key, value] : c.kv)
            {
                if (have.count(key)) continue;
                // Roll the half-made entity back: nothing was recorded yet.
                if (parent != NullEntity && !uiChild)
                    if (Children* pc = m_world->GetComponent<Children>(parent))
                        pc->entities.erase(std::remove(pc->entities.begin(), pc->entities.end(), e), pc->entities.end());
                m_world->DestroyEntity(e);
                std::string known;
                for (const auto& k2 : have) known += (known.empty() ? "" : ", ") + k2;
                Fail("Create", "unknown_key", c.tag + " has no key '" + key + "' (it writes: " + known + ")");
            }
        }

        if (uiChild && !siblingGiven)
            if (UI::UIRect* r = m_world->GetComponent<UI::UIRect>(e)) r->siblingIndex = nextSibling;
        if (UndoEnabled())
        {
            SyncedEditContext();
            PushEdit(Editor::Undo::MakeCreateAction(Editor::Undo::EntitySubtreeSnapshot::Capture(*m_world, e),
                                                    "Create " + name));
        }
        m_selectedEntity = e;
        MarkHierarchyDirty();
        return out(e);
    });

    // {name, parent, position, rotation, scale} of a spawn, read and checked
    // before anything is created.
    struct Placement { Entity parent = NullEntity; TransformArgs xf; std::string name; };
    auto placement = [this, live](const char* fn, const sol::optional<sol::table>& optsIn)
    {
        Placement pl;
        if (!optsIn) return pl;
        const sol::table o = *optsIn;
        if (const sol::object p = o["parent"]; p.valid() && p != sol::lua_nil)
        {
            if (!p.is<LuaEntity>()) Fail(fn, "bad_value", "parent wants an entity id");
            pl.parent = live(fn, p.as<LuaEntity>(), "the parent");
            RefuseParent(fn, *m_world, pl.parent);
        }
        pl.xf   = ReadTransform(fn, o);
        pl.name = o.get_or<std::string>("name", "");
        return pl;
    };
    auto place = [this](Entity e, const Placement& pl)
    {
        if (!pl.name.empty()) m_world->SetName(e, pl.name);
        ApplyTransform(*m_world, e, pl.xf);
        if (pl.parent != NullEntity) LinkParent(*m_world, e, pl.parent);
    };

    // ---- InstantiatePrefab ----
    ed.set_function("InstantiatePrefab", [this, guard, out, placement, place](const std::string& path,
                                                                              sol::optional<sol::table> optsIn) -> LuaEntity
    {
        guard("InstantiatePrefab");
        if (!m_assetMgr) Fail("InstantiatePrefab", "no_world", "the editor has no asset manager");
        std::string ext = Sys::PathToUtf8(Sys::U8Path(path).extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".ipfb" && ext != ".iscn")
            Fail("InstantiatePrefab", "bad_value", "a prefab is an .ipfb file, a model an .iscn: " + path);
        std::error_code ec;
        if (!std::filesystem::exists(Sys::U8Path(path), ec))
            Fail("InstantiatePrefab", "load_failed", "no such file: " + path);
        Resource::MeshLibrary* meshLib = m_renderer ? m_renderer->GetMeshLibrary() : nullptr;
        if (ext == ".iscn" && !meshLib) Fail("InstantiatePrefab", "no_world", "the editor has no mesh library");
        const Placement pl = placement("InstantiatePrefab", optsIn);

        Entity root = NullEntity;
        std::string uiRefusal;
        RecordSpawn(ext == ".iscn" ? "Spawn Scene" : "Instantiate Prefab", [&]
        {
            // An .iscn model is placed the way the viewport drop places one.
            if (ext == ".iscn")
            {
                const auto res = SceneInstanceLoader::Load(Sys::RelativizeAssetPath(path), *m_world, *m_assetMgr,
                                                           *meshLib, m_renderer);
                root = res.success ? res.rootEntity : NullEntity;
            }
            else
                root = Resource::LoadPrefab(Sys::RelativizeAssetPath(path), *m_world, *m_assetMgr, m_renderer,
                                            m_animClipSys);
            if (root == NullEntity) return;
            // A UI prefab is a DETACHED copy on a UI parent (a canvas or a UI node; none given =
            // the selection's canvas, else the first one) - never a 3D placement - and nothing
            // else goes under a UI node. A refusal destroys what was spawned: nothing is recorded.
            const bool uiRoot   = UI::IsUINode(*m_world, root);
            const bool uiParent = pl.parent != NullEntity && UI::IsUINode(*m_world, pl.parent);
            if (uiRoot && (pl.xf.hasPos || pl.xf.hasRot || pl.xf.hasScale))
                uiRefusal = "a UI prefab has no 3D placement (move it with Entity.set CvRect)";
            else if (uiRoot && pl.parent != NullEntity && m_world->HasComponent<UI::UICanvas>(root))
                uiRefusal = "a canvas prefab is a root: it takes no parent";
            else if (uiRoot && pl.parent != NullEntity && !uiParent)
                uiRefusal = "a UI prefab goes under a canvas or a UI node, not '" + m_world->GetName(pl.parent) + "'";
            else if (!uiRoot && uiParent)
                uiRefusal = "only a UI prefab goes under a canvas or a UI node";
            else if (uiRoot)
            {
                AdoptUIPrefabRoot(root, pl.parent, &uiRefusal);
                if (uiRefusal.empty() && !pl.name.empty()) m_world->SetName(root, pl.name);
            }
            else
                place(root, pl);
            if (!uiRefusal.empty())
            {
                Hierarchy::DestroySubtree(*m_world, root);
                root = NullEntity;
            }
        });
        if (!uiRefusal.empty()) Fail("InstantiatePrefab", "bad_value", uiRefusal);
        if (root == NullEntity) Fail("InstantiatePrefab", "load_failed", "'" + path + "' did not load (see the log)");
        m_selectedEntity = root;
        return out(root);
    });

    // ---- PlaceMesh ----
    // ONE mesh of a model as a StaticMesh owner - what dragging a Mesh item out of the
    // Content Browser does. `mesh` is a MeshKey (16 hex digits, as Asset.meshes lists
    // it) or a mesh name. One undo entry ("Place Mesh").
    ed.set_function("PlaceMesh", [this, guard, out, placement, place](const std::string& model,
                                                                      const std::string& mesh,
                                                                      sol::optional<sol::table> optsIn) -> LuaEntity
    {
        guard("PlaceMesh");
        MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
        if (!reg) Fail("PlaceMesh", "no_world", "the editor has no mesh library");
        const std::string modelRef = Sys::RelativizeAssetPath(model);
        const MeshAssetRegistry::Model& m = reg->GetModel(modelRef, m_world->Epoch());
        if (m.meshes.empty())
            Fail("PlaceMesh", "load_failed", m.error.empty() ? "'" + model + "' names no mesh" : m.error);
        uint64_t key = 0;
        const MeshAssetRegistry::Mesh* found =
            Resource::MeshKeyFromHex(mesh, key) ? MeshAssetRegistry::Find(m, key, std::string()) : nullptr;
        if (!found) found = MeshAssetRegistry::Find(m, 0, mesh);
        if (!found) Fail("PlaceMesh", "bad_value", "the model has no mesh '" + mesh + "' (Asset.meshes lists them)");
        const Placement pl = placement("PlaceMesh", optsIn);

        Entity e = NullEntity;
        RecordSpawn("Place Mesh", [&]
        {
            // Like Create: in front of the camera, unless it goes under a parent
            // (then its position is the parent-relative one given, or the origin).
            e = CreateStaticMeshEntity(modelRef, found->key, found->name,
                                       pl.parent == NullEntity ? ComputeSpawnPosition()
                                                               : DirectX::XMFLOAT3{ 0.f, 0.f, 0.f });
            place(e, pl);
        });
        if (e == NullEntity) Fail("PlaceMesh", "load_failed", "the mesh could not be placed");
        m_selectedEntity = e;
        MarkHierarchyDirty();
        return out(e);
    });

    // ---- ExtractMesh ----
    // Extract Mesh...: ONE Mesh of a model written out as a model of its own - same key,
    // same SectionKeys - beside the model (or at `dest`), stamped and listed; `retarget`
    // (default true) re-points the open scene's Static Meshes of it as one undo entry
    // ("Extract Mesh"). The files are not undone.
    //   Editor.ExtractMesh(model, key|name, {dest, retarget, overwrite})
    ed.set_function("ExtractMesh", [this, guard](sol::this_state ts, const std::string& model,
                                                 const std::string& mesh, sol::optional<sol::table> optsIn) -> sol::table
    {
        guard("ExtractMesh");
        MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
        if (!reg) Fail("ExtractMesh", "no_world", "the editor has no mesh registry");
        const std::string modelRef = Sys::RelativizeAssetPath(model);
        const MeshAssetRegistry::Model& m = reg->GetModel(modelRef, m_world->Epoch());
        uint64_t key = 0;
        const MeshAssetRegistry::Mesh* found =
            Resource::MeshKeyFromHex(mesh, key) ? MeshAssetRegistry::Find(m, key, std::string()) : nullptr;
        if (!found) found = MeshAssetRegistry::Find(m, 0, mesh);
        if (!found) Fail("ExtractMesh", "bad_value", "the model has no mesh '" + mesh + "' (Asset.meshes lists them)");
        std::string dest;
        bool retarget = true, overwrite = false;
        if (optsIn)
        {
            const sol::table o = *optsIn;
            dest      = o.get_or<std::string>("dest", "");
            retarget  = o.get_or("retarget", true);
            overwrite = o.get_or("overwrite", false);
        }
        MeshExtractOutcome res;
        if (!ExtractModelMesh(modelRef, found->key, dest, retarget, overwrite, res))
        {
            const std::string code = res.file.code == "not_found" ? std::string("bad_value") : res.file.code;
            Fail("ExtractMesh", code.c_str(), res.file.error);
        }
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["iscn"]       = res.file.iscn;
        t["meshlib"]    = res.file.meshlib;
        t["key"]        = Resource::MeshKeyToHex(res.file.key);
        t["name"]       = res.file.meshName;
        t["sections"]   = res.file.sections;
        t["vertices"]   = res.file.vertices;
        t["indices"]    = res.file.indices;
        t["retargeted"] = res.retargeted;
        return t;
    });

    // ---- UnpackModel ----
    // Unpack Model Instance: a .iscn instance's root (SceneRef) -> authored nodes whose
    // meshes are Static Meshes; one undo entry. What the Outliner's menu item does.
    ed.set_function("UnpackModel", [this, guard, live](sol::this_state ts, const LuaEntity& id) -> sol::table
    {
        guard("UnpackModel");
        const Entity root = live("UnpackModel", id);
        UnpackOutcome res;
        if (!UnpackModelInstance(root, res)) Fail("UnpackModel", res.code.c_str(), res.error);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["owners"]        = res.owners;
        t["parts"]         = res.parts;
        t["colliders"]     = res.colliders;
        t["slotOverrides"] = res.slotOverrides;
        return t;
    });

    // ---- References / MoveAssets ----
    // The Content Browser's reverse reference index (AssetReferenceScanner, "Find
    // References"): which containers name `path` - scenes, prefabs, materials and models
    // (.iscn F / K / M lines; keys "F.file", "F.mat", "K.file", "M.file"). Rebuilt first
    // (incremental). Read-only.
    //   Editor.References(path) -> {{file, key}, ...}
    ed.set_function("References", [this](sol::this_state ts, const std::string& path) -> sol::table
    {
        m_refScanner.SetExtraRoots(RefScanExtraRoots());
        m_refScanner.Rebuild(m_assetDir);
        sol::state_view lua(ts);
        sol::table rows = lua.create_table();
        int i = 0;
        for (const Editor::AssetReferenceScanner::Referrer& r : m_refScanner.FindReferrers(path))
            rows[++i] = lua.create_table_with("file", r.file, "key", r.key);
        return rows;
    });
    // The browser's file move (MoveAssetsInto): each file with its .meta into `dest`, every
    // indexed reference repointed, a moved model's own relative references re-based. A
    // file operation - no undo entry. Returns how many moved.
    //   Editor.MoveAssets({path, ...}, dest)
    ed.set_function("MoveAssets", [this, guard](const sol::table& paths, const std::string& dest) -> int
    {
        guard("MoveAssets");
        std::vector<std::string> src;
        for (const auto& [k, v] : paths)
        {
            (void)k;
            if (!v.is<std::string>()) Fail("MoveAssets", "bad_value", "MoveAssets({paths}, dest)");
            src.push_back(v.as<std::string>());
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(Sys::U8Path(dest), ec))
            Fail("MoveAssets", "bad_value", "no such folder: " + dest);
        return static_cast<int>(MoveAssetsInto(src, dest));
    });

    // ---- Mesh Editor ----
    // ONE Mesh of a model: its own view (view modes, overlays, section solo / isolate), its
    // UV layout and its DEFAULT material slots - staged in the panel with its own undo, written
    // to the model's .iscn by Save (no scene undo entry: a model file is not scene state).
    // Section indices are 0-based, in the Mesh's section order (the panel's list).
    //   Editor.OpenMeshEditor(model, key|name, {discard, view, show, ...}) -> state   (MeshEditorSet's keys)
    //   Editor.MeshEditor() -> state          Editor.MeshEditorSet{...} -> state
    //   Editor.MeshEditorStage(section, material|"") -> state
    //   Editor.MeshEditorUndo() / Redo() / Revert() -> bool
    //   Editor.MeshEditorSave() -> {lines, collision, hullsAppended, refreshed, shapeKeys, sockets}
    //   Editor.MeshEditorClose({discard}) -> true
    //   Editor.MeshEditorShot(path, {what = "uv"|"overlay", px}) -> lit pixels
    auto meState = [this](sol::this_state ts) -> sol::table
    {
        const Editor::MeshEditorWindow::StateView s = Editor::MeshEditorWindow::Get().State(m_renderer);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["open"]     = s.open;
        t["loaded"]   = s.loaded;
        t["dirty"]    = s.dirty;
        t["model"]    = s.model;
        t["ref"]      = s.ref;
        t["mesh"]     = s.meshName;
        t["key"]      = s.meshKey ? Resource::MeshKeyToHex(s.meshKey) : std::string();
        t["error"]    = s.error;
        t["viewId"]   = s.viewId == ~0u ? -1 : static_cast<int>(s.viewId);
        t["lease"]    = s.lease;
        t["renders"]  = static_cast<double>(s.renders);
        t["viewMode"] = std::string(Editor::MeshEditorWindow::ViewModeName(s.viewMode));
        t["show"]     = Editor::MeshEditorWindow::ShowNames(s.show);
        char mask[24];
        std::snprintf(mask, sizeof(mask), "%016llx", static_cast<unsigned long long>(s.visibleMask));
        t["visibleMask"] = std::string(mask);
        t["selected"]    = s.selected;
        // size = what the panel asks for (0x0: its box not measured yet), pending = the
        // measured size a resize waits to switch to (the debounce; == size when none
        // does), output = what the picture was drawn at (0x0 while there is none - a
        // resize re-creates it). A settled picture: output == size == pending.
        t["size"]        = lua.create_table_with("w", s.renderW, "h", s.renderH);
        t["pending"]     = lua.create_table_with("w", s.pendingW, "h", s.pendingH);
        t["output"]      = lua.create_table_with("w", s.outputW, "h", s.outputH);
        t["triangles"]   = s.triangles;
        t["vertices"]    = s.vertices;
        t["stride"]      = s.stride;
        t["uvChannels"]  = s.uvChannels;
        t["tangents"]    = s.tangents;
        t["colors"]      = s.colors;
        t["skinned"]     = s.skinned;
        t["morph"]       = s.morph;
        t["identityFromFile"] = s.identityFromFile;
        t["geometryBytes"]    = static_cast<double>(s.geometryBytes);
        t["skeleton"]         = s.skeleton;
        t["skeletonBones"]    = s.skeletonBones;
        t["boundsMin"] = lua.create_table_with(1, s.boundsMin[0], 2, s.boundsMin[1], 3, s.boundsMin[2]);
        t["boundsMax"] = lua.create_table_with(1, s.boundsMax[0], 2, s.boundsMax[1], 3, s.boundsMax[2]);
        t["undo"]         = s.undoDepth;
        t["redo"]         = s.redoDepth;
        t["overlayLines"] = s.overlayLines;
        t["uv"] = lua.create_table_with("channel", s.uvChannel, "section", s.uvSection,
                                        "triangles", s.uvTriangles, "drawn", s.uvDrawn,
                                        "min", lua.create_table_with(1, s.uvMin[0], 2, s.uvMin[1]),
                                        "max", lua.create_table_with(1, s.uvMax[0], 2, s.uvMax[1]),
                                        "outside01", s.uvOutside01);
        sol::table secs = lua.create_table();
        for (std::size_t k = 0; k < s.sections.size(); ++k)
        {
            const auto& c = s.sections[k];
            sol::table r = lua.create_table();
            r["index"]     = static_cast<int>(k);
            r["entry"]     = c.entry;
            r["key"]       = Resource::MeshKeyToHex(c.key);
            r["slot"]      = c.slot;
            r["material"]  = c.material;
            r["saved"]     = c.saved;
            r["editable"]  = c.editable;
            r["visible"]   = c.visible;
            r["triangles"] = c.triangles;
            r["vertices"]  = c.vertices;
            secs[static_cast<int>(k) + 1] = r;
        }
        t["sections"] = secs;
        // The Mesh's sockets (Editor.MeshEditorSockets() lists them) and the skeleton's
        // weight heatmap.
        t["sockets"] = lua.create_table_with("count", s.sockets, "dirty", s.socketsDirty, "lock", s.socketLock,
                                             "selected", s.selectedSocket, "drawn", s.overlaySockets);
        t["weights"] = lua.create_table_with("bone", s.weightsBone, "live", s.weightsLive, "parts", s.weightParts,
                                             "revision", static_cast<double>(s.weightsRevision),
                                             "skinBones", s.skinBones);
        return t;
    };
    ed.set_function("MeshEditor", [meState](sol::this_state ts) -> sol::table { return meState(ts); });
    // view / show / visible / solo / select / dim / uv / orbit / socket / weightsBone,
    // each optional.
    auto meApply = [](const char* fn, const sol::table& o)
    {
        auto& me = Editor::MeshEditorWindow::Get();
        if (const sol::object v = o["view"]; v.valid() && v != sol::lua_nil)
        {
            std::uint32_t mode = 0;
            if (!v.is<std::string>() || !Editor::MeshEditorWindow::ParseViewMode(v.as<std::string>(), mode) ||
                !me.SetViewMode(mode))
                Fail(fn, "bad_value", "view is lit, unlit, wireframe, basecolor, normal, roughness, metallic, ao or depth");
        }
        if (const sol::object v = o["show"]; v.valid() && v != sol::lua_nil)
        {
            std::uint32_t bits = 0;
            if (!v.is<std::string>() || !Editor::MeshEditorWindow::ParseShow(v.as<std::string>(), bits))
                Fail(fn, "bad_value", "show is \"bounds+pivot+grid+normals+tangents+bitangents\", \"all\" or \"none\"");
            me.SetShow(bits);
        }
        if (const sol::object v = o["visible"]; v.valid() && v != sol::lua_nil)
        {
            std::uint64_t mask = 0;
            if (v.is<std::string>())
            {
                const std::string h = v.as<std::string>();
                char* end = nullptr;
                mask = std::strtoull(h.c_str(), &end, 16);
                if (h.empty() || !end || *end) Fail(fn, "bad_value", "visible is a hex mask or a list of section indices");
            }
            else if (v.is<sol::table>())
                for (const auto& [k, idx] : v.as<sol::table>())
                {
                    (void)k;
                    if (!idx.is<int>() || idx.as<int>() < 0 || idx.as<int>() > 63)
                        Fail(fn, "bad_value", "visible lists section indices 0..63");
                    mask |= 1ull << idx.as<int>();
                }
            else Fail(fn, "bad_value", "visible is a hex mask or a list of section indices");
            if (!me.SetVisibleMask(mask)) Fail(fn, "bad_value", "no mesh is open");
        }
        if (const sol::object v = o["solo"]; v.valid() && v != sol::lua_nil)
            if (!v.is<int>() || !me.Solo(v.as<int>())) Fail(fn, "bad_value", "solo is a section index (-1 = show all)");
        if (const sol::object v = o["select"]; v.valid() && v != sol::lua_nil)
            if (!v.is<int>() || !me.Select(v.as<int>())) Fail(fn, "bad_value", "select is a section index (-1 = none)");
        if (const sol::object v = o["dim"]; v.valid() && v != sol::lua_nil) me.SetDimOthers(v.as<bool>());
        if (const sol::object v = o["uv"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<sol::table>()) Fail(fn, "bad_value", "uv = {channel = 0..3, section = -1|index}");
            const sol::table u = v.as<sol::table>();
            if (!me.SetUv(u.get_or("channel", 0), u.get_or("section", -1)))
                Fail(fn, "bad_value", "uv = {channel = 0..3, section = -1|index}");
        }
        if (const sol::object v = o["orbit"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<sol::table>()) Fail(fn, "bad_value", "orbit = {yaw, pitch, dist, pan = {x, y, z}}");
            const sol::table b = v.as<sol::table>();
            me.SetOrbit(b.get_or("yaw", 0.f), b.get_or("pitch", 0.f), b.get_or("dist", 1.f));
            if (const sol::object pv = b["pan"]; pv.valid() && pv.is<sol::table>())
            {
                const sol::table p = pv.as<sol::table>();
                me.SetPan(p.get_or(1, 0.f), p.get_or(2, 0.f), p.get_or(3, 0.f));
            }
        }
        // A socket to select; the weight heatmap's bone (a name, an index, or false).
        if (const sol::object v = o["socket"]; v.valid() && v != sol::lua_nil)
        {
            if (v.is<bool>() && !v.as<bool>()) me.SelectSocket("");
            else if (!v.is<std::string>() || !me.SelectSocket(v.as<std::string>()))
                Fail(fn, "bad_value", "socket names a staged socket of the Mesh (or false)");
        }
        if (const sol::object v = o["weightsBone"]; v.valid() && v != sol::lua_nil)
        {
            int b = -2;   // refused unless it names a bone (or is false)
            if (v.is<bool>() && !v.as<bool>()) b = -1;
            else if (v.is<int>()) b = v.as<int>() >= 0 ? v.as<int>() : -2;
            else if (v.is<std::string>())
            {
                const int named = me.BoneIndex(v.as<std::string>());
                b = named >= 0 ? named : -2;
            }
            if (b < -1 || !me.SetWeightsBone(b))
                Fail(fn, "bad_value", "weightsBone names a bone of the Mesh's skeleton that its sections pair with "
                                      "(a name, an index, or false)");
        }
    };
    ed.set_function("OpenMeshEditor", [this, meState, meApply](sol::this_state ts, const std::string& model,
                                                               const std::string& mesh,
                                                               sol::optional<sol::table> optsIn) -> sol::table
    {
        bool discard = false;
        if (optsIn) discard = (*optsIn).get_or("discard", false);
        std::string code, why;
        if (!Editor::MeshEditorWindow::Get().OpenNow(MeshEditorCtx(), model, mesh, discard, code, why))
            Fail("OpenMeshEditor", code.c_str(), why);
        if (optsIn) meApply("OpenMeshEditor", *optsIn);
        return meState(ts);
    });
    ed.set_function("MeshEditorSet", [meState, meApply](sol::this_state ts, const sol::table& opts) -> sol::table
    {
        if (!Editor::MeshEditorWindow::Get().State(nullptr).loaded)
            Fail("MeshEditorSet", "bad_value", "no mesh is open (Editor.OpenMeshEditor)");
        meApply("MeshEditorSet", opts);
        return meState(ts);
    });
    ed.set_function("MeshEditorStage", [meState](sol::this_state ts, int section, const std::string& material) -> sol::table
    {
        std::string why;
        if (!Editor::MeshEditorWindow::Get().StageMaterial(section, material, why))
            Fail("MeshEditorStage", why.find("not this model") != std::string::npos ? "unsupported" : "bad_value", why);
        return meState(ts);
    });
    ed.set_function("MeshEditorUndo",   []() { return Editor::MeshEditorWindow::Get().Undo(); });
    ed.set_function("MeshEditorRedo",   []() { return Editor::MeshEditorWindow::Get().Redo(); });
    ed.set_function("MeshEditorRevert", []() { return Editor::MeshEditorWindow::Get().Revert(); });
    ed.set_function("MeshEditorSave", [this](sol::this_state ts) -> sol::table
    {
        std::string why;
        Editor::MeshEditorWindow::SaveReport rep;
        if (!Editor::MeshEditorWindow::Get().SaveNow(MeshEditorCtx(), why, &rep))
            Fail("MeshEditorSave", GetPlayState() != ViewportPlayState::Stopped ? "requires_edit_mode" : "write_failed", why);
        sol::state_view lua(ts);
        sol::table t = lua.create_table_with("lines", rep.lines, "collision", rep.collision,
                                             "hullsAppended", rep.hullsAppended, "refreshed", rep.refreshed,
                                             "shapeKeys", rep.shapeKeys);
        t["sockets"] = rep.sockets;
        return t;
    });

    // ---- Mesh Editor: the Mesh's sockets ----
    // `SO mesh=<key>` records - named attachment points in the Mesh's own space, STAGED in the panel's
    // undo ring (MeshEditorUndo / Redo / Revert) and written by MeshEditorSave (this Mesh's records only);
    // a StaticMesh owner of the Mesh publishes them (offset x the owner's world).
    //   Editor.MeshEditorSockets() -> {lock, dirty, selected, sockets = {{name, t = {x,y,z}, q = {x,y,z,w},
    //                                   s = {x,y,z}, euler = {x,y,z}}}}
    //   Editor.MeshEditorSocketAdd([name]) -> name       (at the Mesh's pivot; "" / nil = "socket", made unique)
    //   Editor.MeshEditorSocketSet(name, {name, t, q | euler, s}) -> true   (each field optional)
    //   Editor.MeshEditorSocketRemove(name) -> true
    auto meSocketRow = [](sol::state_view lua, const Resource::ModelSocket& so) -> sol::table
    {
        const DirectX::XMFLOAT3 e = MathUtils::RollPitchYawDegreesFromQuaternion(so.q);
        return lua.create_table_with("name", so.name,
                                     "t", lua.create_table_with("x", so.t.x, "y", so.t.y, "z", so.t.z),
                                     "q", lua.create_table_with("x", so.q.x, "y", so.q.y, "z", so.q.z, "w", so.q.w),
                                     "s", lua.create_table_with("x", so.s.x, "y", so.s.y, "z", so.s.z),
                                     "euler", lua.create_table_with("x", e.x, "y", e.y, "z", e.z));
    };
    auto meLoaded = [](const char* fn)
    {
        if (!Editor::MeshEditorWindow::Get().State(nullptr).loaded)
            Fail(fn, "bad_value", "no mesh is open (Editor.OpenMeshEditor)");
    };
    ed.set_function("MeshEditorSockets", [meLoaded, meSocketRow](sol::this_state ts) -> sol::table
    {
        meLoaded("MeshEditorSockets");
        const auto& me = Editor::MeshEditorWindow::Get();
        sol::state_view lua(ts);
        sol::table list = lua.create_table();
        int i = 0;
        for (const Resource::ModelSocket& so : me.StagedSockets()) list[++i] = meSocketRow(lua, so);
        const Editor::MeshEditorWindow::StateView st = me.State(nullptr);
        return lua.create_table_with("lock", me.SocketLock(), "dirty", me.SocketsDirty(),
                                     "selected", st.selectedSocket, "sockets", list);
    });
    ed.set_function("MeshEditorSocketAdd", [meLoaded](sol::optional<std::string> name) -> std::string
    {
        meLoaded("MeshEditorSocketAdd");
        std::string out, why;
        if (!Editor::MeshEditorWindow::Get().AddSocket(name.value_or(""), out, why))
            Fail("MeshEditorSocketAdd", "refused", why);
        return out;
    });
    ed.set_function("MeshEditorSocketSet", [meLoaded](const std::string& name, const sol::table& o) -> bool
    {
        const char* fn = "MeshEditorSocketSet";
        meLoaded(fn);
        auto& me = Editor::MeshEditorWindow::Get();
        const int i = me.FindSocket(name);
        if (i < 0) Fail(fn, "not_found", "no socket '" + name + "' is staged on the Mesh");
        Resource::ModelSocket v = me.StagedSockets()[static_cast<std::size_t>(i)];
        auto vec = [&](const char* key, int n, float* out) -> bool
        {
            const sol::object ov = o[key];
            if (!ov.valid() || ov == sol::lua_nil) return false;
            if (!ov.is<sol::table>()) Fail(fn, "bad_value", std::string(key) + " is {x, y, z" + (n == 4 ? ", w}" : "}"));
            const sol::table t = ov.as<sol::table>();
            static const char* kNames[] = { "x", "y", "z", "w" };
            for (int k = 0; k < n; ++k)
            {
                sol::object c = t[kNames[k]];
                if (!c.valid() || c == sol::lua_nil) c = t[k + 1];
                if (!c.is<double>()) Fail(fn, "bad_value", std::string(key) + " is {x, y, z" + (n == 4 ? ", w}" : "}"));
                out[k] = static_cast<float>(c.as<double>());
            }
            return true;
        };
        if (const sol::object nv = o["name"]; nv.valid() && nv != sol::lua_nil)
        {
            if (!nv.is<std::string>()) Fail(fn, "bad_value", "name is a string");
            v.name = nv.as<std::string>();
        }
        float f[4];
        if (vec("t", 3, f)) v.t = { f[0], f[1], f[2] };
        if (vec("q", 4, f)) v.q = { f[0], f[1], f[2], f[3] };
        else if (vec("euler", 3, f))
            DirectX::XMStoreFloat4(&v.q, DirectX::XMQuaternionRotationRollPitchYaw(
                DirectX::XMConvertToRadians(f[0]), DirectX::XMConvertToRadians(f[1]), DirectX::XMConvertToRadians(f[2])));
        if (vec("s", 3, f)) v.s = { f[0], f[1], f[2] };
        std::string why;
        if (!me.SetSocket(name, v, why)) Fail(fn, "refused", why);
        return true;
    });
    ed.set_function("MeshEditorSocketRemove", [meLoaded](const std::string& name) -> bool
    {
        meLoaded("MeshEditorSocketRemove");
        auto& me = Editor::MeshEditorWindow::Get();
        if (me.FindSocket(name) < 0)
            Fail("MeshEditorSocketRemove", "not_found", "no socket '" + name + "' is staged on the Mesh");
        std::string why;
        if (!me.RemoveSocket(name, why)) Fail("MeshEditorSocketRemove", "refused", why);
        return true;
    });
    ed.set_function("MeshEditorClose", [](sol::optional<sol::table> optsIn) -> bool
    {
        std::string why;
        const bool discard = optsIn ? (*optsIn).get_or("discard", false) : false;
        if (!Editor::MeshEditorWindow::Get().CloseNow(discard, why)) Fail("MeshEditorClose", "dirty", why);
        return true;
    });
    ed.set_function("MeshEditorShot", [this](const std::string& path, sol::optional<sol::table> optsIn) -> double
    {
        std::string what = "uv";
        int px = 512;
        if (optsIn)
        {
            what = (*optsIn).get_or<std::string>("what", "uv");
            px   = (*optsIn).get_or("px", 512);
        }
        std::string why;
        long long lit = -1;
        if (what == "uv")
        {
            if (px < 16 || px > 4096) Fail("MeshEditorShot", "bad_value", "px is 16..4096");
            lit = Editor::MeshEditorWindow::Get().WriteUvPng(path, static_cast<std::uint32_t>(px), why);
        }
        else if (what == "overlay")
            lit = Editor::MeshEditorWindow::Get().WriteOverlayPng(m_renderer, path, why);
        else Fail("MeshEditorShot", "bad_value", "what is \"uv\" or \"overlay\"");
        if (lit < 0) Fail("MeshEditorShot", "write_failed", why);
        return static_cast<double>(lit);
    });

    // ---- Skeleton Editor ----
    // A model's skeleton: the tree (filter / search), the picture's overlay (bone draw scope, show
    // bits), the selection and the report Resource::InspectSkeleton makes (the same one
    // -assettool=skeleton_report prints); sockets, virtual bones and the humanoid profile are
    // staged and saved below. Bones are named exactly (case-sensitive, as clips bind) or by 0-based index.
    //   Editor.OpenSkeletonEditor(path, {discard, select, scope, show, filter, search, orbit, ...}) -> state
    //   Editor.SkeletonEditor() -> state          Editor.SkeletonEditorSet{...} -> state
    //   Editor.SkeletonEditorPick(x, y) -> {bone, name} | {socket} | nil   (the picture's OUTPUT pixels)
    //   Editor.SkeletonEditorJoint(bone) -> {x, y} | nil         (where its joint lands)
    //   Editor.SkeletonEditorShot(path) -> lit pixels of the overlay layer PNG
    //   Editor.SkeletonEditorClose() -> true
    auto skeState = [this](sol::this_state ts) -> sol::table
    {
        const Editor::SkeletonEditorWindow::StateView s = Editor::SkeletonEditorWindow::Get().State(m_renderer);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["open"]         = s.open;
        t["loaded"]       = s.loaded;
        t["preview"]      = s.preview;
        t["path"]         = s.path;
        t["model"]        = s.model;
        t["iskel"]        = s.iskel;
        t["error"]        = s.error;
        t["viewId"]       = s.viewId == ~0u ? -1 : static_cast<int>(s.viewId);
        t["renders"]      = static_cast<double>(s.renders);
        // size / pending / output as in the Mesh Editor's state (settled: output == size ==
        // pending), except that a stage sharing the WorldCull lane draws at the lane's size
        // (stage.sharingLane).
        t["size"]         = lua.create_table_with("w", s.renderW, "h", s.renderH);
        t["pending"]      = lua.create_table_with("w", s.pendingW, "h", s.pendingH);
        t["output"]       = lua.create_table_with("w", s.outputW, "h", s.outputH);
        t["mesh"]         = s.meshVisible;   // Show > Mesh (the Reference Pose host)
        t["selected"]     = s.selected;
        t["selectedName"] = s.selectedName;
        t["scope"]        = s.scope;
        t["show"]         = s.show;
        t["filter"]       = s.filter;
        t["search"]       = s.search;
        t["treeRows"]     = s.treeRows;
        t["overlay"]      = lua.create_table_with("bones", s.drawnBones, "joints", s.drawnJoints,
                                                  "lines", s.overlayLines, "labels", s.labels,
                                                  "sockets", s.drawnSockets, "virtualBones", s.drawnVirtualBones);
        // The staged sockets' summary (Editor.SkeletonEditorSockets() lists them).
        t["selectedSocket"] = s.selectedSocket;
        t["socketLock"]     = s.socketLock;
        t["sockets"]        = s.sockets;
        t["socketsDirty"]   = s.socketsDirty;
        t["undo"]           = s.undoDepth;
        t["redo"]           = s.redoDepth;
        t["gizmo"]          = s.gizmo;
        t["gizmoGestures"]  = s.gizmoGestures;
        // The view's tenancy (capture {view, lease}), the weight heatmap, the animation list.
        t["lease"]   = s.lease;
        t["weights"] = lua.create_table_with("on", s.weightsOn, "live", s.weightsLive, "bone", s.weightsBone,
                                             "parts", s.weightParts, "skipped", s.weightSkipped,
                                             "revision", static_cast<double>(s.weightsRevision));
        t["animations"] = lua.create_table_with("rows", s.animRows, "pending", s.animPending);
        // The staged virtual bones' summary (Editor.SkeletonEditorVirtualBones() lists them).
        t["selectedVirtualBone"]  = s.selectedVirtualBone;
        t["virtualBones"]         = s.virtualBones;
        t["virtualBonesApplied"]  = s.virtualBonesApplied;
        t["virtualBonesDirty"]    = s.virtualBonesDirty;
        t["dirty"]                = s.dirty;
        t["staleInstances"]       = s.staleInstances;
        t["pickTargetFrom"]       = s.pickTargetFrom;
        // The host, and the Animation host's stage (a hidden PreviewStage in the main World).
        t["host"] = s.host;
        sol::table stg = lua.create_table();
        stg["shown"]       = s.stageShown;
        stg["up"]          = s.stageUp;
        stg["pinned"]      = s.stagePinned;
        stg["sharingLane"] = s.stageSharingLane;
        stg["root"]        = s.stageRoot == NullEntity ? -1 : static_cast<int>(s.stageRoot);
        stg["skeleton"]    = s.stageSkeleton == NullEntity ? -1 : static_cast<int>(s.stageSkeleton);
        stg["entities"]    = s.stageEntities;
        stg["spawns"]      = s.stageSpawns;
        stg["viewId"]      = s.stageViewId == ~0u ? -1 : static_cast<int>(s.stageViewId);
        stg["originY"]     = s.stageOriginY;
        stg["posedFrames"] = static_cast<double>(s.stagePosedFrames);
        stg["restFrames"]  = static_cast<double>(s.stageRestFrames);
        stg["status"]      = s.stageStatus;
        stg["pinOwners"]   = s.pinOwners;
        // The clip on the stage ("" path = the rest pose).
        sol::table clip = lua.create_table();
        clip["path"]       = s.clipPath;
        clip["source"]     = s.clipSource;
        clip["name"]       = s.clipName;
        clip["bound"]      = s.clipBound;
        clip["playing"]    = s.clipPlaying;
        clip["loop"]       = s.clipLooping;
        clip["t"]          = s.clipTime;
        clip["duration"]   = s.clipDuration;
        clip["fps"]        = s.clipFps;
        clip["speed"]      = s.clipSpeed;
        clip["frames"]     = s.clipFrames;
        clip["channels"]   = s.clipChannels;
        clip["boundBones"] = s.clipBoundBones;
        stg["clip"] = clip;
        stg["ghost"] = s.drawnGhost;
        stg["posedBones"] = s.posedBones;
        t["stage"] = stg;
        const Resource::SkeletonStats& k = s.stats;
        sol::table st = lua.create_table();
        st["bones"] = k.bones;               st["roots"] = k.roots;
        st["leaves"] = k.leaves;             st["maxDepth"] = k.maxDepth;
        st["ikChains"] = k.ikChains;         st["ikLinks"] = k.ikLinks;
        st["grantTargets"] = k.grantTargets; st["twist"] = k.twist;
        st["weightsKnown"] = k.weightsKnown; st["skinStreams"] = k.skinStreams;
        st["skinVertices"] = k.skinVertices; st["maxInfluences"] = k.maxInfluences;
        st["weighted"] = k.weighted;         st["unweighted"] = k.unweighted;
        st["dummyLeaves"] = k.dummyLeaves;   st["chainName"] = k.chainName;
        st["chainAutoAttach"] = k.chainAutoAttach;
        st["humanoidMapped"] = k.humanoidMapped;
        st["humanoidValid"] = k.humanoidValid;
        st["modelSize"] = k.modelSize;
        t["stats"]    = st;
        // The staged humanoid profile (Editor.SkeletonEditorHumanoid() lists its roles).
        t["humanoid"] = lua.create_table_with("source", s.humanoidSource, "valid", k.humanoidValid,
                                              "mapped", k.humanoidMapped, "dirty", s.humanoidDirty,
                                              "userEdited", s.humanoidUserEdited, "convention", s.humanoidConvention,
                                              "missing", s.humanoidMissing);
        t["tab"]  = s.tab;
        t["mask"] = lua.create_table_with("mode", s.maskMode, "root", s.maskRoot, "resolved", s.maskResolved,
                                          "bones", s.maskBones);
        if (s.maskMode != "off")
        {
            // The bones themselves (0-based indices, the tree's; the staged virtual
            // bones after the real ones) - Skeleton.BoneMask's shape.
            const auto& se = Editor::SkeletonEditorWindow::Get();
            sol::table list = lua.create_table();
            int n = 0;
            for (std::uint32_t b = 0; b < se.MaskSize(); ++b)
                if (se.InMask(static_cast<int>(b))) list[++n] = b;
            sol::table mk = t["mask"];
            mk["list"] = list;
        }
        sol::table ov = t["overlay"];
        ov["humanoid"] = s.drawnHumanoid;
        ov["masked"]   = s.drawnMasked;
        t["errors"]   = s.errors;
        t["warnings"] = s.warnings;
        t["infos"]    = s.infos;
        return t;
    };
    // A bone named or indexed; -2 = no such bone.
    auto skeBone = [](const sol::object& v) -> int
    {
        const auto& se = Editor::SkeletonEditorWindow::Get();
        const SkeletonAsset* sk = se.Skeleton();
        if (!sk) return -2;
        if (v.is<int>())
        {
            const int i = v.as<int>();
            return (i >= 0 && static_cast<std::uint32_t>(i) < sk->boneCount) ? i : -2;
        }
        if (!v.is<std::string>()) return -2;
        const std::string name = v.as<std::string>();
        for (std::uint32_t b = 0; b < sk->boneCount; ++b)
            if (name == std::string(sk->boneNames[b], strnlen(sk->boneNames[b], sizeof(sk->boneNames[b]))))
                return static_cast<int>(b);
        return -2;
    };
    // Each key optional: select, scope, show, mesh, filter, search, orbit, socket, gizmo,
    // weights, host, clip (clipName) and its transport (playing, time, speed, loop),
    // virtualBone, tab, role, mask (maskRoot).
    auto skeApply = [skeBone](const char* fn, const sol::table& o)
    {
        auto& se = Editor::SkeletonEditorWindow::Get();
        if (const sol::object v = o["select"]; v.valid() && v != sol::lua_nil)
        {
            if (v.is<bool>() && !v.as<bool>()) se.Select(-1);
            else
            {
                const int b = skeBone(v);
                if (b < 0 || !se.Select(b)) Fail(fn, "bad_value", "select names a bone of this skeleton (a name, an index, or false)");
            }
        }
        if (const sol::object v = o["scope"]; v.valid() && v != sol::lua_nil)
        {
            Editor::SkeletonEditorWindow::BoneScope sc = Editor::SkeletonEditorWindow::BoneScope::All;
            if (!v.is<std::string>() || !Editor::SkeletonEditorWindow::ParseBoneScope(v.as<std::string>(), sc))
                Fail(fn, "bad_value", "scope is none, selected, parents, children, family or all");
            se.SetScope(sc);
        }
        if (const sol::object v = o["show"]; v.valid() && v != sol::lua_nil)
        {
            std::uint32_t bits = 0;
            if (!v.is<std::string>() || !Editor::SkeletonEditorWindow::ParseShow(v.as<std::string>(), bits))
                Fail(fn, "bad_value", "show is \"joints+octahedra+names+allnames+axes+allaxes+categories+grid+sockets+"
                                      "virtualbones\", \"all\" or \"none\"");
            se.SetShow(bits);
        }
        // Show > Mesh: false = the Reference Pose host's picture without the model (the bones
        // over the backdrop).
        if (const sol::object v = o["mesh"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<bool>()) Fail(fn, "bad_value", "mesh is true or false");
            se.SetMeshVisible(v.as<bool>());
        }
        if (const sol::object v = o["filter"]; v.valid() && v != sol::lua_nil)
        {
            Editor::SkeletonEditorWindow::TreeFilter f = Editor::SkeletonEditorWindow::TreeFilter::All;
            if (!v.is<std::string>() || !Editor::SkeletonEditorWindow::ParseTreeFilter(v.as<std::string>(), f))
                Fail(fn, "bad_value", "filter is all, weighted, deform, ik, twist, chain, humanoid or issues");
            se.SetFilter(f);
        }
        if (const sol::object v = o["search"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<std::string>()) Fail(fn, "bad_value", "search is a string");
            se.SetSearch(v.as<std::string>());
        }
        if (const sol::object v = o["orbit"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<sol::table>()) Fail(fn, "bad_value", "orbit = {yaw, pitch, dist, pan = {x, y, z}}");
            const sol::table b = v.as<sol::table>();
            se.SetOrbit(b.get_or("yaw", 0.f), b.get_or("pitch", 0.f), b.get_or("dist", 1.f));
            if (const sol::object pv = b["pan"]; pv.valid() && pv.is<sol::table>())
            {
                const sol::table p = pv.as<sol::table>();
                se.SetPan(p.get_or(1, 0.f), p.get_or(2, 0.f), p.get_or(3, 0.f));
            }
        }
        // A socket to select (after `select`: selecting a bone clears it), the gizmo.
        if (const sol::object v = o["socket"]; v.valid() && v != sol::lua_nil)
        {
            if (v.is<bool>() && !v.as<bool>()) se.SelectSocket("");
            else if (!v.is<std::string>() || !se.SelectSocket(v.as<std::string>()))
                Fail(fn, "bad_value", "socket names a staged socket (or false)");
        }
        if (const sol::object v = o["gizmo"]; v.valid() && v != sol::lua_nil)
        {
            Editor::SkeletonEditorWindow::GizmoOp op = Editor::SkeletonEditorWindow::GizmoOp::Translate;
            if (!v.is<std::string>() || !Editor::SkeletonEditorWindow::ParseGizmoOp(v.as<std::string>(), op))
                Fail(fn, "bad_value", "gizmo is off, translate, rotate or scale");
            se.SetGizmoOp(op);
        }
        // The weight heatmap (the selected bone's weights).
        if (const sol::object v = o["weights"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<bool>()) Fail(fn, "bad_value", "weights is true or false");
            se.SetWeights(v.as<bool>());
        }
        // The host - "reference" (the bind pose in the studio) or "stage" (the live pose on a
        // hidden stage; spawned by the next frames that draw the panel).
        if (const sol::object v = o["host"]; v.valid() && v != sol::lua_nil)
        {
            Editor::SkeletonEditorWindow::Host h = Editor::SkeletonEditorWindow::Host::Reference;
            if (!v.is<std::string>() || !Editor::SkeletonEditorWindow::ParseHost(v.as<std::string>(), h))
                Fail(fn, "bad_value", "host is reference or stage");
            se.SetHost(h);
        }
        // The stage's clip - clip = an .ianim path (clipName = one of its clips) or false
        // (the rest pose; applied by the next ProcessPendingActions), then the transport.
        if (const sol::object v = o["clip"]; v.valid() && v != sol::lua_nil)
        {
            if (v.is<bool>() && !v.as<bool>()) se.RequestClip("");
            else if (v.is<std::string>()) se.RequestClip(v.as<std::string>(), o.get_or<std::string>("clipName", ""));
            else Fail(fn, "bad_value", "clip is an .ianim path, or false (the rest pose)");
        }
        auto& cp = se.ClipPlayer();
        if (const sol::object v = o["playing"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<bool>()) Fail(fn, "bad_value", "playing is true or false");
            cp.SetPlaying(v.as<bool>());
        }
        if (const sol::object v = o["time"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<double>()) Fail(fn, "bad_value", "time is seconds");
            cp.SetTime(v.as<double>());
        }
        if (const sol::object v = o["speed"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<double>()) Fail(fn, "bad_value", "speed is a number");
            cp.SetSpeed(static_cast<float>(v.as<double>()));
        }
        if (const sol::object v = o["loop"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<bool>()) Fail(fn, "bad_value", "loop is true or false");
            cp.SetLooping(v.as<bool>());
        }
        // A virtual bone to select (after `select` / `socket`, which clear it).
        if (const sol::object v = o["virtualBone"]; v.valid() && v != sol::lua_nil)
        {
            if (v.is<bool>() && !v.as<bool>()) se.SelectVirtualBone("");
            else if (!v.is<std::string>() || !se.SelectVirtualBone(v.as<std::string>()))
                Fail(fn, "bad_value", "virtualBone names a staged virtual bone (or false)");
        }
        // The right-hand tab; a humanoid role's bone to select; the bone mask preview.
        if (const sol::object v = o["tab"]; v.valid() && v != sol::lua_nil)
        {
            Editor::SkeletonEditorWindow::Tab tb = Editor::SkeletonEditorWindow::Tab::Details;
            if (!v.is<std::string>() || !Editor::SkeletonEditorWindow::ParseTab(v.as<std::string>(), tb))
                Fail(fn, "bad_value", "tab is details, check, animations or retarget");
            se.RequestTab(tb);
        }
        if (const sol::object v = o["role"]; v.valid() && v != sol::lua_nil)
        {
            const int r = v.is<std::string>() ? SkeHumanoidRole(v.as<std::string>()) : -1;
            if (r < 0) Fail(fn, "bad_value", "role is a humanoid role name (hips, leftHand, ...)");
            if (!se.SelectRole(static_cast<HumanoidBone>(r))) Fail(fn, "not_found", "that role is not mapped");
        }
        if (const sol::object v = o["mask"]; v.valid() && v != sol::lua_nil)
        {
            int mode = -1;
            if (v.is<bool>() && !v.as<bool>()) mode = -1;
            else if (!v.is<std::string>() || !Editor::SkeletonEditorWindow::ParseMaskMode(v.as<std::string>(), mode))
                Fail(fn, "bad_value", "mask is off, full, upper, lower or custom (or false)");
            se.SetMask(mode, o.get_or<std::string>("maskRoot", se.MaskRoot()));
        }
        else if (const sol::object r = o["maskRoot"]; r.valid() && r != sol::lua_nil)
        {
            if (!r.is<std::string>()) Fail(fn, "bad_value", "maskRoot is a bone name");
            se.SetMask(se.MaskMode(), r.as<std::string>());
        }
    };
    ed.set_function("OpenSkeletonEditor", [this, skeState, skeApply](sol::this_state ts, const std::string& path,
                                                                     sol::optional<sol::table> optsIn) -> sol::table
    {
        std::string code, why;
        const bool discard = optsIn && optsIn->get_or("discard", false);
        if (!Editor::SkeletonEditorWindow::Get().OpenNow(SkeletonEditorCtx(), path, code, why, discard))
            Fail("OpenSkeletonEditor", code.c_str(), why);
        if (optsIn) skeApply("OpenSkeletonEditor", *optsIn);
        return skeState(ts);
    });
    ed.set_function("SkeletonEditor", [skeState](sol::this_state ts) -> sol::table { return skeState(ts); });
    ed.set_function("SkeletonEditorSet", [skeState, skeApply](sol::this_state ts, const sol::table& opts) -> sol::table
    {
        if (!Editor::SkeletonEditorWindow::Get().State(nullptr).loaded)
            Fail("SkeletonEditorSet", "bad_value", "no skeleton is open (Editor.OpenSkeletonEditor)");
        skeApply("SkeletonEditorSet", opts);
        return skeState(ts);
    });
    ed.set_function("SkeletonEditorPick", [this](sol::this_state ts, float x, float y) -> sol::object
    {
        // What a click does: the nearest joint or socket within 12 px (a tie: the bone).
        auto& se = Editor::SkeletonEditorWindow::Get();
        const int b  = se.PickAt(m_renderer, x, y);
        const int si = se.PickSocketAt(m_renderer, x, y);
        float bx = 0.f, by = 0.f, sx = 0.f, sy = 0.f;
        const bool boneAt = b >= 0 && se.ProjectJoint(m_renderer, b, bx, by);
        const bool sockAt = si >= 0 && se.ProjectSocket(m_renderer, si, sx, sy);
        const float bd = boneAt ? (bx - x) * (bx - x) + (by - y) * (by - y) : FLT_MAX;
        const float sd = sockAt ? (sx - x) * (sx - x) + (sy - y) * (sy - y) : FLT_MAX;
        sol::state_view lua(ts);
        if (sockAt && sd < bd)
        {
            const std::string name = se.Sockets()[static_cast<std::size_t>(si)].name;
            se.SelectSocket(name);
            return sol::make_object(lua, lua.create_table_with("socket", name));
        }
        if (b < 0) return sol::make_object(lua, sol::lua_nil);
        se.Select(b);
        const SkeletonAsset* sk = se.Skeleton();
        return sol::make_object(lua, lua.create_table_with(
            "bone", b, "name", std::string(sk->boneNames[b], strnlen(sk->boneNames[b], sizeof(sk->boneNames[b])))));
    });
    ed.set_function("SkeletonEditorJoint", [this, skeBone](sol::this_state ts, const sol::object& bone) -> sol::object
    {
        const int b = skeBone(bone);
        if (b < 0) Fail("SkeletonEditorJoint", "bad_value", "no such bone in the open skeleton");
        float x = 0.f, y = 0.f;
        sol::state_view lua(ts);
        if (!Editor::SkeletonEditorWindow::Get().ProjectJoint(m_renderer, b, x, y))
            return sol::make_object(lua, sol::lua_nil);
        return sol::make_object(lua, lua.create_table_with("x", x, "y", y));
    });
    ed.set_function("SkeletonEditorShot", [this](const std::string& path) -> double
    {
        std::string why;
        const long long lit = Editor::SkeletonEditorWindow::Get().WriteOverlayPng(m_renderer, path, why);
        if (lit < 0) Fail("SkeletonEditorShot", "write_failed", why);
        return static_cast<double>(lit);
    });
    ed.set_function("SkeletonEditorClose", []() -> bool
    {
        Editor::SkeletonEditorWindow::Get().Close();
        return true;
    });

    // ---- Skeleton Editor: the model's sockets ----
    // STAGED like the Mesh Editor's slots: every edit one step of the panel's undo ring, Save writes
    // the model's SO records and every placed instance re-derives them. A vector is {x, y, z}; a
    // rotation {x, y, z, w} (q) or degrees in the Inspector's order (euler, RollPitchYaw).
    //   Editor.SkeletonEditorSockets() -> {lock, dirty, undo, redo, selected, gizmo, gestures,
    //       sockets = {{name, bone, boneIndex, virtualBone, t, q, s, euler, source, orphan, mesh?, model = {x,y,z} | nil}}}
    //   Editor.SkeletonEditorSocketAdd(bone [, name]) -> name     (name "" / nil = "<bone>_socket", made unique)
    //   Editor.SkeletonEditorSocketSet(name, {name, bone, keepWorld, t, q | euler, s}) -> its row
    //   Editor.SkeletonEditorSocketRemove(name) -> true
    //   Editor.SkeletonEditorSocketGizmo(name, {op = "translate"|"rotate"|"scale", v = {x,y,z}, deg}) -> its row
    //   Editor.SkeletonEditorSocketCopy([names]) -> SO records text
    //   Editor.SkeletonEditorSocketPaste(text [, bone]) -> {names}
    //   Editor.SkeletonEditorSocketPoint(name) -> {x, y} | nil   (where it lands in the OUTPUT pixels)
    //   Editor.SkeletonEditorUndo() / SkeletonEditorRedo() / SkeletonEditorRevert() -> bool
    //   Editor.SkeletonEditorSave([{humanoid = true}]) -> {model, written, records, refreshed, virtualBones,
    //       stale, humanoidWritten, humanoidPushed}
    //   Editor.SkeletonEditorAttach(entity, socket) -> true   (the nearest placed instance of the model)
    //   Editor.AttachToSocket(entity, target, socket) -> true (any owner of that socket; one undo step)
    auto skeSocketRow = [](sol::state_view lua, int i) -> sol::table
    {
        const auto& se = Editor::SkeletonEditorWindow::Get();
        const Resource::ModelSocket& s = se.Sockets()[static_cast<std::size_t>(i)];
        const DirectX::XMFLOAT3 e = MathUtils::RollPitchYawDegreesFromQuaternion(s.q);
        sol::table r = lua.create_table();
        r["name"]   = s.name;
        r["bone"]   = s.bone;
        r["t"]      = lua.create_table_with(1, s.t.x, 2, s.t.y, 3, s.t.z);
        r["q"]      = lua.create_table_with(1, s.q.x, 2, s.q.y, 3, s.q.z, 4, s.q.w);
        r["s"]      = lua.create_table_with(1, s.s.x, 2, s.s.y, 3, s.s.z);
        r["euler"]  = lua.create_table_with(1, e.x, 2, e.y, 3, e.z);
        r["source"] = s.source;
        r["orphan"] = s.orphan;
        if (!s.IsSkeletonSocket()) r["mesh"] = Resource::MeshKeyToHex(s.meshKey);
        int bone = -1;
        if (const SkeletonAsset* sk = se.Skeleton(); sk && s.IsSkeletonSocket())
            for (std::uint32_t b = 0; b < sk->boneCount && bone < 0; ++b)
                if (s.bone == std::string(sk->boneNames[b], strnlen(sk->boneNames[b], sizeof(sk->boneNames[b]))))
                    bone = static_cast<int>(b);
        r["boneIndex"] = bone;
        // Its bone is a staged virtual bone (boneIndex -1: not a real bone).
        r["virtualBone"] = bone < 0 && s.IsSkeletonSocket() && se.FindVirtualBone(s.bone) >= 0;
        DirectX::XMFLOAT4X4 m;
        if (se.SocketModelMatrix(i, m)) r["model"] = lua.create_table_with(1, m._41, 2, m._42, 3, m._43);
        return r;
    };
    auto skeLoaded = [](const char* fn)
    {
        if (!Editor::SkeletonEditorWindow::Get().State(nullptr).loaded)
            Fail(fn, "bad_value", "no skeleton is open (Editor.OpenSkeletonEditor)");
    };
    auto skeFind = [skeLoaded](const char* fn, const std::string& name) -> int
    {
        skeLoaded(fn);
        const int i = Editor::SkeletonEditorWindow::Get().FindSocket(name);
        if (i < 0) Fail(fn, "not_found", "no socket '" + name + "' is staged");
        return i;
    };
    ed.set_function("SkeletonEditorSockets", [skeSocketRow, skeLoaded](sol::this_state ts) -> sol::table
    {
        skeLoaded("SkeletonEditorSockets");
        auto& se = Editor::SkeletonEditorWindow::Get();
        const auto st = se.State(nullptr);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["lock"]     = st.socketLock;
        t["dirty"]    = st.socketsDirty;
        t["undo"]     = st.undoDepth;
        t["redo"]     = st.redoDepth;
        t["selected"] = st.selectedSocket;
        t["gizmo"]    = st.gizmo;
        t["gestures"] = st.gizmoGestures;
        sol::table rows = lua.create_table();
        for (int i = 0; i < static_cast<int>(se.Sockets().size()); ++i) rows[i + 1] = skeSocketRow(lua, i);
        t["sockets"] = rows;
        return t;
    });
    ed.set_function("SkeletonEditorSocketAdd", [skeLoaded](const std::string& bone, sol::optional<std::string> name)
                                                   -> std::string
    {
        skeLoaded("SkeletonEditorSocketAdd");
        std::string out, why;
        if (!Editor::SkeletonEditorWindow::Get().AddSocket(bone, name.value_or(""), out, why))
            Fail("SkeletonEditorSocketAdd", "refused", why);
        return out;
    });
    ed.set_function("SkeletonEditorSocketSet", [skeSocketRow, skeFind](sol::this_state ts, const std::string& name,
                                                                       const sol::table& f) -> sol::table
    {
        const char* fn = "SkeletonEditorSocketSet";
        auto& se = Editor::SkeletonEditorWindow::Get();
        skeFind(fn, name);
        // Every field read (and its shape checked) first, then one all-or-nothing edit: a
        // refused name leaves no bone change staged.
        std::optional<std::string> bone;
        if (const sol::object b = f["bone"]; b.valid() && b != sol::lua_nil)
        {
            if (!b.is<std::string>()) Fail(fn, "bad_value", "bone is a bone name");
            bone = b.as<std::string>();
        }
        std::optional<DirectX::XMFLOAT3> t, s;
        std::optional<DirectX::XMFLOAT4> q;
        std::optional<std::string>       newName;
        float n3[3], n4[4];
        if (const sol::object o = f["t"]; o.valid() && o != sol::lua_nil)
        {
            if (!ReadNumbers(o, n3, 3)) Fail(fn, "bad_value", "t wants {x, y, z}");
            t = DirectX::XMFLOAT3(n3[0], n3[1], n3[2]);
        }
        if (const sol::object o = f["q"]; o.valid() && o != sol::lua_nil)
        {
            if (!ReadNumbers(o, n4, 4)) Fail(fn, "bad_value", "q wants {x, y, z, w}");
            q = DirectX::XMFLOAT4(n4[0], n4[1], n4[2], n4[3]);
        }
        if (const sol::object o = f["euler"]; o.valid() && o != sol::lua_nil)
        {
            if (!ReadNumbers(o, n3, 3)) Fail(fn, "bad_value", "euler wants {x, y, z} degrees");
            q = MathUtils::EulerDegreesToQuaternion(DirectX::XMFLOAT3(n3[0], n3[1], n3[2]));
        }
        if (const sol::object o = f["s"]; o.valid() && o != sol::lua_nil)
        {
            if (!ReadNumbers(o, n3, 3)) Fail(fn, "bad_value", "s wants {x, y, z}");
            s = DirectX::XMFLOAT3(n3[0], n3[1], n3[2]);
        }
        if (const sol::object o = f["name"]; o.valid() && o != sol::lua_nil)
        {
            if (!o.is<std::string>()) Fail(fn, "bad_value", "name is a string");
            newName = o.as<std::string>();
        }
        std::string why;
        const bool ok = se.EditSocket(name, bone ? &*bone : nullptr, f.get_or("keepWorld", false),
                                      [&](Resource::ModelSocket& v)
                                      {
                                          if (t) v.t = *t;
                                          if (q) v.q = *q;
                                          if (s) v.s = *s;
                                          if (newName) v.name = *newName;
                                      },
                                      why);
        if (!ok) Fail(fn, "refused", why);
        return skeSocketRow(sol::state_view(ts), se.FindSocket(newName ? *newName : name));
    });
    ed.set_function("SkeletonEditorSocketRemove", [skeFind](const std::string& name) -> bool
    {
        skeFind("SkeletonEditorSocketRemove", name);
        std::string why;
        if (!Editor::SkeletonEditorWindow::Get().RemoveSocket(name, why)) Fail("SkeletonEditorSocketRemove", "refused", why);
        return true;
    });
    ed.set_function("SkeletonEditorSocketGizmo", [skeSocketRow, skeFind](sol::this_state ts, const std::string& name,
                                                                         const sol::table& f) -> sol::table
    {
        const char* fn = "SkeletonEditorSocketGizmo";
        skeFind(fn, name);
        Editor::SkeletonEditorWindow::GizmoOp op = Editor::SkeletonEditorWindow::GizmoOp::Translate;
        if (!Editor::SkeletonEditorWindow::ParseGizmoOp(f.get_or<std::string>("op", "translate"), op) ||
            op == Editor::SkeletonEditorWindow::GizmoOp::Off)
            Fail(fn, "bad_value", "op is translate, rotate or scale");
        float v[3] = { 0.f, 0.f, 0.f };
        if (const sol::object o = f["v"]; !ReadNumbers(o, v, 3)) Fail(fn, "bad_value", "v wants {x, y, z}");
        std::string why;
        auto& se = Editor::SkeletonEditorWindow::Get();
        if (!se.GizmoDrag(name, op, DirectX::XMFLOAT3(v[0], v[1], v[2]), f.get_or("deg", 0.f), why)) Fail(fn, "refused", why);
        return skeSocketRow(sol::state_view(ts), se.FindSocket(name));
    });
    ed.set_function("SkeletonEditorSocketCopy", [skeLoaded](sol::optional<sol::table> names) -> std::string
    {
        skeLoaded("SkeletonEditorSocketCopy");
        std::vector<std::string> list;
        if (names)
            for (const auto& [k, v] : *names)
                if (v.is<std::string>()) list.push_back(v.as<std::string>());
        return Editor::SkeletonEditorWindow::Get().CopySockets(list);
    });
    ed.set_function("SkeletonEditorSocketPaste", [skeLoaded](sol::this_state ts, const std::string& text,
                                                             sol::optional<std::string> bone) -> sol::table
    {
        skeLoaded("SkeletonEditorSocketPaste");
        std::vector<std::string> added;
        std::string why;
        if (!Editor::SkeletonEditorWindow::Get().PasteSockets(text, bone.value_or(""), added, why))
            Fail("SkeletonEditorSocketPaste", "refused", why);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        for (std::size_t i = 0; i < added.size(); ++i) t[i + 1] = added[i];
        return t;
    });
    ed.set_function("SkeletonEditorSocketPoint", [this, skeFind](sol::this_state ts, const std::string& name) -> sol::object
    {
        const int i = skeFind("SkeletonEditorSocketPoint", name);
        float x = 0.f, y = 0.f;
        sol::state_view lua(ts);
        if (!Editor::SkeletonEditorWindow::Get().ProjectSocket(m_renderer, i, x, y)) return sol::make_object(lua, sol::lua_nil);
        return sol::make_object(lua, lua.create_table_with("x", x, "y", y));
    });
    ed.set_function("SkeletonEditorUndo",   [skeLoaded]() { skeLoaded("SkeletonEditorUndo"); return Editor::SkeletonEditorWindow::Get().Undo(); });
    ed.set_function("SkeletonEditorRedo",   [skeLoaded]() { skeLoaded("SkeletonEditorRedo"); return Editor::SkeletonEditorWindow::Get().Redo(); });
    ed.set_function("SkeletonEditorRevert", [skeLoaded]() { skeLoaded("SkeletonEditorRevert"); return Editor::SkeletonEditorWindow::Get().Revert(); });
    ed.set_function("SkeletonEditorSave", [this, skeLoaded](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
    {
        skeLoaded("SkeletonEditorSave");
        std::string why;
        Editor::SkeletonEditorWindow::SaveReport rep;
        // {humanoid = true} writes the humanoid section even without an unsaved edit (a
        // profile the runtime built but the .iskel does not hold).
        const bool forceHumanoid = opts && opts->get_or("humanoid", false);
        if (!Editor::SkeletonEditorWindow::Get().SaveNow(SkeletonEditorCtx(), why, &rep, forceHumanoid))
            Fail("SkeletonEditorSave", "refused", why);
        sol::state_view lua(ts);
        sol::table t = lua.create_table_with("model", rep.model, "written", rep.written, "records", rep.records,
                                             "refreshed", rep.refreshed, "virtualBones", rep.virtualBones,
                                             "stale", rep.stale);
        t["humanoidWritten"] = rep.humanoidWritten;
        t["humanoidPushed"]  = rep.humanoidPushed;
        return t;
    });

    // ---- Skeleton Editor: the humanoid mapping ----
    // The skeleton's humanoid profile (retargeting; an AnimLayer's Upper / Lower Body mask), STAGED
    // with the sockets and virtual bones (one undo ring - SkeletonEditorUndo / Redo / Revert - and one
    // Save, which writes the .iskel's humanoid section; the World's skeletons of that .iskel take it).
    // Roles by their VRM names (HumanoidRoleName); bones by name or index, false = unmapped.
    //   Editor.SkeletonEditorHumanoid() -> {source, valid, mapped, dirty, userEdited, convention,
    //       hipsTranslation = {bone, name} | nil, missing = {role...}, roles = {{role, bone, name, required}...}}
    //   Editor.SkeletonEditorHumanoidSet{roles = {leftHand = "Hand_L" | 12 | false, ...}, hipsTranslation,
    //       autoDetect = true, clear = true} -> the same   (clear / autoDetect first, then the rest; each change
    //       one undo step)
    //   Editor.SkeletonEditorOpenRetarget() -> {entity}   (a placed instance selected, the Retarget window open)
    auto skeHumanoid = [](sol::this_state ts) -> sol::table
    {
        const auto& se = Editor::SkeletonEditorWindow::Get();
        const Editor::SkeletonEditorWindow::StateView s = se.State(nullptr);
        const HumanoidProfile& hp = se.Humanoid();
        const SkeletonAsset* sk = se.Skeleton();
        auto boneName = [sk](int b) -> std::string
        {
            if (!sk || b < 0 || static_cast<std::uint32_t>(b) >= sk->boneCount) return {};
            return std::string(sk->boneNames[b], strnlen(sk->boneNames[b], sizeof(sk->boneNames[b])));
        };
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["source"]     = s.humanoidSource;
        t["valid"]      = hp.IsValid();
        t["dirty"]      = s.humanoidDirty;
        t["userEdited"] = hp.userEdited != 0;
        t["convention"] = static_cast<int>(hp.convention);
        sol::table roles = lua.create_table(), missing = lua.create_table();
        int nRoles = 0, nMissing = 0;
        for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r)
        {
            const HumanoidBone role = static_cast<HumanoidBone>(r);
            const int b = hp.boneForRole[r];
            if (b >= 0)
                roles[++nRoles] = lua.create_table_with("role", HumanoidRoleName(role), "bone", b, "name", boneName(b),
                                                        "required", IsRequiredHumanoidBone(role));
            else if (IsRequiredHumanoidBone(role))
                missing[++nMissing] = HumanoidRoleName(role);
        }
        t["mapped"]  = nRoles;
        t["roles"]   = roles;
        t["missing"] = missing;
        if (hp.hipsTranslationBone >= 0)
            t["hipsTranslation"] = lua.create_table_with("bone", hp.hipsTranslationBone,
                                                         "name", boneName(hp.hipsTranslationBone));
        return t;
    };
    ed.set_function("SkeletonEditorHumanoid", [skeLoaded, skeHumanoid](sol::this_state ts) -> sol::table
    {
        skeLoaded("SkeletonEditorHumanoid");
        return skeHumanoid(ts);
    });
    ed.set_function("SkeletonEditorHumanoidSet", [skeLoaded, skeHumanoid, skeBone](sol::this_state ts,
                                                                                 const sol::table& o) -> sol::table
    {
        const char* fn = "SkeletonEditorHumanoidSet";
        skeLoaded(fn);
        auto& se = Editor::SkeletonEditorWindow::Get();
        // A bone value: a name / an index, or false (unmapped).
        auto boneOf = [&](const sol::object& v, const std::string& what) -> int
        {
            if (v.is<bool>() && !v.as<bool>()) return -1;
            const int b = skeBone(v);
            if (b < 0) Fail(fn, "bad_value", what + " names a bone of this skeleton (a name, an index, or false)");
            return b;
        };
        // Everything checked first, then applied: a bad entry (or a read-only .iskel)
        // changes nothing, clear / autoDetect included.
        if (!se.HumanoidLock().empty()) Fail(fn, "refused", se.HumanoidLock() + " - its humanoid mapping");
        std::vector<std::pair<int, int>> sets;
        if (const sol::object rv = o["roles"]; rv.valid() && rv != sol::lua_nil)
        {
            if (!rv.is<sol::table>()) Fail(fn, "bad_value", "roles = {<role> = bone | false, ...}");
            for (const auto& [k, v] : rv.as<sol::table>())
            {
                const int r = k.is<std::string>() ? SkeHumanoidRole(k.as<std::string>()) : -1;
                if (r < 0) Fail(fn, "bad_value", "roles keys are humanoid role names (hips, leftHand, ...)");
                sets.emplace_back(r, boneOf(v, "roles." + k.as<std::string>()));
            }
        }
        std::optional<int> hips;
        if (const sol::object v = o["hipsTranslation"]; v.valid() && v != sol::lua_nil) hips = boneOf(v, "hipsTranslation");
        if (o.get_or("clear", false)) se.ClearHumanoid();
        if (o.get_or("autoDetect", false)) se.AutoDetectHumanoid();
        std::string why;
        for (const auto& [r, b] : sets)
            if (!se.SetHumanoidRole(static_cast<HumanoidBone>(r), b, why)) Fail(fn, "refused", why);
        if (hips && !se.SetHipsTranslation(*hips, why)) Fail(fn, "refused", why);
        return skeHumanoid(ts);
    });
    ed.set_function("SkeletonEditorOpenRetarget", [this, skeLoaded](sol::this_state ts) -> sol::table
    {
        skeLoaded("SkeletonEditorOpenRetarget");
        const std::string model = Editor::SkeletonEditorWindow::Get().State(nullptr).model;
        if (model.empty()) Fail("SkeletonEditorOpenRetarget", "refused", "the open skeleton has no model to be placed");
        const std::string why = OpenRetargetFor(model);
        if (!why.empty()) Fail("SkeletonEditorOpenRetarget", "refused", why);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["entity"] = LuaId::Out(*m_world, m_selectedEntity);
        return t;
    });

    // ---- Selection and the main viewport's skeleton (Show > Skeleton) ----
    //   Editor.ViewportSkeleton([on]) -> {on, entity | nil, bones, lines, sockets, frame}
    // The selection's skeleton (its live pose) and sockets as depth-aware debug lines; what the
    // LAST frame drew (a change also logs [SKELOVERLAY] entity= bones= lines= sockets=).
    // The editor's selection, as a click in the Outliner sets it (the viewport skeleton and the
    // Retarget window read it).   Editor.Select(id) -> true   Editor.ClearSelection() -> true
    ed.set_function("Select", [this, live](const LuaEntity& id) -> bool
    {
        if (!m_world) Fail("Select", "no_world", "the editor has no world");
        SetSelectionFromUndo(live("Select", id));
        return true;
    });
    ed.set_function("ClearSelection", [this]() -> bool
    {
        SetSelectionFromUndo(NullEntity);
        return true;
    });
    ed.set_function("ViewportSkeleton", [this](sol::this_state ts, sol::optional<bool> on) -> sol::table
    {
        if (on) m_showSelectionSkeleton = *on;
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["on"]      = m_showSelectionSkeleton;
        if (m_world && m_skelOverlay.entity != NullEntity && m_world->IsAlive(m_skelOverlay.entity))
            t["entity"] = LuaId::Out(*m_world, m_skelOverlay.entity);
        t["bones"]   = m_skelOverlay.bones;
        t["lines"]   = m_skelOverlay.lines;
        t["sockets"] = m_skelOverlay.sockets;
        t["frame"]   = static_cast<double>(m_skelOverlay.frame);
        return t;
    });

    // ---- Skeleton Editor: the model's virtual bones ----
    // STAGED with the sockets (one undo ring - SkeletonEditorUndo / Redo / Revert - and one Save). A
    // virtual bone joins two REAL bones: its parent is `from`, every frame it is where `to` is. The
    // rules are Resource::ApplyVirtualBones' (what a spawn applies). A placed instance takes a saved
    // change when it spawns again (Editor.SkeletonEditor().staleInstances counts the others).
    //   Editor.SkeletonEditorVirtualBones() -> {lock, dirty, selected, stale,
    //       bones = {{name, from, to, fromIndex, toIndex, orphan, applied, problem, sockets, model = {x,y,z} | nil}}}
    //   Editor.SkeletonEditorVirtualBoneAdd(from, to [, name]) -> name   (name "" / nil = "VB <from>_<to>", made unique)
    //   Editor.SkeletonEditorVirtualBoneRename(name, newName) -> true    (its sockets follow)
    //   Editor.SkeletonEditorVirtualBoneRemove(name) -> true             (its sockets move onto `to`, where they were)
    ed.set_function("SkeletonEditorVirtualBones", [skeLoaded](sol::this_state ts) -> sol::table
    {
        skeLoaded("SkeletonEditorVirtualBones");
        auto& se = Editor::SkeletonEditorWindow::Get();
        const auto st = se.State(nullptr);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["lock"]     = st.socketLock;
        t["dirty"]    = st.virtualBonesDirty;
        t["selected"] = st.selectedVirtualBone;
        t["stale"]    = st.staleInstances;
        sol::table rows = lua.create_table();
        const SkeletonAsset* sk = se.Skeleton();
        for (int v = 0; v < static_cast<int>(se.VirtualBones().size()); ++v)
        {
            const Resource::ModelVirtualBone& vb = se.VirtualBones()[static_cast<std::size_t>(v)];
            sol::table r = lua.create_table();
            r["name"]      = vb.name;
            r["from"]      = vb.from;
            r["to"]        = vb.to;
            r["fromIndex"] = se.VirtualBoneFrom(v);
            r["toIndex"]   = se.VirtualBoneTo(v);
            r["orphan"]    = vb.orphan;
            const std::string problem = se.VirtualBoneProblem(v);
            r["applied"]   = problem.empty();
            r["problem"]   = problem;
            int socks = 0;
            for (const Resource::ModelSocket& s : se.Sockets()) socks += (s.IsSkeletonSocket() && s.bone == vb.name) ? 1 : 0;
            r["sockets"]   = socks;
            // At the bind pose: its target's place.
            if (const int to = se.VirtualBoneTo(v); problem.empty() && sk && to >= 0 && static_cast<std::uint32_t>(to) < sk->boneCount)
            {
                DirectX::XMVECTOR det;
                DirectX::XMFLOAT4X4 m;
                DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixInverse(&det, DirectX::XMLoadFloat4x4(&sk->inverseBindPose[to])));
                r["model"] = lua.create_table_with(1, m._41, 2, m._42, 3, m._43);
            }
            rows[v + 1] = r;
        }
        t["bones"] = rows;
        return t;
    });
    ed.set_function("SkeletonEditorVirtualBoneAdd", [skeLoaded](const std::string& from, const std::string& to,
                                                                sol::optional<std::string> name) -> std::string
    {
        skeLoaded("SkeletonEditorVirtualBoneAdd");
        std::string out, why;
        if (!Editor::SkeletonEditorWindow::Get().AddVirtualBone(from, to, name.value_or(""), out, why))
            Fail("SkeletonEditorVirtualBoneAdd", "refused", why);
        return out;
    });
    ed.set_function("SkeletonEditorVirtualBoneRename", [skeLoaded](const std::string& name, const std::string& newName) -> bool
    {
        skeLoaded("SkeletonEditorVirtualBoneRename");
        auto& se = Editor::SkeletonEditorWindow::Get();
        if (se.FindVirtualBone(name) < 0)
            Fail("SkeletonEditorVirtualBoneRename", "not_found", "no virtual bone '" + name + "' is staged");
        std::string why;
        if (!se.RenameVirtualBone(name, newName, why)) Fail("SkeletonEditorVirtualBoneRename", "refused", why);
        return true;
    });
    // ---- Skeleton Editor: the Animation host's preview attachments ----
    // A model (.iscn) or a prefab (.ipfb) shown on a socket of the stage (following it by name);
    // the list is the user's (per model, WritableDataDir()/cache/skeleton_editor/<key>.json), never
    // an asset's and never the scene's.
    //   Editor.SkeletonEditorAttachments() -> {{socket, asset, root, error, parked}}   (root -1: not on a stage;
    //   parked: spawned, its socket not on the stage - held below the scene)
    //   Editor.SkeletonEditorAttachmentAdd(socket, asset) -> true
    //   Editor.SkeletonEditorAttachmentRemove(socket [, asset]) -> true   (asset nil: every one of the socket)
    ed.set_function("SkeletonEditorAttachments", [skeLoaded](sol::this_state ts) -> sol::table
    {
        skeLoaded("SkeletonEditorAttachments");
        const auto& se = Editor::SkeletonEditorWindow::Get();
        sol::state_view lua(ts);
        sol::table rows = lua.create_table();
        for (std::size_t i = 0; i < se.Attachments().size(); ++i)
        {
            const auto& a = se.Attachments()[i];
            const Entity root = se.AttachmentRoot(i);
            rows[static_cast<int>(i) + 1] = lua.create_table_with(
                "socket", a.socket, "asset", a.asset, "root", root == NullEntity ? -1 : static_cast<int>(root),
                "error", se.AttachmentError(i), "parked", se.AttachmentParked(i));
        }
        return rows;
    });
    // Editor.SkeletonEditorAttachmentMove(socket, asset, toSocket) -> true: onto another
    // socket of the list (the "Move here" of one whose socket is gone).
    ed.set_function("SkeletonEditorAttachmentMove", [skeLoaded](const std::string& socket, const std::string& asset,
                                                                const std::string& toSocket) -> bool
    {
        skeLoaded("SkeletonEditorAttachmentMove");
        std::string why;
        if (!Editor::SkeletonEditorWindow::Get().MoveAttachment(socket, asset, toSocket, why))
            Fail("SkeletonEditorAttachmentMove", "refused", why);
        return true;
    });
    ed.set_function("SkeletonEditorAttachmentAdd", [skeLoaded](const std::string& socket, const std::string& asset) -> bool
    {
        skeLoaded("SkeletonEditorAttachmentAdd");
        std::string why;
        if (!Editor::SkeletonEditorWindow::Get().AddAttachment(socket, asset, why))
            Fail("SkeletonEditorAttachmentAdd", "refused", why);
        return true;
    });
    // Temporary posing on the stage - Editor.SkeletonEditorPose{bone = name | index, euler = {x, y, z}}
    // (degrees in the bone's own frame; all 0 = that bone's pose removed) or {reset = true};
    // -> {bones = the posed bone count}.
    ed.set_function("SkeletonEditorPose", [skeLoaded, skeBone](sol::this_state ts, const sol::table& o) -> sol::table
    {
        const char* fn = "SkeletonEditorPose";
        skeLoaded(fn);
        auto& se = Editor::SkeletonEditorWindow::Get();
        if (o.get_or("reset", false)) se.ResetPose();
        if (const sol::object bv = o["bone"]; bv.valid() && bv != sol::lua_nil)
        {
            const int b = skeBone(bv);
            if (b < 0) Fail(fn, "bad_value", "bone names a bone of the open skeleton (a name or an index)");
            float e[3] = { 0.f, 0.f, 0.f };
            if (const sol::object ev = o["euler"]; ev.valid() && ev != sol::lua_nil && !ReadNumbers(ev, e, 3))
                Fail(fn, "bad_value", "euler wants {x, y, z} degrees");
            se.SetBonePose(b, DirectX::XMFLOAT3(e[0], e[1], e[2]));
        }
        sol::state_view lua(ts);
        return lua.create_table_with("bones", static_cast<int>(se.PoseEdits().size()));
    });
    ed.set_function("SkeletonEditorAttachmentRemove", [skeLoaded](const std::string& socket,
                                                                  sol::optional<std::string> asset) -> bool
    {
        skeLoaded("SkeletonEditorAttachmentRemove");
        if (!Editor::SkeletonEditorWindow::Get().RemoveAttachment(socket, asset.value_or("")))
            Fail("SkeletonEditorAttachmentRemove", "not_found", "no preview attachment on '" + socket + "' matches");
        return true;
    });
    ed.set_function("SkeletonEditorVirtualBoneRemove", [skeLoaded](const std::string& name) -> bool
    {
        skeLoaded("SkeletonEditorVirtualBoneRemove");
        auto& se = Editor::SkeletonEditorWindow::Get();
        if (se.FindVirtualBone(name) < 0)
            Fail("SkeletonEditorVirtualBoneRemove", "not_found", "no virtual bone '" + name + "' is staged");
        std::string why;
        if (!se.RemoveVirtualBone(name, why)) Fail("SkeletonEditorVirtualBoneRemove", "refused", why);
        return true;
    });
    ed.set_function("SkeletonEditorAttach", [this, guard, live, skeLoaded](const LuaEntity& id, const std::string& socket) -> bool
    {
        guard("SkeletonEditorAttach");
        skeLoaded("SkeletonEditorAttach");
        const Entity item = live("SkeletonEditorAttach", id);
        const std::string model = Editor::SkeletonEditorWindow::Get().State(nullptr).model;
        if (model.empty()) Fail("SkeletonEditorAttach", "refused", "the open skeleton has no model to be placed");
        std::string why;
        const Entity target = NearestModelInstance(item, model, socket, why);
        if (target == NullEntity) Fail("SkeletonEditorAttach", "not_found", why);
        why = AttachToSocket(item, target, socket);
        if (!why.empty()) Fail("SkeletonEditorAttach", "refused", why);
        return true;
    });
    // The compatible-animation list, scanned now: {folder = , files = {...}} (both optional:
    // none = the model's folder) -> {{anim, clip, verdict, channels, boundBones, bones, frames, duration}}.
    ed.set_function("SkeletonEditorAnimations", [skeLoaded](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
    {
        skeLoaded("SkeletonEditorAnimations");
        std::string folder;
        std::vector<std::string> files;
        if (opts)
        {
            folder = opts->get_or<std::string>("folder", "");
            if (const sol::object f = (*opts)["files"]; f.valid() && f.is<sol::table>())
                for (const auto& [k, v] : f.as<sol::table>())
                    if (v.is<std::string>()) files.push_back(v.as<std::string>());
        }
        auto& se = Editor::SkeletonEditorWindow::Get();
        std::string why;
        if (!se.ScanAnimations(folder, files, why)) Fail("SkeletonEditorAnimations", "refused", why);
        sol::state_view lua(ts);
        sol::table rows = lua.create_table();
        int i = 0;
        for (const auto& r : se.AnimationRows())
        {
            sol::table row = lua.create_table();
            row["anim"] = r.anim;
            row["clip"] = r.clip;
            row["verdict"] = r.verdict;
            row["channels"] = r.channels;
            row["boundBones"] = r.boundBones;
            row["bones"] = r.bones;
            row["frames"] = r.frames;
            row["duration"] = r.duration;
            rows[++i] = row;
        }
        return rows;
    });
    ed.set_function("AttachToSocket", [this, guard, live](const LuaEntity& id, const LuaEntity& owner,
                                                          const std::string& socket) -> bool
    {
        guard("AttachToSocket");
        const Entity item   = live("AttachToSocket", id);
        const Entity target = live("AttachToSocket", owner, "the socket's owner");
        const std::string why = AttachToSocket(item, target, socket);
        if (!why.empty()) Fail("AttachToSocket", "refused", why);
        return true;
    });

    // ---- Texture Viewer ----
    // One .itex per tab: a channel mask, a mip, a cube face / cross / panorama, a value
    // mapping, drawn by a Renderer texture view; the file's description beside it.
    //   Editor.OpenTextureViewer(path [, {view keys}]) -> state   (an open tab of the file is focused)
    //   Editor.TextureViewer() -> {open, active, views = {live, targetBytes}, tabs = {{path, format, formatLabel,
    //       width, height, depth, mipLevels, arraySize, dimension, cube, srgb, preset, resident, residentWhy?, gpuBytes,
    //       viewId, renders, settled, output = {w, h}, view = {ch, mip, face, layout, map, ev, tonemap, range, bg,
    //       filtered, tile, zoom, pan, cmp, with, split, k}}}}   (tabs and `active` are 1-based)
    //   Editor.TextureViewerSet{ch, mip, face, slice, layout, map, ev, tonemap, range = {a, b}, bg, filtered,
    //       tile, zoom = "fit" | n, pan = {x, y}, size = {w, h}, cmp = "off" | "split" | "diff",
    //       with = "mip" | tab | path, split = 0..1, k = n} -> state   (the active tab; all or nothing)
    //   Editor.TextureViewerActivate(tab) -> state        Editor.TextureViewerClose{tab = n | all = true} -> state
    //   Editor.TextureViewerShot(path) -> job: the active tab's picture, written once it has settled
    auto tvCtx = [this]() { return TextureViewerCtx(); };
    auto tvState = [tvCtx](sol::this_state ts) -> sol::table
    {
        const Editor::TextureViewerContext ctx = tvCtx();
        const Editor::TextureViewerWindow::StateView s = Editor::TextureViewerWindow::Get().State(ctx);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["open"]   = s.open;
        t["active"] = s.active + 1;
        // The renderer side: every live texture view (the viewer's, the Content
        // Browser's cube tiles) and the bytes of their targets - the leak check.
        if (ctx.renderer)
            if (TextureViewService* views = ctx.renderer->GetTextureViews())
                t["views"] = lua.create_table_with("live", views->LiveCount(),
                                                   "targetBytes", static_cast<double>(views->TargetBytes()));
        sol::table tabs = lua.create_table();
        for (std::size_t i = 0; i < s.tabs.size(); ++i)
        {
            const Editor::TextureViewerWindow::TabState& ts2 = s.tabs[i];
            const Editor::TextureViewerWindow::ViewState& v = ts2.view;
            sol::table r = lua.create_table();
            r["path"]        = ts2.path;
            r["format"]      = ts2.info.formatName;
            r["formatLabel"] = ts2.info.formatLabel;
            r["width"]       = ts2.info.width;
            r["height"]      = ts2.info.height;
            r["depth"]       = ts2.info.depth;
            r["mipLevels"]   = ts2.info.mipLevels;
            r["arraySize"]   = ts2.info.arraySize;
            r["dimension"]   = ts2.info.dimension;
            r["cube"]        = ts2.info.isCube;
            r["srgb"]        = ts2.info.isSRGB;
            r["preset"]      = ts2.info.preset;
            r["resident"]    = ts2.resident;
            if (!ts2.residentWhy.empty()) r["residentWhy"] = ts2.residentWhy;
            r["gpuBytes"]    = static_cast<double>(ts2.gpuBytes);
            r["viewId"]      = ts2.viewId == ~0u ? -1 : static_cast<int>(ts2.viewId);
            r["renders"]     = static_cast<double>(ts2.renders);
            r["settled"]     = ts2.settled;
            r["output"]      = lua.create_table_with("w", ts2.outW, "h", ts2.outH);
            sol::table vt = lua.create_table();
            vt["ch"]       = TextureView::ChannelsName(v.channels);
            vt["mip"]      = v.mip;
            vt["face"]     = v.slice;
            vt["layout"]   = std::string(TextureView::CubeLayoutName(v.cubeLayout));
            vt["map"]      = std::string(TextureView::MapName(v.map));
            vt["ev"]       = v.ev;
            vt["tonemap"]  = v.tonemap;
            vt["range"]    = lua.create_table_with(1, v.rangeMin, 2, v.rangeMax);
            vt["bg"]       = std::string(TextureView::BackgroundName(v.background));
            vt["filtered"] = v.filtered;
            vt["tile"]     = v.tile;
            if (v.zoom <= 0.f) vt["zoom"] = std::string("fit"); else vt["zoom"] = v.zoom;
            vt["pan"]      = lua.create_table_with(1, v.pan[0], 2, v.pan[1]);
            // The comparison: picture B is "mip" (this texture's next mip) or a tab's path.
            vt["cmp"]      = std::string(TextureView::CompareName(v.compare));
            vt["with"]     = v.compareWith.empty() ? std::string("mip") : v.compareWith;
            vt["split"]    = v.split;
            vt["k"]        = v.diffScale;
            r["view"] = vt;
            tabs[static_cast<int>(i) + 1] = r;
        }
        t["tabs"] = tabs;
        return t;
    };
    // A Lua table of view keys -> "key=value;..." (the -texviewer_view grammar).
    auto tvSpec = [](const char* fn, const sol::table& o) -> std::string
    {
        std::string spec;
        auto num = [](double d) -> std::string
        {
            if (std::floor(d) == d && std::fabs(d) < 1e9) return std::to_string(static_cast<long long>(d));
            char b[48];
            std::snprintf(b, sizeof(b), "%.9g", d);
            return b;
        };
        for (const auto& kv : o)
        {
            if (!kv.first.is<std::string>()) Fail(fn, "bad_value", "keys are names (ch, mip, face, ...)");
            const std::string key = kv.first.as<std::string>();
            const sol::object& val = kv.second;
            std::string s;
            if (val.is<bool>())              s = val.as<bool>() ? "1" : "0";
            else if (val.is<double>())       s = num(val.as<double>());
            else if (val.is<std::string>())  s = val.as<std::string>();
            else if (val.is<sol::table>())
            {
                const sol::table pair = val.as<sol::table>();
                const sol::object a = pair[1], b = pair[2];
                if (!a.is<double>() || !b.is<double>()) Fail(fn, "bad_value", key + " wants two numbers");
                s = num(a.as<double>()) + (key == "size" ? "x" : ":") + num(b.as<double>());
            }
            else Fail(fn, "bad_value", key + " has an unusable value");
            if (s.find(';') != std::string::npos) Fail(fn, "bad_value", key + ": ';' is not allowed");
            spec += key + "=" + s + ";";
        }
        return spec;
    };
    ed.set_function("TextureViewer", [tvState](sol::this_state ts) -> sol::table { return tvState(ts); });
    ed.set_function("OpenTextureViewer", [tvCtx, tvState, tvSpec](sol::this_state ts, const std::string& path,
                                                                   sol::optional<sol::table> opts) -> sol::table
    {
        auto& tv = Editor::TextureViewerWindow::Get();
        std::string why;
        if (!tv.OpenNow(tvCtx(), path, why)) Fail("OpenTextureViewer", "refused", why);
        if (opts && !tv.ApplySpec(tvSpec("OpenTextureViewer", *opts), why)) Fail("OpenTextureViewer", "bad_value", why);
        return tvState(ts);
    });
    ed.set_function("TextureViewerSet", [tvState, tvSpec](sol::this_state ts, const sol::table& opts) -> sol::table
    {
        std::string why;
        if (!Editor::TextureViewerWindow::Get().ApplySpec(tvSpec("TextureViewerSet", opts), why))
            Fail("TextureViewerSet", "bad_value", why);
        return tvState(ts);
    });
    // The Content Browser's Import Settings...: open @p path with its Import section
    // unfolded (the tab opens in ProcessPendingActions: frames(1) first).
    ed.set_function("TextureViewerImportSettings", [](const std::string& path) -> bool
    {
        if (path.empty()) Fail("TextureViewerImportSettings", "bad_value", "a .itex path");
        Editor::TextureViewerWindow::Get().RequestImportSettings(path);
        return true;
    });
    ed.set_function("TextureViewerActivate", [tvState](sol::this_state ts, int tab) -> sol::table
    {
        if (!Editor::TextureViewerWindow::Get().Activate(tab - 1)) Fail("TextureViewerActivate", "not_found", "no tab " + std::to_string(tab));
        return tvState(ts);
    });
    ed.set_function("TextureViewerClose", [tvCtx, tvState](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
    {
        auto& tv = Editor::TextureViewerWindow::Get();
        const Editor::TextureViewerContext ctx = tvCtx();
        if (opts && opts->get_or("all", false)) tv.CloseAll(ctx);
        else if (!tv.CloseTab(ctx, opts ? opts->get_or("tab", 0) - 1 : -1))
            Fail("TextureViewerClose", "not_found", "no such tab");
        return tvState(ts);
    });
    ed.set_function("TextureViewerShot", [](const std::string& path) -> double
    {
        auto& tv = Editor::TextureViewerWindow::Get();
        if (tv.ActiveTab() < 0) Fail("TextureViewerShot", "refused", "no texture open");
        if (path.empty()) Fail("TextureViewerShot", "bad_value", "a .png path");
        return static_cast<double>(RequiredJob(tv.QueueShot(path, Logger::CurrentRequestId())));
    });
    // What one texel IS, and what the whole (mip, slice) is.
    //   Editor.TextureViewerProbe(x, y [, {face, side = "a" | "b"}]) -> job: {x, y, mip, face, side, value =
    //       {r,g,b,a} (the GPU's read - what the shaders see), stored = {..} (the file's value), raw (hex: the texel,
    //       or its BC block), block}   (side "b": the comparison's picture B, (x, y) in B's shown mip)
    //   Editor.TextureViewerStats([{mip, face, normal, mipChain}]) -> job: {mip, slice, width, height, texels,
    //       channels, min, max, mean, stddev, nan, inf (per channel), alpha, monochrome, normal?, mipMaxDrift?, ms}
    ed.set_function("TextureViewerProbe", [](int x, int y, sol::optional<sol::table> opts) -> double
    {
        auto& tv = Editor::TextureViewerWindow::Get();
        if (tv.ActiveTab() < 0) Fail("TextureViewerProbe", "refused", "no texture open");
        if (x < 0 || y < 0) Fail("TextureViewerProbe", "bad_value", "x and y are texels of the shown mip (>= 0)");
        const int face = opts ? opts->get_or("face", -1) : -1;
        const std::string side = opts ? opts->get_or<std::string>("side", "a") : std::string("a");
        if (side != "a" && side != "b") Fail("TextureViewerProbe", "bad_value", "side is \"a\" or \"b\"");
        return static_cast<double>(RequiredJob(tv.QueueProbe(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), face,
                                                 Logger::CurrentRequestId(), side == "b")));
    });
    ed.set_function("TextureViewerStats", [tvCtx](sol::optional<sol::table> opts) -> double
    {
        auto& tv = Editor::TextureViewerWindow::Get();
        const Editor::TextureViewerWindow::StateView s = tv.State(tvCtx());
        if (s.active < 0 || s.active >= static_cast<int>(s.tabs.size()))
            Fail("TextureViewerStats", "refused", "no texture open");
        const Editor::TextureViewerWindow::TabState& t = s.tabs[static_cast<std::size_t>(s.active)];
        Resource::AnalyzeOptions o;
        o.mip         = opts ? opts->get_or("mip", static_cast<int>(t.view.mip)) : t.view.mip;
        o.slice       = opts ? opts->get_or("face", static_cast<int>(t.view.slice)) : t.view.slice;
        o.normalCheck = opts ? opts->get_or("normal", false) : false;
        o.mipChain    = opts ? opts->get_or("mipChain", false) : false;
        if (o.mip >= (std::max)(1u, t.info.mipLevels)) Fail("TextureViewerStats", "bad_value", "no such mip");
        return static_cast<double>(RequiredJob(tv.QueueStats(o, Logger::CurrentRequestId())));
    });

    // ---- UI Editor ----
    // A Canvas UI tree in a designer view of its own: laid out at the tab's DESIGN resolution
    // by the game screen's own kernel, drawn by a Renderer UI view; the game screen's
    // UIComputedRect is never written. Tabs and `active` are 1-based. Every UI Editor binding
    // is described with LuaDoc (help("Editor.OpenUIEditor")).
    auto uieCtx = [this]() { return UIEditorCtx(); };
    auto uieHex = [](std::uint64_t v) -> std::string
    {
        char b[20];
        std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
        return b;
    };
    auto uieState = [this, uieCtx, uieHex](sol::this_state ts) -> sol::table
    {
        using W = Editor::UIEditorWindow;
        const Editor::UIEditorContext ctx = uieCtx();
        const W::StateView s = W::Get().State(ctx);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["open"]       = s.open;
        t["active"]     = s.active + 1;
        t["mainScreen"] = lua.create_table_with("w", s.mainScreen.x, "h", s.mainScreen.y);
        // The renderer side: every live UI view and the bytes of their targets - the leak check.
        if (ctx.renderer)
            if (UIViewService* views = ctx.renderer->GetUIViews())
                t["views"] = lua.create_table_with("live", views->LiveCount(),
                                                   "targetBytes", static_cast<double>(views->TargetBytes()));
        sol::table tabs = lua.create_table();
        for (std::size_t i = 0; i < s.tabs.size(); ++i)
        {
            const W::TabState& tab = s.tabs[i];
            const UIView::Status& st = tab.status;
            sol::table r = lua.create_table();
            if (m_world && tab.subject != NullEntity) r["subject"] = LuaId::Out(*m_world, tab.subject);
            r["name"]          = tab.name;
            r["guid"]          = tab.guid;
            // What the tab edits - a scene's canvas, a .iui (its own ring and dirt), a read-only
            // .ipfb template - and the file's state.
            r["kind"]          = std::string(W::TabKindName(tab.kind));
            if (!tab.path.empty()) r["path"] = tab.path;
            r["dirty"]         = tab.dirty;
            r["readOnly"]      = tab.readOnly;
            if (!tab.readOnlyWhy.empty()) r["readOnlyWhy"] = tab.readOnlyWhy;
            r["diskChanged"]   = tab.diskChanged;
            r["parked"]        = tab.parked;
            r["undo"]          = static_cast<int>(tab.undo);   // counts: Lua integers
            r["redo"]          = static_cast<int>(tab.redo);
            if (tab.view.frame.mode == UIView::RootFrame::Mode::Custom)
                r["frame"] = lua.create_table_with("w", tab.view.frame.size.x, "h", tab.view.frame.size.y);
            else
                r["frame"] = std::string("fill");
            r["res"]           = std::string(W::ResolutionName(tab.view.res));
            r["design"]        = lua.create_table_with("w", tab.designW, "h", tab.designH);
            r["zoom"]          = tab.zoom;
            r["fit"]           = tab.view.zoom <= 0.f;
            r["pan"]           = lua.create_table_with("x", tab.pan[0], "y", tab.pan[1]);
            r["bg"]            = std::string(W::BackgroundName(tab.view.background));
            r["state"]         = std::string(W::StateName(tab.view.state));
            r["stateTarget"]   = std::string(tab.view.stateSelected ? "selected" : "all");
            r["hidden"]        = tab.view.showHidden;
            r["realtime"]      = tab.view.realtime;
            r["viewId"]        = tab.viewId == ~0u ? -1 : static_cast<int>(tab.viewId);
            r["renders"]       = static_cast<double>(st.renders);
            r["paints"]        = static_cast<double>(st.paints);
            r["settled"]       = tab.settled;
            r["current"]       = st.current;
            r["target"]        = lua.create_table_with("w", st.width, "h", st.height);
            r["canvasScale"]   = st.canvasScale;
            r["nodes"]         = st.nodes;
            r["cmds"]          = st.cmds;
            r["verts"]         = st.verts;
            r["indices"]       = st.indices;
            r["effects"]       = st.effects;
            r["truncated"]     = st.truncated;
            r["designHash"]    = st.paints ? uieHex(st.designHash) : std::string();
            r["pendingImages"] = st.pendingImages;
            if (m_world && tab.selected != NullEntity) r["selected"] = LuaId::Out(*m_world, tab.selected);
            r["safe"]          = tab.view.safeArea;
            r["grid"]          = tab.view.grid;
            r["gridRef"]       = tab.view.gridRef;
            r["rulers"]        = tab.view.rulers;
            r["snap"]          = tab.view.snap;
            sol::table runs = lua.create_table();
            for (std::uint32_t k = 0; k < st.runRectCount; ++k)
                runs[k + 1] = lua.create_table_with("x", st.runRects[k].x, "y", st.runRects[k].y,
                                                    "invW", st.runRects[k].invW, "invH", st.runRects[k].invH);
            r["runRects"]      = runs;
            // The designer-only flags.
            sol::table hid = lua.create_table(), lk = lua.create_table();
            if (m_world)
            {
                int k = 1;
                for (const Entity e : tab.designHidden) hid[k++] = LuaId::Out(*m_world, e);
                k = 1;
                for (const Entity e : tab.locked) lk[k++] = LuaId::Out(*m_world, e);
            }
            r["designHidden"]  = hid;
            r["locked"]        = lk;
            if (!st.why.empty()) r["why"] = st.why;
            tabs[static_cast<int>(i) + 1] = r;
        }
        t["tabs"] = tabs;
        return t;
    };
    // A Lua table of view keys -> "key=value;..." (the UIEditorWindow::ApplySpec grammar).
    auto uieSpec = [](const char* fn, const sol::table& o, bool skipSubject) -> std::string
    {
        std::string spec;
        auto num = [](double d) -> std::string
        {
            if (std::floor(d) == d && std::fabs(d) < 1e9) return std::to_string(static_cast<long long>(d));
            char b[48];
            std::snprintf(b, sizeof(b), "%.9g", d);
            return b;
        };
        for (const auto& kv : o)
        {
            if (!kv.first.is<std::string>())
                Fail(fn, "bad_value", "keys are names (res, zoom, pan, bg, state, hidden, realtime, size)");
            const std::string key = kv.first.as<std::string>();
            if (skipSubject && (key == "entity" || key == "name" || key == "path")) continue;
            const sol::object& val = kv.second;
            std::string s;
            if (val.is<bool>())             s = val.as<bool>() ? "1" : "0";
            else if (val.is<double>())      s = num(val.as<double>());
            else if (val.is<std::string>()) s = val.as<std::string>();
            else if (val.is<sol::table>())
            {
                const sol::table pair = val.as<sol::table>();
                const sol::object a = pair[1], b = pair[2];
                if (!a.is<double>() || !b.is<double>()) Fail(fn, "bad_value", key + " wants two numbers {a, b}");
                s = num(a.as<double>()) + ((key == "size" || key == "res") ? "x" : ":") + num(b.as<double>());
            }
            else Fail(fn, "bad_value", key + " has an unusable value");
            if (s.find(';') != std::string::npos) Fail(fn, "bad_value", key + ": ';' is not allowed");
            spec += key + "=" + s + ";";
        }
        return spec;
    };
    // A node of the active tab by NAME: none -> not_found, several -> ambiguous (never
    // "the first in paint order": every Palette Button has a child called Label). An id
    // always says which.
    auto uieByName = [this, uieCtx](const char* fn, const std::string& name) -> Entity
    {
        std::vector<UI::LayoutNode> nodes;
        float scale = 1.f;
        UI::Vec2 design{ 0.f, 0.f };
        std::string why;
        if (!Editor::UIEditorWindow::Get().Layout(uieCtx(), nodes, scale, design, why)) Fail(fn, "refused", why);
        Entity found = NullEntity;
        int    n     = 0;
        for (const UI::LayoutNode& ln : nodes)
            if (m_world->GetName(ln.e) == name && n++ == 0) found = ln.e;
        if (n == 0) Fail(fn, "not_found", "no node of the open canvas is named '" + name + "'");
        if (n > 1)
            Fail(fn, "ambiguous", std::to_string(n) + " nodes of the open canvas are named '" + name +
                                      "': name one by its id (Editor.UIEditorLayout lists them)");
        return found;
    };
    LuaDoc::Fn(ed, "Editor", "UIEditor", [uieState](sol::this_state ts) -> sol::table { return uieState(ts); },
        { .doc = "The UI Editor's tabs: each canvas, its design resolution, zoom / pan, and what its designer view "
                 "drew (renders, nodes, the design-pixel draw-list hash).",
          .returns = "table", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor });
    LuaDoc::Fn(ed, "Editor", "OpenUIEditor",
        [this, uieCtx, uieState, uieSpec](sol::this_state ts, const sol::table& opts) -> sol::table
        {
            if (!m_world) Fail("OpenUIEditor", "no_world", "the editor has no world");
            // A file: a .iui in an asset tab of its own, a UI .ipfb read-only.
            const sol::object pathObj = opts.get<sol::object>("path");
            if (pathObj.valid() && pathObj.get_type() != sol::type::lua_nil)
            {
                if (!pathObj.is<std::string>()) Fail("OpenUIEditor", "bad_value", "path is a .iui / .ipfb path");
                const std::string path = pathObj.as<std::string>();
                auto& w = Editor::UIEditorWindow::Get();
                std::string why;
                const std::string spec = uieSpec("OpenUIEditor", opts, true);   // a bad key's TYPE fails first
                const int before = w.TabCount();
                if (!w.OpenAssetNow(uieCtx(), path, why))
                    Fail("OpenUIEditor",
                         why.rfind("cannot read", 0) == 0 || why.find("no such") != std::string::npos ? "not_found" : "refused",
                         why);
                if (!spec.empty() && !w.ApplySpec(spec, why))
                {
                    // All or nothing: the tab this call opened is closed again.
                    if (w.TabCount() > before) w.CloseTab(uieCtx(), w.ActiveTab(), true);
                    Fail("OpenUIEditor", "bad_value", why);
                }
                return uieState(ts);
            }
            Entity e = NullEntity;
            const sol::object ent = opts.get<sol::object>("entity");
            const sol::object nm  = opts.get<sol::object>("name");
            if (ent.valid() && ent.get_type() != sol::type::lua_nil)
            {
                if (!ent.is<LuaEntity>()) Fail("OpenUIEditor", "bad_value", "entity is an entity id");
                e = LuaId::ResolveLive(*m_world, ent.as<LuaEntity>());
                if (e == NullEntity)
                    Fail("OpenUIEditor", "stale_handle", "the entity is not alive (destroyed, or its id was reused)");
            }
            else if (nm.is<std::string>())
            {
                const std::string name = nm.as<std::string>();
                e = Editor::UIEditorWindow::FindCanvasByName(*m_world, name);
                if (e == NullEntity) Fail("OpenUIEditor", "not_found", "no UICanvas entity is named '" + name + "'");
            }
            else
                Fail("OpenUIEditor", "bad_value",
                     "OpenUIEditor{entity = id}, OpenUIEditor{name = \"<canvas name>\"} or OpenUIEditor{path = \"x.iui\"}");
            auto& w = Editor::UIEditorWindow::Get();
            std::string why;
            const std::string spec = uieSpec("OpenUIEditor", opts, true);   // a bad key's TYPE fails first
            const int before = w.TabCount();
            if (!w.OpenNow(uieCtx(), e, why)) Fail("OpenUIEditor", "refused", why);
            if (!spec.empty() && !w.ApplySpec(spec, why))
            {
                // All or nothing: the tab this call opened is closed again.
                if (w.TabCount() > before) w.CloseTab(uieCtx(), w.ActiveTab(), true);
                Fail("OpenUIEditor", "bad_value", why);
            }
            return uieState(ts);
        },
        { .doc = "Open a Canvas in the UI Editor (a UI node opens the canvas above it, selected), or a file: a .iui UI "
                 "asset in a tab of its own (its own undo; Save / Revert), a UI .ipfb read-only. An open tab of it is "
                 "focused. View keys as UIEditorSet (a bad one: nothing opened).",
          .params = { { "opts", "{entity = id | name = canvas name | path = .iui / .ipfb, res, zoom, pan, bg, state, "
                                "hidden, realtime, size, frame}" } },
          // World, not Settings: a file tab spawns its stage's entities.
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "stale_handle", "not_found", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorSet",
        [uieState, uieSpec](sol::this_state ts, const sol::table& opts) -> sol::table
        {
            std::string why;
            if (!Editor::UIEditorWindow::Get().ApplySpec(uieSpec("UIEditorSet", opts, false), why))
                Fail("UIEditorSet", "bad_value", why);
            return uieState(ts);
        },
        { .doc = "Change the active UI Editor tab's view, all or nothing: res, zoom, pan, bg, state, hidden, "
                 "realtime, size (the pinned view box).",
          .params = { { "opts", "{res = \"reference\" | \"match_main\" | {w, h}, zoom = \"fit\" | n, pan = {x, y}, "
                                "bg, state, hidden, realtime, size = {w, h}}" } },
          .returns = "table", .effect = LuaDoc::Effect::Settings, .context = LuaDoc::Context::Editor,
          .errors = { "bad_value" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorLayout",
        [this, uieCtx](sol::this_state ts) -> sol::table
        {
            std::vector<UI::LayoutNode> nodes;
            float scale = 1.f;
            UI::Vec2 design{ 0.f, 0.f };
            std::string why;
            if (!Editor::UIEditorWindow::Get().Layout(uieCtx(), nodes, scale, design, why))
                Fail("UIEditorLayout", "refused", why);
            sol::state_view lua(ts);
            sol::table t = lua.create_table();
            t["design"] = lua.create_table_with("w", design.x, "h", design.y);
            t["scale"]  = scale;
            sol::table rows = lua.create_table();
            int i = 1;
            for (const UI::LayoutNode& n : nodes)
            {
                sol::table r = lua.create_table();
                r["id"]      = LuaId::Out(*m_world, n.e);
                r["name"]    = m_world->GetName(n.e);
                // Its UI parent (a stage node's tree is not the Outliner's).
                if (const Entity p = UI::ResolveUIParent(*m_world, n.e); p != NullEntity)
                    r["parent"] = LuaId::Out(*m_world, p);
                r["x"]       = n.cr.rect.mn.x;
                r["y"]       = n.cr.rect.mn.y;
                r["w"]       = n.cr.rect.Width();
                r["h"]       = n.cr.rect.Height();
                r["clipX"]   = n.cr.clipRect.mn.x;
                r["clipY"]   = n.cr.clipRect.mn.y;
                r["clipW"]   = n.cr.clipRect.Width();
                r["clipH"]   = n.cr.clipRect.Height();
                r["culled"]  = n.cr.culled;
                r["hidden"]  = n.hidden;
                r["depth"]   = n.cr.depth;
                r["sortKey"] = n.cr.sortKey;
                rows[i++] = r;
            }
            t["nodes"] = rows;
            return t;
        },
        { .doc = "The active UI Editor tab's canvas laid out exactly as its designer view paints it, in design "
                 "pixels (the geometry oracle; compare Engine.GetUIComputedRect at the same resolution).",
          .returns = "table", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor,
          .errors = { "refused" } });
    // Selection and the Rect Tool, through the code the mouse runs.
    LuaDoc::Fn(ed, "Editor", "UIEditorSelect",
        [this, uieCtx, uieState, uieByName](sol::this_state ts, sol::object target) -> sol::table
        {
            if (!m_world) Fail("UIEditorSelect", "no_world", "the editor has no world");
            auto& w = Editor::UIEditorWindow::Get();
            Entity e = NullEntity;
            if (target.is<std::string>())
                e = uieByName("UIEditorSelect", target.as<std::string>());
            else if (target.is<LuaEntity>())
            {
                e = LuaId::ResolveLive(*m_world, target.as<LuaEntity>());
                if (e == NullEntity) Fail("UIEditorSelect", "stale_handle", "the entity is not alive");
            }
            else if (target.valid() && target.get_type() != sol::type::lua_nil)
                Fail("UIEditorSelect", "bad_value", "UIEditorSelect(id | name | nil)");
            std::string why;
            if (!w.Select(uieCtx(), e, why)) Fail("UIEditorSelect", "refused", why);
            return uieState(ts);
        },
        { .doc = "Select a node of the active UI Editor tab by id or name (nil clears; a name two nodes share is "
                 "ambiguous); scene tabs share the editor's selection.",
          .params = { { "target", "an entity id, a node name, or nil" } },
          .returns = "table", .effect = LuaDoc::Effect::Settings, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "refused", "not_found", "ambiguous", "stale_handle", "bad_value" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorSelectAt",
        [this, uieCtx](sol::this_state ts, double x, double y, sol::optional<sol::table> opts) -> sol::object
        {
            if (!m_world) Fail("UIEditorSelectAt", "no_world", "the editor has no world");
            auto& w = Editor::UIEditorWindow::Get();
            const Editor::UIEditorContext ctx = uieCtx();
            std::string why;
            if (w.ActiveTab() < 0) Fail("UIEditorSelectAt", "refused", "no canvas open");
            w.SyncLayout(ctx);   // a scripted call: this frame's nodes
            const bool cycle = opts ? opts->get_or("cycle", false) : false;
            const Entity hit = w.HitTest(ctx, UI::Vec2{ static_cast<float>(x), static_cast<float>(y) }, cycle,
                                         w.Selection(ctx));
            if (!w.Select(ctx, hit, why)) Fail("UIEditorSelectAt", "refused", why);
            return sol::make_object(ts, LuaId::Out(*m_world, hit));
        },
        { .doc = "Select the node of the active UI Editor tab under design pixel (x, y) - the click's hit test: "
                 "front to back, every node; empty space = the canvas. {cycle = true} = Alt+click.",
          .params = { { "x", "design px" }, { "y", "design px" }, { "opts", "{cycle = boolean}", nullptr, true } },
          .returns = "EntityId", .effect = LuaDoc::Effect::Settings, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "refused" } });
    // What a double-click on the main view opens in the UI Editor - the same hit test,
    // as a call.
    LuaDoc::Fn(ed, "Editor", "ScreenUIAt",
        [this](sol::this_state ts, double x, double y) -> sol::object
        {
            if (!m_world) Fail("ScreenUIAt", "no_world", "the editor has no world");
            const Entity e = ScreenUIAt(static_cast<float>(x), static_cast<float>(y));
            if (e == NullEntity) return sol::make_object(ts, sol::lua_nil);
            return sol::make_object(ts, LuaId::Out(*m_world, e));
        },
        { .doc = "The Canvas UI node drawn at (x, y) of the main view (UIComputedRect pixels = the viewport image's): "
                 "the frontmost by sortKey among nodes that draw (an image or a text), inside their clip - never a "
                 "canvas root, never a UI Editor stage. nil: none. A double-click there opens it in the UI Editor.",
          .params = { { "x", "main view px" }, { "y", "main view px" } },
          .returns = "EntityId|nil", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor,
          .errors = { "no_world" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorDrag",
        [this, uieCtx, uieState](sol::this_state ts, const sol::table& o) -> sol::table
        {
            if (!m_world) Fail("UIEditorDrag", "no_world", "the editor has no world");
            const int handle = Editor::UIEditorWindow::HandleFromName(o.get_or<std::string>("handle", "body"));
            if (handle < 0)
                Fail("UIEditorDrag", "bad_value", "handle is body, tl, tr, br, bl, t, r, b, l, anchor_min, anchor_max, "
                     "anchor_tr, anchor_bl, anchor or pivot");
            const double dx = o.get_or("dx", 0.0), dy = o.get_or("dy", 0.0);
            if (!std::isfinite(dx) || !std::isfinite(dy)) Fail("UIEditorDrag", "bad_value", "dx / dy are numbers");
            std::string why;
            if (!Editor::UIEditorWindow::Get().Drag(uieCtx(), handle,
                                                     UI::Vec2{ static_cast<float>(dx), static_cast<float>(dy) },
                                                     o.get_or("ctrl", false), o.get_or("shift", false),
                                                     o.get_or("alt", false), why))
                Fail("UIEditorDrag", "refused", why);
            return uieState(ts);
        },
        { .doc = "Drag a handle of the selected UI node by (dx, dy) DESIGN px through the Rect Tool's own code "
                 "(snapping unless alt, ctrl quantises, shift keeps a corner's aspect) - one undo step.",
          .params = { { "opts", "{handle = body|tl|tr|br|bl|t|r|b|l, dx, dy, ctrl, shift, alt}" } },
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorAnchorPreset",
        [this, uieCtx, uieState](sol::this_state ts, const std::string& name, sol::optional<sol::table> opts) -> sol::table
        {
            if (!m_world) Fail("UIEditorAnchorPreset", "no_world", "the editor has no world");
            if (!Editor::UIEditorWindow::IsAnchorPreset(name))
                Fail("UIEditorAnchorPreset", "bad_value", "a preset: top_left, top_center, top_right, top_stretch, "
                     "middle_left .. middle_stretch, bottom_left .. bottom_stretch, stretch_left, stretch_center, "
                     "stretch_right, stretch_full");
            std::string why;
            if (!Editor::UIEditorWindow::Get().ApplyAnchorPreset(uieCtx(), name, opts ? opts->get_or("pivot", false) : false,
                                                                 opts ? opts->get_or("position", false) : false,
                                                                 opts ? opts->get_or("keepVisual", true) : true, why))
                Fail("UIEditorAnchorPreset", "refused", why);
            return uieState(ts);
        },
        { .doc = "Set the selected UI node's anchors to a 4x4 preset, solving offset and size so the rect stays "
                 "where it is; {pivot = true} also takes the preset's pivot (Alt), {position = true} puts the "
                 "rect onto the anchors (Shift), {keepVisual = false} writes the raw anchors. One undo step.",
          .params = { { "name", "top_left .. stretch_full" },
                      { "opts", "{pivot = boolean, position = boolean, keepVisual = boolean}", nullptr, true } },
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "refused" } });
    // The panels' operations - the Palette, the Hierarchy's menu, the Details' Native
    // Size, the inline text editor, a texture drop, the Check page - each the code the
    // mouse runs and ONE undo step. A node is an id or a name of the open canvas.
    auto uieNode = [this, uieCtx, uieByName](const char* fn, const sol::object& target, bool nilIsSelection) -> Entity
    {
        if (!m_world) Fail(fn, "no_world", "the editor has no world");
        if (!target.valid() || target.get_type() == sol::type::lua_nil)
        {
            if (!nilIsSelection) Fail(fn, "bad_value", "a node: an entity id or a name");
            // The ACTIVE TAB's selection - an asset tab's is its own (the scene's selection is
            // another world's entity there).
            const Entity sel = Editor::UIEditorWindow::Get().Selection(uieCtx());
            if (sel == NullEntity) Fail(fn, "refused", "nothing is selected in the active tab");
            return sel;
        }
        if (target.is<std::string>()) return uieByName(fn, target.as<std::string>());
        if (!target.is<LuaEntity>()) Fail(fn, "bad_value", "a node: an entity id or a name");
        const Entity e = LuaId::ResolveLive(*m_world, target.as<LuaEntity>());
        if (e == NullEntity) Fail(fn, "stale_handle", "the entity is not alive");
        return e;
    };
    LuaDoc::Fn(ed, "Editor", "UIEditorCreate",
        [this, uieCtx, uieNode](sol::this_state ts, const sol::table& o) -> sol::object
        {
            if (!m_world) Fail("UIEditorCreate", "no_world", "the editor has no world");
            const sol::object ty = o.get<sol::object>("type"), tp = o.get<sol::object>("template");
            std::string what;
            Editor::UITemplates::Widget widget{};
            if (ty.is<std::string>())
            {
                what = ty.as<std::string>();
                if (!Editor::UITemplates::FromName(what, widget))
                    Fail("UIEditorCreate", "bad_value", "type is panel, image, text, button, sprite, clip_panel or empty_rect");
            }
            else if (tp.is<std::string>())
                what = tp.as<std::string>();
            else
                Fail("UIEditorCreate", "bad_value", "UIEditorCreate{type = \"button\" | template = \"<.ipfb>\", parent, at = {x, y}}");
            Entity parent = NullEntity;
            const sol::object po = o.get<sol::object>("parent");
            if (po.valid() && po.get_type() != sol::type::lua_nil) parent = uieNode("UIEditorCreate", po, false);
            bool hasAt = false;
            UI::Vec2 at{ 0.f, 0.f };
            const sol::object ao = o.get<sol::object>("at");
            if (ao.valid() && ao.get_type() != sol::type::lua_nil)
            {
                if (!ao.is<sol::table>()) Fail("UIEditorCreate", "bad_value", "at = {x, y} (design px)");
                const sol::table a = ao.as<sol::table>();
                const sol::object x = a[1], y = a[2];
                if (!x.is<double>() || !y.is<double>()) Fail("UIEditorCreate", "bad_value", "at = {x, y} (design px)");
                hasAt = true;
                at = { static_cast<float>(x.as<double>()), static_cast<float>(y.as<double>()) };
            }
            std::string why;
            const Entity e = Editor::UIEditorWindow::Get().CreateWidget(uieCtx(), what, parent, hasAt, at, why);
            if (e == NullEntity) Fail("UIEditorCreate", "refused", why);
            return sol::make_object(ts, LuaId::Out(*m_world, e));
        },
        { .doc = "Make a Palette widget (type) or a UI template (template = .ipfb, a detached copy) under parent - none "
                 "given: the selection (a text node's parent), else the canvas - last among its siblings, its pivot on "
                 "at = {x, y} design px, else centred. One undo step; it becomes the selection.",
          .params = { { "opts", "{type = panel|image|text|button|sprite|clip_panel|empty_rect | template = path, "
                                "parent = id | name, at = {x, y}}" } },
          .returns = "EntityId", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "not_found", "ambiguous", "stale_handle", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorReorder",
        [uieCtx, uieState, uieNode](sol::this_state ts, sol::object target, int delta) -> sol::table
        {
            const Entity e = uieNode("UIEditorReorder", target, false);
            std::string why;
            if (!Editor::UIEditorWindow::Get().Reorder(uieCtx(), e, delta, why)) Fail("UIEditorReorder", "refused", why);
            return uieState(ts);
        },
        { .doc = "Move a node delta places later (> 0, painted over its siblings) or earlier (< 0) among its "
                 "siblings, clamped at the ends. One undo step.",
          .params = { { "target", "an entity id or a node name" }, { "delta", "places; negative = earlier" } },
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "not_found", "ambiguous", "stale_handle", "refused" } });
    // The Hierarchy's drop as a call: Editor.Reparent refuses a stage node (transient),
    // so without this a script cannot restructure an asset tab.
    LuaDoc::Fn(ed, "Editor", "UIEditorReparent",
        [uieCtx, uieState, uieNode](sol::this_state ts, sol::object target, sol::object parent,
                                    sol::optional<sol::table> opts) -> sol::table
        {
            const Entity e = uieNode("UIEditorReparent", target, false);
            const Entity p = uieNode("UIEditorReparent", parent, false);
            int index = -1;
            if (opts)
            {
                const sol::object io = opts->get<sol::object>("index");
                if (io.valid() && io.get_type() != sol::type::lua_nil)
                {
                    const double d = io.is<double>() ? io.as<double>() : 0.0;
                    if (!io.is<double>() || d < 1.0 || d != std::floor(d) || d > 1.0e6)
                        Fail("UIEditorReparent", "bad_value", "index is 1-based: 1 = first among the new parent's children");
                    index = static_cast<int>(d) - 1;
                }
            }
            std::string why;
            if (!Editor::UIEditorWindow::Get().Reparent(uieCtx(), e, p, index, why))
            {
                // MoveUINode's codes ("cycle: ...") and the window's ("refused: ...").
                const auto colon = why.find(": ");
                const std::string code = colon == std::string::npos ? "refused" : why.substr(0, colon);
                Fail("UIEditorReparent", code.c_str(), colon == std::string::npos ? why : why.substr(colon + 2));
            }
            return uieState(ts);
        },
        { .doc = "The Hierarchy's drop as a call: move a node of the active tab under another node of it (the "
                 "root included); {index = n} = n-th among the new parent's children in paint order (1-based, "
                 "omitted = last). One undo step - an asset tab's own ring (Editor.Reparent refuses a stage node).",
          .params = { { "target", "an entity id or a node name" }, { "parent", "an entity id or a node name" },
                      { "opts", "{index = n} (optional)" } },
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "not_found", "ambiguous", "stale_handle", "refused", "cycle" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorFlags",
        [uieCtx, uieState, uieNode](sol::this_state ts, sol::object target, const sol::table& o) -> sol::table
        {
            const Entity e = uieNode("UIEditorFlags", target, false);
            auto& w = Editor::UIEditorWindow::Get();
            std::string why;
            const sol::object hid = o.get<sol::object>("hidden"), lk = o.get<sol::object>("locked");
            if ((hid.valid() && hid.get_type() != sol::type::lua_nil && !hid.is<bool>()) ||
                (lk.valid() && lk.get_type() != sol::type::lua_nil && !lk.is<bool>()))
                Fail("UIEditorFlags", "bad_value", "{hidden = boolean, locked = boolean}");
            if (hid.is<bool>() && !w.SetDesignerFlag(uieCtx(), e, false, hid.as<bool>(), why))
                Fail("UIEditorFlags", "refused", why);
            if (lk.is<bool>() && !w.SetDesignerFlag(uieCtx(), e, true, lk.as<bool>(), why))
                Fail("UIEditorFlags", "refused", why);
            return uieState(ts);
        },
        { .doc = "The designer-only flags of a node of the open canvas (never saved, never in the game): hidden = "
                 "not painted or hit in the designer (its subtree too), locked = a click goes through it.",
          .params = { { "target", "an entity id or a node name" }, { "flags", "{hidden = boolean, locked = boolean}" } },
          .returns = "table", .effect = LuaDoc::Effect::Settings, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "not_found", "ambiguous", "stale_handle", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorSetText",
        [uieCtx, uieState, uieNode](sol::this_state ts, sol::object target, const std::string& text) -> sol::table
        {
            const Entity e = uieNode("UIEditorSetText", target, false);
            std::string why;
            if (!Editor::UIEditorWindow::Get().SetText(uieCtx(), e, text, why)) Fail("UIEditorSetText", "refused", why);
            return uieState(ts);
        },
        { .doc = "Set a text node's text the way the designer's inline editor commits it (double-click a text). "
                 "One undo step.",
          .params = { { "target", "an entity id or a node name" }, { "text", "the new text" } },
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "not_found", "ambiguous", "stale_handle", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorDropTexture",
        [this, uieCtx](sol::this_state ts, const std::string& path, double x, double y) -> sol::object
        {
            if (!m_world) Fail("UIEditorDropTexture", "no_world", "the editor has no world");
            if (!std::isfinite(x) || !std::isfinite(y)) Fail("UIEditorDropTexture", "bad_value", "x / y are design px");
            std::string why;
            const Entity e = Editor::UIEditorWindow::Get().DropTexture(
                uieCtx(), path, UI::Vec2{ static_cast<float>(x), static_cast<float>(y) }, why);
            if (e == NullEntity) Fail("UIEditorDropTexture", "refused", why);
            return sol::make_object(ts, LuaId::Out(*m_world, e));
        },
        { .doc = "Drop a texture on the designer at (x, y) design px, as the Content Browser drag does: onto the image "
                 "under it (its texture), else a new Image there at the texture's size. Returns the node. One undo step.",
          .params = { { "path", "a .itex" }, { "x", "design px" }, { "y", "design px" } },
          .returns = "EntityId", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorNativeSize",
        [uieCtx, uieState, uieNode](sol::this_state ts, sol::object target) -> sol::table
        {
            const Entity e = uieNode("UIEditorNativeSize", target, true);
            std::string why;
            if (!Editor::UIEditorWindow::Get().NativeSize(uieCtx(), e, why)) Fail("UIEditorNativeSize", "refused", why);
            return uieState(ts);
        },
        { .doc = "Size an image node to its texture: one reference px a texel across its uv window (a sprite sheet: "
                 "one frame); the pivot stays. nil = the selection. One undo step.",
          .params = { { "target", "an entity id, a node name, or nil (the selection)", nullptr, true } },
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "not_found", "ambiguous", "stale_handle", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorWrap",
        [this, uieCtx, uieNode](sol::this_state ts, sol::object target) -> sol::object
        {
            const Entity e = uieNode("UIEditorWrap", target, true);
            std::string why;
            auto& w = Editor::UIEditorWindow::Get();
            if (!w.WrapInRect(uieCtx(), e, why)) Fail("UIEditorWrap", "refused", why);
            // After the wrap the wrapper is the TAB's selection (an asset tab's is its own - the
            // scene's selection there is another entity).
            return sol::make_object(ts, LuaId::Out(*m_world, w.Selection(uieCtx())));
        },
        { .doc = "Wrap a node in a new Empty Rect that takes its place and its rect (the node then fills it: the "
                 "picture does not change). Returns the wrapper. One undo step.",
          .params = { { "target", "an entity id, a node name, or nil (the selection)", nullptr, true } },
          .returns = "EntityId", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "bad_value", "not_found", "ambiguous", "stale_handle", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorCheck",
        [this, uieCtx](sol::this_state ts) -> sol::table
        {
            if (!m_world) Fail("UIEditorCheck", "no_world", "the editor has no world");
            auto& w = Editor::UIEditorWindow::Get();
            if (w.ActiveTab() < 0) Fail("UIEditorCheck", "refused", "no canvas open");
            sol::state_view lua(ts);
            sol::table out = lua.create_table();
            int i = 1;
            for (const Editor::UIEditorWindow::Issue& is : w.Check(uieCtx()))
            {
                sol::table r = lua.create_table();
                if (m_world->IsAlive(is.e))
                {
                    r["id"]   = LuaId::Out(*m_world, is.e);
                    r["name"] = m_world->GetName(is.e);
                }
                r["kind"] = is.kind;
                r["text"] = is.text;
                out[i++] = r;
            }
            return out;
        },
        { .doc = "The UI Editor's Check page for the open canvas: {{id, name, kind, text}} - kind is size (resolves to "
                 "nothing), offscreen (off the screen at every preset), texture / font (missing), name (siblings "
                 "share it), unreachable / cycle (a UI node of the world under no canvas).",
          .returns = "table", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorActivate",
        [uieCtx, uieState](sol::this_state ts, int tab) -> sol::table
        {
            auto& w = Editor::UIEditorWindow::Get();
            // A held drag / nudge is the old tab's step (the ring is the active tab's).
            w.CommitGestures(uieCtx());
            if (!w.Activate(tab - 1)) Fail("UIEditorActivate", "not_found", "no tab " + std::to_string(tab));
            return uieState(ts);
        },
        { .doc = "Make UI Editor tab @p tab (1-based) the active one.",
          .params = { { "tab", "1-based tab index" } },
          .returns = "table", .effect = LuaDoc::Effect::Settings, .context = LuaDoc::Context::Editor,
          .errors = { "not_found" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorClose",
        [uieCtx, uieState](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
        {
            auto& w = Editor::UIEditorWindow::Get();
            const Editor::UIEditorContext ctx = uieCtx();
            // An asset tab with unsaved edits closes only with {discard = true}.
            const bool discard = opts ? opts->get_or("discard", false) : false;
            if (opts && opts->get_or("all", false))
            {
                if (!discard && !w.UnsavedAssets().empty())
                    Fail("UIEditorClose", "dirty", "unsaved UI assets: " + w.UnsavedAssets().front() +
                                                       (w.UnsavedAssets().size() > 1 ? " ..." : "") +
                                                       " - save them, or {all = true, discard = true}");
                w.CloseAll(ctx);
            }
            else
            {
                std::string why;
                if (!w.CloseTab(ctx, opts ? opts->get_or("tab", 0) - 1 : -1, discard, &why))
                    Fail("UIEditorClose", why.find("unsaved") != std::string::npos ? "dirty" : "not_found", why);
            }
            return uieState(ts);
        },
        { .doc = "Close a UI Editor tab: {tab = n} (1-based), {all = true}, or the active one. An asset tab with unsaved "
                 "edits is refused (dirty) unless {discard = true}; its stage goes with it.",
          .params = { { "opts", "{tab = n} | {all = true} [, discard = true]; nothing = the active tab" } },
          // World, not Settings: a file tab's stage entities are destroyed (and
          // {discard = true} throws its unsaved edits away).
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "not_found", "dirty" } });
    // ---- UI asset tabs ----
    // A .iui opened by Editor.OpenUIEditor{path = ...} edits a stage of its own: its
    // steps are the TAB's (UIEditorUndo / Redo), never the scene's history or dirt.
    LuaDoc::Fn(ed, "Editor", "UIEditorSave",
        [uieCtx](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
        {
            std::string as;
            if (opts)
            {
                const sol::object o = opts->get<sol::object>("as");
                if (o.valid() && o.get_type() != sol::type::lua_nil)
                {
                    if (!o.is<std::string>()) Fail("UIEditorSave", "bad_value", "as is a .iui path");
                    as = o.as<std::string>();
                }
            }
            auto& w = Editor::UIEditorWindow::Get();
            if (w.ActiveTab() < 0) Fail("UIEditorSave", "refused", "no tab open");
            std::string why;
            std::size_t bytes = 0, nodes = 0;
            // Save As over another existing file only with {overwrite = true}.
            const bool overwrite = opts ? opts->get_or("overwrite", false) : false;
            if (!w.Save(uieCtx(), as, why, &bytes, &nodes, overwrite)) Fail("UIEditorSave", "refused", why);
            const auto s = w.State(uieCtx());
            sol::state_view lua(ts);
            return lua.create_table_with("path", s.tabs[static_cast<std::size_t>(s.active)].path, "bytes",
                                         static_cast<double>(bytes), "nodes", static_cast<double>(nodes));
        },
        { .doc = "Save the active UI Editor asset tab: its stage captured, read back and written atomically as the "
                 "canonical .iui. {as = path}: Save As (the tab follows the new file; another existing file only "
                 "with overwrite = true). Synchronous.",
          .params = { { "opts", "{as = .iui path, overwrite = boolean}", nullptr, true } },
          .returns = "table", .returnsDoc = "{path, bytes, nodes}", .effect = LuaDoc::Effect::Disk,
          .context = LuaDoc::Context::Editor,
          .errors = { "refused", "bad_value" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorRevert",
        [uieCtx, uieState](sol::this_state ts) -> sol::table
        {
            std::string why;
            if (!Editor::UIEditorWindow::Get().Revert(uieCtx(), why)) Fail("UIEditorRevert", "refused", why);
            return uieState(ts);
        },
        { .doc = "Read the active asset (or template) tab's file again: the stage respawned from it, unsaved edits and "
                 "the tab's undo history dropped.",
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorUndo",
        [uieCtx, uieState](sol::this_state ts) -> sol::table
        {
            std::string why;
            if (!Editor::UIEditorWindow::Get().Undo(uieCtx(), false, why)) Fail("UIEditorUndo", "refused", why);
            return uieState(ts);
        },
        { .doc = "Undo the last step of the active UI Editor ASSET tab's own history (a scene tab's are the scene's: "
                 "Editor.Undo).",
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorRedo",
        [uieCtx, uieState](sol::this_state ts) -> sol::table
        {
            std::string why;
            if (!Editor::UIEditorWindow::Get().Undo(uieCtx(), true, why)) Fail("UIEditorRedo", "refused", why);
            return uieState(ts);
        },
        { .doc = "Redo a step of the active UI Editor ASSET tab's own history.",
          .returns = "table", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorDuplicate",
        [this, uieCtx, uieByName](sol::this_state ts, sol::object target) -> sol::object
        {
            if (!m_world) Fail("UIEditorDuplicate", "no_world", "the editor has no world");
            auto& w = Editor::UIEditorWindow::Get();
            const Editor::UIEditorContext ctx = uieCtx();
            Entity e = NullEntity;
            if (target.is<LuaEntity>()) e = LuaId::ResolveLive(*m_world, target.as<LuaEntity>());
            else if (target.is<std::string>()) e = uieByName("UIEditorDuplicate", target.as<std::string>());
            else
                e = w.Selection(ctx);
            if (e == NullEntity) Fail("UIEditorDuplicate", "not_found", "no such node of the active tab");
            std::string why;
            const Entity c = w.Duplicate(ctx, e, why);
            if (c == NullEntity) Fail("UIEditorDuplicate", "refused", why);
            return sol::make_object(ts, LuaId::Out(*m_world, c));
        },
        { .doc = "Copy a node of the active UI Editor tab (id, name or nil = the selection) beside itself with its "
                 "subtree - one undo step (an asset tab's own); the copy becomes the selection.",
          .params = { { "target", "an entity id, a node name, or nil", nullptr, true } },
          .returns = "EntityId", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "refused", "not_found", "ambiguous" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorConvert",
        [uieCtx, uieState](sol::this_state ts, const std::string& path, sol::optional<sol::table> opts) -> sol::table
        {
            std::string why;
            if (!Editor::UIEditorWindow::Get().Convert(uieCtx(), path, opts ? opts->get_or("overwrite", false) : false, why))
                Fail("UIEditorConvert", "refused", why);
            return uieState(ts);
        },
        { .doc = "Write the active read-only .ipfb template tab as a .iui UI asset at path and open it (the template's "
                 "tab closes). An existing file is refused unless {overwrite = true}.",
          .params = { { "path", "the .iui to write" }, { "opts", "{overwrite = boolean}", nullptr, true } },
          .returns = "table", .effect = LuaDoc::Effect::Disk, .context = LuaDoc::Context::Editor,
          .errors = { "refused" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorSaveBranch",
        [this, uieCtx, uieByName](sol::this_state ts, sol::object target, const std::string& path,
                                  sol::optional<sol::table> opts) -> sol::table
        {
            if (!m_world) Fail("UIEditorSaveBranch", "no_world", "the editor has no world");
            auto& w = Editor::UIEditorWindow::Get();
            const Editor::UIEditorContext ctx = uieCtx();
            Entity e = NullEntity;
            if (target.is<LuaEntity>()) e = LuaId::ResolveLive(*m_world, target.as<LuaEntity>());
            else if (target.is<std::string>()) e = uieByName("UIEditorSaveBranch", target.as<std::string>());
            if (e == NullEntity) Fail("UIEditorSaveBranch", "not_found", "no such node of the active tab");
            std::string why;
            std::size_t nodes = 0;
            if (!w.SaveBranch(ctx, e, path, opts ? opts->get_or("overwrite", false) : false,
                              opts ? opts->get_or("open", false) : false, why, &nodes))
                Fail("UIEditorSaveBranch", "refused", why);
            sol::state_view lua(ts);
            return lua.create_table_with("path", path, "nodes", static_cast<double>(nodes));
        },
        { .doc = "Write a node of the active UI Editor tab (id or name) and its subtree as a new .iui UI asset (a canvas: "
                 "a canvas asset); the scene is not changed. {open = true} opens it; an existing file is refused unless "
                 "{overwrite = true}.",
          .params = { { "target", "an entity id or a node name" }, { "path", "the .iui to write" },
                      { "opts", "{open = boolean, overwrite = boolean}", nullptr, true } },
          .returns = "table", .returnsDoc = "{path, nodes}", .effect = LuaDoc::Effect::Disk,
          .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "refused", "not_found", "ambiguous" } });
    // Extract to UI Asset: the branch becomes a .iui and the scene a reference to it.
    LuaDoc::Fn(ed, "Editor", "UIEditorExtract",
        [this, uieCtx, uieByName](sol::this_state ts, sol::object target, const std::string& path,
                                  sol::optional<sol::table> opts) -> sol::object
        {
            if (!m_world) Fail("UIEditorExtract", "no_world", "the editor has no world");
            auto& w = Editor::UIEditorWindow::Get();
            const Editor::UIEditorContext ctx = uieCtx();
            Entity e = NullEntity;
            if (target.is<LuaEntity>()) e = LuaId::ResolveLive(*m_world, target.as<LuaEntity>());
            else if (target.is<std::string>()) e = uieByName("UIEditorExtract", target.as<std::string>());
            if (e == NullEntity) Fail("UIEditorExtract", "not_found", "no such node of the active tab");
            std::string why;
            const Entity host = w.Extract(ctx, e, path, opts ? opts->get_or("overwrite", false) : false, why);
            if (host == NullEntity) Fail("UIEditorExtract", "refused", why);
            return sol::make_object(ts, LuaId::Out(*m_world, host));
        },
        { .doc = "Extract to UI Asset: a node of the active SCENE tab (id or name) and its subtree written as a .iui at "
                 "path, and replaced in the scene by a UI Document referencing it (a UI node filling its parent at its "
                 "place; a canvas: an entity of its own). The picture does not change; one undo step. Returns the host. "
                 "An existing file is refused unless {overwrite = true}.",
          .params = { { "target", "an entity id or a node name" }, { "path", "the .iui to write" },
                      { "opts", "{overwrite = boolean}", nullptr, true } },
          // Disk: it writes the .iui (the scene change is one undo step; the file is not).
          .returns = "EntityId", .effect = LuaDoc::Effect::Disk, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "refused", "not_found", "ambiguous" } });
    LuaDoc::Fn(ed, "Editor", "UIEditorShot",
        [](const std::string& path) -> double
        {
            auto& w = Editor::UIEditorWindow::Get();
            if (w.ActiveTab() < 0) Fail("UIEditorShot", "refused", "no canvas open");
            if (path.empty()) Fail("UIEditorShot", "bad_value", "a .png path");
            return static_cast<double>(RequiredJob(w.QueueShot(path, Logger::CurrentRequestId())));
        },
        { .doc = "A job: write the active UI Editor tab's picture (its view box) to a PNG once it has settled.",
          .params = { { "path", "the .png to write" } },
          .returns = "integer", .effect = LuaDoc::Effect::Job, .context = LuaDoc::Context::Editor,
          .errors = { "refused", "bad_value", "busy" } });

    // ---- Material Editor ----
    // One .imat / .imatinst per tab; edits are STAGED in the tab's session (its own undo,
    // never the scene's) and, in an INSTANCE, every edited key joins the override set
    // (UE's rule - an .imatinst writes only the keys in that set).
    //   Editor.OpenMaterialEditor(path | {entity = id}) -> state   (an open tab of the file is focused)
    //   Editor.MaterialEditor() -> {open, active, viewId, livePushes, tabs = {{path, kind, dirty, instance, parent,
    //       explicit, overrides = {...}, unset = {...}, undo, redo, diskChanged, live, ...}}}   (1-based)
    //   Editor.MaterialEditorSet{key = value, ...} -> state  ONE undo step; the .imat keys
    //       (blendMode = 1, roughnessMin = 0.2, baseColor = {1, 0, 0, 1}, castShadow = false,
    //       cparam_Tint = {..}, tex_NORMALMAP = "0 asset/x.itex"); an unknown key refuses the lot
    //   Editor.MaterialEditorOverride(key, on) -> state      tick / untick (untick = the parent's value)
    //   Editor.MaterialEditorUndo() / Redo() / Revert() / ResetOverrides() -> state
    //   Editor.MaterialEditorSave([{as = path, instance = true}]) -> job   (instance: a new .imatinst of it)
    //   Editor.MaterialEditorClose([{tab = n, all = true, discard = true}]) -> state   (dirty: refused)
    //   Editor.MaterialEditorActivate(tab) -> state          Editor.MaterialEditorView(mode) -> state
    auto matEdState = [](sol::this_state ts) -> sol::table
    {
        const Editor::MaterialEditorWindow::StateView v = Editor::MaterialEditorWindow::Get().State();
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["open"]   = v.open;
        t["active"] = v.active + 1;
        t["viewId"] = v.viewId == ~0u ? -1 : static_cast<int>(v.viewId);
        sol::table tabs = lua.create_table();
        for (std::size_t i = 0; i < v.tabs.size(); ++i)
        {
            const auto& ts2 = v.tabs[i];
            sol::table r = lua.create_table();
            r["path"]        = ts2.path;
            r["kind"]        = ts2.kind;
            r["dirty"]       = ts2.dirty;
            r["instance"]    = ts2.instance;
            if (ts2.instance) r["parent"] = ts2.parent;
            r["explicit"]    = ts2.explicitFormat;
            sol::table ov = lua.create_table();
            for (std::size_t k = 0; k < ts2.overrides.size(); ++k) ov[k + 1] = ts2.overrides[k];
            r["overrides"]   = ov;
            sol::table un = lua.create_table();
            for (std::size_t k = 0; k < ts2.unset.size(); ++k) un[k + 1] = ts2.unset[k];
            r["unset"]       = un;
            r["undo"]        = ts2.undo;
            r["redo"]        = ts2.redo;
            r["diskChanged"] = ts2.diskChanged;
            r["live"]        = ts2.live;   // the scene reads its unsaved edits
            if (!ts2.graph.empty())        // the tab's graph (Editor.MaterialGraph)
            {
                r["graph"]      = ts2.graph;
                r["graphDirty"] = ts2.graphDirty;
            }
            if (!ts2.rtProxy.empty())      // what rays / shadows see of it
            {
                r["rtProxy"]       = ts2.rtProxy;
                r["shadowMaskGap"] = ts2.shadowMaskGap;
                if (!ts2.mirrorMask.empty()) r["mirrorMask"] = ts2.mirrorMask;
            }
            if (ts2.entity.entity != NullEntity)   // an entity tab's entity
                r["entity"] = LuaEntity{ ts2.entity.entity, ts2.entity.generation & LuaId::kGenMask };
            tabs[static_cast<int>(i) + 1] = r;
        }
        t["tabs"] = tabs;
        t["livePushes"] = v.livePushes;
        return t;
    };
    auto matEdSession = [](const char* fn) -> Editor::MaterialEditSession&
    {
        Editor::MaterialEditSession* s = Editor::MaterialEditorWindow::Get().ActiveSession();
        if (!s) Fail(fn, "refused", "no material is open (Editor.OpenMaterialEditor)");
        return *s;
    };
    ed.set_function("MaterialEditor", [matEdState](sol::this_state ts) -> sol::table { return matEdState(ts); });
    // A file, or {entity = id}: that entity's own material - its edits are its
    // MaterialOverride, steps of the scene's undo history.
    ed.set_function("OpenMaterialEditor", [this, live, matEdState](sol::this_state ts, sol::object arg) -> sol::table
    {
        static constexpr char kFn[] = "OpenMaterialEditor";
        std::string why;
        if (arg.is<std::string>())
        {
            if (!Editor::MaterialEditorWindow::Get().OpenNow(arg.as<std::string>(), why)) Fail(kFn, "refused", why);
        }
        else if (arg.is<sol::table>())
        {
            const sol::object ev = arg.as<sol::table>()["entity"];
            if (!ev.valid() || !ev.is<LuaEntity>()) Fail(kFn, "bad_value", "a table argument is {entity = id}");
            if (!m_world) Fail(kFn, "no_world", "the editor has no world");
            const Entity e = live(kFn, ev.as<LuaEntity>());
            if (!Editor::MaterialEditorWindow::Get().OpenEntityNow(*m_world, e, why)) Fail(kFn, "refused", why);
        }
        else
            Fail(kFn, "bad_value", "a material path, or {entity = id}");
        return matEdState(ts);
    });
    // An entity tab's buttons - "promote" ({path = .imatinst}), "apply", "clear".
    ed.set_function("MaterialEditorEntity", [this, guard, matEdState](sol::this_state ts, const std::string& action,
                                                                      sol::optional<sol::table> opts) -> sol::table
    {
        static constexpr char kFn[] = "MaterialEditorEntity";
        guard(kFn);
        EntityHandle h;
        if (!Editor::MaterialEditorWindow::Get().ActiveEntity(h))
            Fail(kFn, "refused", "the active tab is not an entity tab (Editor.OpenMaterialEditor{entity = id})");
        std::string why;
        const std::string path = opts ? opts->get_or<std::string>("path", "") : std::string();
        if (!MaterialEntityAction(h, action, path, why))
        {
            const auto colon = why.find(':');
            Fail(kFn, colon == std::string::npos ? "refused" : why.substr(0, colon).c_str(),
                 colon == std::string::npos ? why : why.substr(colon + 2));
        }
        return matEdState(ts);
    });
    ed.set_function("MaterialEditorSet", [matEdState, matEdSession](sol::this_state ts, const sol::table& opts) -> sol::table
    {
        static constexpr char kFn[] = "MaterialEditorSet";
        Editor::MaterialEditSession& s = matEdSession(kFn);
        auto num = [](double d) -> std::string
        {
            if (std::floor(d) == d && std::fabs(d) < 1e9) return std::to_string(static_cast<long long>(d));
            char b[48];
            std::snprintf(b, sizeof(b), "%.9g", d);
            return b;
        };
        std::vector<std::pair<std::string, std::string>> kv;
        for (const auto& [k, v] : opts)
        {
            if (!k.is<std::string>()) Fail(kFn, "bad_value", "keys are the .imat's (blendMode, roughnessMin, ...)");
            if (const auto* field = Resource::Authoring::FindField("Material", k.as<std::string>());
                field && v.get_type() == sol::type::boolean)
                Fail(kFn, "bad_value", k.as<std::string>() + " requires " + field->type);
            std::string value;
            switch (v.get_type())
            {
            case sol::type::boolean: value = v.as<bool>() ? "1" : "0"; break;
            case sol::type::number:  value = num(v.as<double>()); break;
            case sol::type::string:  value = v.as<std::string>(); break;
            case sol::type::table:
            {
                for (const auto& [ik, iv] : v.as<sol::table>())
                {
                    if (iv.get_type() != sol::type::number) Fail(kFn, "bad_value", k.as<std::string>() + ": numbers only");
                    value += (value.empty() ? "" : " ") + num(iv.as<double>());
                }
                break;
            }
            default: Fail(kFn, "bad_value", k.as<std::string>() + " has an unusable value");
            }
            kv.emplace_back(k.as<std::string>(), value);
        }
        // pairs() order is not the table's: the same keys apply in the same order.
        std::sort(kv.begin(), kv.end());
        std::string why;
        if (!s.Set(kv, why)) Fail(kFn, "bad_value", why);
        return matEdState(ts);
    });
    // Editor.MaterialEditorValues{"roughnessMin", "blendMode", ...} -> {roughnessMin = "0.250000", ...}:
    // the active tab's STAGED values as the .imat spells them (a key the format does not know: absent).
    ed.set_function("MaterialEditorValues", [matEdSession](sol::this_state ts, const sol::table& keys) -> sol::table
    {
        Editor::MaterialEditSession& s = matEdSession("MaterialEditorValues");
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        for (const auto& [i, k] : keys)
        {
            if (!k.is<std::string>()) Fail("MaterialEditorValues", "bad_value", "a list of keys");
            const std::string v = s.ValueOf(k.as<std::string>());
            if (!v.empty()) out[k.as<std::string>()] = v;
        }
        return out;
    });
    ed.set_function("MaterialEditorOverride", [matEdState, matEdSession](sol::this_state ts, const std::string& key,
                                                                    bool on) -> sol::table
    {
        std::string why;
        if (!matEdSession("MaterialEditorOverride").SetOverride(key, on, why)) Fail("MaterialEditorOverride", "refused", why);
        return matEdState(ts);
    });
    ed.set_function("MaterialEditorUndo", [matEdState, matEdSession](sol::this_state ts) -> sol::table
    {
        Editor::MaterialEditSession& s = matEdSession("MaterialEditorUndo");
        if (s.IsEntity())
            Fail("MaterialEditorUndo", "refused", "an entity tab's edits are steps of the scene's history: Editor.Undo()");
        if (!s.Undo()) Fail("MaterialEditorUndo", "refused", "nothing to undo");
        return matEdState(ts);
    });
    ed.set_function("MaterialEditorRedo", [matEdState, matEdSession](sol::this_state ts) -> sol::table
    {
        Editor::MaterialEditSession& s = matEdSession("MaterialEditorRedo");
        if (s.IsEntity())
            Fail("MaterialEditorRedo", "refused", "an entity tab's edits are steps of the scene's history: Editor.Redo()");
        if (!s.Redo()) Fail("MaterialEditorRedo", "refused", "nothing to redo");
        return matEdState(ts);
    });
    ed.set_function("MaterialEditorRevert", [matEdState, matEdSession](sol::this_state ts) -> sol::table
    {
        std::string why;
        matEdSession("MaterialEditorRevert");
        // The material and its graph.
        if (!Editor::MaterialEditorWindow::Get().RevertActive(why)) Fail("MaterialEditorRevert", "refused", why);
        return matEdState(ts);
    });
    ed.set_function("MaterialEditorResetOverrides", [matEdState, matEdSession](sol::this_state ts) -> sol::table
    {
        std::string why;
        if (!matEdSession("MaterialEditorResetOverrides").ResetOverrides(why))
            Fail("MaterialEditorResetOverrides", "refused", why);
        return matEdState(ts);
    });
    ed.set_function("MaterialEditorSave", [matEdSession](sol::optional<sol::table> opts) -> double
    {
        matEdSession("MaterialEditorSave");
        const std::string as = opts ? opts->get_or<std::string>("as", "") : std::string();
        const bool instance  = opts ? opts->get_or("instance", false) : false;
        if (instance && as.empty()) Fail("MaterialEditorSave", "bad_value", "{instance = true} needs {as = <path>.imatinst}");
        return static_cast<double>(RequiredJob(Editor::MaterialEditorWindow::Get().QueueSave(as, instance, Logger::CurrentRequestId())));
    });
    ed.set_function("MaterialEditorClose", [matEdState](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
    {
        auto& mw = Editor::MaterialEditorWindow::Get();
        const bool discard = opts ? opts->get_or("discard", false) : false;
        std::string why;
        bool ok = false;
        if (opts && opts->get_or("all", false)) ok = mw.CloseAll(discard, why);
        else ok = mw.CloseTab(opts ? opts->get_or("tab", 0) - 1 : -1, discard, why);
        if (!ok) Fail("MaterialEditorClose", why.find("unsaved") != std::string::npos ? "dirty" : "not_found", why);
        return matEdState(ts);
    });
    ed.set_function("MaterialEditorActivate", [matEdState](sol::this_state ts, int tab) -> sol::table
    {
        if (!Editor::MaterialEditorWindow::Get().Activate(tab - 1))
            Fail("MaterialEditorActivate", "not_found", "no tab " + std::to_string(tab));
        return matEdState(ts);
    });
    ed.set_function("MaterialEditorView", [matEdState](sol::this_state ts, const std::string& mode) -> sol::table
    {
        std::string why;
        if (!Editor::MaterialEditorWindow::Get().SetViewMode(mode, why)) Fail("MaterialEditorView", "bad_value", why);
        return matEdState(ts);
    });

    // ---- Material graph ----
    // The ACTIVE tab's graph - the model the canvas draws; every edit is a batch of ops
    // (one undo step of the graph's own ring), the same vocabulary the canvas speaks.
    //   Editor.MaterialGraph() -> {path, material, dirty, revision, undo, redo, output, live, preview?, hideUnrelated,
    //       nodes = {{id, type, label, pos = {x, y}, props, inputs}}, links = {{from, fromPin, to, toPin}},
    //       comments = {{id, rect, text, color}}, compile = {ok, errors, warnings, graphHash, hlslSha256}}
    //   Editor.MaterialGraphEdit(ops) -> {ids = {...}, revision}   ops: {op = "add", type = "Multiply",
    //       pos = {x, y}, props = {...}, inputs = {...}, as = "m"}, {op = "connect", from = id | "m", fromPin,
    //       to, toPin}, disconnect, set {node, props, inputs, unset, unsetInputs}, move, remove, comment,
    //       setcomment, uncomment (Editor/MaterialGraphDocument.h); all or nothing
    //   Editor.MaterialGraphCompile([{hlsl = true}]) -> the compiler's report (compiled, graphHash,
    //       hlslSha256, errors, warnings, diagnostics, stats, params, textures[, hlsl])
    //   Editor.MaterialGraphCompile{preview = {node = id[, pin]}} compiles what previewing that node would
    //   Editor.MaterialGraphApply() -> job                     the tab's Save: graph + generated HLSL + .imat
    //   Editor.MaterialGraphUndo() / MaterialGraphRedo() -> Editor.MaterialGraph()
    //   Editor.MaterialGraphCreate([{from = "standard" | "empty"}]) -> Editor.MaterialGraph()
    //   Editor.MaterialGraphCopy(ids) -> text                  Editor.MaterialGraphPaste(text[, {offset = {x, y}}]) -> {ids}
    //   Editor.MaterialGraphPreview([{node = id, pin = "Out"}]) -> {node, pin}   (no node: stop; the preview only)
    //   Editor.MaterialGraphFind(text) -> {matches = {ids}}
    auto graphOf = [](const char* fn) -> Editor::MaterialGraphDocument&
    {
        auto& mw = Editor::MaterialEditorWindow::Get();
        Editor::MaterialGraphDocument* g = mw.ActiveGraph();
        if (!g)
        {
            const std::string err = mw.ActiveGraphError();
            Fail(fn, "refused", !err.empty() ? "the active tab's graph did not open: " + err
                                              : std::string("the active tab has no graph (Editor.MaterialGraphCreate)"));
        }
        return *g;
    };
    auto graphState = [this, graphOf](sol::this_state ts, const char* fn) -> sol::object
    {
        auto& mw = Editor::MaterialEditorWindow::Get();
        Editor::MaterialGraphDocument& g = graphOf(fn);
        const MaterialGraph::CompileResult& r = g.Compiled(mw.ActiveGraphOptions());
        using Json = nlohmann::json;
        auto values = [](const MaterialGraph::ValueMap& m) {
            Json o = Json::object();
            for (const auto& [k, v] : m)
            {
                switch (v.kind)
                {
                    case MaterialGraph::Value::Kind::Number:  o[k] = v.num; break;
                    case MaterialGraph::Value::Kind::Bool:    o[k] = v.b; break;
                    case MaterialGraph::Value::Kind::String:  o[k] = v.str; break;
                    case MaterialGraph::Value::Kind::Numbers: o[k] = v.nums; break;
                    default: break;
                }
            }
            return o;
        };
        Json nodes = Json::array(), links = Json::array(), comments = Json::array();
        for (const MaterialGraph::Node& n : g.Graph().nodes)
        {
            Json e{ { "id", n.id }, { "type", n.type }, { "pos", { n.pos[0], n.pos[1] } }, { "props", values(n.props) },
                    { "inputs", values(n.inputs) } };
            if (const MaterialGraph::NodeDesc* d = MaterialGraph::DescribeNode(n)) e["label"] = d->label;
            // A texture node's thumbnail is resident (the canvas draws it).
            if (n.type == "TextureSample" || n.type == "TextureSampleParameter2D")
                e["thumbnail"] = mw.NodeThumbnailResident(n.PropString(n.type == "TextureSample" ? "texture" : "default"));
            nodes.push_back(std::move(e));
        }
        for (const MaterialGraph::Link& l : g.Graph().links)
            links.push_back(Json{ { "from", l.fromNode }, { "fromPin", l.fromPin }, { "to", l.toNode }, { "toPin", l.toPin } });
        for (const MaterialGraph::Comment& c : g.Graph().comments)
            comments.push_back(Json{ { "id", c.id }, { "rect", { c.rect[0], c.rect[1], c.rect[2], c.rect[3] } },
                                     { "text", c.text }, { "color", { c.color[0], c.color[1], c.color[2], c.color[3] } } });
        int errors = 0, warnings = 0;
        for (const MaterialGraph::Diagnostic& d : r.diagnostics)
        {
            if (d.severity == MaterialGraph::Severity::Error) ++errors;
            else if (d.severity == MaterialGraph::Severity::Warning) ++warnings;
        }
        const Editor::MaterialEditSession* s = mw.ActiveSession();
        const Editor::MaterialEditorWindow::GraphLiveView lv = mw.ActiveGraphLive();   // the graph's live compile
        Json live{ { "active", lv.active }, { "pending", lv.pending }, { "built", lv.built }, { "edit", lv.edit },
                   { "compiles", lv.compiles }, { "failures", lv.failures }, { "lastMs", lv.lastMs },
                   { "dxcErrors", lv.dxcErrors } };
        Json dxc = Json::array();   // DXC's errors, on their nodes (a Custom node's body)
        for (const auto& [node, message] : lv.dxc) dxc.push_back(Json{ { "node", node }, { "message", message } });
        live["dxc"] = std::move(dxc);
        // The shader pool's ratchet (what an edit's hot reload costs).
        if (m_renderer)
            live["shaderPool"] = { { "used", m_renderer->ShaderPoolUsed() }, { "capacity", m_renderer->ShaderPoolCapacity() } };
        Json j{ { "live", std::move(live) }, { "path", g.Path() }, { "material", s ? s->Path() : std::string() }, { "dirty", g.Dirty() },
                { "revision", g.Revision() }, { "undo", g.UndoDepth() }, { "redo", g.RedoDepth() },
                { "output", g.OutputId() }, { "nodes", std::move(nodes) }, { "links", std::move(links) },
                { "comments", std::move(comments) },
                { "compile", { { "ok", r.ok }, { "errors", errors }, { "warnings", warnings }, { "graphHash", r.graphHash },
                               { "hlslSha256", r.ok ? Sys::Sha256Hex(r.hlsl.data(), r.hlsl.size()) : std::string() } } } };
        // The previewed node, Hide Unrelated.
        const auto [previewNode, previewPin] = mw.GraphPreview();
        if (previewNode) j["preview"] = Json{ { "node", previewNode }, { "pin", previewPin } };
        j["hideUnrelated"] = mw.GraphHideUnrelated();
        sol::state_view view(ts);
        return JsonToLua(view, j);
    };
    // A refusal's "code: reason" (MaterialGraphDocument) -> Editor.<fn>: code: reason.
    auto graphFail = [](const char* fn, const std::string& why)
    {
        const std::size_t colon = why.find(": ");
        if (colon == std::string::npos) Fail(fn, "refused", why);
        Fail(fn, why.substr(0, colon).c_str(), why.substr(colon + 2));
    };
    ed.set_function("MaterialGraph", [graphState](sol::this_state ts) -> sol::object
    {
        return graphState(ts, "MaterialGraph");
    });
    ed.set_function("MaterialGraphEdit", [graphOf, graphFail](sol::this_state ts, const sol::object& ops) -> sol::table
    {
        static constexpr char kFn[] = "MaterialGraphEdit";
        Editor::MaterialGraphDocument& g = graphOf(kFn);
        if (ops.get_type() != sol::type::table) Fail(kFn, "bad_value", "a list of ops ({op = \"add\", ...}, ...)");
        std::vector<Editor::GraphOp> parsed;
        std::string why;
        if (!Editor::ParseGraphOps(LuaToJson(ops).dump(), parsed, why)) graphFail(kFn, why);
        std::vector<std::uint32_t> ids;
        if (!g.Apply(parsed, &ids, why)) graphFail(kFn, why);
        LOG_INFO("[MATGRAPH] edit %s: %zu op(s) new=%zu revision=%llu nodes=%zu links=%zu", g.Path().c_str(),
                 parsed.size(), ids.size(), static_cast<unsigned long long>(g.Revision()), g.Graph().nodes.size(),
                 g.Graph().links.size());
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        sol::table idt = lua.create_table();
        for (std::size_t i = 0; i < ids.size(); ++i) idt[i + 1] = ids[i];
        out["ids"]      = idt;
        out["revision"] = static_cast<double>(g.Revision());
        return out;
    });
    ed.set_function("MaterialGraphCompile", [graphOf](sol::this_state ts, sol::optional<sol::table> opts) -> sol::object
    {
        static constexpr char kFn[] = "MaterialGraphCompile";
        Editor::MaterialGraphDocument& g = graphOf(kFn);
        MaterialGraph::CompileOptions o = Editor::MaterialEditorWindow::Get().ActiveGraphOptions();
        if (opts)
            if (const sol::object pv = (*opts)["preview"]; pv.valid() && pv != sol::lua_nil)
            {
                // What previewing that node compiles (unlit, opaque) - without starting it.
                if (!pv.is<sol::table>()) Fail(kFn, "bad_value", "preview wants {node = id[, pin = \"Out\"]}");
                const sol::table t = pv.as<sol::table>();
                o.previewNode  = static_cast<std::uint32_t>(t.get_or("node", 0.0));
                o.previewPin   = t.get_or<std::string>("pin", "");
                o.shadingModel = ShadingModel::Unlit;
                o.blendMode    = BlendMode::Opaque;
            }
        const MaterialGraph::CompileResult& r = g.Compiled(o);
        const bool withHlsl = opts ? opts->get_or("hlsl", false) : false;
        sol::state_view view(ts);
        return JsonToLua(view, nlohmann::json::parse(MaterialGraph::CompileReportJson(r, withHlsl)));
    });
    ed.set_function("MaterialGraphApply", [graphOf]() -> double
    {
        graphOf("MaterialGraphApply");
        return static_cast<double>(RequiredJob(Editor::MaterialEditorWindow::Get().QueueSave({}, false, Logger::CurrentRequestId())));
    });
    ed.set_function("MaterialGraphUndo", [graphOf, graphState](sol::this_state ts) -> sol::object
    {
        if (!graphOf("MaterialGraphUndo").Undo()) Fail("MaterialGraphUndo", "refused", "nothing to undo");
        return graphState(ts, "MaterialGraphUndo");
    });
    ed.set_function("MaterialGraphRedo", [graphOf, graphState](sol::this_state ts) -> sol::object
    {
        if (!graphOf("MaterialGraphRedo").Redo()) Fail("MaterialGraphRedo", "refused", "nothing to redo");
        return graphState(ts, "MaterialGraphRedo");
    });
    ed.set_function("MaterialGraphCreate", [graphState](sol::this_state ts, sol::optional<sol::table> opts) -> sol::object
    {
        const std::string from = opts ? opts->get_or<std::string>("from", "standard") : std::string("standard");
        std::string why;
        if (!Editor::MaterialEditorWindow::Get().CreateGraph(from, why)) Fail("MaterialGraphCreate", "refused", why);
        return graphState(ts, "MaterialGraphCreate");
    });
    // Editor.MaterialGraphView{fit = true, frame = true, select = {ids}, hideUnrelated = bool} ->
    // {selection = {ids}, hideUnrelated, related = {ids}}: the canvas (applied when it next draws;
    // select before frame frames those). related: what Hide Unrelated keeps for the selection.
    ed.set_function("MaterialGraphView", [graphOf](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
    {
        static constexpr char kFn[] = "MaterialGraphView";
        graphOf(kFn);
        std::vector<std::uint32_t> ids;
        bool select = false;
        if (opts)
            if (const sol::object s = (*opts)["select"]; s.valid() && s != sol::lua_nil)
            {
                if (!s.is<sol::table>()) Fail(kFn, "bad_value", "select wants a list of node ids");
                for (const auto& [k, v] : s.as<sol::table>())
                {
                    if (v.get_type() != sol::type::number) Fail(kFn, "bad_value", "select wants node ids");
                    ids.push_back(static_cast<std::uint32_t>(v.as<double>()));
                }
                select = true;
            }
        auto& mw = Editor::MaterialEditorWindow::Get();
        mw.GraphView(opts ? opts->get_or("fit", false) : false, opts ? opts->get_or("frame", false) : false,
                     select ? &ids : nullptr);
        if (opts)
            if (const sol::object h = (*opts)["hideUnrelated"]; h.valid() && h != sol::lua_nil)
            {
                if (h.get_type() != sol::type::boolean) Fail(kFn, "bad_value", "hideUnrelated wants true or false");
                mw.SetGraphHideUnrelated(h.as<bool>());
            }
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        sol::table sel = lua.create_table();
        const std::vector<std::uint32_t> cur = mw.GraphSelection();
        for (std::size_t i = 0; i < cur.size(); ++i) sel[i + 1] = cur[i];
        out["selection"]     = sel;
        out["hideUnrelated"] = mw.GraphHideUnrelated();
        sol::table rel = lua.create_table();
        const std::vector<std::uint32_t> related = graphOf(kFn).Related(cur);
        for (std::size_t i = 0; i < related.size(); ++i) rel[i + 1] = related[i];
        out["related"] = rel;
        return out;
    });
    ed.set_function("MaterialGraphPreview", [graphOf](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
    {
        static constexpr char kFn[] = "MaterialGraphPreview";
        graphOf(kFn);
        const std::uint32_t node = opts ? static_cast<std::uint32_t>(opts->get_or("node", 0.0)) : 0u;
        const std::string pin    = opts ? opts->get_or<std::string>("pin", "") : std::string();
        std::string why;
        auto& mw = Editor::MaterialEditorWindow::Get();
        if (!mw.SetGraphPreview(node, pin, why))
        {
            const std::size_t colon = why.find(": ");
            if (colon == std::string::npos) Fail(kFn, "refused", why);
            Fail(kFn, why.substr(0, colon).c_str(), why.substr(colon + 2));
        }
        const auto [cur, curPin] = mw.GraphPreview();
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        out["node"] = cur;
        out["pin"]  = curPin;
        return out;
    });
    // A new Material Function (.imatfunc) in a tab of its own, written on Save.
    ed.set_function("MaterialFunctionCreate", [graphState](sol::this_state ts, const std::string& path) -> sol::object
    {
        std::string why;
        if (!Editor::MaterialEditorWindow::Get().CreateFunction(path, why)) Fail("MaterialFunctionCreate", "refused", why);
        return graphState(ts, "MaterialFunctionCreate");
    });
    ed.set_function("MaterialGraphFind", [graphOf](sol::this_state ts, const std::string& text) -> sol::table
    {
        const std::vector<std::uint32_t> ids = graphOf("MaterialGraphFind").Find(text);
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        sol::table m = lua.create_table();
        for (std::size_t i = 0; i < ids.size(); ++i) m[i + 1] = ids[i];
        out["matches"] = m;
        return out;
    });
    // Graph palette and parameters:
    //   Editor.MaterialGraphPalette{from = id, pin = "RGB"} | {to = id, pin = "A"} -> {{type, label, pin}}
    //       the node types a wire dragged from that output (from) or input (to) into empty space may
    //       become, each with the new node's pin it connects (the canvas's type-filtered palette)
    //   Editor.MaterialGraphParameters() -> {{name, type, nodes = {ids}, props = {...the defining node's}}}
    //   Editor.MaterialGraphSetParameter(name, props) -> Editor.MaterialGraphParameters()
    //       props on EVERY node of the name (one undo step); props.name renames (param_name_duplicate)
    ed.set_function("MaterialGraphPalette", [graphOf](sol::this_state ts, const sol::table& opt) -> sol::table
    {
        static constexpr char kFn[] = "MaterialGraphPalette";
        Editor::MaterialGraphDocument& g = graphOf(kFn);
        const sol::optional<std::uint32_t> from = opt["from"];
        const sol::optional<std::uint32_t> to = opt["to"];
        const std::string pin = opt.get_or<std::string>("pin", "");
        if (from.has_value() == to.has_value())
            Fail(kFn, "bad_value", "{from = <node>, pin = <an output>} or {to = <node>, pin = <an input>}");
        const std::uint32_t id = from ? *from : *to;
        const MaterialGraph::Node* n = g.Graph().FindNode(id);
        const MaterialGraph::NodeDesc* d = n ? MaterialGraph::DescribeNode(*n) : nullptr;
        if (!d) Fail(kFn, "unknown_node", "no node #" + std::to_string(id));
        const MaterialGraph::PinDesc* p = from ? d->Output(pin) : d->Input(pin);
        if (!p) Fail(kFn, "unknown_pin", "#" + std::to_string(id) + " has no " + (from ? "output" : "input") + " '" + pin + "'");
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        int i = 0;
        for (const Editor::MaterialGraphDocument::PaletteEntry& e :
             Editor::MaterialGraphDocument::CompatibleNodes(p->components, from.has_value()))
        {
            sol::table r = lua.create_table();
            r["type"]  = e.desc->type;
            r["label"] = e.desc->label;
            r["pin"]   = e.pin;
            out[++i]   = r;
        }
        return out;
    });
    auto paramsState = [graphOf](sol::this_state ts, const char* fn) -> sol::object
    {
        Editor::MaterialGraphDocument& g = graphOf(fn);
        using Json = nlohmann::json;
        Json rows = Json::array();
        for (const Editor::MaterialGraphDocument::ParameterRow& r : g.Parameters())
        {
            Json props = Json::object();
            for (const auto& [k, v] : r.first->props)
            {
                switch (v.kind)
                {
                    case MaterialGraph::Value::Kind::Number:  props[k] = v.num; break;
                    case MaterialGraph::Value::Kind::Bool:    props[k] = v.b; break;
                    case MaterialGraph::Value::Kind::String:  props[k] = v.str; break;
                    case MaterialGraph::Value::Kind::Numbers: props[k] = v.nums; break;
                    default: break;
                }
            }
            rows.push_back(Json{ { "name", r.name }, { "type", r.type }, { "nodes", r.nodes }, { "props", std::move(props) } });
        }
        sol::state_view view(ts);
        return JsonToLua(view, rows);
    };
    ed.set_function("MaterialGraphParameters", [paramsState](sol::this_state ts) -> sol::object
    {
        return paramsState(ts, "MaterialGraphParameters");
    });
    ed.set_function("MaterialGraphSetParameter", [graphOf, graphFail, paramsState](sol::this_state ts, const std::string& name,
                                                                                 const sol::object& props) -> sol::object
    {
        static constexpr char kFn[] = "MaterialGraphSetParameter";
        Editor::MaterialGraphDocument& g = graphOf(kFn);
        if (props.get_type() != sol::type::table) Fail(kFn, "bad_value", "props: a table ({default = 0.5, group = \"Look\", ...})");
        // The op parser's values (numbers, bools, strings, number arrays) - one set op's props.
        nlohmann::json op{ { "op", "set" }, { "node", 1 }, { "props", LuaToJson(props) } };
        std::vector<Editor::GraphOp> parsed;
        std::string why;
        if (!Editor::ParseGraphOps(nlohmann::json::array({ op }).dump(), parsed, why) || parsed.empty()) graphFail(kFn, why);
        if (!g.SetParameter(name, parsed.front().props, why)) graphFail(kFn, why);
        return paramsState(ts, kFn);
    });
    ed.set_function("MaterialGraphCopy", [graphOf](const sol::table& ids) -> std::string
    {
        Editor::MaterialGraphDocument& g = graphOf("MaterialGraphCopy");
        std::vector<std::uint32_t> v;
        for (const auto& [k, id] : ids)
        {
            if (id.get_type() != sol::type::number) Fail("MaterialGraphCopy", "bad_value", "a list of node ids");
            v.push_back(static_cast<std::uint32_t>(id.as<double>()));
        }
        return g.CopyText(v);
    });
    ed.set_function("MaterialGraphPaste", [graphOf, graphFail](sol::this_state ts, const std::string& text,
                                                               sol::optional<sol::table> opts) -> sol::table
    {
        static constexpr char kFn[] = "MaterialGraphPaste";
        Editor::MaterialGraphDocument& g = graphOf(kFn);
        float offset[2] = { 40.0f, 40.0f };
        if (opts)
            if (const sol::object o = (*opts)["offset"]; o.valid() && o != sol::lua_nil && !ReadNumbers(o, offset, 2))
                Fail(kFn, "bad_value", "offset wants {x, y}");
        std::vector<std::uint32_t> ids;
        std::string why;
        if (!g.Paste(text, offset, &ids, why)) graphFail(kFn, why);
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        sol::table idt = lua.create_table();
        for (std::size_t i = 0; i < ids.size(); ++i) idt[i + 1] = ids[i];
        out["ids"] = idt;
        return out;
    });

    // ---- Mesh Editor: shape keys ----
    // The picture's weights: never staged, never saved (a preview of the model's .imorph).
    //   Editor.MeshEditorShapeKeys() -> {file, normals, sections, solo, heatmap, revision, active,
    //       live, targets = {{name, weight, sections, maxDelta}}}   (solo / heatmap: a name, or false)
    //   Editor.MeshEditorShapeKeySet{weights = {name = w}, reset, solo = name|false, heatmap = name|false}
    //       -> the same table; weights clamp to 0..1; reset = every weight to its DEFAULT (MK)
    // The MK / MP records are staged like the slots (the same undo ring, the same Save):
    //   each target also reads {category, default, min, max, label, source}; the table has presets =
    //   {{name, weights = {name = w}, source}}, lock ("" = editable) and dirty.
    //   Editor.MeshEditorShapeKeyMeta{name, category, default, min, max, label} -> the table
    //       (fields left out keep their value; all at their defaults removes the record)
    //   Editor.MeshEditorShapeKeyPreset{save = name | delete = name | apply = name} -> the table
    //       (save = the counting weights the picture shows; apply sets the picture's weights)
    auto skState = [this](sol::this_state ts) -> sol::table
    {
        const Editor::MeshEditorWindow::StateView s = Editor::MeshEditorWindow::Get().State(m_renderer);
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        t["file"]     = s.shapeKeyFile;
        t["normals"]  = s.shapeKeyNormals;
        t["sections"] = s.shapeKeySections;
        auto nameOf = [&](int i) -> sol::object
        {
            if (i < 0 || i >= static_cast<int>(s.shapeKeys.size())) return sol::make_object(lua, false);
            return sol::make_object(lua, s.shapeKeys[static_cast<std::size_t>(i)].name);
        };
        t["solo"]     = nameOf(s.shapeKeySolo);
        t["heatmap"]  = nameOf(s.shapeKeyHeatmap);
        t["revision"] = static_cast<double>(s.shapeKeyRevision);
        t["active"]   = s.shapeKeyActive;
        t["live"]     = s.shapeKeyLive;
        sol::table targets = lua.create_table();
        for (std::size_t i = 0; i < s.shapeKeys.size(); ++i)
        {
            const auto& k = s.shapeKeys[i];
            sol::table r = lua.create_table_with("name", k.name, "weight", k.weight, "sections", k.sections,
                                                 "maxDelta", k.maxDelta);
            r["category"] = k.meta.category;
            r["default"]  = k.meta.def;
            r["min"]      = k.meta.min;
            r["max"]      = k.meta.max;
            r["label"]    = k.meta.label;
            r["source"]   = k.meta.source;   // "" = the author's (or none), else src=
            targets[static_cast<int>(i) + 1] = r;
        }
        t["targets"] = targets;
        sol::table presets = lua.create_table();
        for (std::size_t i = 0; i < s.shapeKeyPresets.size(); ++i)
        {
            const auto& pr = s.shapeKeyPresets[i];
            sol::table w = lua.create_table();
            for (const auto& [n, v] : pr.weights) w[n] = v;
            presets[static_cast<int>(i) + 1] = lua.create_table_with("name", pr.name, "weights", w,
                                                                     "source", pr.source);
        }
        t["presets"] = presets;
        t["lock"]    = s.shapeKeyLock;
        t["dirty"]   = s.shapeKeysDirty;
        return t;
    };
    ed.set_function("MeshEditorShapeKeys", [skState](sol::this_state ts) -> sol::table { return skState(ts); });
    ed.set_function("MeshEditorShapeKeySet", [skState](sol::this_state ts, const sol::table& o) -> sol::table
    {
        static constexpr char kFn[] = "MeshEditorShapeKeySet";
        auto& me = Editor::MeshEditorWindow::Get();
        if (!me.State(nullptr).loaded) Fail(kFn, "bad_value", "no mesh is open (Editor.OpenMeshEditor)");
        Editor::MeshShapeKeys& sk = me.ShapeKeys();
        auto target = [&](const sol::object& v, const char* what) -> int
        {
            if (v.is<bool>() && !v.as<bool>()) return -1;
            if (!v.is<std::string>()) Fail(kFn, "bad_value", std::string(what) + " is a shape key's name, or false");
            const int i = sk.Find(v.as<std::string>());
            if (i < 0) Fail(kFn, "bad_value", "the Mesh has no shape key '" + v.as<std::string>() + "'");
            return i;
        };
        if (o.get_or("reset", false)) me.ResetShapeKeyWeights();
        if (const sol::object v = o["weights"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<sol::table>()) Fail(kFn, "bad_value", "weights is {name = weight, ...}");
            for (const auto& [k, w] : v.as<sol::table>())
            {
                if (!k.is<std::string>() || !w.is<double>())
                    Fail(kFn, "bad_value", "weights is {name = weight, ...}");
                const int i = sk.Find(k.as<std::string>());
                if (i < 0) Fail(kFn, "bad_value", "the Mesh has no shape key '" + k.as<std::string>() + "'");
                if (!sk.SetWeight(static_cast<std::size_t>(i), static_cast<float>(w.as<double>())))
                    Fail(kFn, "bad_value", "a weight is a finite number");
            }
        }
        if (const sol::object v = o["solo"]; v.valid() && v != sol::lua_nil) sk.SetSolo(target(v, "solo"));
        if (const sol::object v = o["heatmap"]; v.valid() && v != sol::lua_nil) sk.SetHeatmap(target(v, "heatmap"));
        return skState(ts);
    });
    ed.set_function("MeshEditorShapeKeyMeta", [skState](sol::this_state ts, const sol::table& o) -> sol::table
    {
        static constexpr char kFn[] = "MeshEditorShapeKeyMeta";
        auto& me = Editor::MeshEditorWindow::Get();
        if (!me.State(nullptr).loaded) Fail(kFn, "bad_value", "no mesh is open (Editor.OpenMeshEditor)");
        const sol::optional<std::string> name = o.get<sol::optional<std::string>>("name");
        if (!name) Fail(kFn, "bad_value", "name is the shape key's");
        Resource::ShapeKeyMeta m;
        if (const Resource::ShapeKeyMeta* cur = me.StagedShapeKeys().FindMeta(*name)) m = *cur;
        m.name = *name;
        if (const sol::object v = o["category"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<std::string>()) Fail(kFn, "bad_value", "category is a string");
            m.category = v.as<std::string>();
        }
        auto num = [&](const char* key, float& dst)
        {
            const sol::object v = o[key];
            if (!v.valid() || v == sol::lua_nil) return;
            if (!v.is<double>()) Fail(kFn, "bad_value", std::string(key) + " is a number");
            dst = static_cast<float>(v.as<double>());
        };
        num("default", m.def);
        num("min", m.min);
        num("max", m.max);
        if (const sol::object v = o["label"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<std::string>()) Fail(kFn, "bad_value", "label is a string");
            m.label = v.as<std::string>();
        }
        std::string why;
        if (!me.StageShapeKeyMeta(m, why))
            Fail(kFn, me.ShapeKeyLock().empty() ? "bad_value" : "unsupported", why);
        return skState(ts);
    });
    ed.set_function("MeshEditorShapeKeyPreset", [skState](sol::this_state ts, const sol::table& o) -> sol::table
    {
        static constexpr char kFn[] = "MeshEditorShapeKeyPreset";
        auto& me = Editor::MeshEditorWindow::Get();
        if (!me.State(nullptr).loaded) Fail(kFn, "bad_value", "no mesh is open (Editor.OpenMeshEditor)");
        std::string why;
        bool ok = true;
        if (const sol::optional<std::string> n = o.get<sol::optional<std::string>>("save"))        ok = me.SaveShapeKeyPreset(*n, why);
        else if (const sol::optional<std::string> d = o.get<sol::optional<std::string>>("delete")) ok = me.DeleteShapeKeyPreset(*d, why);
        else if (const sol::optional<std::string> a = o.get<sol::optional<std::string>>("apply"))  ok = me.ApplyShapeKeyPreset(*a, why);
        else Fail(kFn, "bad_value", "one of save / delete / apply = a preset name");
        if (!ok) Fail(kFn, me.ShapeKeyLock().empty() ? "bad_value" : "unsupported", why);
        return skState(ts);
    });

    // ---- Mesh Editor: asset collision ----
    // Staged like the slots (the same undo ring, the same Save). Prim indices are 0-based.
    //   Editor.MeshEditorCollision() -> {editable, lock, library, dirty, selected, overlayLines,
    //       complexity, layer, trigger, friction?, restitution?, prims = {...}, text, savedText, saved,
    //       gizmo, gizmoGestures}
    //   Editor.MeshEditorCollisionAdd(kind, {section = -1|i, prim = i}) -> state + {added}
    //       kind: box sphere capsule kdop10x kdop10y kdop10z kdop18 kdop26 convex, or copy {prim}
    //   Editor.MeshEditorCollisionSet{complexity, layer, trigger, friction, restitution, select, gizmo,
    //       prim = i, center = {x,y,z}, rotation = {x,y,z,w} | euler = {x,y,z}, halfExtents, radius,
    //       halfHeight, name} -> state (one undo step)
    //   Editor.MeshEditorCollisionRemove(i | -1) -> state
    //   Editor.MeshEditorCollisionGizmo{prim, op = "translate"|"rotate"|"scale", v = {x,y,z}, deg} -> state
    auto complexityName = [](Resource::MeshBodySetup::Complexity c)
    {
        return c == Resource::MeshBodySetup::Complexity::Complex ? "complex"
             : c == Resource::MeshBodySetup::Complexity::None    ? "none" : "simple";
    };
    auto collState = [complexityName](sol::this_state ts) -> sol::table
    {
        const Editor::MeshEditorWindow::CollisionState s = Editor::MeshEditorWindow::Get().Collision();
        sol::state_view lua(ts);
        auto v3 = [&](const DirectX::XMFLOAT3& v) { return lua.create_table_with(1, v.x, 2, v.y, 3, v.z); };
        sol::table t = lua.create_table();
        t["editable"]     = s.lock.empty();
        t["lock"]         = s.lock;
        t["library"]      = s.library;
        t["dirty"]        = s.dirty;
        t["selected"]     = s.selected;
        t["overlayLines"] = s.overlayLines;
        t["complexity"]   = std::string(complexityName(s.staged.complexity));
        t["layer"]        = static_cast<int>(s.staged.layer);
        t["trigger"]      = s.staged.trigger;
        if (s.staged.friction >= 0.f) t["friction"] = s.staged.friction;
        if (s.staged.restitution >= 0.f) t["restitution"] = s.staged.restitution;
        sol::table prims = lua.create_table();
        for (std::size_t i = 0; i < s.staged.prims.size(); ++i)
        {
            const Resource::CollisionPrim& p = s.staged.prims[i];
            sol::table r = lua.create_table();
            static const char* kTypes[] = { "box", "sphere", "capsule", "hull", "mesh" };
            r["index"]    = static_cast<int>(i);
            r["type"]     = std::string(kTypes[static_cast<int>(p.type)]);
            r["name"]     = p.name;
            r["inBody"]   = p.InBody();
            r["center"]   = v3(p.center);
            r["rotation"] = lua.create_table_with(1, p.rotation.x, 2, p.rotation.y, 3, p.rotation.z, 4, p.rotation.w);
            if (p.type == Resource::CollisionPrim::Type::Box) r["halfExtents"] = v3(p.halfExtents);
            if (p.type == Resource::CollisionPrim::Type::Sphere || p.type == Resource::CollisionPrim::Type::Capsule)
                r["radius"] = p.radius;
            if (p.type == Resource::CollisionPrim::Type::Capsule) r["halfHeight"] = p.halfHeight;
            if (p.type == Resource::CollisionPrim::Type::Hull || p.type == Resource::CollisionPrim::Type::Mesh)
            {
                r["lib"] = p.lib;
                r["sec"] = p.sectionKey ? Resource::MeshKeyToHex(p.sectionKey) : std::string();
            }
            r["pending"] = i < s.pending.size() ? s.pending[i] : 0u;   // points waiting for Save
            r["shown"]   = i < s.shown.size() ? s.shown[i] : 0u;       // points / vertices the overlay has
            prims[static_cast<int>(i) + 1] = r;
        }
        t["prims"]     = prims;
        t["text"]      = Resource::FormatBodySetup(s.staged);
        t["savedText"] = Resource::FormatBodySetup(s.saved);
        t["saved"]     = lua.create_table_with("complexity", std::string(complexityName(s.saved.complexity)),
                                               "prims", static_cast<int>(s.saved.prims.size()));
        // The gizmo's operation and how many drags it (or MeshEditorCollisionGizmo) applied.
        t["gizmo"]         = std::string(Editor::MeshEditorWindow::GizmoOpName(Editor::MeshEditorWindow::Get().GetGizmoOp()));
        t["gizmoGestures"] = Editor::MeshEditorWindow::Get().GizmoGestures();
        return t;
    };
    auto collFailCode = []() -> const char*
    {
        const std::string lock = Editor::MeshEditorWindow::Get().CollisionLock();
        return lock.empty() || lock == "no mesh is open" ? "bad_value" : "unsupported";
    };
    ed.set_function("MeshEditorCollision", [collState](sol::this_state ts) -> sol::table { return collState(ts); });
    ed.set_function("MeshEditorCollisionAdd", [collState, collFailCode](sol::this_state ts, const std::string& kind,
                                                                        sol::optional<sol::table> optsIn) -> sol::table
    {
        const char* fn = "MeshEditorCollisionAdd";
        auto& me = Editor::MeshEditorWindow::Get();
        if (!me.State(nullptr).loaded) Fail(fn, "bad_value", "no mesh is open (Editor.OpenMeshEditor)");
        std::string why;
        int added = -1;
        if (kind == "copy")
        {
            const int prim = optsIn ? (*optsIn).get_or("prim", -1) : -1;
            if (!me.DuplicateCollision(prim, why, &added)) Fail(fn, collFailCode(), why);
        }
        else
        {
            Editor::MeshEditorWindow::CollisionAdd k{};
            if (!Editor::MeshEditorWindow::ParseCollisionAdd(kind, k))
                Fail(fn, "bad_value", "kind is box, sphere, capsule, kdop10x, kdop10y, kdop10z, kdop18, kdop26, convex or copy");
            const int section = optsIn ? (*optsIn).get_or("section", -1) : -1;
            if (!me.AddCollision(k, section, why, &added)) Fail(fn, collFailCode(), why);
        }
        sol::table t = collState(ts);
        t["added"] = added;
        return t;
    });
    ed.set_function("MeshEditorCollisionSet", [collState, collFailCode](sol::this_state ts, const sol::table& o) -> sol::table
    {
        const char* fn = "MeshEditorCollisionSet";
        auto& me = Editor::MeshEditorWindow::Get();
        if (!me.State(nullptr).loaded) Fail(fn, "bad_value", "no mesh is open (Editor.OpenMeshEditor)");
        Resource::MeshBodySetup next = me.Collision().staged;
        bool edited = false;
        if (const sol::object v = o["complexity"]; v.valid() && v != sol::lua_nil)
        {
            const std::string c = v.is<std::string>() ? v.as<std::string>() : std::string();
            if      (c == "simple")  next.complexity = Resource::MeshBodySetup::Complexity::Simple;
            else if (c == "complex") next.complexity = Resource::MeshBodySetup::Complexity::Complex;
            else if (c == "none")    next.complexity = Resource::MeshBodySetup::Complexity::None;
            else Fail(fn, "bad_value", "complexity is simple, complex or none");
            edited = true;
        }
        if (const sol::object v = o["layer"]; v.valid() && v != sol::lua_nil)
        {
            if (!v.is<int>() || v.as<int>() < 0 || v.as<int>() > 8) Fail(fn, "bad_value", "layer is 0..8");
            next.layer = static_cast<std::uint8_t>(v.as<int>());
            edited = true;
        }
        if (const sol::object v = o["trigger"]; v.valid() && v != sol::lua_nil)
        {
            next.trigger = v.as<bool>();
            edited = true;
        }
        for (const char* key : { "friction", "restitution" })
            if (const sol::object v = o[key]; v.valid() && v != sol::lua_nil)
            {
                if (!v.is<double>()) Fail(fn, "bad_value", std::string(key) + " is a number (negative = the RigidBody's)");
                (key[0] == 'f' ? next.friction : next.restitution) = static_cast<float>(v.as<double>());
                edited = true;
            }
        if (const sol::object pv = o["prim"]; pv.valid() && pv != sol::lua_nil)
        {
            const int i = pv.is<int>() ? pv.as<int>() : -1;
            if (i < 0 || i >= static_cast<int>(next.prims.size()))
                Fail(fn, "bad_value", "prim is an index 0.." + std::to_string(static_cast<int>(next.prims.size()) - 1));
            Resource::CollisionPrim& p = next.prims[static_cast<std::size_t>(i)];
            if (const sol::object v = o["center"]; v.valid() && v != sol::lua_nil)
                if (!ReadNumbers(v, &p.center.x, 3)) Fail(fn, "bad_value", "center wants {x, y, z}");
            if (const sol::object v = o["rotation"]; v.valid() && v != sol::lua_nil)
                if (!ReadNumbers(v, &p.rotation.x, 4)) Fail(fn, "bad_value", "rotation wants a quaternion {x, y, z, w}");
            if (const sol::object v = o["euler"]; v.valid() && v != sol::lua_nil)
            {
                float e[3];
                if (!ReadNumbers(v, e, 3)) Fail(fn, "bad_value", "euler wants degrees {x, y, z}");
                DirectX::XMStoreFloat4(&p.rotation, DirectX::XMQuaternionRotationRollPitchYaw(
                    DirectX::XMConvertToRadians(e[0]), DirectX::XMConvertToRadians(e[1]), DirectX::XMConvertToRadians(e[2])));
            }
            if (const sol::object v = o["halfExtents"]; v.valid() && v != sol::lua_nil)
                if (!ReadNumbers(v, &p.halfExtents.x, 3)) Fail(fn, "bad_value", "halfExtents wants {x, y, z}");
            if (const sol::object v = o["radius"]; v.valid() && v != sol::lua_nil)
            {
                if (!v.is<double>()) Fail(fn, "bad_value", "radius is a number");
                p.radius = static_cast<float>(v.as<double>());
            }
            if (const sol::object v = o["halfHeight"]; v.valid() && v != sol::lua_nil)
            {
                if (!v.is<double>()) Fail(fn, "bad_value", "halfHeight is a number");
                p.halfHeight = static_cast<float>(v.as<double>());
            }
            if (const sol::object v = o["name"]; v.valid() && v != sol::lua_nil) p.name = v.as<std::string>();
            edited = true;
        }
        if (edited)
        {
            std::string why;
            if (!me.StageCollision(next, why)) Fail(fn, collFailCode(), why);
        }
        if (const sol::object v = o["select"]; v.valid() && v != sol::lua_nil)
            if (!v.is<int>() || !me.SelectCollision(v.as<int>())) Fail(fn, "bad_value", "select is a prim index (-1 = none)");
        if (const sol::object v = o["gizmo"]; v.valid() && v != sol::lua_nil)
        {
            Editor::MeshEditorWindow::GizmoOp op{};
            if (!v.is<std::string>() || !Editor::MeshEditorWindow::ParseGizmoOp(v.as<std::string>(), op))
                Fail(fn, "bad_value", "gizmo is off, translate, rotate or scale");
            me.SetGizmoOp(op);
        }
        return collState(ts);
    });
    // One gizmo drag, as the panel's gizmo applies it (the same display-space matrix and
    // write-back; one undo step): translate by v (the Mesh's units), rotate by deg about the
    // prim's local axis v, scale by the factors v.
    ed.set_function("MeshEditorCollisionGizmo", [collState, collFailCode](sol::this_state ts, const sol::table& o) -> sol::table
    {
        const char* fn = "MeshEditorCollisionGizmo";
        auto& me = Editor::MeshEditorWindow::Get();
        if (!me.State(nullptr).loaded) Fail(fn, "bad_value", "no mesh is open (Editor.OpenMeshEditor)");
        Editor::MeshEditorWindow::GizmoOp op{};
        const sol::object ov = o["op"];
        if (!ov.valid() || !ov.is<std::string>() || !Editor::MeshEditorWindow::ParseGizmoOp(ov.as<std::string>(), op))
            Fail(fn, "bad_value", "op is translate, rotate or scale");
        const int prim = o.get_or("prim", -1);
        DirectX::XMFLOAT3 v = { 0.f, 0.f, 0.f };
        if (const sol::object vv = o["v"]; !ReadNumbers(vv, &v.x, 3)) Fail(fn, "bad_value", "v wants {x, y, z}");
        const float deg = o.get_or("deg", 0.f);
        std::string why;
        if (!me.GizmoDrag(prim, op, v, deg, why)) Fail(fn, collFailCode(), why);
        return collState(ts);
    });
    ed.set_function("MeshEditorCollisionRemove", [collState, collFailCode](sol::this_state ts, int index) -> sol::table
    {
        std::string why;
        if (!Editor::MeshEditorWindow::Get().RemoveCollision(index, why)) Fail("MeshEditorCollisionRemove", collFailCode(), why);
        return collState(ts);
    });

    // ---- Edit Collider / Fit to Bounds / Create > Collision ----
    //   Editor.EditCollider(on, {op}) -> on    the viewport gizmo drives the selection's Collider
    //   Editor.ColliderGizmo{entity, op = "translate"|"rotate"|"scale", v = {x,y,z}, deg} -> gestures
    //                                           one drag as the gizmo applies it; one undo step
    //   Editor.FitCollider(entity) -> true      box / sphere / capsule to the entity's mesh bounds; one undo step
    //   Editor.CreateCollision(kind, {parent}) -> entity   box / sphere / capsule; one undo step
    ed.set_function("EditCollider", [this](bool on, sol::optional<sol::table> optsIn) -> bool
    {
        SetEditCollider(on);
        // {op}: the viewport gizmo's operation (W / E / R) - never mid-drag, which
        // would strand ImGuizmo's mbUsing.
        if (optsIn)
            if (const sol::object ov = (*optsIn)["op"]; ov.valid() && ov != sol::lua_nil)
            {
                const std::string op = ov.is<std::string>() ? ov.as<std::string>() : std::string();
                if (ImGuizmo::IsUsingAny()) Fail("EditCollider", "busy", "a gizmo drag is held");
                if      (op == "translate") m_gizmoOperation = ImGuizmo::TRANSLATE;
                else if (op == "rotate")    m_gizmoOperation = ImGuizmo::ROTATE;
                else if (op == "scale")     m_gizmoOperation = ImGuizmo::SCALE;
                else Fail("EditCollider", "bad_value", "op is translate, rotate or scale");
            }
        return EditColliderEnabled();
    });
    ed.set_function("ColliderGizmo", [this, guard, live](const sol::table& o) -> std::uint32_t
    {
        const char* fn = "ColliderGizmo";
        guard(fn);
        const sol::object ev = o["entity"];
        if (!ev.valid() || !ev.is<LuaEntity>()) Fail(fn, "bad_value", "entity is an entity id");
        const Entity e = live(fn, ev.as<LuaEntity>());
        const std::string ops = o.get_or<std::string>("op", "");
        ColliderGizmoOp op{};
        if (ops == "translate") op = ColliderGizmoOp::Translate;
        else if (ops == "rotate") op = ColliderGizmoOp::Rotate;
        else if (ops == "scale") op = ColliderGizmoOp::Scale;
        else Fail(fn, "bad_value", "op is translate, rotate or scale");
        DirectX::XMFLOAT3 v = { 0.f, 0.f, 0.f };
        if (const sol::object vv = o["v"]; !ReadNumbers(vv, &v.x, 3)) Fail(fn, "bad_value", "v wants {x, y, z}");
        std::string why;
        if (!ColliderGizmoDrag(e, op, v, o.get_or("deg", 0.f), why)) Fail(fn, "bad_value", why);
        return ColliderGizmoGestures();
    });
    ed.set_function("FitCollider", [this, guard, live](const LuaEntity& id) -> bool
    {
        const char* fn = "FitCollider";
        guard(fn);
        const Entity e = live(fn, id);
        ColliderComponent* c = m_world->GetComponent<ColliderComponent>(e);
        if (!c) Fail(fn, "bad_value", "the entity has no Collider");
        const std::type_index type(typeid(ColliderComponent));
        const std::string before = Editor::Undo::SerializeComponentBlob(GetComponentRegistry(), *m_world, e, type);
        std::string why;
        if (!FitColliderToBounds(e, *c, why)) Fail(fn, "bad_value", why);
        const std::string after = Editor::Undo::SerializeComponentBlob(GetComponentRegistry(), *m_world, e, type);
        if (!before.empty() && after != before)
            PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(m_world->MakeHandle(e), type, before, after,
                                                                         "Collider"));
        return true;
    });
    ed.set_function("CreateCollision", [this, guard, live, out](const std::string& kind,
                                                                  sol::optional<sol::table> optsIn) -> LuaEntity
    {
        const char* fn = "CreateCollision";
        guard(fn);
        ColliderComponent::Shape shape{};
        if (kind == "box") shape = ColliderComponent::Shape::Box;
        else if (kind == "sphere") shape = ColliderComponent::Shape::Sphere;
        else if (kind == "capsule") shape = ColliderComponent::Shape::Capsule;
        else Fail(fn, "bad_value", "kind is box, sphere or capsule");
        Entity parent = NullEntity;
        if (optsIn)
            if (const sol::object pv = (*optsIn)["parent"]; pv.valid() && pv != sol::lua_nil)
            {
                if (!pv.is<LuaEntity>()) Fail(fn, "bad_value", "parent is an entity id");
                parent = live(fn, pv.as<LuaEntity>(), "the parent");
            }
        Entity made = NullEntity;
        RecordSpawn(kind == "box" ? "Box Collision" : kind == "sphere" ? "Sphere Collision" : "Capsule Collision",
                    [&] { made = CreateCollisionEntity(shape, parent); });
        if (made == NullEntity) Fail(fn, "bad_value", "nothing was created");
        return out(made);
    });

    // ---- Browse ----
    // What the Content Browser shows: its folder, its view, which models are expanded into
    // their Meshes - so a script can put a listing on screen (and capture the panel). Not an
    // edit; no undo entry.
    //   Editor.Browse(dir, {view = "tiles"|"list"|"details", expand = {<model .iscn>, ...}})
    // `expand` ADDS to the expanded set; {collapse = true} clears it first.
    ed.set_function("Browse", [this, guard](const std::string& dir, sol::optional<sol::table> optsIn) -> std::string
    {
        guard("Browse");
        std::error_code ec;
        if (!std::filesystem::is_directory(Sys::U8Path(dir), ec))
            Fail("Browse", "bad_value", "no such folder: " + dir);
        if (optsIn)
        {
            const sol::table o = *optsIn;
            if (const sol::object v = o["view"]; v.valid() && v != sol::lua_nil)
            {
                const std::string view = v.is<std::string>() ? v.as<std::string>() : std::string();
                if      (view == "tiles")   m_assetViewMode = AssetViewMode::Tiles;
                else if (view == "list")    m_assetViewMode = AssetViewMode::List;
                else if (view == "details") m_assetViewMode = AssetViewMode::Details;
                else Fail("Browse", "bad_value", "view is tiles, list or details");
            }
            if (o.get_or("collapse", false)) m_expandedModels.clear();
            if (const sol::object ex = o["expand"]; ex.valid() && ex != sol::lua_nil)
            {
                if (!ex.is<sol::table>()) Fail("Browse", "bad_value", "expand wants a list of model (.iscn) paths");
                for (const auto& [k, v] : ex.as<sol::table>())
                {
                    (void)k;
                    if (!v.is<std::string>()) Fail("Browse", "bad_value", "expand wants paths");
                    m_expandedModels.insert(Sys::RelativizeAssetPath(v.as<std::string>()));
                }
            }
        }
        SetCurrentAssetDir(dir);
        m_showContentBrowser = true;
        return dir;
    });
    // What a DOUBLE-CLICK on an item of the Content Browser's listing does - through the
    // handler the item itself runs: a skeleton (.iskel) opens in the Skeleton Editor, a
    // material in the Material Editor (the Inspector instead with the
    // inspector.legacyMaterial preference on), a VFX / sequence in its editor, a .meshlib /
    // .ipfb in the Asset Preview, a model shows / hides its Meshes. The item has to be
    // LISTED: Browse to its folder first (the listing is read the next frame the panel draws).
    //   Editor.ActivateAsset(path) -> true
    ed.set_function("ActivateAsset", [this](const std::string& path) -> bool
    {
        auto key = [](const std::string& p)
        { return Resource::AssetRefs::NormalizeRefPath(Sys::RelativizeAssetPath(p)); };
        const std::string want = key(path);
        for (int i = 0; i < static_cast<int>(m_assets.size()); ++i)
            if (key(m_assets[i].path) == want)
            {
                HandleAssetItemActivate(i, /*clicked=*/false, /*doubleClicked=*/true);
                return true;
            }
        Fail("ActivateAsset", "not_found", "'" + path + "' is not in the Content Browser's listing ('" + m_currentDir +
                                               "') - Editor.Browse to its folder first");
        return false;
    });

    // ---- CreatePrimitive ----
    // The Create > 3D Object meshes; they save as themselves (unlike a prefab instance's
    // parts).
    ed.set_function("CreatePrimitive", [this, guard, out, placement, place](const std::string& kindIn,
                                                                            sol::optional<sol::table> optsIn) -> LuaEntity
    {
        guard("CreatePrimitive");
        static constexpr const char* kKinds[] = { "cube", "sphere", "cone", "plane", "torus" };   // PrimitiveMeshType
        std::string kind = kindIn;
        for (char& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        int type = -1;
        for (int i = 0; i < 5; ++i)
            if (kind == kKinds[i]) type = i;
        if (type < 0) Fail("CreatePrimitive", "bad_value", "'" + kindIn + "' - one of cube, sphere, cone, plane, torus");
        const Placement pl = placement("CreatePrimitive", optsIn);
        // {staticMesh = true}: what Create > 3D Object makes - a Static Mesh of
        // engine/meshes/Primitives.iscn. Without it, the MeshHandle primitive: an entity that
        // carries its own MaterialComponent, which scripts that read the material OFF the
        // created entity (Engine.GetMaterial(id), Render.explain(id)) rely on - a Static Mesh
        // keeps its materials on its derived sections.
        const bool asStatic = optsIn && optsIn->get_or("staticMesh", false);

        Entity e = NullEntity;
        const std::string label = "Create " + kind;
        RecordSpawn(label.c_str(), [&]
        {
            const DirectX::XMFLOAT3 at = pl.parent == NullEntity && !pl.xf.hasPos ? ComputeSpawnPosition()
                                                                                 : DirectX::XMFLOAT3{ 0.f, 0.f, 0.f };
            e = asStatic ? CreatePrimitiveStaticMesh(type, at) : MeshSpawner::Spawn(type, *m_world, at);
            if (e != NullEntity) place(e, pl);
        });
        if (e == NullEntity) Fail("CreatePrimitive", "load_failed", "the mesh did not spawn (see the log)");
        m_selectedEntity = e;
        return out(e);
    });

    // ---- DetachPrefab ----
    // What the inspector's Prefab Ref (x) does: the instance stops being a reference and
    // saves as plain scene data, parts and all. Undoable.
    ed.set_function("DetachPrefab", [this, guard, live](const LuaEntity& id) -> std::string
    {
        guard("DetachPrefab");
        const Entity e = live("DetachPrefab", id);
        const PrefabSourcePath* p = m_world->GetComponent<PrefabSourcePath>(e);
        if (!p || p->path.empty())
            Fail("DetachPrefab", "bad_value", "the entity is not a prefab instance's root (it has no Prefab Ref)");
        const PrefabSourcePath was = *p;
        m_world->RemoveComponent<PrefabSourcePath>(e);
        if (UndoEnabled())
            PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(
                m_world->MakeHandle(e),
                [was](World& w, Entity ent)
                {
                    if (PrefabSourcePath* c = w.GetComponent<PrefabSourcePath>(ent)) *c = was;
                    else w.AddComponent<PrefabSourcePath>(ent, PrefabSourcePath(was));
                },
                [](World& w, Entity ent)
                {
                    if (w.GetComponent<PrefabSourcePath>(ent)) w.RemoveComponent<PrefabSourcePath>(ent);
                },
                "Detach Prefab"));
        MarkHierarchyDirty();
        return was.path;
    });

    // ---- Delete / Duplicate / Reparent / Rename ----
    ed.set_function("Delete", [this, guard, live](sol::object target) -> int
    {
        guard("Delete");
        std::vector<Entity> ids;
        if (target.is<LuaEntity>()) ids.push_back(live("Delete", target.as<LuaEntity>()));
        else if (target.is<sol::table>())
            for (auto& [k, v] : target.as<sol::table>())
            {
                if (!v.is<LuaEntity>()) Fail("Delete", "bad_value", "Delete(id) or Delete({ids})");
                ids.push_back(live("Delete", v.as<LuaEntity>()));
            }
        else Fail("Delete", "bad_value", "Delete(id) or Delete({ids})");
        for (Entity e : ids) RefusePart("Delete", *m_world, e, "the entity");   // the next load re-spawns it

        // Roots only: DeleteEntities destroys whole subtrees, so an id whose
        // ancestor is also listed would be destroyed twice.
        std::unordered_set<Entity> listed(ids.begin(), ids.end());
        std::vector<Entity> roots;
        for (Entity e : ids)
        {
            bool covered = false;
            for (const Parent* p = m_world->GetComponent<Parent>(e); p && p->entity != NullEntity;
                 p = m_world->GetComponent<Parent>(p->entity))
                if (listed.count(p->entity)) { covered = true; break; }
            if (!covered && std::find(roots.begin(), roots.end(), e) == roots.end()) roots.push_back(e);
        }
        DeleteEntities(roots);
        return static_cast<int>(roots.size());
    });

    ed.set_function("Duplicate", [this, guard, live, out](const LuaEntity& id) -> LuaEntity
    {
        guard("Duplicate");
        const Entity src = live("Duplicate", id);
        RefusePart("Duplicate", *m_world, src, "the entity");   // the copy would be a sibling inside
        const Entity copy = DuplicateEntity(src);
        if (copy == NullEntity) Fail("Duplicate", "load_failed", "the entity could not be duplicated (see the log)");
        return out(copy);
    });

    ed.set_function("Reparent", [this, guard, live](const LuaEntity& id, sol::object parentObj,
                                                    sol::optional<sol::table> opts)
    {
        guard("Reparent");
        const Entity child = live("Reparent", id);
        RefusePart("Reparent", *m_world, child, "the entity");   // the next load puts it back
        Entity parent = NullEntity;
        if (parentObj.valid() && parentObj != sol::lua_nil)
        {
            if (!parentObj.is<LuaEntity>()) Fail("Reparent", "bad_value", "Reparent(id, parent | nil)");
            parent = live("Reparent", parentObj.as<LuaEntity>(), "the parent");
            RefuseParent("Reparent", *m_world, parent);
        }
        // A UI node moves on its UIParent edge (nil = under its canvas); {index = n}
        // (1-based, among the new parent's UI children in paint order) puts it there. One
        // undo step.
        const int index = opts ? opts->get_or("index", 0) : 0;
        const bool uiNode = m_world->HasComponent<UI::UIRect>(child);
        if (index != 0 && !uiNode) Fail("Reparent", "bad_value", "index orders Canvas UI nodes only");
        std::string why;
        if (!(uiNode ? MoveUINode(child, parent, index > 0 ? index - 1 : -1, &why)
                     : ReparentWithUndo(child, parent, &why)))
        {
            const auto colon = why.find(':');
            const std::string code = colon == std::string::npos ? "bad_value" : why.substr(0, colon);
            Fail("Reparent", code.c_str(), colon == std::string::npos ? why : why.substr(colon + 2));
        }
    });
    LuaDoc::Fn(ed, "Editor", "Children",
        [this, out](sol::this_state ts, const LuaEntity& id) -> sol::table
        {
            if (!m_world) Fail("Children", "no_world", "the editor has no world");
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) Fail("Children", "stale_handle", "the entity is not alive");
            sol::state_view lua(ts);
            sol::table t = lua.create_table();
            int i = 1;
            for (const Entity c : OutlinerChildrenOf(e)) t[i++] = out(c);
            return t;
        },
        { .doc = "The Outliner's children of an entity in the order it draws them: a Canvas UI node's in "
                 "paint order (siblingIndex, then id), the rest by id.",
          .params = { { "id", "the entity" } },
          .returns = "table", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor,
          .errors = { "no_world", "stale_handle" } });

    ed.set_function("Rename", [this, guard, live](const LuaEntity& id, const std::string& name)
    {
        guard("Rename");
        const Entity e = live("Rename", id);
        RefusePart("Rename", *m_world, e, "the entity");   // a root's name is on its N line
        const std::string before = m_world->GetName(e);
        if (before == name) return;
        m_world->SetName(e, name);
        if (UndoEnabled())
            PushEdit(std::make_unique<Editor::Undo::RenameAction>(m_world->MakeHandle(e), before, name));
        MarkHierarchyDirty();
    });

    ed.set_function("AssignMaterial", [this, guard, live](const LuaEntity& id, const std::string& path,
                                                          sol::optional<sol::table> opts)
    {
        guard("AssignMaterial");
        const Entity e = live("AssignMaterial", id);
        // A prefab instance saves none of its materials (the .ipfb holds them);
        // an .iscn part's material swap IS saved (the scene's MR lines).
        if (const SavedBy s = SavedByOf(*m_world, e); s.prefab)
            FailSavedBy("AssignMaterial", *m_world, s, "the entity");
        std::string why;

        // A StaticMesh owner's material is per SLOT: {slot = <slot name | SectionKey hex>};
        // an empty path clears the slot's override. Its derived sections are transient, so a
        // script cannot target them - this is the one way in.
        if (const StaticMeshComponent* sm = m_world->GetComponent<StaticMeshComponent>(e))
        {
            const std::string slot = opts ? opts->get_or<std::string>("slot", "") : std::string();
            MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
            const MeshAssetRegistry::Mesh* mesh =
                reg ? MeshAssetRegistry::Find(reg->GetModel(sm->model, m_world->Epoch()), sm->meshKey, sm->meshName)
                    : nullptr;
            if (!mesh) Fail("AssignMaterial", "not_resolved", "the Static Mesh does not resolve to a mesh");
            const MeshAssetRegistry::Section* sec = nullptr;
            uint64_t key = 0;
            const bool byKey = Resource::MeshKeyFromHex(slot, key);
            for (const MeshAssetRegistry::Section& s : mesh->sections)
                if ((byKey && s.key == key) || (!slot.empty() && s.slot == slot)) { sec = &s; break; }
            if (!sec && slot.empty() && mesh->sections.size() == 1) sec = &mesh->sections[0];
            if (!sec)
                Fail("AssignMaterial", "bad_value", slot.empty()
                     ? "a Static Mesh with several slots needs {slot = <name or SectionKey>}"
                     : "the mesh has no slot '" + slot + "'");
            if (!AssignStaticMeshSlot(e, sec->key, path, &why))
            {
                const auto colon = why.find(':');
                Fail("AssignMaterial", colon == std::string::npos ? "load_failed" : why.substr(0, colon).c_str(),
                     colon == std::string::npos ? why : why.substr(colon + 2));
            }
            return;
        }
        if (opts && opts->get<sol::object>("slot").valid())
            Fail("AssignMaterial", "bad_value", "{slot = ...} is for a Static Mesh owner");

        if (!AssignMaterial(e, path, &why))
        {
            const auto colon = why.find(':');
            const std::string code = colon == std::string::npos ? "load_failed" : why.substr(0, colon);
            Fail("AssignMaterial", code.c_str(), colon == std::string::npos ? why : why.substr(colon + 2));
        }
    });

    // ---- The Inspector's material row ----
    // What the row shows, and what its buttons do - through the handler the row itself
    // runs (RunMaterialRowAction), so a script proves the click.
    //   Editor.InspectorMaterial([id]) -> {entity, legacy, path, tag, summary, parent,
    //       parentResolved, overrides, unsaved, full, thumb, warnings = {{code, text}},
    //       slots = {{slot, key, path, state, tag, summary, parent, warnings, thumb}}}
    //       (id defaults to the selection; slots: a Static Mesh owner's)
    //   Editor.InspectorMaterialAction(action[, {entity = id, slot = <name | SectionKey hex>,
    //       path = p}]) -> {action, path}. action: assign | browse | clear | edit | editHere |
    //       createInstance | find | copyPath | saveAsAsset | promote | clearOverrides; the file
    //       a dialog would ask for is `path` (browse, saveAsAsset, promote).
    auto slotKeyOf = [this](const char* fn, Entity e, const std::string& slot) -> uint64_t
    {
        const StaticMeshComponent* sm = m_world->GetComponent<StaticMeshComponent>(e);
        if (!sm) Fail(fn, "bad_value", "{slot = ...} is for a Static Mesh owner");
        MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
        const MeshAssetRegistry::Mesh* mesh =
            reg ? MeshAssetRegistry::Find(reg->GetModel(sm->model, m_world->Epoch()), sm->meshKey, sm->meshName)
                : nullptr;
        if (!mesh) Fail(fn, "not_resolved", "the Static Mesh does not resolve to a mesh");
        uint64_t key = 0;
        const bool byKey = Resource::MeshKeyFromHex(slot, key);
        for (const MeshAssetRegistry::Section& s : mesh->sections)
            if ((byKey && s.key == key) || s.slot == slot) return s.key;
        Fail(fn, "bad_value", "the mesh has no slot '" + slot + "'");
        return 0;
    };
    auto rowThumbReady = [this](const std::string& path)
    {
        const auto it = m_materialRowTiles.find(path);
        if (it == m_materialRowTiles.end()) return false;
        const AssetEntry& ae = it->second;
        return (ae.previewReady && ae.previewTile.IsValid()) || ae.thumbnail != Resource::kInvalidTextureHandle;
    };
    ed.set_function("InspectorMaterial", [this, live, out, rowThumbReady](sol::this_state ts,
                                                                           sol::object idIn) -> sol::table
    {
        const char* fn = "InspectorMaterial";
        if (!m_world) Fail(fn, "no_world", "the editor has no world");
        Entity e = m_selectedEntity;
        if (idIn.valid() && idIn.get_type() != sol::type::lua_nil)
        {
            if (!idIn.is<LuaEntity>()) Fail(fn, "bad_value", "the argument is an entity id");
            e = live(fn, idIn.as<LuaEntity>());
        }
        if (e == NullEntity || !m_world->IsAlive(e)) Fail(fn, "no_selection", "nothing is selected: pass an entity id");
        sol::state_view lua(ts);
        auto fill = [&](sol::table& t, const Editor::MaterialRowInfo& r)
        {
            t["path"]    = r.path;
            t["tag"]     = r.tag;
            t["summary"] = r.summary;
            t["parent"]  = r.parent;
            t["parentResolved"] = r.parentResolved;
            t["thumb"]   = rowThumbReady(r.path);
            sol::table w = lua.create_table();
            int n = 0;
            for (const auto& [code, text] : r.warnings)
                w[++n] = lua.create_table_with("code", code, "text", text);
            t["warnings"] = w;
        };
        sol::table t = lua.create_table();
        t["entity"] = out(e);
        if (m_world->HasComponent<MaterialComponent>(e))
        {
            const Editor::MaterialRowInfo r = MaterialRowFor(e);
            fill(t, r);
            t["overrides"] = r.entityOverrides;
            t["unsaved"]   = r.unsaved;
        }
        if (const StaticMeshComponent* sm = m_world->GetComponent<StaticMeshComponent>(e))
        {
            MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
            const MeshAssetRegistry::Mesh* mesh =
                reg ? MeshAssetRegistry::Find(reg->GetModel(sm->model, m_world->Epoch()), sm->meshKey, sm->meshName)
                    : nullptr;
            sol::table slots = lua.create_table();
            if (mesh)
                for (size_t k = 0; k < mesh->sections.size(); ++k)
                {
                    const MeshAssetRegistry::Section& sec = mesh->sections[k];
                    std::string path;
                    bool overridden = false;
                    StaticMeshSlotMaterial(e, sec.key, path, overridden);
                    const Editor::MaterialRowInfo r = MaterialSlotRowFor(e, sec.key, path, overridden);
                    sol::table s = lua.create_table();
                    s["slot"]  = sec.slot;
                    s["key"]   = Resource::MeshKeyToHex(sec.key);
                    s["state"] = std::string(r.slotState ? r.slotState : "");
                    fill(s, r);
                    slots[k + 1] = s;
                }
            t["slots"] = slots;
        }
        return t;
    });
    ed.set_function("InspectorMaterialAction", [this, guard, live, slotKeyOf](sol::this_state ts,
                                                                              const std::string& action,
                                                                              sol::optional<sol::table> opts)
                                                                              -> sol::table
    {
        const char* fn = "InspectorMaterialAction";
        guard(fn);
        Editor::MaterialRowResult a;
        a.action = Editor::MaterialRowActionFromName(action);
        if (a.action == Editor::MaterialRowAction::None)
            Fail(fn, "bad_value", "unknown action '" + action + "' (assign | browse | clear | edit | editHere | "
                                  "createInstance | find | copyPath | saveAsAsset | promote | clearOverrides)");
        Entity e = m_selectedEntity;
        std::string slot;
        if (opts)
        {
            if (const sol::object ev = (*opts)["entity"]; ev.valid())
            {
                if (!ev.is<LuaEntity>()) Fail(fn, "bad_value", "entity is an entity id");
                e = live(fn, ev.as<LuaEntity>());
            }
            a.path = opts->get_or<std::string>("path", "");
            slot   = opts->get_or<std::string>("slot", "");
        }
        if (e == NullEntity || !m_world->IsAlive(e)) Fail(fn, "no_selection", "nothing is selected: pass {entity = id}");
        const uint64_t key = slot.empty() ? 0 : slotKeyOf(fn, e, slot);
        std::string why;
        if (!RunMaterialRowAction(e, key, a, &why))
        {
            const auto colon = why.find(':');
            Fail(fn, colon == std::string::npos ? "refused" : why.substr(0, colon).c_str(),
                 colon == std::string::npos ? why : why.substr(colon + 2));
        }
        std::string path;
        bool overridden = false;
        if (key) StaticMeshSlotMaterial(e, key, path, overridden);
        else if (const MaterialSourcePath* src = m_world->GetComponent<MaterialSourcePath>(e)) path = src->path;
        sol::state_view lua(ts);
        return lua.create_table_with("action", std::string(Editor::MaterialRowActionName(a.action)), "path", path);
    });

    // ---- Components ----
    // The inspector's own Add Component / Remove Component - its per-type add and remove,
    // its conditions, its undo actions - named by the SERIALIZER tag (what .iscene writes
    // and Entity.get shows), so what a script adds is what the scene saves. AddComponent's
    // fields go through that serializer as Create's do, every key checked; undo removes the
    // component, redo brings it back with those fields.
    struct CompRef
    {
        const ComponentSerializer*  ser   = nullptr;
        const ComponentEditorEntry* entry = nullptr;
        std::type_index             type  = std::type_index(typeid(void));
    };
    auto componentRef = [this](const char* fn, const std::string& tag) -> CompRef
    {
        for (const auto& [type, ser] : GetComponentRegistry().All())
        {
            if (ser.tag != tag) continue;
            const auto it = m_componentEditors.find(type);
            if (!ser.serialize || !ser.deserialize || it == m_componentEditors.end() || !it->second.add ||
                !it->second.remove || !it->second.fetch)
                Fail(fn, "not_addable", "'" + tag + "' is not one the inspector adds and removes "
                                        "(Editor.Components() lists those)");
            return { &ser, &it->second, type };
        }
        Fail(fn, "unknown_component", "no serializable component '" + tag + "' (Editor.Components() lists the "
                                      "ones the inspector adds and removes)");
    };
    // A component the save would drop: any on a part of an instance, and on a
    // prefab instance's root all but the lines its pure reference keeps
    // (SceneSerializer: PrefabRef, Guid, Tag, Persistent, EdFolder). An .iscn
    // placement's root is saved whole.
    auto refuseComponentEdit = [this](const char* fn, Entity e, const std::string& tag)
    {
        const SavedBy s = SavedByOf(*m_world, e);
        if (s.root == NullEntity || (s.isRoot && !s.prefab)) return;
        if (s.isRoot && (tag == "Guid" || tag == "Tag" || tag == "Persistent" || tag == "EdFolder")) return;
        FailSavedBy(fn, *m_world, s, s.isRoot ? "its " + tag + " (the root keeps only Guid, Tag, Persistent and "
                                                 "EdFolder)"
                                              : std::string("the entity"));
    };

    ed.set_function("AddComponent", [this, guard, live, componentRef, refuseComponentEdit](
                                        sol::this_state ts, const LuaEntity& id, const std::string& tag, sol::optional<sol::table> fieldsIn)
    {
        guard("AddComponent");
        const Entity e = live("AddComponent", id);
        const CompRef c = componentRef("AddComponent", tag);
        refuseComponentEdit("AddComponent", e, tag);
        if (c.entry->fetch(*m_world, e))
            Fail("AddComponent", "already_present", "the entity already has " + tag + " (Entity.set changes it)");
        if (c.entry->condition && !c.entry->condition(*m_world, e))
            Fail("AddComponent", "not_allowed", "the inspector does not offer " + tag + " for this entity");
        // Every value converted BEFORE the world changes: a bad request adds nothing.
        KVMap kv;
        if (!Resource::Authoring::Fields(tag).empty())
        {
            // Fields Resource::Authoring covers go through the same typed adapter as Create and
            // runtime AddComponent (Vec3 included); do not pre-convert them with the narrower
            // ValueText first.
            sol::table fields = fieldsIn ? *fieldsIn : sol::state_view(ts).create_table();
            for (const auto& [key, text] : LuaAuthoringCreateValues(tag, fields)) kv[key] = text;
        }
        else if (fieldsIn)
            for (auto& [k, v] : *fieldsIn)
            {
                if (!k.is<std::string>()) Fail("AddComponent", "bad_value", tag + " keys are names");
                std::string text;
                if (!ValueText(v, text))
                    Fail("AddComponent", "bad_value", tag + "." + k.as<std::string>() +
                                                      " wants a number, a boolean, a string or a list of numbers");
                kv[k.as<std::string>()] = std::move(text);
            }

        c.entry->add(*m_world, e);
        ComponentSerializerRegistry& reg = GetComponentRegistry();
        const std::string defaults = Editor::Undo::SerializeComponentBlob(reg, *m_world, e, c.type);
        if (!kv.empty())
        {
            // Then check every key really landed (a serializer ignores keys it
            // does not know); only a complete component is recorded.
            c.ser->deserialize(*m_world, e, kv, m_assetMgr);
            std::ostringstream ss;
            c.ser->serialize(*m_world, e, ss);
            const auto have = LineKeys(ss.str());
            for (const auto& [key, value] : kv)
            {
                if (have.count(key)) continue;
                c.entry->remove(*m_world, e);
                std::string known;
                for (const auto& k2 : have) known += (known.empty() ? "" : ", ") + k2;
                Fail("AddComponent", "unknown_key", tag + " has no key '" + key + "' (it writes: " + known + ")");
            }
        }
        if (UndoEnabled())
        {
            SyncedEditContext();
            const EntityHandle h     = m_world->MakeHandle(e);
            const std::string  label = ComponentLabelFor(c.type);
            if (kv.empty())
                PushEdit(std::make_unique<Editor::Undo::AddComponentAction>(h, c.entry->add, c.entry->remove, label));
            else
            {
                // ONE undo entry: the add (defaults) and the fields as an edit of
                // it, so a redo brings the fields back too.
                m_editHistory.BeginMacro("Add " + label);
                PushEdit(std::make_unique<Editor::Undo::AddComponentAction>(h, c.entry->add, c.entry->remove, label));
                PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(
                    h, c.type, defaults, Editor::Undo::SerializeComponentBlob(reg, *m_world, e, c.type), label));
                m_editHistory.EndMacro();
            }
        }
        MarkHierarchyDirty();
    });

    ed.set_function("RemoveComponent", [this, guard, live, componentRef, refuseComponentEdit](
                                           const LuaEntity& id, const std::string& tag)
    {
        guard("RemoveComponent");
        const Entity e = live("RemoveComponent", id);
        const CompRef c = componentRef("RemoveComponent", tag);
        refuseComponentEdit("RemoveComponent", e, tag);
        if (!c.entry->fetch(*m_world, e))
            Fail("RemoveComponent", "no_component", "the entity has no " + tag);
        if (UndoEnabled())
        {
            // Its data first, so the undo puts back exactly what was there.
            SyncedEditContext();
            PushEdit(std::make_unique<Editor::Undo::RemoveComponentAction>(
                m_world->MakeHandle(e), c.type, c.entry->add, c.entry->remove,
                Editor::Undo::SerializeComponentBlob(GetComponentRegistry(), *m_world, e, c.type),
                ComponentLabelFor(c.type)));
        }
        c.entry->remove(*m_world, e);
        MarkHierarchyDirty();
    });

    // {{tag, label, category}} by tag: what AddComponent / RemoveComponent take -
    // the inspector's components that have a serializer.
    ed.set_function("Components", [this](sol::this_state ts) -> sol::table
    {
        std::vector<std::tuple<std::string, std::string, std::string>> rows;
        for (const auto& [type, ser] : GetComponentRegistry().All())
        {
            const auto it = m_componentEditors.find(type);
            if (!ser.serialize || !ser.deserialize || it == m_componentEditors.end() || !it->second.add ||
                !it->second.remove || !it->second.fetch)
                continue;
            rows.emplace_back(ser.tag, ComponentLabelFor(type), it->second.category);
        }
        std::sort(rows.begin(), rows.end());
        sol::state_view view(ts);
        sol::table t = view.create_table();
        int i = 1;
        for (const auto& [tag, label, category] : rows)
            t[i++] = view.create_table_with("tag", tag, "label", label, "category", category);
        return t;
    });

    // ---- SavePrefab ----
    // The Outliner's Save as Prefab (Resource::SavePrefab) without its dialog: the root's
    // subtree - with what follows it (socket followers, trails) - as an .ipfb at `path`,
    // written ATOMICALLY (a failed save leaves an existing file whole), its .meta identity
    // minted or kept. Not an edit: no undo entry. Like the menu item it gives every mesh a
    // material FILE: a MaterialComponent without one is written as
    // <prefab dir>/Materials/<name>_mat<n>.imat and its entity takes that
    // MaterialSourcePath - those files come back in `materials`.
    //
    // Without link the source stays plain entities. {link = true} makes it an instance of
    // the new prefab, in place (LinkPrefabAction: ONE undo entry, `linked` / `members` in
    // the result): the scene saves it as a reference to the file and its parts take no
    // edits (prefab_instance). Refused, before anything is written, where the scene save
    // would drop the link (the root is a part of another instance or of an .iscn
    // placement). The root gets a Guid first, into the file: a scene reload hands it back
    // as the seed and LoadPrefab keeps the baked identities for it, so the parts - and
    // references to them from outside - stay the same.
    ed.set_function("SavePrefab", [this, guard, live](sol::this_state ts, const LuaEntity& id,
                                                      const std::string& path,
                                                      sol::optional<sol::table> optsIn) -> sol::table
    {
        guard("SavePrefab");
        const Entity root = live("SavePrefab", id);
        std::string ext = Sys::PathToUtf8(Sys::U8Path(path).extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".ipfb") Fail("SavePrefab", "bad_value", "a prefab is an .ipfb file: " + path);
        if (m_world->HasComponent<PreviewStageTag>(root))
            Fail("SavePrefab", "bad_value", "the entity belongs to the Chain Physics Preview stage (a transient, "
                                            "stripped copy) - save the source prefab instead");
        bool link = false;
        if (optsIn)
            for (const auto& [k, v] : *optsIn)
            {
                const std::string key = k.is<std::string>() ? k.as<std::string>() : std::string("?");
                if (key != "link")
                    Fail("SavePrefab", "unknown_key", "SavePrefab(root, path, {link = true|false}) takes no '" + key + "'");
                if (!v.is<bool>()) Fail("SavePrefab", "bad_value", "link wants true or false");
                link = v.as<bool>();
            }
        if (link)
        {
            RefusePart("SavePrefab", *m_world, root, "the root");
            ECS::EnsureGuidOn(*m_world, root);
        }

        // The meshes whose material the save will write out as a file.
        std::vector<Entity> needFile;
        for (std::vector<Entity> stack{ root }; !stack.empty();)
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (m_world->HasComponent<MaterialComponent>(e) && !m_world->HasComponent<MaterialSourcePath>(e))
                needFile.push_back(e);
            if (const Children* ch = m_world->GetComponent<Children>(e))
                stack.insert(stack.end(), ch->entities.begin(), ch->entities.end());
        }

        // Written beside the target (so its Materials/ folder is the target's),
        // then swapped in whole.
        std::error_code ec;
        const std::filesystem::path target = Sys::U8Path(path);
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
        const std::string tmp = path + ".saving";
        std::vector<Entity> saved;   // what the file holds: the instance's members, if linked
        if (!Resource::SavePrefab(root, *m_world, tmp, &saved))
        {
            std::filesystem::remove(Sys::U8Path(tmp), ec);
            Fail("SavePrefab", "write_failed", "cannot write " + path + " (the log's PrefabSerializer line says why)");
        }
        std::vector<uint8_t> bytes;
        const bool read = Sys::ReadFileBytes(tmp, bytes);
        bool locked = false;
        const bool written = read && Sys::WriteFileBytesAtomic(path, bytes.data(), bytes.size(), &locked);
        std::filesystem::remove(Sys::U8Path(tmp), ec);
        if (!written)
            Fail("SavePrefab", "write_failed", locked ? path + " is held open by another reader; it was left as it was"
                                                      : "cannot write " + path);

        const ECS::Guid guid = Resource::AssetGuidIndex::Get().EnsureGuid(path);
        m_needsRescan = true;   // the Asset Browser lists it

        int members = 0;
        if (link)
        {
            std::optional<PrefabSourcePath> rootBefore;
            if (const PrefabSourcePath* p = m_world->GetComponent<PrefabSourcePath>(root)) rootBefore = *p;
            std::vector<LinkPrefabAction::Member> tagged;
            for (const Entity e : saved)
            {
                if (e == root || !m_world->IsAlive(e)) continue;
                std::optional<PrefabInstanceMember> before;
                if (const PrefabInstanceMember* m = m_world->GetComponent<PrefabInstanceMember>(e)) before = *m;
                std::optional<PrefabSourcePath> sourceBefore;
                if (const PrefabSourcePath* s = m_world->GetComponent<PrefabSourcePath>(e)) sourceBefore = *s;
                tagged.push_back({ m_world->MakeHandle(e), before, sourceBefore });
            }
            members = static_cast<int>(tagged.size());
            auto action = std::make_unique<LinkPrefabAction>(m_world->MakeHandle(root), rootBefore,
                                                             Sys::RelativizeAssetPath(path), std::move(tagged));
            action->Apply(SyncedEditContext());
            if (UndoEnabled()) PushEdit(std::move(action));
            MarkHierarchyDirty();
        }

        sol::state_view view(ts);
        sol::table out = view.create_table();
        out["path"]  = path;
        out["guid"]  = guid.IsValid() ? guid.ToHex() : std::string();
        out["bytes"] = bytes.size();
        sol::table mats = view.create_table();
        int n = 1;
        for (const Entity e : needFile)
            if (const MaterialSourcePath* m = m_world->GetComponent<MaterialSourcePath>(e))
                mats[n++] = m->path;
        out["materials"] = mats;
        out["linked"]    = link;
        out["members"]   = members;
        return out;
    });

    // ---- CreateMaterialInstance ----
    // The Asset Browser's Create Material Instance - an .imatinst that follows its parent
    // for everything it does not override - with the overrides set in the same call:
    // params (the Inspector's schema parameters), textures (slots), meta (kMetaKeys: modes
    // and flags) and custom / customTextures (the custom shader's own, as its reflection
    // names them). Exactly those names are the file's override set (SaveMaterialInstance's
    // explicit set: <param>, tex_<SLOT>, <meta key>, cparam_<name>, ctex_<name>), so the
    // rest keeps following the parent. dest defaults to the browser's name, MI_<stem>
    // beside the parent, suffixed, never overwriting; a named dest IS overwritten,
    // atomically, its .meta identity kept. Not an edit (a file): Editor.AssignMaterial puts
    // it on a mesh, undoably.
    ed.set_function("CreateMaterialInstance", [this](sol::this_state ts, const std::string& parentIn,
                                                     sol::optional<sol::table> optsIn) -> sol::table
    {
        namespace fs = std::filesystem;
        std::string pext = Sys::PathToUtf8(Sys::U8Path(parentIn).extension());
        for (char& c : pext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (pext != ".imat" && pext != ".imatinst")
            Fail("CreateMaterialInstance", "bad_value", "the parent is an .imat or .imatinst: " + parentIn);
        MaterialComponent mat{};
        if (!Resource::LoadMaterial(parentIn, mat))
            Fail("CreateMaterialInstance", "load_failed", "the parent did not load: " + parentIn);
        const std::string parentRef = Sys::RelativizeAssetPath(parentIn);

        // Every override resolved and converted BEFORE anything is written.
        std::unordered_set<std::string> overrides;
        if (optsIn)
            if (const sol::object p = (*optsIn)["params"]; p.valid() && p != sol::lua_nil)
            {
                if (!p.is<sol::table>()) Fail("CreateMaterialInstance", "bad_value", "params wants {name = value}");
                for (auto& [k, v] : p.as<sol::table>())
                {
                    if (!k.is<std::string>()) Fail("CreateMaterialInstance", "bad_value", "params are keyed by name");
                    const std::string name = k.as<std::string>();
                    const MaterialSchema::FieldDesc* f = nullptr;
                    for (const auto& row : MaterialSchema::kMaterialPBRSchema)
                        if (name == row.name) { f = &row; break; }
                    if (!f)
                    {
                        std::string known;
                        for (const auto& row : MaterialSchema::kMaterialPBRSchema)
                            known += std::string(known.empty() ? "" : ", ") + row.name;
                        Fail("CreateMaterialInstance", "unknown_key", "no material parameter '" + name +
                                                                      "' (the Inspector's: " + known + ")");
                    }
                    float* dst = MaterialSchema::FieldPtr(mat, *f);
                    using K = MaterialSchema::Kind;
                    if (f->kind == K::Float || f->kind == K::FloatComponent)
                    {
                        if (!v.is<double>() || !std::isfinite(v.as<double>()))
                            Fail("CreateMaterialInstance", "bad_value", name + " wants a number");
                        dst[f->kind == K::Float ? 0 : f->component] = static_cast<float>(v.as<double>());
                    }
                    else
                    {
                        // Color3: {r, g, b}; Color4: {r, g, b} (alpha kept) or {r, g, b, a}.
                        float rgba[4]{ dst[0], dst[1], dst[2], dst[3] };
                        const bool three = ReadNumbers(v, rgba, 3);
                        if (!three && (f->kind == K::Color3 || !ReadNumbers(v, rgba, 4)))
                            Fail("CreateMaterialInstance", "bad_value",
                                 name + (f->kind == K::Color3 ? " wants {r, g, b}" : " wants {r, g, b} or {r, g, b, a}"));
                        for (int i = 0; i < (f->kind == K::Color3 ? 3 : 4); ++i) dst[i] = rgba[i];
                    }
                    overrides.insert(name);
                }
            }
        // textures = {<SLOT> = "<.itex>" | false}: the instance binds that slot -
        // or, false, clears it (the parent's texture is then NOT inherited; the
        // file says `<none>`). "tex_<SLOT>" joins the override set.
        if (optsIn)
            if (const sol::object tx = (*optsIn)["textures"]; tx.valid() && tx != sol::lua_nil)
            {
                if (!tx.is<sol::table>())
                    Fail("CreateMaterialInstance", "bad_value", "textures wants {SLOT = path | false}");
                struct Slot { const char* name; MaterialComponent::TEXTURESLOT slot; };
                static const Slot kSlots[] = {
#define TEX_SLOT_(e, label, defpath) { #e, MaterialComponent::e },
                    MATERIAL_TEXTURE_SLOTS(TEX_SLOT_)
#undef TEX_SLOT_
                };
                for (auto& [k, v] : tx.as<sol::table>())
                {
                    const std::string slotName = k.is<std::string>() ? k.as<std::string>() : std::string();
                    const Slot* s = nullptr;
                    for (const auto& e : kSlots)
                        if (slotName == e.name) { s = &e; break; }
                    if (!s)
                    {
                        std::string known;
                        for (const auto& e : kSlots) known += std::string(known.empty() ? "" : ", ") + e.name;
                        Fail("CreateMaterialInstance", "unknown_key", "no texture slot '" + slotName + "' (" + known + ")");
                    }
                    std::string path;
                    if (v.is<std::string>())
                    {
                        path = v.as<std::string>();
                        for (char& c : path) if (c == '\\') c = '/';
                        // Through the search roots (a worktree's shared content
                        // root too), this exact spelling.
                        if (!Resource::AssetFS::Get().ExistsExact(path))
                            Fail("CreateMaterialInstance", "load_failed", "no such texture: " + path);
                    }
                    else if (!(v.is<bool>() && !v.as<bool>()))
                        Fail("CreateMaterialInstance", "bad_value", slotName + " wants a texture path, or false to clear it");
                    mat.textures[s->slot].name = path;
                    overrides.insert(std::string("tex_") + s->name);
                }
            }

        // meta = {key = value}: the material's modes and flags (kMetaKeys: blendMode, domain,
        // customShadingModel, doubleSided, castShadow, texTiling, customShaderPath, ...). An
        // enum takes its name, any case, or its number; a path takes false to clear it. Applied
        // through the loader's own parser behind a "matfmt = 3" header, so no shaderType fold
        // runs.
        if (optsIn)
            if (const sol::object mo = (*optsIn)["meta"]; mo.valid() && mo != sol::lua_nil)
            {
                if (!mo.is<sol::table>()) Fail("CreateMaterialInstance", "bad_value", "meta wants {key = value}");
                std::string text = "matfmt = 3\n";
                for (auto& [k, v] : mo.as<sol::table>())
                {
                    const std::string key = k.is<std::string>() ? k.as<std::string>() : std::string();
                    const MetaKey* m = nullptr;
                    for (const MetaKey& e : kMetaKeys)
                        if (key == e.key) { m = &e; break; }
                    if (!m)
                    {
                        std::string known;
                        for (const MetaKey& e : kMetaKeys) known += std::string(known.empty() ? "" : ", ") + e.key;
                        Fail("CreateMaterialInstance", "unknown_key", "no material meta key '" + key + "' (" + known + ")");
                    }
                    const char* code = nullptr;
                    std::string why;
                    const std::string value = MetaValueText(*m, v, code, why);
                    if (value.empty()) Fail("CreateMaterialInstance", code, key + " " + why);
                    text += key + " = " + value + "\n";
                    overrides.insert(key);
                }
                Resource::ApplyMaterialTextDelta(mat, text);
            }

        // custom = {name = number | {x, y[, z[, w]]} | bool}, customTextures =
        // {name = "<.itex>" | false}: the custom shader's own parameters and
        // textures (cparam_<name> / ctex_<name>). The names are the SHADER's -
        // its reflection, Synced as the Inspector does - so one it does not
        // declare is refused rather than written where the renderer ignores it.
        const sol::object cu = optsIn ? sol::object((*optsIn)["custom"]) : sol::object{};
        const sol::object ct = optsIn ? sol::object((*optsIn)["customTextures"]) : sol::object{};
        const bool hasCustom = cu.valid() && cu != sol::lua_nil;
        const bool hasCustomTex = ct.valid() && ct != sol::lua_nil;
        if (hasCustom || hasCustomTex)
        {
            if ((hasCustom && !cu.is<sol::table>()) || (hasCustomTex && !ct.is<sol::table>()))
                Fail("CreateMaterialInstance", "bad_value", "custom / customTextures want {name = value}");
            if (!mat.HasAnyCustomShader())
                Fail("CreateMaterialInstance", "bad_value", "custom / customTextures need a custom shader and this "
                     "material has none (meta.useCustomShader and meta.customShaderPath give it one)");
            MaterialComponent probe{};
            probe.useCustomShader        = true;
            probe.customShaderPath       = mat.customShaderPath;
            probe.customVertexShaderPath = mat.customVertexShaderPath;
            // ...and what picks the variant the material draws with, so the names accepted are
            // the ones the renderer's reflection gives it.
            probe.userBlendMode          = mat.userBlendMode;
            probe.alphaRef               = mat.alphaRef;
            probe.customShadingModel     = mat.customShadingModel;
            probe.flags                 = mat.flags;
            std::string why;
            if (!ReflectCustomShaders(m_renderer, probe, why)) Fail("CreateMaterialInstance", "load_failed", why);

            if (hasCustom)
                for (auto& [k, v] : cu.as<sol::table>())
                {
                    const std::string name = k.is<std::string>() ? k.as<std::string>() : std::string();
                    const auto pm = probe.customParamMeta.find(name);
                    if (pm == probe.customParamMeta.end() || pm->second.isTextureSlot)
                    {
                        std::vector<std::string> names;
                        for (const auto& [n, meta] : probe.customParamMeta)
                            if (!meta.isTextureSlot) names.push_back(n);
                        std::sort(names.begin(), names.end());
                        std::string known;
                        for (const std::string& n : names) known += std::string(known.empty() ? "" : ", ") + n;
                        Fail("CreateMaterialInstance", "unknown_key", "the custom shader declares no parameter '" +
                             name + "' (" + (known.empty() ? std::string("it declares none") : known) + ")");
                    }
                    const auto& meta = pm->second;
                    const auto cur = mat.customParams.find(name);
                    std::array<float, 4> val = cur != mat.customParams.end() ? cur->second : meta.defaultValue;
                    const size_t n = meta.numComponents >= 1 && meta.numComponents <= 4 ? meta.numComponents : 4;
                    float xyzw[4]{ val[0], val[1], val[2], val[3] };
                    if (meta.isBool)
                    {
                        if (!v.is<bool>()) Fail("CreateMaterialInstance", "bad_value", name + " wants true or false");
                        xyzw[0] = v.as<bool>() ? 1.f : 0.f;
                    }
                    else if (n == 1 && v.is<double>() && std::isfinite(v.as<double>()))
                        xyzw[0] = static_cast<float>(v.as<double>());
                    else if (!ReadNumbers(v, xyzw, n))
                        Fail("CreateMaterialInstance", "bad_value",
                             name + (n == 1 ? " wants a number" : " wants {" + std::to_string(n) + " numbers}"));
                    mat.customParams[name] = { xyzw[0], xyzw[1], xyzw[2], xyzw[3] };
                    overrides.insert("cparam_" + name);
                }

            if (hasCustomTex)
                for (auto& [k, v] : ct.as<sol::table>())
                {
                    const std::string name = k.is<std::string>() ? k.as<std::string>() : std::string();
                    if (probe.customTextures.find(name) == probe.customTextures.end())
                    {
                        std::vector<std::string> names;
                        for (const auto& [n, t] : probe.customTextures) names.push_back(n);
                        std::sort(names.begin(), names.end());
                        std::string known;
                        for (const std::string& n : names) known += std::string(known.empty() ? "" : ", ") + n;
                        Fail("CreateMaterialInstance", "unknown_key", "the custom shader declares no texture '" +
                             name + "' (" + (known.empty() ? std::string("it declares none") : known) + ")");
                    }
                    std::string path;
                    if (v.is<std::string>())
                    {
                        path = v.as<std::string>();
                        for (char& c : path) if (c == '\\') c = '/';
                        if (!Resource::AssetFS::Get().ExistsExact(path))
                            Fail("CreateMaterialInstance", "load_failed", "no such texture: " + path);
                    }
                    else if (!(v.is<bool>() && !v.as<bool>()))
                        Fail("CreateMaterialInstance", "bad_value", name + " wants a texture path, or false to clear it");
                    mat.customTextures[name].name = path;   // the uv set stays the parent's
                    overrides.insert("ctex_" + name);
                }
        }

        std::vector<std::pair<std::string, std::string>> authoredParams;
        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
            if (overrides.count(f.name))
            {
                const float* p = MaterialSchema::FieldPtr(mat, f);
                std::ostringstream text;
                using K = MaterialSchema::Kind;
                const int count = f.kind == K::Color4 ? 4 : f.kind == K::Color3 ? 3 : 1;
                for (int i = 0; i < count; ++i) { if (i) text << ' '; text << p[f.kind == K::FloatComponent ? f.component : i]; }
                authoredParams.emplace_back(f.name, text.str());
            }
        std::string validationError;
        if (!Resource::ValidateMaterialEdit(mat, authoredParams, validationError))
            Fail("CreateMaterialInstance", "bad_value", validationError);
        std::error_code ec;
        std::string dest = optsIn ? optsIn->get_or<std::string>("dest", "") : std::string();
        if (!dest.empty())
        {
            std::string dext = Sys::PathToUtf8(Sys::U8Path(dest).extension());
            for (char& c : dext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (dext != ".imatinst") Fail("CreateMaterialInstance", "bad_value", "dest is an .imatinst: " + dest);
            if (const fs::path d = Sys::U8Path(dest); d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
        }
        else
        {
            // The browser's name: MI_<stem>[_n].imatinst beside the parent.
            std::string stem = Sys::PathToUtf8(Sys::U8Path(parentIn).stem());
            if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".imat") == 0) stem.erase(stem.size() - 5);
            if (stem.rfind("MI_", 0) != 0) stem = "MI_" + stem;
            const fs::path dir = Sys::U8Path(parentIn).parent_path();
            for (int suffix = 0; suffix < 1000 && dest.empty(); ++suffix)
            {
                const fs::path cand = dir / Sys::U8Path(suffix == 0 ? stem + ".imatinst"
                                                                   : stem + "_" + std::to_string(suffix) + ".imatinst");
                if (!fs::exists(cand, ec)) dest = Sys::RelativizeAssetPath(Sys::PathToUtf8(cand));
            }
            if (dest.empty()) Fail("CreateMaterialInstance", "write_failed", "no free name for " + stem);
        }

        // Written beside the target, then swapped in whole; the cache entry of
        // the TARGET is what readers look up, so drop it after the swap.
        const std::string tmp = dest + ".saving";
        if (!Resource::SaveMaterialInstance(tmp, parentRef, mat, &overrides))
        {
            fs::remove(Sys::U8Path(tmp), ec);
            Fail("CreateMaterialInstance", "write_failed", "cannot write " + dest);
        }
        std::vector<uint8_t> bytes;
        bool locked = false;
        const bool written = Sys::ReadFileBytes(tmp, bytes) &&
                             Sys::WriteFileBytesAtomic(dest, bytes.data(), bytes.size(), &locked);
        fs::remove(Sys::U8Path(tmp), ec);
        Resource::InvalidateMaterialsDependingOn(tmp);
        if (!written)
            Fail("CreateMaterialInstance", "write_failed", locked ? dest + " is held open by another reader; it was "
                                                                          "left as it was"
                                                                        : "cannot write " + dest);
        Resource::InvalidateMaterialsDependingOn(dest);
        const ECS::Guid guid = Resource::AssetGuidIndex::Get().EnsureGuid(dest);
        m_needsRescan = true;

        sol::state_view view(ts);
        sol::table out = view.create_table();
        out["path"]   = dest;
        out["parent"] = parentRef;
        out["guid"]   = guid.IsValid() ? guid.ToHex() : std::string();
        std::vector<std::string> names(overrides.begin(), overrides.end());
        std::sort(names.begin(), names.end());
        sol::table ov = view.create_table();
        for (size_t i = 0; i < names.size(); ++i) ov[i + 1] = names[i];
        out["overrides"] = ov;
        return out;
    });

    // ---- Commands: what the menus, the command palette and the shortcuts run ----
    LuaDoc::Fn(ed, "Editor", "Commands", [this](sol::this_state ts) -> sol::table
        {
            EnsureCommandsRegistered();
            sol::state_view lua(ts);
            sol::table out = lua.create_table();
            auto& reg = Editor::CommandRegistry::Get();
            int i = 0;
            for (const Editor::Command& c : reg.All())
            {
                sol::table t = lua.create_table();
                t["id"]       = c.id;
                t["category"] = c.category;
                t["label"]    = reg.LabelOf(c);
                t["chord"]    = Editor::FormatChord(reg.ChordOf(c));
                t["userChord"] = reg.HasUserChord(c.id);
                std::string why;
                t["enabled"] = reg.IsEnabled(c, &why);
                if (!why.empty()) t["why"] = why;
                if (c.checked) t["checked"] = c.checked();
                out[++i] = t;
            }
            return out;
        },
        { .doc = "Every editor command in registry order: its id, category, label, chord (the user's when "
                 "userChord), whether it can run now (why not) and a toggle's checked state.",
          .returns = "table", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor });
    LuaDoc::Fn(ed, "Editor", "Command", [this](const std::string& id) -> bool
        {
            EnsureCommandsRegistered();
            auto& reg = Editor::CommandRegistry::Get();
            if (!reg.Find(id)) Fail("Command", "not_found", "no command '" + id + "' (Editor.Commands lists them)");
            std::string why;
            if (!reg.Execute(id, &why)) Fail("Command", "disabled", "'" + id + "' cannot run now: " + why);
            return true;
        },
        { .doc = "Runs an editor command by id, exactly as its menu item does: doc.save / doc.undo act on the "
                 "active document (Editor.ActiveDocument), file.save_all writes every unsaved one.",
          .params = { { "id", "a command id from Editor.Commands, e.g. \"doc.save\"" } },
          .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "not_found", "disabled" } });
    LuaDoc::Fn(ed, "Editor", "RunUITests", [this](sol::optional<sol::table> optsIn) -> std::uint64_t
        {
            std::string filter, junit, scratch;
            if (optsIn)
            {
                filter  = optsIn->get_or<std::string>("filter", filter);
                junit   = optsIn->get_or<std::string>("junit", junit);
                scratch = optsIn->get_or<std::string>("scratch", scratch);
            }
            EnsureCommandsRegistered();
            const std::uint64_t job = AdmitJob("ui_tests", filter.empty() ? std::string("all") : filter,
                                               Logger::CurrentRequestId());
            std::string why;
            if (!Editor::UITests::Queue(*this, filter, junit, scratch, job, why))
            {
                Harness::Jobs::Fail(job, "refused", why, {});
                Fail("RunUITests", "refused", why);
            }
            return job;
        },
        { .doc = "Runs the editor's UI tests (Dear ImGui Test Engine: real key presses and clicks on the editor's "
                 "windows) whose category/name matches filter; a job whose output lists each test and passed / "
                 "failed counts, and fails when any test failed.",
          .params = { { "opts", "{filter = \"commands\" (substring; \"\" = all), junit = path for the JUnit XML, "
                                "scratch = folder for fixture copies (default <artifacts>/uitests)}", "table", true } },
          .returns = "integer", .returnsDoc = "the job id (job wait)",
          .effect = LuaDoc::Effect::Job, .context = LuaDoc::Context::Editor, .errors = { "refused", "busy" } });
    LuaDoc::Fn(ed, "Editor", "ActiveDocument", [this](sol::this_state ts) -> sol::table
        {
            EnsureCommandsRegistered();
            sol::state_view lua(ts);
            sol::table t = lua.create_table();
            Editor::Document& d = m_documents.Active();
            t["kind"]  = d.kind;
            t["title"] = d.title ? d.title() : d.kind;
            t["save"]  = m_documents.SaveLabel();
            t["undo"]  = m_documents.UndoLabel();
            t["redo"]  = m_documents.RedoLabel();
            return t;
        },
        { .doc = "The document Save / Undo / Redo act on now - the asset tab or panel that had keyboard focus last "
                 "frame, else the scene: its kind (scene, material, ui, mesh, skeleton, sequence, notify, animgraph, "
                 "animset, chainpreview), title and the menus' labels.",
          .returns = "table", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor });

    // ---- History ----
    ed.set_function("Undo", [this, guard]() -> std::string
    {
        guard("Undo");
        if (m_editHistory.MacroOpen()) Fail("Undo", "busy", "not inside Editor.Batch");
        if (!m_editHistory.CanUndo()) Fail("Undo", "nothing_to_undo", "the undo stack is empty");
        const std::string label = m_editHistory.UndoLabel();
        EditUndo();
        return label;
    });
    ed.set_function("Redo", [this, guard]() -> std::string
    {
        guard("Redo");
        if (m_editHistory.MacroOpen()) Fail("Redo", "busy", "not inside Editor.Batch");
        if (!m_editHistory.CanRedo()) Fail("Redo", "nothing_to_undo", "the redo stack is empty");
        const std::string label = m_editHistory.RedoLabel();
        EditRedo();
        return label;
    });
    // A re-import waiting for its review, as Lua (nil when none):
    // Editor.State().reimportReview and Editor.ReimportReview.
    auto reimportReviewState = [this](sol::state_view view) -> sol::object
    {
        if (!m_reimportReview) return sol::make_object(view, sol::lua_nil);
        using Renamed = Resource::MeshIdentityAdoption::MeshRenamed;
        const ModelReimportReview& r = *m_reimportReview;
        nlohmann::json o = nlohmann::json::object();
        o["model"]  = r.model;
        o["source"] = r.request.source;
        nlohmann::json renamed = nlohmann::json::array();
        const auto& rn = r.report.identity.renamedMeshes;
        for (size_t i = 0; i < rn.size(); ++i)
            renamed.push_back({ { "from", rn[i].from }, { "to", rn[i].to }, { "key", Resource::MeshKeyToHex(rn[i].key) },
                                { "how", rn[i].how == Renamed::How::Explicit ? "explicit"
                                       : rn[i].how == Renamed::How::Geometry ? "geometry" : "name" },
                                { "keep", i < r.keepRename.size() && r.keepRename[i] != 0 } });
        nlohmann::json removed = nlohmann::json::array(), added = nlohmann::json::array(),
                       secRemoved = nlohmann::json::array();
        for (const auto& m : r.report.identity.removedMeshes) removed.push_back(m.name);
        for (const auto& m : r.report.identity.addedMeshes) added.push_back(m.name);
        for (const auto& s : r.report.identity.removedSections) secRemoved.push_back(s.mesh + "/" + s.slot);
        o["renamed"]         = renamed;
        o["removedMeshes"]   = removed;
        o["addedMeshes"]     = added;
        o["removedSections"] = secRemoved;
        return JsonToLua(view, o);
    };

    // The "Import Model" options dialog, as Lua (nil when none):
    // Editor.State().importDialog and Editor.ImportDialog.
    auto importDialogState = [this](sol::state_view view) -> sol::object
    {
        if (!m_importDialog) return sol::make_object(view, sol::lua_nil);
        const ModelImportDialog& d = *m_importDialog;
        nlohmann::json o = nlohmann::json::object();
        o["sources"]  = d.sources;
        o["dir"]      = d.outDir;
        o["reimport"] = d.reimport;
        o["options"]  = Resource::FormatImportOptions(d.options);
        return JsonToLua(view, o);
    };

    ed.set_function("State", [this, reimportReviewState, importDialogState](sol::this_state ts) -> sol::table
    {
        sol::state_view view(ts);
        sol::table t = view.create_table();
        t["scene"]      = m_currentScenePath;
        t["dirty"]      = m_sceneDirty;
        t["editMode"]   = GetPlayState() == ViewportPlayState::Stopped;
        t["canUndo"]    = m_editHistory.CanUndo();
        t["canRedo"]    = m_editHistory.CanRedo();
        t["undoLabel"]  = std::string(m_editHistory.UndoLabel());
        t["redoLabel"]  = std::string(m_editHistory.RedoLabel());
        // The next undo / redo is an AI agent's edit - made while the control
        // channel served a request (AgentEditAction; the labels stay the edits' names).
        t["undoByAgent"] = m_editHistory.UndoByAgent();
        t["redoByAgent"] = m_editHistory.RedoByAgent();
        t["undoCount"]  = m_editHistory.UndoCount();
        t["redoCount"]  = m_editHistory.RedoCount();
        t["inBatch"]    = m_editHistory.MacroOpen();
        if (m_playModeController)
        {
            t["mode"]    = std::string(ModeName(m_playModeController->Mode()));
            t["simTime"] = m_playModeController->Clock().elapsed;
        }
        if (m_world && m_selectedEntity != NullEntity && m_world->IsAlive(m_selectedEntity))
            t["selection"] = LuaId::Out(*m_world, m_selectedEntity);

        // The Content Browser (Editor.Browse sets it): folder, view, the models
        // expanded into their Meshes, and how many rows its LAST DRAW listed,
        // `meshes` of them Mesh items. Counts, not names: those rows point into
        // the mesh registry's views, which a world clear drops between draws.
        sol::table b = view.create_table();
        b["dir"]  = Sys::RelativizeAssetPath(m_currentDir);
        b["view"] = std::string(m_assetViewMode == AssetViewMode::List    ? "list"
                              : m_assetViewMode == AssetViewMode::Details ? "details" : "tiles");
        std::vector<std::string> expanded(m_expandedModels.begin(), m_expandedModels.end());
        std::sort(expanded.begin(), expanded.end());
        sol::table ex = view.create_table();
        for (size_t i = 0; i < expanded.size(); ++i) ex[i + 1] = expanded[i];
        b["expanded"] = ex;
        int meshRows = 0;
        for (const AssetDisplayRow& r : m_assetRows) meshRows += r.mesh >= 0 ? 1 : 0;
        b["rows"]   = static_cast<int>(m_assetRows.size());
        b["meshes"] = meshRows;
        // The Mesh item tiles alive (shown recently), how many have a picture (rendered this
        // session or served from the disk cache) and how many the lane gave up on.
        int ready = 0, gaveUp = 0;
        for (const auto& [ref, tile] : m_meshTiles)
        {
            if ((tile.previewReady && tile.previewTile.IsValid()) ||
                tile.thumbnail != Resource::kInvalidTextureHandle) ++ready;
            if (tile.previewGaveUp) ++gaveUp;
        }
        b["meshTiles"]       = static_cast<int>(m_meshTiles.size());
        b["meshTilesReady"]  = ready;
        b["meshTilesGaveUp"] = gaveUp;
        t["browser"] = b;
        // The Inspector's flashed material slot: the second viewport click on a Static Mesh
        // names the section it hit (owner, 0-based slot, seconds ago).
        if (m_slotFlashOwner != NullEntity && m_world && m_world->IsAlive(m_slotFlashOwner))
            t["slotFlash"] = view.create_table_with(
                "owner", LuaId::Out(*m_world, m_slotFlashOwner), "index", m_slotFlashIndex,
                "age", ImGui::GetCurrentContext() ? ImGui::GetTime() - m_slotFlashAt : -1.0);
        // Edit Collider, and how many collider drags it (or Editor.ColliderGizmo) committed.
        t["editCollider"]     = m_editCollider;
        t["colliderGestures"] = m_colliderGizmoGestures;
        // A re-import waiting for its review (Editor.ReimportReview answers it).
        t["reimportReview"] = reimportReviewState(view);
        // The options dialog waiting (Editor.ImportDialog answers it).
        t["importDialog"] = importDialogState(view);
        // Model imports running on a worker (their jobs finish when they are written), and
        // images waiting in the texture import's review.
        t["imports"]       = static_cast<int>(m_modelImportTasks.size());
        t["textureReview"] = static_cast<int>(m_pendingTextureImports.size());
        return t;
    });

    // ---- Play / Pause / Step / Stop ----
    // The toolbar's buttons from a script: each queues the SAME request on the
    // PlayModeController (Play from Edit also writes the toolbar's pre-Play autosave) and
    // answers a job. The transition is never inside the request - BeginFrame applies it at
    // the next frame boundary - so the job succeeds in the frame it was applied, {mode,
    // frame, simTime, entities, resumed?}, or fails "superseded" when another request (the
    // toolbar, -play_frames, a scene load's Stop) replaced it first: the controller keeps
    // one. A request the state machine would silently ignore is refused with the reason,
    // and so is one queued behind another.
    auto playControl = [this](const char* fn) -> EngineMode
    {
        if (!m_world || !m_playModeController) Fail(fn, "no_world", "the editor has no world or play-mode controller");
        if (m_scriptedPlay.jobId || m_playModeController->HasPendingRequest())
            Fail(fn, "busy", "a Play / Pause / Step / Stop is already queued for the next frame - wait for its job");
        return m_playModeController->Mode();
    };
    auto queuePlay = [this](const char* kind, EngineMode from, EngineMode target, bool resumed) -> std::uint64_t
    {
        m_scriptedPlay.jobId       = AdmitJob(kind, std::string(ModeName(from)) + " -> " + ModeName(target),
                                                           Logger::CurrentRequestId());
        m_scriptedPlay.target      = target;
        m_scriptedPlay.resumed     = resumed;
        m_scriptedPlay.queuedFrame = Logger::CurrentFrame();
        return m_scriptedPlay.jobId;
    };
    ed.set_function("Play", [this, playControl, queuePlay]() -> std::uint64_t
    {
        const EngineMode mode = playControl("Play");
        if (mode == EngineMode::Playing)
            Fail("Play", "already_playing", "Play is running - Stop, then Play, for a fresh session from the authored world");
        const bool resumed = mode != EngineMode::Edit;   // Paused, or its one Stepping frame
        if (!resumed && (!m_pendingSaveScenePath.empty() || m_pendingNewScene || !m_pendingLoadScenePath.empty()))
            Fail("Play", "busy", "a scene save or scene change is queued - wait for its job");
        const auto job = queuePlay("play", mode, EngineMode::Playing, resumed);
        if (!resumed) WritePrePlayAutosave();
        m_playModeController->RequestPlay();
        return job;
    });
    ed.set_function("Pause", [this, playControl, queuePlay]() -> std::uint64_t
    {
        const EngineMode mode = playControl("Pause");
        if (mode == EngineMode::Edit) Fail("Pause", "not_playing", "the editor is in Edit mode - Editor.Play() first");
        if (mode != EngineMode::Playing)
            Fail("Pause", "already_paused", "Play is paused - Editor.Play() resumes it, Editor.Step() advances one tick");
        const auto job = queuePlay("pause", mode, EngineMode::Paused, false);
        m_playModeController->RequestPause();
        return job;
    });
    ed.set_function("Step", [this, playControl, queuePlay]() -> std::uint64_t
    {
        const EngineMode mode = playControl("Step");
        if (mode == EngineMode::Edit) Fail("Step", "not_playing", "the editor is in Edit mode - Editor.Play() first");
        if (mode == EngineMode::Playing)
            Fail("Step", "not_paused", "Step advances one fixed tick from Pause - Editor.Pause() first");
        const auto job = queuePlay("step", mode, EngineMode::Stepping, false);
        m_playModeController->RequestStep();
        return job;
    });
    ed.set_function("Stop", [this, playControl, queuePlay]() -> std::uint64_t
    {
        const EngineMode mode = playControl("Stop");
        if (mode == EngineMode::Edit) Fail("Stop", "not_playing", "the editor is in Edit mode already");
        const auto job = queuePlay("stop", mode, EngineMode::Edit, false);
        m_playModeController->RequestStop();
        return job;
    });

    // ---- Collision bake (Tools > Bake > Collision Meshes) ----
    // Editor.BakeCollision{ ratio, error, minTriangles, skipSkinned, output } -> job: the
    // popup's Bake - every mesh the world draws, simplified into ONE .meshlib; the job's
    // output names it and counts {baked, failed, skipped, trisBefore, trisAfter}.
    // Editor.UseBakedCollision() -> the popup's "Use baked as collision": a writable model's
    // parts get the bake as the model's asset collision, the rest a Mesh collider of their
    // own; one undo step. -> { models = {...}, modelMeshes, refreshed, perEntity, replaced,
    // skippedSections, perEntityModels = {{model, why}} }.
    ed.set_function("BakeCollision", [this, guard](sol::optional<sol::table> optsIn) -> std::uint64_t
    {
        guard("BakeCollision");
        if (m_collisionBake.IsRunning() || m_collisionBakeScripted)
            Fail("BakeCollision", "busy", "a collision bake is already running");
        Tools::CollisionMesh::BakeOptions o = m_collisionBakeOpts;
        if (optsIn)
        {
            const sol::table& t = *optsIn;
            o.targetTriangleRatio = t.get_or("ratio", o.targetTriangleRatio);
            o.targetError         = t.get_or("error", o.targetError);
            o.minTriangles        = t.get_or("minTriangles", o.minTriangles);
            o.skipSkinned         = t.get_or("skipSkinned", o.skipSkinned);
            o.outputPath          = t.get_or<std::string>("output", o.outputPath);
        }
        if (o.outputPath.empty() || !o.outputPath.ends_with(".meshlib"))
            Fail("BakeCollision", "bad_value", "output must name a .meshlib");
        m_collisionBakeJobId    = AdmitJob("bake_collision", o.outputPath, Logger::CurrentRequestId());
        m_collisionBakeOpts     = o;
        m_collisionBakeScripted = true;
        Harness::Jobs::Start(m_collisionBakeJobId);
        m_collisionBake.Begin(*m_world, o);
        return m_collisionBakeJobId;
    });
    ed.set_function("UseBakedCollision", [this, guard](sol::this_state ts) -> sol::table
    {
        guard("UseBakedCollision");
        const BakedCollisionReport r = UseBakedAsCollision();
        if (!r.error.empty()) Fail("UseBakedCollision", "refused", r.error);
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        sol::table models = lv.create_table();
        for (size_t i = 0; i < r.models.size(); ++i) models[i + 1] = r.models[i];
        t["models"]          = models;
        t["modelMeshes"]     = r.modelMeshes;
        t["refreshed"]       = r.refreshed;
        t["perEntity"]       = r.perEntity;
        t["replaced"]        = r.replaced;
        t["skippedSections"] = r.skippedSections;
        sol::table kept = lv.create_table();
        for (size_t i = 0; i < r.perEntityModels.size(); ++i)
        {
            sol::table row = lv.create_table();
            row["model"] = r.perEntityModels[i].first;
            row["why"]   = r.perEntityModels[i].second;
            kept[i + 1]  = row;
        }
        t["perEntityModels"] = kept;
        return t;
    });

    // ---- Scene jobs ----
    ed.set_function("SaveScene", [this, guard](sol::optional<std::string> pathIn) -> std::uint64_t
    {
        guard("SaveScene");
        if (!m_pendingSaveScenePath.empty()) Fail("SaveScene", "busy", "a save is already queued");
        const std::string path = pathIn ? *pathIn : m_currentScenePath;
        if (path.empty() || !path.ends_with(".iscene"))
            Fail("SaveScene", "bad_value", "SaveScene(path.iscene) - this scene has no file yet");
        const auto saveJob = AdmitJob("save_scene", path, Logger::CurrentRequestId());
        if (!RequestSaveScene(path)) {
            Harness::Jobs::Fail(saveJob, "save_refused", "save request refused", {});
            Fail("SaveScene", "load_failed", "the save was refused (see the log)");
        }
        m_pendingSaveRequestId = Logger::CurrentRequestId();
        m_pendingSaveJobId     = saveJob;
        return m_pendingSaveJobId;
    });
    ed.set_function("NewScene", [this, guard](sol::optional<sol::table> opts) -> std::uint64_t
    {
        guard("NewScene");
        if (m_pendingNewScene || !m_pendingLoadScenePath.empty())
            Fail("NewScene", "busy", "a scene change is already queued");
        const bool discard = opts && opts->get_or("discard", false);
        if (m_sceneDirty && !discard)
            Fail("NewScene", "unsaved_changes", "the scene has unsaved edits - SaveScene first, or {discard = true}");
        m_pendingNewSceneJobId = AdmitJob("new_scene", "", Logger::CurrentRequestId());
        m_pendingNewScene      = true;
        return m_pendingNewSceneJobId;
    });
    ed.set_function("OpenScene", [this, guard](const std::string& path, sol::optional<sol::table> opts) -> std::uint64_t
    {
        guard("OpenScene");
        if (m_pendingNewScene || !m_pendingLoadScenePath.empty())
            Fail("OpenScene", "busy", "a scene change is already queued");
        std::error_code ec;
        if (!path.ends_with(".iscene") || !std::filesystem::exists(Sys::U8Path(path), ec))
            Fail("OpenScene", "load_failed", "no such .iscene: " + path);
        const bool discard = opts && opts->get_or("discard", false);
        if (m_sceneDirty && !discard)
            Fail("OpenScene", "unsaved_changes", "the scene has unsaved edits - SaveScene first, or {discard = true}");
        // Straight to the deferred load: a script cannot answer the unsaved-
        // changes or autosave-recovery modals File > Open would raise.
        m_pendingLoadJobId     = AdmitJob("open_scene", path, Logger::CurrentRequestId());
        m_pendingLoadScenePath = path;
        return m_pendingLoadJobId;
    });

    // ---- Asset import ----
    // The same import File > Import > Texture's confirm runs (ImportTextureFile), as a job
    // at the frame boundary. Importing onto an existing .itex IS the reimport: its guid is
    // kept, and a texture the engine already draws from that path is swapped in place -
    // output.reload.jobId is that swap's receipt (a "hotreload.texture" job: in effect from
    // which frame, or why not). Not an edit: no undo entry, allowed in Play too.
    // A model import's options:
    //   renames = { [from] = to, ... }   from: a previous Mesh's name or its MeshKey
    //                                    (16 hex digits); to: the new Mesh's name
    //   geometryRenames = false          no geometry match (a gone Mesh and a new
    //                                    one are that, whatever their geometry)
    //   dryRun = true                    the report only; nothing written
    auto readModelImportOptions = [](const char* fn, const sol::table& opts, ModelImportRequest& req)
    {
        if (const sol::object r = opts["renames"]; r.valid() && r.get_type() != sol::type::lua_nil)
        {
            if (r.get_type() != sol::type::table)
                Fail(fn, "bad_value", "renames = { [\"<old name or MeshKey>\"] = \"<new name>\", ... }");
            for (const auto& [k, v] : r.as<sol::table>())
            {
                if (k.get_type() != sol::type::string || v.get_type() != sol::type::string)
                    Fail(fn, "bad_value", "renames: every entry is [\"<old name or MeshKey>\"] = \"<new name>\"");
                req.renames.push_back({ k.as<std::string>(), v.as<std::string>() });
            }
            // pairs() order is not the table's: the same renames apply in the same order.
            std::sort(req.renames.begin(), req.renames.end(),
                      [](const Resource::MeshRename& a, const Resource::MeshRename& b) { return a.from < b.from; });
        }
        req.geometryRenames = opts.get_or("geometryRenames", true);
        req.dryRun          = opts.get_or("dryRun", false);
        // options = { normals = "compute", smooth = 45, morphs = false, ... }: the IM
        // record's names and values; what is not given is the default.
        if (const sol::object o = opts["options"]; o.valid() && o.get_type() != sol::type::lua_nil)
        {
            if (o.get_type() != sol::type::table)
                Fail(fn, "bad_value", "options = { normals = \"import\" | \"compute\", smooth = <deg>, ... }");
            Resource::ModelImportOptions parsed;
            for (const auto& [k, v] : o.as<sol::table>())
            {
                if (k.get_type() != sol::type::string)
                    Fail(fn, "bad_value", "options: every key is an option's name");
                std::string value;
                switch (v.get_type())
                {
                case sol::type::boolean: value = v.as<bool>() ? "1" : "0"; break;
                case sol::type::number:
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%g", v.as<double>());
                    value = buf;
                    break;
                }
                case sol::type::string:  value = v.as<std::string>(); break;
                default: Fail(fn, "bad_value", "options." + k.as<std::string>() + ": a string, number or boolean");
                }
                std::string why;
                if (!Resource::SetImportOption(parsed, k.as<std::string>(), value, &why))
                    Fail(fn, "bad_value", "options: " + why);
            }
            req.options = parsed;
        }
    };

    // A texture import's choices beyond its format: maxSize (the longest edge; 0 = the
    // source's), mipFilter (default / box / linear / cubic / triangle / point), flipGreen,
    // mips (false: one mip). They are recorded in the .itex's .meta for its Reimport.
    auto readTextureImportOptions = [](const char* fn, const sol::table& opts, PendingTextureImport& item)
    {
        if (const sol::optional<std::string> f = opts["format"]; f &&
            !Resource::TextureImporter::ParseFormatId(*f, item.selectedFormat))
        {
            std::string known;
            for (uint8_t i = 0; i < static_cast<uint8_t>(Resource::TextureImportFormat::Count); ++i)
                known += std::string(known.empty() ? "" : ", ") +
                         Resource::TextureImporter::GetFormatId(static_cast<Resource::TextureImportFormat>(i));
            Fail(fn, "bad_value", "unknown format '" + *f + "' (known: " + known + ")");
        }
        if (const sol::object m = opts["maxSize"]; m.valid() && m.get_type() != sol::type::lua_nil)
        {
            if (m.get_type() != sol::type::number || m.as<double>() < 0 || m.as<double>() > 16384 ||
                m.as<double>() != std::floor(m.as<double>()))
                Fail(fn, "bad_value", "maxSize is a whole number of texels, 0..16384 (0 = the source's size)");
            item.options.maxSize = static_cast<uint32_t>(m.as<double>());
        }
        if (const sol::optional<std::string> mf = opts["mipFilter"]; mf &&
            !Resource::TextureImporter::ParseMipFilterId(*mf, item.options.mipFilter))
            Fail(fn, "bad_value", "mipFilter is default, box, linear, cubic, triangle or point - not '" + *mf + "'");
        for (const char* key : { "flipGreen", "mips" })
            if (const sol::object b = opts[key]; b.valid() && b.get_type() != sol::type::lua_nil)
            {
                if (b.get_type() != sol::type::boolean) Fail(fn, "bad_value", std::string(key) + " is true or false");
                (std::string(key) == "flipGreen" ? item.options.flipGreen : item.options.generateMips) = b.as<bool>();
            }
    };

    ed.set_function("Import", [this, readModelImportOptions, readTextureImportOptions](const std::string& source,
                                                             sol::optional<sol::table> opts) -> std::uint64_t
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(Sys::U8Path(source), ec))
            Fail("Import", "load_failed", "no such file: " + source);
        std::string ext = Sys::PathToUtf8(Sys::U8Path(source).extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // A MODEL (File > Import > Model's formats) is several files the
        // importer names - the .iscn, .meshlib, Materials/*.imat, .iskel, ... -
        // so dest is their DIRECTORY. The job's output lists them, names the
        // .iscn (Editor.InstantiatePrefab places it) and reports what the
        // writer kept: an existing .imat is never overwritten and the previous
        // .iscn's material bindings are carried over (a reimport).
        static constexpr const char* kModelExts[] = { ".obj", ".gltf", ".glb", ".pmx", ".vrm", ".fbx",
                                                      ".dae", ".3ds", ".ply", ".stl" };
        if (std::any_of(std::begin(kModelExts), std::end(kModelExts), [&](const char* e) { return ext == e; }))
        {
            const std::string dir = opts ? opts->get_or<std::string>("dest", "") : std::string();
            std::string dirExt = Sys::PathToUtf8(Sys::U8Path(dir).extension());
            for (char& c : dirExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (dir.empty() || dirExt == ".iscn" || dirExt == ".itex" || dirExt == ".imat")
                Fail("Import", "bad_value", "Import(model, {dest = \"<directory>\"}) - a model is several files "
                                            "(.iscn, .meshlib, Materials/*.imat, ...), named by the importer");
            ScriptedImport s;
            s.model               = true;
            s.item.sourcePath     = source;
            s.item.outputPath     = dir;
            s.modelRequest.source = source;
            s.modelRequest.outDir = dir;
            readModelImportOptions("Import", *opts, s.modelRequest);
            s.dialog              = opts->get_or("dialog", false);     // the Import Model options dialog first
            if (s.dialog && s.modelRequest.dryRun)
                Fail("Import", "bad_value", "dialog and dryRun together - the dialog leads to the import");
            if (s.dialog && (m_importDialog || ReviewPending()))
                Fail("Import", "busy", "another import is waiting for its dialog");
            s.requestId           = Logger::CurrentRequestId();
            s.jobId               = AdmitJob("import", source + " -> " + dir + "/", s.requestId);
            m_scriptedImports.push_back(std::move(s));
            return m_scriptedImports.back().jobId;
        }

        const std::vector<const char*> exts = Resource::TextureImporter().GetSourceExtensions();
        if (std::none_of(exts.begin(), exts.end(), [&](const char* e) { return ext == e; }))
            Fail("Import", "unsupported_source",
                 "'" + ext + "' - a texture source is .png .jpg .jpeg .tga .bmp or .hdr; a model source is "
                 ".obj .gltf .glb .pmx .vrm .fbx .dae .3ds .ply or .stl");
        const std::string dest = opts ? opts->get_or<std::string>("dest", "") : std::string();
        if (!dest.ends_with(".itex"))
            Fail("Import", "bad_value", "Import(source, {dest = \"<path>.itex\"}) - name the destination");

        PendingTextureImport item;
        item.sourcePath      = source;
        item.outputPath      = dest;
        item.suggestedFormat = Resource::TextureImporter::SuggestFormat(source);
        item.selectedFormat  = item.suggestedFormat;
        if (opts) readTextureImportOptions("Import", *opts, item);   // format, maxSize, mipFilter, flipGreen, mips

        ScriptedImport s;
        s.item      = std::move(item);
        s.requestId = Logger::CurrentRequestId();
        s.jobId     = AdmitJob("import", source + " -> " + dest, s.requestId);
        m_scriptedImports.push_back(std::move(s));
        return m_scriptedImports.back().jobId;
    });

    // ---- Reimport ----
    // Editor.Reimport(model, {source, renames, geometryRenames, dryRun, review, dialog}):
    // the model's .iscn back through the import it came from - its SR record's source (or
    // `source`: Reimport From..., the record follows it) into its own folder under its own
    // name. A job, like Editor.Import's, whose output adds `diff` (what happened to the
    // Meshes and sections) and `sourceRecord`. review = true is the Content Browser's flow:
    // when the report asks for it (a Mesh or a section gone, a geometry-matched rename) the
    // review popup opens and the job finishes when it is answered - the popup's buttons, or
    // Editor.ReimportReview.
    ed.set_function("Reimport", [this, readModelImportOptions, readTextureImportOptions](const std::string& model,
                                                               sol::optional<sol::table> opts) -> std::uint64_t
    {
        std::string ext = Sys::PathToUtf8(Sys::U8Path(model).extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // A TEXTURE: its .meta's import record (or, for a file without one, the guess: the
        // format its DXGI format names, a same-named image beside it) with only the keys given
        // changed: {source, format, maxSize, mipFilter, flipGreen, mips}. The job is
        // Editor.Import's; no source anywhere -> no_source_record, a source not there ->
        // source_missing. The record follows what was used.
        if (ext == ".itex")
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(Sys::U8Path(model), ec))
                Fail("Reimport", "load_failed", "no such texture: " + model);
            const Resource::TextureImportRecord rec = Resource::TextureImportRecordFor(model);
            ScriptedImport s;
            s.reimport            = model;
            s.item.outputPath     = model;
            s.item.sourcePath     = rec.source;
            s.item.options        = rec.settings;
            s.item.selectedFormat = rec.settings.format;
            bool formatGiven = false;
            if (opts)
            {
                s.sourceOverride = opts->get_or<std::string>("source", "");
                if (!s.sourceOverride.empty()) s.item.sourcePath = s.sourceOverride;
                formatGiven = (*opts)["format"].valid() && (*opts)["format"].get_type() != sol::type::lua_nil;
                readTextureImportOptions("Reimport", *opts, s.item);
            }
            if (!rec.known && !formatGiven && !s.item.sourcePath.empty())
                s.item.selectedFormat = Resource::TextureImporter::SuggestFormat(s.item.sourcePath);
            s.item.suggestedFormat = s.item.selectedFormat;
            s.requestId = Logger::CurrentRequestId();
            s.jobId     = AdmitJob("import", "reimport " + model, s.requestId);
            m_scriptedImports.push_back(std::move(s));
            return m_scriptedImports.back().jobId;
        }
        if (ext != ".iscn")
            Fail("Reimport", "bad_value", "Reimport(path) - a model's .iscn or a texture's .itex, not '" + model + "'");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(Sys::U8Path(model), ec))
            Fail("Reimport", "load_failed", "no such model: " + model);
        ScriptedImport s;
        s.model    = true;
        s.reimport = model;
        if (opts)
        {
            s.sourceOverride = opts->get_or<std::string>("source", "");
            s.review         = opts->get_or("review", false);
            s.dialog         = opts->get_or("dialog", false);   // Reimport With Options...
            readModelImportOptions("Reimport", *opts, s.modelRequest);
            if ((s.review || s.dialog) && s.modelRequest.dryRun)
                Fail("Reimport", "bad_value", "review / dialog and dryRun together - they lead to the write");
            if (s.dialog && !s.sourceOverride.empty())
                Fail("Reimport", "bad_value", "dialog and source together - the dialog reimports from the record");
        }
        if (s.dialog && (m_importDialog || ReviewPending()))
            Fail("Reimport", "busy", "another import is waiting for its dialog");
        if (s.review && ReviewPending())
            Fail("Reimport", "busy", "another re-import is waiting for its review" +
                 (m_reimportReview ? " (" + m_reimportReview->model + ")" : std::string(" (being prepared)")));
        s.requestId = Logger::CurrentRequestId();
        s.jobId     = AdmitJob("import", "reimport " + model, s.requestId);
        m_scriptedImports.push_back(std::move(s));
        return m_scriptedImports.back().jobId;
    });

    // Editor.DropFiles(paths, {model = <.iscn>}): what files dropped on the window from the
    // OS do (WM_DROPFILES): model sources onto @p model (as if dropped on its Content
    // Browser tile: its Reimport From, the review when needed), else the Import Model
    // dialog into the Content Browser's folder; images the texture import's review.
    // Returns what it did.
    ed.set_function("DropFiles", [this](sol::table paths, sol::optional<sol::table> opts) -> std::string
    {
        std::vector<std::string> list;
        for (const auto& [k, v] : paths)
        {
            if (v.get_type() != sol::type::string) Fail("DropFiles", "bad_value", "DropFiles({<path>, ...})");
            list.push_back(v.as<std::string>());
        }
        if (list.empty()) Fail("DropFiles", "bad_value", "DropFiles({<path>, ...}) - no file");
        std::string model = opts ? opts->get_or<std::string>("model", "") : std::string();
        if (!model.empty())
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(Sys::U8Path(model), ec))
                Fail("DropFiles", "load_failed", "no such model: " + model);
        }
        return HandleDroppedFiles(list, model);
    });

    // Editor.ImportDialog() -> the open "Import Model" dialog ({sources, dir,
    // reimport, options = "IM ..."}) or nil. Editor.ImportDialog{accept = true |
    // false, options = {...}} answers it as its buttons do (options: set before
    // accepting, the rest as the dialog shows them). Returns what it answered.
    ed.set_function("ImportDialog", [this, importDialogState](sol::optional<sol::table> opts,
                                                              sol::this_state ts) -> sol::object
    {
        sol::state_view view(ts);
        if (!opts) return importDialogState(view);
        if (!m_importDialog)
            Fail("ImportDialog", "no_dialog", "no import is waiting for its dialog");
        const sol::optional<bool> accept = (*opts)["accept"];
        if (!accept)
            Fail("ImportDialog", "bad_value", "ImportDialog{accept = true | false [, options = {...}]}");
        if (const sol::object o = (*opts)["options"]; o.valid() && o.get_type() != sol::type::lua_nil)
        {
            if (o.get_type() != sol::type::table)
                Fail("ImportDialog", "bad_value", "options = { normals = \"compute\", ... }");
            for (const auto& [k, v] : o.as<sol::table>())
            {
                if (k.get_type() != sol::type::string) Fail("ImportDialog", "bad_value", "options: named ones");
                std::string value = v.get_type() == sol::type::boolean ? (v.as<bool>() ? "1" : "0")
                                  : v.get_type() == sol::type::string  ? v.as<std::string>()
                                  : v.get_type() == sol::type::number  ? std::to_string(v.as<double>())
                                                                       : std::string();
                std::string why;
                if (!Resource::SetImportOption(m_importDialog->options, k.as<std::string>(), value, &why))
                    Fail("ImportDialog", "bad_value", "options: " + why);
            }
        }
        sol::object answered = importDialogState(view);
        AnswerModelImportDialog(*accept);
        return answered;
    });

    // Editor.ReimportReview() -> the open review ({model, source, renamed =
    // {{from, to, key, how, keep}}, removedMeshes, addedMeshes, removedSections})
    // or nil. Editor.ReimportReview{accept = true | false, keep = {<new names>}}
    // answers it as the popup's buttons do - keep: the geometry renames that keep
    // their key (without it: as ticked, all of them). Returns what it answered.
    ed.set_function("ReimportReview", [this, reimportReviewState](sol::optional<sol::table> opts,
                                                                  sol::this_state ts) -> sol::object
    {
        sol::state_view view(ts);
        if (!opts) return reimportReviewState(view);
        if (!m_reimportReview)
            Fail("ReimportReview", "no_review", "no re-import is waiting for a review");
        const sol::optional<bool> accept = (*opts)["accept"];
        if (!accept)
            Fail("ReimportReview", "bad_value", "ReimportReview{accept = true | false [, keep = {<new names>}]}");
        if (const sol::optional<sol::table> keep = (*opts)["keep"])
        {
            std::unordered_set<std::string> names;
            for (const auto& [k, v] : *keep)
                if (v.get_type() == sol::type::string) names.insert(v.as<std::string>());
            const auto& rn = m_reimportReview->report.identity.renamedMeshes;
            for (size_t i = 0; i < rn.size() && i < m_reimportReview->keepRename.size(); ++i)
                m_reimportReview->keepRename[i] = names.count(rn[i].to) ? 1 : 0;
        }
        sol::object answered = reimportReviewState(view);
        AnswerModelReimportReview(*accept);
        return answered;
    });
}

// AttachRefusal / AttachToSocket: the editor's undoable EquipToSocket. The
// FollowSocket's name hash is the authority, its index a cache; FollowSystem composes
// the follower's local transform in the socket's space, so a zero LocalTransform sits
// ON the socket.
std::string EditorLayer::AttachRefusal(Entity item)
{
    if (!m_world) return "no world";
    if (GetPlayState() != ViewportPlayState::Stopped) return "the editor is playing - Stop first";
    if (item == NullEntity || !m_world->IsAlive(item)) return "the entity to attach is not alive";
    if (m_world->GetComponent<Parent>(item))
        return "the entity has a parent - a follower is a root (FollowSystem and the parent would both place it); "
               "unparent it first";
    if (const SavedBy s = SavedByOf(*m_world, item); s.root != NullEntity && !(s.isRoot && !s.prefab))
        return "the entity is inside the instance of '" + s.path + "': the scene would not save its attachment";
    return {};
}

std::string EditorLayer::AttachToSocket(Entity item, Entity target, const std::string& socket)
{
    if (std::string why = AttachRefusal(item); !why.empty()) return why;
    if (target == NullEntity || !m_world->IsAlive(target)) return "the socket's owner is not alive";
    if (item == target) return "an entity cannot follow its own socket";
    for (Entity p = target; p != NullEntity;)
    {
        if (p == item) return "the socket's owner is inside the entity to attach";
        const Parent* par = m_world->GetComponent<Parent>(p);
        p = par ? par->entity : NullEntity;
    }
    // An owner that already follows the item (a sword on the hand, then the character
    // onto the sword's socket) would resolve against the item while the item resolves
    // against it.
    if (FollowsEntity(*m_world, target, item))
        return "the socket's owner follows the entity to attach (directly or through what it hangs on) - they "
               "would follow each other";
    const SocketComponent* sc = m_world->GetComponent<SocketComponent>(target);
    const uint32_t hash = SocketComponent::HashName(socket.c_str());
    const uint32_t idx  = sc ? sc->FindByName(hash) : SocketComponent::kNotFound;
    if (idx == SocketComponent::kNotFound) return "the owner has no socket '" + socket + "'";

    const std::type_index type(typeid(FollowSocketComponent));
    const auto ed = m_componentEditors.find(type);
    if (ed == m_componentEditors.end() || !ed->second.add || !ed->second.remove)
        return "FollowSocket is not an inspector component here";
    ComponentSerializerRegistry& reg = GetComponentRegistry();
    const bool had = m_world->HasComponent<FollowSocketComponent>(item);
    if (!had) ed->second.add(*m_world, item);
    const std::string before = Editor::Undo::SerializeComponentBlob(reg, *m_world, item, type);
    FollowSocketComponent* fs = m_world->GetComponent<FollowSocketComponent>(item);
    if (!fs) return "the FollowSocket did not add";
    fs->target.BindAndStamp(*m_world, target);
    fs->socketNameHash  = hash;
    fs->socketIndex     = idx;
    fs->cachedSocketGen = 0;   // the first tick resolves it
    const std::string after = Editor::Undo::SerializeComponentBlob(reg, *m_world, item, type);
    LocalTransform* lt = m_world->GetComponent<LocalTransform>(item);
    const LocalTransform ltBefore = lt ? *lt : LocalTransform{};
    if (lt) *lt = LocalTransform{};
    if (UndoEnabled())
    {
        SyncedEditContext();
        const EntityHandle h = m_world->MakeHandle(item);
        m_editHistory.BeginMacro("Attach to Socket");
        if (!had)
            PushEdit(std::make_unique<Editor::Undo::AddComponentAction>(h, ed->second.add, ed->second.remove,
                                                                         "Follow Socket"));
        PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(h, type, before, after, "Follow Socket"));
        if (lt) PushEdit(std::make_unique<Editor::Undo::TransformEditAction>(h, ltBefore, *lt));
        m_editHistory.EndMacro();
    }
    LOG_INFO("[EDITOR] attach: entity %u follows socket '%s' (index %u) of entity %u", item, socket.c_str(), idx,
             target);
    return {};
}

// Editor.Play / Pause / Step / Stop's job, every frame from ProcessPendingActions
// - which runs AFTER the frame's PlayModeController::BeginFrame. A request
// queued in frame N (the control channel's run, a console chunk) is applied by
// the first BeginFrame after it, so the first check in a LATER frame sees its
// outcome: the mode it leads to, or another mode because a request queued after
// it in the same frame replaced it in the controller's one slot.
void EditorLayer::ProcessScriptedPlay()
{
    if (!m_scriptedPlay.jobId || !m_playModeController) return;
    if (Logger::CurrentFrame() == m_scriptedPlay.queuedFrame) return;   // no BeginFrame since
    const std::uint64_t job  = m_scriptedPlay.jobId;
    const EngineMode    mode = m_playModeController->Mode();
    m_scriptedPlay.jobId = 0;
    nlohmann::json o = nlohmann::json::object();
    o["mode"]     = ModeName(mode);
    o["frame"]    = Logger::CurrentFrame();
    o["simTime"]  = m_playModeController->Clock().elapsed;
    o["entities"] = m_world ? m_world->GetEntities().size() : 0;
    if (mode != m_scriptedPlay.target)
    {
        Harness::Jobs::Fail(job, "superseded",
                            std::string("another request replaced this one before the frame boundary applied it "
                                        "(the toolbar, Ctrl+P, -play_frames, a scene load's Stop): the editor is ") +
                            ModeName(mode) + ", not " + ModeName(m_scriptedPlay.target), {}, o.dump());
        return;
    }
    if (mode == EngineMode::Playing) o["resumed"] = m_scriptedPlay.resumed;
    Harness::Jobs::Succeed(job, o.dump());
}
