#include "Scripting/LuaEntityBindings.h"
#include "ECS/IKRigAuthoring.h"

#include "ECS/ECS.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/FollowComponents.h"
#include "ECS/GuidRegistry.h"
#include "ECS/HierarchyComponents.h"     // LocalTransform, GlobalTransform
#include "ECS/StaticMeshComponents.h"
#include "ECS/PhysicsComponents.h"       // ColliderComponent
#include "ECS/TagComponent.h"
#include "ECS/TODComponents.h"           // SunLightTag, MoonLightTag, TODLightComponent
#include "Resource/AssetRefs.h"
#include "Resource/ComponentSerializers.h"
#include "Reflection/ReflectSerializer.h"   // WriteKeys: a described component is written field by field
#include "Resource/ComponentWrite.h"     // the runtime-state carry around a write
#include "Resource/AuthoringSchema.h"
#include "Scene/TransformSystem.h"
#include "Scripting/LuaMathTypes.h"      // LuaVec3 (a Vec3 is a 3-vector value)
#include "Scripting/ScriptComponent.h"
#include "UI/UICanvas.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)
#include "Scripting/LuaDoc.h"        // Entity.* is described as it is bound
#include "Scripting/LuaEntityWrite.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>
#include <chrono>
#include <deque>
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    // One component as Lua: a list of lines, each line its key/value pairs (a
    // serializer writes one line per slot, e.g. one per attached script).
    using Line = std::vector<std::pair<std::string, sol::object>>;

    const std::string& EntitySession()
    {
        // One identity per process, shared by every VM/connection; no PID reuse.
        // Do not consume Guid::Generate: -seed makes that stream repeatable,
        // and reads must not change the GUIDs minted by later authoring.
        static const std::string s_id = [] {
            FILETIME start{}, exit{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &start, &exit, &kernel, &user))
                throw sol::error("Entity: session_unavailable: cannot read process creation time");
            char text[64];
            std::snprintf(text, sizeof(text), "%lu-%08lx%08lx", GetCurrentProcessId(), start.dwHighDateTime, start.dwLowDateTime);
            return std::string(text);
        }();
        return s_id;
    }

    void CheckContext(const char* api, const World& world, const sol::optional<sol::table>& opts)
    {
        if (!opts) return;
        const sol::optional<std::string> session = opts->get<sol::optional<std::string>>("session");
        if (session && *session != EntitySession())
            throw sol::error(std::string(api) + ": stale_session: entity belongs to another engine process; find it again");
        const sol::optional<double> epoch = opts->get<sol::optional<double>>("epoch");
        if (epoch && *epoch != static_cast<double>(world.Epoch()))
            throw sol::error(std::string(api) + ": stale_epoch: world cleared, reloaded or restored since lookup; find it again");
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // ==== Values ====
    // A GUID is written as hex (Guid: hex=..., AttachmentRef <prefix>Guid=...).
    // It must stay a string: an all-digit hex id would otherwise be read as a
    // number and lose bits past 2^53.
    bool IsGuidKey(const std::string& key)
    {
        return key == "hex" || (key.size() >= 4 && key.compare(key.size() - 4, 4, "Guid") == 0);
    }

    // One number as the serializer wrote it: an integer when the text is one,
    // else a float; false when the text is not entirely a number.
    bool ParseNumber(const std::string& v, bool& isInt, long long& i, double& d)
    {
        if (v.empty()) return false;
        // strtod also takes "inf", "nan" and hex floats ("0x1p4"); a serializer
        // writes none of those as a number, but a name could be "Inf".
        const unsigned char c0 = static_cast<unsigned char>(v[0]);
        if (!(std::isdigit(c0) || c0 == '-' || c0 == '+' || c0 == '.')) return false;
        if (v.find_first_of("xXnNiI") != std::string::npos) return false;
        const char* s = v.c_str();
        char* end = nullptr;
        errno = 0;
        i = std::strtoll(s, &end, 10);
        if (end && *end == '\0' && errno == 0) { isInt = true; return true; }
        end = nullptr;
        d = std::strtod(s, &end);
        if (!end || *end != '\0' || end == s) return false;
        isInt = false;
        return true;
    }

    sol::object NumberObject(sol::state_view& lua, bool isInt, long long i, double d)
    {
        return isInt ? sol::make_object(lua, static_cast<lua_Integer>(i)) : sol::make_object(lua, d);
    }

    // A serialized field value as Lua: numbers as numbers, "a_b_c" of numbers
    // (the serializers' vector form, color=%.4f_%.4f_%.4f) as a list of numbers,
    // true / false as booleans, everything else as its percent-DECODED text
    // (names and paths are written percent-encoded). GUIDs stay text.
    sol::object FieldValue(sol::state_view& lua, const std::string& key, const std::string& v)
    {
        if (IsGuidKey(key)) return sol::make_object(lua, v);
        if (v == "true")  return sol::make_object(lua, true);
        if (v == "false") return sol::make_object(lua, false);
        bool isInt = false;
        long long i = 0;
        double d = 0.0;
        if (ParseNumber(v, isInt, i, d)) return NumberObject(lua, isInt, i, d);
        if (v.find('_') != std::string::npos)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            for (;;)
            {
                const size_t us = v.find('_', start);
                parts.push_back(v.substr(start, us == std::string::npos ? std::string::npos : us - start));
                if (us == std::string::npos) break;
                start = us + 1;
            }
            sol::table list = lua.create_table();
            bool numeric = parts.size() >= 2;
            for (size_t k = 0; numeric && k < parts.size(); ++k)
            {
                if (!ParseNumber(parts[k], isInt, i, d)) numeric = false;
                else list[k + 1] = NumberObject(lua, isInt, i, d);
            }
            if (numeric) return list;
        }
        return sol::make_object(lua, Resource::AssetRefs::PercentDecode(v));
    }

    // A serializer's text ("  Tag: k=v k=v\n", one line per slot) as lines.
    // Tokens are split like SceneSerializer::ParseComponentLine - on
    // whitespace, at the first '='.
    std::vector<Line> ParseSerialized(sol::state_view& lua, const std::string& text)
    {
        std::vector<Line> lines;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            Line l;
            std::istringstream tok(line.substr(colon + 1));
            std::string token;
            while (tok >> token)
            {
                const auto eq = token.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                const std::string key = token.substr(0, eq);
                l.emplace_back(key, FieldValue(lua, key, token.substr(eq + 1)));
            }
            lines.push_back(std::move(l));
        }
        return lines;
    }

    // ==== Dedicated read-only readers ====
    // Four registered serializers are not read-only: each calls ECS::EnsureGuidOn, stamping a
    // GuidComponent on the entity a reference points at (the same four WorldStateDump skips).
    // Reading must never change what it reads, so these components are read here: same keys
    // as their serializer, the reference as the target's live id (more use than the GUID).

    // The entity a reference names, found without changing anything: the GUID's
    // registered owner (GuidRegistry::Find is a lookup), else a runtime-bound
    // cache that is still valid. AttachmentRef::Resolve would refresh that cache;
    // the serializers would stamp a GUID on the target.
    Entity PeekRef(World& w, const AttachmentRef& r)
    {
        if (r.ownerGuid.IsValid())
        {
            const Entity e = ECS::GuidRegistry::Get().Find(r.ownerGuid);
            return (e != NullEntity && w.IsAlive(e)) ? e : NullEntity;
        }
        if (r.cachedOwner != NullEntity && w.IsAlive(r.cachedOwner)
            && w.GetGeneration(r.cachedOwner) == r.cachedGeneration)
            return r.cachedOwner;
        return NullEntity;
    }

    // `key` = the target's live id (0 when it resolves to nothing) and, when
    // one exists, `key`Guid = its GUID (the stored one, else the target's own).
    void PutRef(sol::state_view& lua, World& w, Line& l, const std::string& key, Entity target, ECS::Guid guid)
    {
        if (!guid.IsValid() && target != NullEntity) guid = ECS::GuidRegistry::Get().ReverseFind(target);
        if (target != NullEntity) l.emplace_back(key, sol::make_object(lua, LuaId::Out(w, target)));
        else                      l.emplace_back(key, sol::make_object(lua, static_cast<lua_Integer>(0)));
        if (guid.IsValid()) l.emplace_back(key + "Guid", sol::make_object(lua, guid.ToHex()));
    }

    std::vector<Line> ReadFollowSocket(sol::state_view& lua, World& w, Entity e)
    {
        const auto* fs = w.GetComponent<FollowSocketComponent>(e);
        if (!fs) return {};
        Line l;
        l.emplace_back("socket", sol::make_object(lua, static_cast<lua_Integer>(fs->socketIndex)));
        l.emplace_back("socketHash", sol::make_object(lua, static_cast<lua_Integer>(fs->socketNameHash)));
        PutRef(lua, w, l, "target", PeekRef(w, fs->target), fs->target.ownerGuid);
        return { std::move(l) };
    }

    std::vector<Line> ReadFollowEntity(sol::state_view& lua, World& w, Entity e)
    {
        const auto* fe = w.GetComponent<FollowEntityComponent>(e);
        if (!fe) return {};
        Line l;
        PutRef(lua, w, l, "target", PeekRef(w, fe->target), fe->target.ownerGuid);
        return { std::move(l) };
    }

    std::vector<Line> ReadCvParent(sol::state_view& lua, World& w, Entity e)
    {
        const auto* p = w.GetComponent<UI::UIParent>(e);
        if (!p) return {};
        Entity parent = (p->parent != NullEntity && w.IsAlive(p->parent)) ? p->parent : NullEntity;
        if (parent == NullEntity && p->parentGuid.IsValid())
        {
            const Entity g = ECS::GuidRegistry::Get().Find(p->parentGuid);
            if (g != NullEntity && w.IsAlive(g)) parent = g;
        }
        Line l;
        PutRef(lua, w, l, "parent", parent, p->parentGuid);
        return { std::move(l) };
    }

    sol::object ScriptVar(sol::state_view& lua, World& w, const ScriptVarValue& v)
    {
        switch (v.type)
        {
        case ScriptVarType::Float: return sol::make_object(lua, static_cast<double>(v.data.f));
        case ScriptVarType::Int:   return sol::make_object(lua, static_cast<lua_Integer>(v.data.i));
        case ScriptVarType::Bool:  return sol::make_object(lua, v.data.b);
        case ScriptVarType::Float3:
        case ScriptVarType::Color:
        {
            sol::table t = lua.create_table();
            for (int k = 0; k < 3; ++k) t[k + 1] = static_cast<double>(v.data.v3[k]);
            return t;
        }
        case ScriptVarType::Entity:
        {
            // The live target: by the stored GUID, else the session-local id.
            Entity t = NullEntity;
            if (!v.str.empty())
                t = ECS::GuidRegistry::Get().Find(ECS::Guid::FromHex(v.str));
            else if (v.data.entity != 0)
                t = static_cast<Entity>(v.data.entity);
            if (t != NullEntity && w.IsAlive(t)) return sol::make_object(lua, LuaId::Out(w, t));
            return sol::make_object(lua, static_cast<lua_Integer>(0));
        }
        case ScriptVarType::String:
        case ScriptVarType::Asset:
        default:
            return sol::make_object(lua, v.str);
        }
    }

    std::vector<Line> ReadScript(sol::state_view& lua, World& w, Entity e)
    {
        const auto* sc = w.GetComponent<ScriptComponent>(e);
        if (!sc) return {};
        if (sc->scripts.empty())
        {
            Line l;
            l.emplace_back("slots", sol::make_object(lua, static_cast<lua_Integer>(0)));
            return { std::move(l) };
        }
        std::vector<Line> out;
        for (const ScriptInstance& si : sc->scripts)
        {
            Line l;
            l.emplace_back("path", sol::make_object(lua, si.scriptPath));
            l.emplace_back("enabled", sol::make_object(lua, static_cast<lua_Integer>(si.enabled ? 1 : 0)));
            // `var.<name>` as the serializer spells the key; sorted, because
            // `vars` is an unordered_map.
            std::vector<const std::pair<const std::string, ScriptVarValue>*> vars;
            for (const auto& kv : si.vars) vars.push_back(&kv);
            std::sort(vars.begin(), vars.end(), [](auto* a, auto* b) { return a->first < b->first; });
            for (const auto* kv : vars) l.emplace_back("var." + kv->first, ScriptVar(lua, w, kv->second));
            out.push_back(std::move(l));
        }
        return out;
    }

    using Reader = std::vector<Line> (*)(sol::state_view&, World&, Entity);
    struct Dedicated { const char* tag; Reader read; };
    const Dedicated kDedicated[] = {
        { "CvParent",     ReadCvParent },
        { "FollowEntity", ReadFollowEntity },
        { "FollowSocket", ReadFollowSocket },
        { "Script",       ReadScript },
    };

    Reader DedicatedReader(const std::string& tag)
    {
        for (const Dedicated& d : kDedicated)
            if (tag == d.tag) return d.read;
        return nullptr;
    }

    // ==== What to read ====
    // `whole` = components read in full; `keys` = components read for some
    // fields only ("Tag.key"). A tag in both is read in full.
    struct Select
    {
        bool all = false;
        std::set<std::string> whole;
        std::map<std::string, std::set<std::string>> keys;

        bool Empty() const { return !all && whole.empty() && keys.empty(); }
        bool Wants(const std::string& tag) const { return all || whole.count(tag) || keys.count(tag); }
        const std::set<std::string>* KeysFor(const std::string& tag) const
        {
            if (all || whole.count(tag)) return nullptr;
            auto it = keys.find(tag);
            return it == keys.end() ? nullptr : &it->second;
        }
    };

    // Not a serializer tag: LocalTransform lives on the .iscene N line, so the
    // registry has no entry for it. Readable and writable under this name.
    constexpr const char* kTransformTag = "Transform";
    constexpr const char* kAuthoringTag = "Authoring";

    // One entry of ReadableComponents: its tag, serializer and dedicated reader (null when none).
    struct Readable { std::string tag; const ComponentSerializer* s; Reader dedicated; };

    const ComponentSerializer* KnownTag(const std::string& tag)
    {
        return GetComponentRegistry().FindByTag(tag);
    }

    bool IsPseudoTag(const std::string& tag) { return tag == kTransformTag || tag == kAuthoringTag; }

    [[noreturn]] void UnknownTag(const char* fn, const std::string& tag)
    {
        throw sol::error(std::string(fn) + ": unknown component '" + tag +
                         "' (Entity.components() lists them; tags are case-sensitive)");
    }

    // "Tag" or "Tag.key", as a string or a list of them, into `sel`.
    void AddSelect(const char* fn, const sol::object& o, Select& sel, bool fieldsOnly)
    {
        std::vector<std::string> items;
        if (o.is<std::string>()) items.push_back(o.as<std::string>());
        else if (o.is<sol::table>())
            for (const auto& kv : o.as<sol::table>())
                if (kv.second.is<std::string>()) items.push_back(kv.second.as<std::string>());
        for (const std::string& item : items)
        {
            const size_t dot = item.find('.');
            const std::string tag = item.substr(0, dot);
            if (!KnownTag(tag) && !IsPseudoTag(tag)) UnknownTag(fn, tag);
            if (dot == std::string::npos)
            {
                if (fieldsOnly)
                    throw sol::error(std::string(fn) + ": fields wants 'Tag.key', got '" + item + "'");
                sel.whole.insert(tag);
            }
            else sel.keys[tag].insert(item.substr(dot + 1));
        }
    }

    // Lines as a Lua value: a table of fields for one line, a list of them for
    // several. `keys` (optional) keeps only those fields; the keys it names that
    // no line has are added to `missing` as "Tag.key".
    sol::object LinesToLua(sol::state_view& lua, const std::string& tag, const std::vector<Line>& lines,
                           const std::set<std::string>* keys, std::vector<std::string>& missing)
    {
        std::set<std::string> seen;
        std::vector<sol::table> tables;
        for (const Line& line : lines)
        {
            sol::table t = lua.create_table();
            for (const auto& [k, v] : line)
            {
                if (keys && !keys->count(k)) continue;
                seen.insert(k);
                t[k] = v;
            }
            tables.push_back(t);
        }
        if (keys)
            for (const std::string& k : *keys)
                if (!seen.count(k)) missing.push_back(tag + "." + k);
        if (tables.size() == 1) return tables.front();
        sol::table list = lua.create_table();
        for (size_t i = 0; i < tables.size(); ++i) list[i + 1] = tables[i];
        return list;
    }

    // The components of `e` that `sel` asks for, ascending by tag. A component
    // counts only when its serializer's own `has` says so - the rule SaveScene
    // and WorldStateDump apply (MaterialInline is "absent" on an entity whose
    // MaterialRef owns the material, though a MaterialComponent is there).
    std::vector<Readable> ReadableComponents(World& w, Entity e, const Select& sel)
    {
        const auto& all = GetComponentRegistry().All();
        std::vector<Readable> out;
        for (const std::type_index& ti : w.GetComponentTypeIndices(e))
        {
            auto it = all.find(ti);
            if (it == all.end() || !it->second.serialize || !it->second.has) continue;   // DERIVED / unregistered
            const std::string& tag = it->second.tag;
            if (!sel.Wants(tag)) continue;
            if (!it->second.has(w, e)) continue;
            out.push_back({ tag, &it->second, DedicatedReader(tag) });
        }
        std::sort(out.begin(), out.end(), [](const Readable& a, const Readable& b) { return a.tag < b.tag; });
        return out;
    }

    sol::table ListOf(sol::state_view& lua, const std::vector<std::string>& v)
    {
        sol::table t = lua.create_table();
        for (size_t i = 0; i < v.size(); ++i) t[i + 1] = v[i];
        return t;
    }

    sol::table Numbers(sol::state_view& lua, std::initializer_list<float> v)
    {
        sol::table t = lua.create_table();
        int i = 0;
        for (float f : v) t[++i] = static_cast<double>(f);
        return t;
    }

    // The Transform pseudo component: LocalTransform (position, rotation as a
    // quaternion xyzw, scale) plus the world position GlobalTransform holds.
    Line TransformLine(sol::state_view& lua, const LocalTransform& lt, const GlobalTransform* g)
    {
        Line l;
        l.emplace_back("position", Numbers(lua, { lt.translation.x, lt.translation.y, lt.translation.z }));
        l.emplace_back("rotation", Numbers(lua, { lt.rotation.x, lt.rotation.y, lt.rotation.z, lt.rotation.w }));
        l.emplace_back("scale", Numbers(lua, { lt.scale.x, lt.scale.y, lt.scale.z }));
        if (g) l.emplace_back("worldPosition", Numbers(lua, { g->matrix._41, g->matrix._42, g->matrix._43 }));
        return l;
    }

    Line AuthoringLine(sol::state_view& lua, World& w, Entity e)
    {
        auto ref = [&](Entity target) -> sol::object {
            if (target == NullEntity || !w.IsAlive(target)) return sol::make_object(lua, sol::lua_nil);
            return lua.create_table_with("id", LuaId::Out(w, target), "session", EntitySession(), "epoch", w.Epoch());
        };
        Line result;
        const auto* parent = w.GetComponent<Parent>(e);
        result.emplace_back("parent", ref(parent ? parent->entity : NullEntity));
        Entity root = e;
        if (const auto* member = w.GetComponent<PrefabInstanceMember>(e)) root = member->root;
        const auto* prefab = w.IsAlive(root) ? w.GetComponent<PrefabSourcePath>(root) : nullptr;
        result.emplace_back("prefabRoot", ref(prefab ? root : NullEntity));
        result.emplace_back("prefabSource", sol::make_object(lua, prefab ? prefab->path : std::string()));
        sol::table bounds = lua.create_table_with("available", false, "approximate", true,
            "space", "world", "source", "cached WorldAabb; last transform propagation; not exact skinned/WPO geometry");
        if (const auto* box = w.GetComponent<WorldAabb>(e))
        {
            bounds["available"] = true;
            bounds["min"] = Numbers(lua, {box->min.x, box->min.y, box->min.z});
            bounds["max"] = Numbers(lua, {box->max.x, box->max.y, box->max.z});
        }
        result.emplace_back("bounds", bounds);
        sol::table slots = lua.create_table();
        int index = 0;
        if (const auto* material = w.GetComponent<MaterialSourcePath>(e))
            slots[++index] = lua.create_table_with("slot", "default", "path", material->path, "kind", "resolved_source");
        if (const auto* mesh = w.GetComponent<StaticMeshComponent>(e))
            for (const auto& item : mesh->slotOverrides)
            {
                char key[17]; std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(item.sectionKey));
                slots[++index] = lua.create_table_with("slot", std::string(key), "path", item.material, "kind", "override");
            }
        result.emplace_back("materialSlots", slots);
        result.emplace_back("materialScope", sol::make_object(lua, "entity source and authored overrides; inherited model defaults require asset catalog"));
        return result;
    }

    sol::table EntityRow(sol::state_view& lua, World& w, Entity e, const Select& sel)
    {
        sol::table row = lua.create_table();
        row["id"]   = LuaId::Out(w, e);
        row["session"] = EntitySession();
        row["epoch"] = w.Epoch();
        row["name"] = w.GetName(e);
        // A system-owned helper (the editor camera, a preview stage, a game-flow
        // runtime entity) is live ECS state the scene save omits - not scene data.
        if (w.HasComponent<SceneTransientTag>(e)) row["transient"] = true;
        if (sel.Empty()) return row;
        std::vector<std::string> missing;
        sol::table comps = lua.create_table();
        for (const Readable& r : ReadableComponents(w, e, sel))
        {
            std::vector<Line> lines;
            if (r.dedicated) lines = r.dedicated(lua, w, e);
            else
            {
                std::ostringstream ss;
                r.s->serialize(w, e, ss);
                lines = ParseSerialized(lua, ss.str());
            }
            if (lines.empty()) continue;      // the serializer chose to write nothing
            comps[r.tag] = LinesToLua(lua, r.tag, lines, sel.KeysFor(r.tag), missing);
        }
        if (sel.Wants(kTransformTag))
            if (const LocalTransform* lt = w.GetComponent<LocalTransform>(e))
                comps[kTransformTag] = LinesToLua(lua, kTransformTag,
                                                  { TransformLine(lua, *lt, w.GetComponent<GlobalTransform>(e)) },
                                                  sel.KeysFor(kTransformTag), missing);
        if (sel.Wants(kAuthoringTag))
            comps[kAuthoringTag] = LinesToLua(lua, kAuthoringTag, {AuthoringLine(lua, w, e)},
                                            sel.KeysFor(kAuthoringTag), missing);
        row["components"] = comps;
        if (!missing.empty()) row["missing"] = ListOf(lua, missing);
        return row;
    }
}

namespace
{
    // Immutable query snapshots contain JSON values, never Lua references or ECS pointers.
    using PageJson = nlohmann::json;
    using PageClock = std::chrono::steady_clock;

    struct EntitySnapshot
    {
        std::string revision;
        uint32_t epoch = 0;
        uint64_t frame = 0;
        size_t scanned = 0, bytes = 0;
        double scanMs = 0;
        PageClock::time_point created;
        std::vector<PageJson> rows;
    };
    struct EntityPages { std::deque<EntitySnapshot> snapshots; };

    [[noreturn]] void PageError(const char* code, const std::string& message)
    {
        throw sol::error(std::string("Entity.page: ") + code + ": " + message);
    }

    PageJson PageValue(const sol::object& value, int depth = 0)
    {
        if (depth > 12) PageError("snapshot_too_large", "projection exceeds nesting limit; narrow fields");
        switch (value.get_type())
        {
        case sol::type::boolean: return value.as<bool>();
        case sol::type::number:
            if (value.is<lua_Integer>()) return value.as<lua_Integer>();
            if (!std::isfinite(value.as<double>())) PageError("bad_value", "projection contains a non-finite number");
            return value.as<double>();
        case sol::type::string: return value.as<std::string>();
        case sol::type::table:
        {
            const sol::table t = value.as<sol::table>();
            const bool array = t.size() > 0;
            PageJson result = array ? PageJson::array() : PageJson::object();
            if (array)
                for (size_t i = 1; i <= t.size(); ++i) result.push_back(PageValue(t.get<sol::object>(i), depth + 1));
            else
                for (const auto& kv : t) result[kv.first.as<std::string>()] = PageValue(kv.second, depth + 1);
            return result;
        }
        default: PageError("bad_value", "projection contains an unsupported value");
        }
    }

    sol::object PageLua(sol::state_view& lua, const PageJson& value)
    {
        if (value.is_null()) return sol::make_object(lua, sol::lua_nil);
        if (value.is_boolean()) return sol::make_object(lua, value.get<bool>());
        if (value.is_number_integer()) return sol::make_object(lua, value.get<lua_Integer>());
        if (value.is_number()) return sol::make_object(lua, value.get<double>());
        if (value.is_string()) return sol::make_object(lua, value.get<std::string>());
        sol::table t = lua.create_table();
        if (value.is_array())
            for (size_t i = 0; i < value.size(); ++i) t[i + 1] = PageLua(lua, value[i]);
        else
            for (auto it = value.begin(); it != value.end(); ++it) t[it.key()] = PageLua(lua, it.value());
        return sol::make_object(lua, t);
    }

    size_t PageInteger(const sol::table& q, const char* key, size_t fallback, size_t low, size_t high)
    {
        const sol::object v = q.get<sol::object>(key);
        if (v.get_type() == sol::type::lua_nil) return fallback;
        if (v.get_type() != sol::type::number) PageError("bad_query", std::string(key) + " must be an integer");
        const double n = v.as<double>();
        if (!std::isfinite(n) || n < low || n > high || std::floor(n) != n)
            PageError("bad_query", std::string(key) + " is outside its integer range");
        return static_cast<size_t>(n);
    }

    sol::table EntityPage(sol::state_view& lua, World& world, EntityPages& store, const sol::table& q)
    {
        const auto start = PageClock::now();
        const size_t limit = PageInteger(q, "limit", 128, 1, 1000);
        const size_t maxBytes = PageInteger(q, "maxBytes", 32768, 1024, 32768);
        const std::set<std::string> options{ "cursor", "limit", "maxBytes", "name", "tag", "component",
                                            "components", "fields", "all", "subtree", "session", "epoch" };
        const sol::object cursorValue = q.get<sol::object>("cursor");
        if (cursorValue.get_type() != sol::type::lua_nil &&
            (cursorValue.get_type() != sol::type::string || cursorValue.as<std::string>().empty()))
            PageError("bad_query", "cursor must be a nonempty string");
        const std::string cursor = q.get_or<std::string>("cursor", "");
        for (const auto& kv : q)
        {
            if (kv.first.get_type() != sol::type::string) PageError("bad_query", "query keys must be strings");
            const auto key = kv.first.as<std::string>();
            if (!options.count(key)) PageError("bad_query", "unknown option " + key);
            if (!cursor.empty() && key != "cursor" && key != "limit" && key != "maxBytes")
                PageError("bad_query", "cursor already defines filters and projection");
        }
        while (!store.snapshots.empty() && (store.snapshots.front().epoch != world.Epoch() ||
               start - store.snapshots.front().created > std::chrono::seconds(120))) store.snapshots.pop_front();

        EntitySnapshot fresh;
        EntitySnapshot* snapshot = nullptr;
        size_t offset = 0;
        if (!cursor.empty())
        {
            const auto split = cursor.rfind(':');
            if (split == std::string::npos) PageError("stale_cursor", "unknown cursor; start a new query");
            const auto id = cursor.substr(0, split);
            const auto pos = cursor.substr(split + 1);
            if (pos.empty() || pos.size() > 7 || pos.find_first_not_of("0123456789") != std::string::npos)
                PageError("stale_cursor", "invalid offset");
            offset = static_cast<size_t>(std::stoull(pos));
            for (auto& s : store.snapshots) if (s.revision == id) snapshot = &s;
            if (!snapshot || offset >= snapshot->rows.size()) PageError("stale_cursor", "expired or unknown snapshot; start a new query");
        }
        else
        {
            CheckContext("Entity.page", world, sol::optional<sol::table>(q));
            const auto name = Lower(q.get_or<std::string>("name", ""));
            const auto tag = q.get_or<std::string>("tag", "");
            const auto component = q.get_or<std::string>("component", "");
            const auto* mustHave = component.empty() ? nullptr : KnownTag(component);
            if (!component.empty() && !mustHave && !IsPseudoTag(component)) PageError("bad_query", "unknown component " + component);
            Select sel;
            AddSelect("Entity.page", q.get<sol::object>("components"), sel, false);
            AddSelect("Entity.page", q.get<sol::object>("fields"), sel, true);
            sel.all = q.get_or("all", false);
            Entity root = NullEntity;
            if (q.get<sol::object>("subtree").get_type() != sol::type::lua_nil)
            {
                root = LuaId::ResolveLive(world, q.get<LuaEntity>("subtree"));
                if (root == NullEntity) PageError("stale_handle", "subtree root is not alive");
            }
            std::vector<Entity> hits;
            for (Entity e : world.GetEntities())
            {
                if (!world.IsAlive(e)) continue;
                ++fresh.scanned;
                if (fresh.scanned % 256 == 0 && PageClock::now() - start > std::chrono::seconds(2))
                    PageError("query_budget", "scan took over 2 seconds; narrow query");
                if (!name.empty() && Lower(world.GetName(e)).find(name) == std::string::npos) continue;
                if (!tag.empty()) { const auto* t = world.GetComponent<TagComponent>(e); if (!t || !t->Has(tag.c_str())) continue; }
                if (mustHave && !(mustHave->has && mustHave->has(world, e))) continue;
                if (component == kTransformTag && !world.HasComponent<LocalTransform>(e)) continue;
                if (root != NullEntity)
                {
                    Entity ancestor = e;
                    std::set<Entity> visited;
                    while (ancestor != root && ancestor != NullEntity && world.IsAlive(ancestor) && visited.insert(ancestor).second)
                    {
                        if (visited.size() % 256 == 0 && PageClock::now() - start > std::chrono::seconds(2))
                            PageError("query_budget", "subtree traversal took over 2 seconds");
                        const auto* parent = world.GetComponent<Parent>(ancestor);
                        ancestor = parent ? parent->entity : NullEntity;
                    }
                    if (ancestor != root) continue;
                }
                hits.push_back(e);
                if (hits.size() > 100000) PageError("snapshot_too_large", "over 100000 rows; narrow filters");
            }
            std::sort(hits.begin(), hits.end());
            for (Entity e : hits)
            {
                auto row = PageValue(EntityRow(lua, world, e, sel));
                fresh.bytes += row.dump().size();
                if (fresh.bytes > 8 * 1024 * 1024) PageError("snapshot_too_large", "over 8 MiB of row JSON; narrow projection or filters");
                fresh.rows.push_back(std::move(row));
                if (PageClock::now() - start > std::chrono::seconds(2)) PageError("query_budget", "snapshot took over 2 seconds; narrow query");
            }
            static uint64_t s_sequence = 0;
            fresh.revision = EntitySession() + "/" + std::to_string(world.Epoch()) + "/" + std::to_string(++s_sequence);
            fresh.epoch = world.Epoch();
            sol::protected_function frame = lua["Engine"]["Frame"];
            const auto observed = frame();
            if (!observed.valid()) PageError("frame_unavailable", "Engine.Frame failed");
            fresh.frame = observed.get<uint64_t>();
            fresh.created = start;
            fresh.scanMs = std::chrono::duration<double, std::milli>(PageClock::now() - start).count();
            snapshot = &fresh;
        }
        PageJson out{ {"schemaVersion", 1}, {"revision", snapshot->revision}, {"frame", snapshot->frame},
                      {"session", EntitySession()}, {"epoch", snapshot->epoch}, {"consistency", "snapshot"},
                      {"totalMatched", snapshot->rows.size()}, {"returned", 0}, {"scanned", snapshot->scanned},
                      {"scanMs", snapshot->scanMs}, {"rows", PageJson::array()}, {"nextCursor", nullptr},
                      {"bytes", 0}, {"elapsedMs", 0.0} };
        auto finish = [&](size_t next) {
            out["returned"] = out["rows"].size();
            out["nextCursor"] = next < snapshot->rows.size() ? PageJson(snapshot->revision + ":" + std::to_string(next)) : PageJson(nullptr);
            out["elapsedMs"] = std::chrono::duration<double, std::milli>(PageClock::now() - start).count();
            // Fixed point includes the decimal digits of the bytes field itself.
            for (int i = 0; i < 4; ++i) out["bytes"] = out.dump().size();
        };
        size_t next = offset;
        while (next < snapshot->rows.size() && out["rows"].size() < limit)
        {
            out["rows"].push_back(snapshot->rows[next]);
            finish(next + 1);
            if (out["bytes"].get<size_t>() > maxBytes) { out["rows"].erase(out["rows"].end() - 1); break; }
            ++next;
        }
        finish(next);
        if (next == offset && offset < snapshot->rows.size()) PageError("row_too_large", "one row exceeds maxBytes; increase it or narrow projection");
        if (out["bytes"].get<size_t>() > maxBytes) PageError("row_too_large", "page metadata exceeds maxBytes");
        if (cursor.empty() && next < fresh.rows.size())
        {
            size_t retained = fresh.bytes;
            for (const auto& s : store.snapshots) retained += s.bytes;
            while (!store.snapshots.empty() && (store.snapshots.size() >= 4 || retained > 16 * 1024 * 1024))
            { retained -= store.snapshots.front().bytes; store.snapshots.pop_front(); }
            store.snapshots.push_back(std::move(fresh));
        }
        return PageLua(lua, out).as<sol::table>();
    }

    // ==== Writes ====
    // Deserialize replaces the whole component (a default T from the key/values, AddComponent
    // overwrites the slot): a key not passed resets to its default and runtime-only fields are
    // lost. So a write is (1) the current values from the component's own serializer, (2) the
    // caller's keys over them, only keys that serializer writes, (3) the deserialize, with the
    // runtime state carried across it by ComponentWrite::RewriteFromKV (LightData's gatherSeq,
    // a Decal's material, an AudioSource's voice and clip, a RigidBody's live body while
    // nothing authored changed, a Collider's generation, a Perception's memory, an
    // AIBehavior's bindings), (4) the result re-serialized.
    // A tag is writable only when what its subsystem needs after a write is known: nothing
    // (read every frame), a carried field, or a rebuild (a RigidBody whose values changed:
    // PhysicsSystem reaps the orphaned body and builds one from the new values).

    LuaEntityWriteObserver& Observer()
    {
        static LuaEntityWriteObserver s_instance;
        return s_instance;
    }

    [[noreturn]] void SetError(const char* code, const std::string& message)
    {
        throw sol::error(std::string("Entity.set: ") + code + ": " + message);
    }

    // What a write reports after it landed that is not an error: the value is
    // in, but something else decides what the entity does with it.
    using WarnHook = void (*)(World&, Entity, std::vector<std::string>& warnings);

    void LightWarn(World& w, Entity e, std::vector<std::string>& warnings)
    {
        if (w.GetComponent<SunLightTag>(e) || w.GetComponent<MoonLightTag>(e) || w.GetComponent<TODLightComponent>(e))
            warnings.push_back("time of day drives this light: it rewrites color and intensity every frame");
    }
    void RigidBodyWarn(World& w, Entity e, std::vector<std::string>& warnings)
    {
        if (w.GetComponent<CharacterControllerComponent>(e))
            warnings.push_back("a CharacterController on this entity wins: no rigid body is built for it");
        else if (!w.GetComponent<ColliderComponent>(e))
            warnings.push_back("no Collider: a RigidBody without one has no body");
    }

    struct WritePolicy { const char* tag; WarnHook warn; };

    // The warnings a write of the tag reports once it landed.
    WarnHook WarnFor(const std::string& tag)
    {
        if (tag == "LightData") return LightWarn;
        if (tag == "RigidBody") return RigidBodyWarn;
        return nullptr;
    }

    bool LuaWritable(const ComponentSerializer& cs)
    {
        return HasFlag(cs.flags, ComponentFlag::LuaWritable);
    }

    // The modules' writable tags (RegisterLuaEntityWritable), after the flagged serializers'.
    std::vector<WritePolicy>& ModuleWritable()
    {
        static std::vector<WritePolicy> s_policies;
        return s_policies;
    }

    // The tags Entity.set may write are the serializers flagged ComponentFlag::LuaWritable
    // (ComponentFlags.cpp for the hand-written ones, RegisterReflectedSerializer for a described
    // component) and the modules'. A tag is writable only when what its subsystem needs after a
    // write is known: nothing (read every frame), a carried field (ComponentWrite), or a rebuild.
    std::optional<WritePolicy> FindPolicy(const std::string& tag)
    {
        if (const ComponentSerializer* cs = GetComponentRegistry().FindByTag(tag); cs && LuaWritable(*cs))
            return WritePolicy{ cs->tag.c_str(), WarnFor(tag) };
        for (const WritePolicy& p : ModuleWritable())
            if (tag == p.tag) return p;
        return std::nullopt;
    }

    std::vector<std::string> WritableTags()
    {
        std::vector<std::string> out;
        for (const auto& [type, cs] : GetComponentRegistry().All())
            if (LuaWritable(cs)) out.push_back(cs.tag);
        for (const WritePolicy& p : ModuleWritable()) out.emplace_back(p.tag);
        std::sort(out.begin(), out.end());
        return out;
    }

    // Why a tag is not writable, where the reason is known.
    const char* RefusalReason(const std::string& tag)
    {
        static const std::pair<const char*, const char*> kReasons[] = {
            { "Script",              "each write APPENDS a script slot (Engine.AttachScript adds one)" },
            { "MaterialInline",      "it takes the whole .imat text; change the material asset instead" },
            { "MaterialRef",         "the entity's MaterialOverride is not replayed onto the reloaded material; "
                                     "assign a material with Editor.AssignMaterial (undoable)" },
            { "MaterialOverride",    "the bag is stored but not applied to the live material; change a parameter "
                                     "with Engine.SetMaterialParam, or make an .imatinst (Editor.CreateMaterialInstance)" },
            { "ChainPhysics",        "the rest-pose caches reset without ChainPhysicsSystem::Invalidate" },
            { "PPVolume",            "every write re-acquires the profile and the old one is never released" },
            { "CharacterController", "its runtime state resets; move the entity with the Transform pseudo tag" },
            { "Guid",                "it is the entity's identity: every reference to it would break" },
            { "MeshHandle",          "it is a GPU mesh id" },
            { "ParticleEmitter",     "the legacy loader spawns components" },
            { "EdFolder",            "editor outliner data: use the editor's move-folder op" },
            { "CvParent",            "it is a UI hierarchy edge: move the node with Editor.Reparent(id, parent "
                                     "[, {index = n}]), which keeps the GUID and records one undo step" },
        };
        for (const auto& [t, why] : kReasons)
            if (tag == t) return why;
        return nullptr;
    }

    std::string WritableList()
    {
        std::string out;
        for (const std::string& tag : WritableTags()) { if (!out.empty()) out += ", "; out += tag; }
        return out + ", Transform";
    }

    std::optional<std::type_index> TypeOfTag(const std::string& tag)
    {
        for (const auto& [ti, cs] : GetComponentRegistry().All())
            if (cs.tag == tag) return ti;
        return std::nullopt;
    }

    // One "  Tag: k=v k=v" line as ordered pairs (the serializer's order).
    std::vector<std::pair<std::string, std::string>> Pairs(const std::string& line)
    {
        std::vector<std::pair<std::string, std::string>> out;
        const auto colon = line.find(':');
        if (colon == std::string::npos) return out;
        std::istringstream tok(line.substr(colon + 1));
        std::string t;
        while (tok >> t)
        {
            const auto eq = t.find('=');
            if (eq != std::string::npos && eq > 0) out.emplace_back(t.substr(0, eq), t.substr(eq + 1));
        }
        return out;
    }

    std::string NumberText(double d)
    {
        char b[48];
        if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 1e15)
            std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(d));
        else
            std::snprintf(b, sizeof(b), "%.17g", d);
        return b;
    }

    // Space and '%' as the serializers encode them, plus control characters:
    // a raw one would split the token or the line.
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

    bool IsNumericText(const std::string& v)
    {
        bool isInt = false; long long i = 0; double d = 0;
        return ParseNumber(v, isInt, i, d);
    }

    // Numeric parts of "a_b_c", or 0 when it is not an all-numeric vector.
    size_t VectorArity(const std::string& v)
    {
        if (v.find('_') == std::string::npos) return 0;
        size_t n = 0, start = 0;
        for (;;)
        {
            const size_t us = v.find('_', start);
            if (!IsNumericText(v.substr(start, us == std::string::npos ? std::string::npos : us - start))) return 0;
            ++n;
            if (us == std::string::npos) return n;
            start = us + 1;
        }
    }

    [[noreturn]] void BadValue(const std::string& key, const std::string& want, const std::string& current)
    {
        SetError("bad_value", key + " wants " + want + " (it is currently '" + current + "')");
    }

    // A Lua value as the text the serializer would write, shaped like the
    // CURRENT text: a number, a vector "a_b_c" (a list of as many numbers),
    // true/false, or text (percent-encoded). GUID keys take their hex as is.
    std::string ToFieldText(const std::string& key, const sol::object& v, const std::string& current)
    {
        const bool   curBool = current == "true" || current == "false";
        const size_t arity   = VectorArity(current);
        const bool   curNum  = IsNumericText(current);
        if (IsGuidKey(key))
        {
            if (!v.is<std::string>()) BadValue(key, "a hex GUID string", current);
            return v.as<std::string>();
        }
        const sol::type t = v.get_type();
        if (t == sol::type::boolean)
        {
            if (curBool) return v.as<bool>() ? "true" : "false";
            if (curNum)  return v.as<bool>() ? "1" : "0";
            BadValue(key, arity ? "a list of numbers" : "text", current);
        }
        if (t == sol::type::number)
        {
            const double d = v.as<double>();
            if (!std::isfinite(d)) BadValue(key, "a finite number", current);
            if (curBool) return d != 0.0 ? "true" : "false";
            if (arity)   BadValue(key, "a list of " + std::to_string(arity) + " numbers", current);
            return NumberText(d);      // a number field, or a text field that holds one
        }
        if (t == sol::type::table)
        {
            if (!arity) BadValue(key, curNum ? "a number" : curBool ? "a boolean" : "text", current);
            sol::table list = v.as<sol::table>();
            if (list.size() != arity) BadValue(key, "a list of " + std::to_string(arity) + " numbers", current);
            std::string out;
            for (size_t i = 1; i <= arity; ++i)
            {
                sol::object e = list[i];
                if (e.get_type() != sol::type::number || !std::isfinite(e.as<double>()))
                    BadValue(key, "a list of " + std::to_string(arity) + " finite numbers", current);
                if (i > 1) out += '_';
                out += NumberText(e.as<double>());
            }
            return out;
        }
        if (t == sol::type::userdata && v.is<LuaVec3>())
        {
            if (arity != 3) BadValue(key, arity ? "a list of " + std::to_string(arity) + " numbers"
                                                : curNum ? "a number" : curBool ? "a boolean" : "text", current);
            const LuaVec3 p = v.as<LuaVec3>();
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                BadValue(key, "a Vec3 of finite numbers", current);
            return NumberText(p.x) + "_" + NumberText(p.y) + "_" + NumberText(p.z);
        }
        if (t == sol::type::string)
        {
            // The .iscene text form is taken where it has the current value's
            // shape - "2_2_2" for a vector of 3, "0.5" for a number, true /
            // false for a boolean: the form Engine.AddComponent documents.
            const std::string s = v.as<std::string>();
            if (arity)
            {
                if (VectorArity(s) != arity)
                    BadValue(key, "a list of " + std::to_string(arity) + " numbers (or that list as text, a_b_c)", current);
                return s;
            }
            if (curNum)
            {
                if (!IsNumericText(s)) BadValue(key, "a number", current);
                return s;
            }
            if (curBool)
            {
                if (s != "true" && s != "false") BadValue(key, "a boolean", current);
                return s;
            }
            return EncodeText(s);
        }
        BadValue(key, "a number, a list, a Vec3, a boolean or a string", current);
    }

    sol::table PairsTable(sol::state_view& lua, const std::vector<std::pair<std::string, std::string>>& pairs,
                          const std::vector<std::string>& keys)
    {
        sol::table t = lua.create_table();
        for (const std::string& k : keys)
            for (const auto& [pk, pv] : pairs)
                if (pk == k) { t[k] = FieldValue(lua, k, pv); break; }
        return t;
    }

    // The Transform pseudo tag: LocalTransform directly (no serializer).
    sol::table SetTransform(sol::state_view& lua, World& world, Entity e, LuaEntity id, const sol::table& fields)
    {
        LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        if (!lt) SetError("no_component", "the entity has no LocalTransform");
        const LocalTransform before = *lt;
        LocalTransform after = before;
        std::vector<std::string> applied;
        auto floats = [&](const std::string& key, const sol::object& v, float* out, size_t n) {
            if (!v.is<sol::table>()) SetError("bad_value", key + " wants a list of " + std::to_string(n) + " numbers");
            sol::table t = v.as<sol::table>();
            if (t.size() != n) SetError("bad_value", key + " wants a list of " + std::to_string(n) + " numbers");
            std::string text;
            for (size_t i = 0; i < n; ++i)
            {
                sol::object e = t[i + 1];
                if (e.get_type() != sol::type::number || !std::isfinite(e.as<double>()) ||
                    std::fabs(e.as<double>()) > Resource::Authoring::FloatMax)
                    SetError("bad_value", key + " wants finite numbers");
                out[i] = static_cast<float>(e.as<double>());
                if (i) text += '_';
                text += NumberText(e.as<double>());
            }
            std::string why;
            if (!Resource::Authoring::Validate("Transform", {{key, text}}, why)) SetError("bad_value", why);
        };
        for (const auto& kv : fields)
        {
            if (!kv.first.is<std::string>()) SetError("unknown_key", "keys are names: position, rotation, scale");
            const std::string key = kv.first.as<std::string>();
            if (key == "position")      floats(key, kv.second, &after.translation.x, 3);
            else if (key == "scale")    floats(key, kv.second, &after.scale.x, 3);
            else if (key == "rotation")
            {
                floats(key, kv.second, &after.rotation.x, 4);
                // Widen BEFORE multiplying: finite float32 inputs can overflow
                // a float squared norm, normalizing a valid quaternion to zero.
                const double len = std::sqrt(double(after.rotation.x) * after.rotation.x + double(after.rotation.y) * after.rotation.y +
                                            double(after.rotation.z) * after.rotation.z + double(after.rotation.w) * after.rotation.w);
                if (len < 1e-6f) SetError("bad_value", "rotation is a quaternion xyzw; this one has zero length");
                after.rotation.x = float(after.rotation.x / len); after.rotation.y = float(after.rotation.y / len);
                after.rotation.z = float(after.rotation.z / len); after.rotation.w = float(after.rotation.w / len);
            }
            else SetError("unknown_key", "Transform has position, rotation (quaternion xyzw), scale; not '" + key + "'");
            applied.push_back(key);
        }
        *lt = after;
        // A character controller reads its pose once and then owns it: sync the
        // hierarchy now and rebuild its CharacterVirtual here (Character.Teleport's
        // recipe). Everything else follows at the next frame's transform pass.
        if (auto* cc = world.GetComponent<CharacterControllerComponent>(e))
        {
            TransformSystem::Propagate(world);
            cc->MarkDirty();
        }
        if (Observer().transform) Observer().transform(e, before, after);

        std::sort(applied.begin(), applied.end());
        applied.erase(std::unique(applied.begin(), applied.end()), applied.end());
        std::vector<std::string> none;
        sol::table out = lua.create_table();
        out["id"]      = id;
        out["tag"]     = kTransformTag;
        out["applied"] = ListOf(lua, applied);
        const std::set<std::string> keys(applied.begin(), applied.end());
        out["before"]  = LinesToLua(lua, kTransformTag, { TransformLine(lua, before, nullptr) }, &keys, none);
        out["after"]   = LinesToLua(lua, kTransformTag, { TransformLine(lua, after, nullptr) }, &keys, none);
        return out;
    }

    sol::table SetComponent(sol::state_view& lua, World& world, Entity e, LuaEntity id,
                            const std::string& tag, const sol::table& fields)
    {
        if (IsPseudoTag(tag)) return SetTransform(lua, world, e, id, fields);
        const ComponentSerializer* cs = KnownTag(tag);
        if (!cs) SetError("unknown_component", "'" + tag + "' is no component tag (Entity.components() lists them)");
        const std::optional<WritePolicy> pol = FindPolicy(tag);
        if (!pol)
        {
            const char* why = RefusalReason(tag);
            SetError("unsupported_write", tag + (why ? std::string(": ") + why
                                                     : std::string(": its post-write handling is not known yet")) +
                                          " (writable: " + WritableList() + ")");
        }
        if (!cs->has || !cs->has(world, e) || !cs->serialize || !cs->deserialize)
            SetError("no_component", "the entity has no " + tag + " (Entity.set changes an existing component)");

        std::ostringstream ss;
        cs->serialize(world, e, ss);
        const std::string beforeBlob = ss.str();
        std::vector<std::string> lines;
        {
            std::istringstream in(beforeBlob);
            std::string line;
            while (std::getline(in, line))
                if (line.find(':') != std::string::npos) lines.push_back(line);
        }
        if (lines.size() != 1)
            SetError("unsupported_write", tag + " is written as " + std::to_string(lines.size()) +
                                          " lines (slots): a key does not name one value");
        auto beforePairs = Pairs(lines[0]);
        if (tag == "IKRig")
        {
            // IKRig saves only overrides. Expose default keys to merge writes
            // without making the on-disk format verbose or admitting typos.
            IKRigComponent defaults;
            VisitIKRigAuthoring(defaults, [&](const char* key, const auto& value, const auto&) {
                if (std::any_of(beforePairs.begin(), beforePairs.end(), [&](const auto& pair) { return pair.first == key; })) return;
                using T = std::decay_t<decltype(value)>;
                std::string text;
                if constexpr (std::is_same_v<T, std::string>) text = EncodeText(value);
                else if constexpr (std::is_enum_v<T>) text = std::to_string(static_cast<int>(value));
                else if constexpr (std::is_same_v<T, bool>) text = value ? "1" : "0";
                else text = NumberText(value);
                beforePairs.emplace_back(key, std::move(text));
            });
        }
        auto afterPairs = beforePairs;
        Resource::Authoring::FillDefaults(tag, afterPairs);

        std::vector<std::string> applied;
        Resource::Authoring::Values submitted;
        for (const auto& kv : fields)
        {
            if (!kv.first.is<std::string>()) SetError("unknown_key", "keys are " + tag + " field names");
            const std::string key = kv.first.as<std::string>();
            const auto* descriptor = Resource::Authoring::FindField(tag, key);
            if (!Resource::Authoring::Fields(tag).empty() && !descriptor)
            {
                std::string valid;
                for (const auto& f : Resource::Authoring::Fields(tag)) { if (!valid.empty()) valid += ", "; valid += f.key; }
                SetError("unknown_key", tag + " has no key '" + key + "' (authoring fields: " + valid + ")");
            }
            auto it = std::find_if(afterPairs.begin(), afterPairs.end(), [&](const auto& p) { return p.first == key; });
            if (it == afterPairs.end() && descriptor)
            {
                afterPairs.emplace_back(key, Resource::Authoring::SlotDefault(key));
                it = std::prev(afterPairs.end());
            }
            if (it == afterPairs.end())
            {
                std::string valid;
                for (const auto& p : beforePairs) { if (!valid.empty()) valid += ", "; valid += p.first; }
                SetError("unknown_key", tag + " has no key '" + key + "' (its serializer writes: " + valid + ")");
            }
            if (descriptor && kv.second.get_type() == sol::type::boolean && std::string_view(descriptor->type) != "boolean")
                BadValue(key, descriptor->type, it->second);
            if (descriptor && (std::string_view(descriptor->type) == "string" || std::string_view(descriptor->type) == "asset" ||
                               std::string_view(descriptor->type) == "text"))
            {
                if (!kv.second.is<std::string>()) BadValue(key, descriptor->type, it->second);
                it->second = EncodeText(kv.second.as<std::string>());
            }
            else it->second = ToFieldText(key, kv.second, it->second);
            submitted.emplace_back(key, it->second);
            applied.push_back(key);
        }
        // A mesh-section key and the hint it names: a write that moves the hint (a collider's
        // meshId= / meshPath=) without its key drops the key, because the deserializer resolves
        // meshSec= first and the old key would bring the old entry straight back.
        {
            auto wrote = [&](const char* k) { return std::find(applied.begin(), applied.end(), k) != applied.end(); };
            if ((wrote("meshId") || wrote("meshPath")) && !wrote("meshSec"))
                afterPairs.erase(std::remove_if(afterPairs.begin(), afterPairs.end(),
                                                [](const auto& p) { return p.first == "meshSec"; }),
                                 afterPairs.end());
        }

        KVMap kvmap;
        std::string schemaError;
        if (!Resource::Authoring::Validate(tag, afterPairs, schemaError)) SetError("bad_value", schemaError);
        if (!Resource::Authoring::ValidateSubmitted(tag, submitted, afterPairs, schemaError)) SetError("bad_value", schemaError);
        for (const auto& [k, v] : afterPairs) kvmap[k] = v;
        const std::optional<std::type_index> ti = TypeOfTag(tag);
        if (!ti) SetError("unknown_component", "'" + tag + "' has no component type");
        if (cs->fetch && cs->descriptor)
        {
            // A described component is written in place, field by field, through the one writer every
            // surface shares (hard ranges, read-only fields, the type's change hooks): all or nothing,
            // and the runtime state it does not save stays what it was.
            std::string why;
            if (!Reflect::WriteKeys(*cs->descriptor, cs->fetch(world, e), submitted, Reflect::WriteOrigin::Tool, why))
                SetError("bad_value", why);
        }
        else
            ComponentWrite::RewriteFromKV(*cs, *ti, world, e, kvmap, nullptr);
        std::vector<std::string> warnings;
        if (pol->warn) pol->warn(world, e, warnings);

        std::ostringstream ss2;
        cs->serialize(world, e, ss2);
        const std::string afterBlob = ss2.str();
        if (Observer().component) Observer().component(e, *ti, beforeBlob, afterBlob);

        std::sort(applied.begin(), applied.end());
        applied.erase(std::unique(applied.begin(), applied.end()), applied.end());
        sol::table out = lua.create_table();
        out["id"]      = id;
        out["tag"]     = tag;
        out["applied"] = ListOf(lua, applied);
        out["before"]  = PairsTable(lua, beforePairs, applied);
        out["after"]   = PairsTable(lua, Pairs(afterBlob), applied);
        if (!warnings.empty()) out["warnings"] = ListOf(lua, warnings);
        return out;
    }
}

Resource::Authoring::Values LuaAuthoringCreateValues(const std::string& tag, const sol::table& fields)
{
    auto values = Resource::Authoring::Defaults(tag);
    for (const auto& [keyObject, value] : fields)
    {
        if (!keyObject.is<std::string>())
            throw sol::error("Entity.create: unknown_key: keys must be field names");
        const std::string key = keyObject.as<std::string>();
        const auto* descriptor = Resource::Authoring::FindField(tag, key);
        if (!descriptor) throw sol::error("Entity.create: unknown_key: " + tag + "." + key);
        auto it = std::find_if(values.begin(), values.end(), [&](const auto& pair) { return pair.first == key; });
        if (it == values.end()) { values.emplace_back(key, Resource::Authoring::SlotDefault(key)); it = std::prev(values.end()); }
        if (value.get_type() == sol::type::boolean && std::string_view(descriptor->type) != "boolean")
            throw sol::error("Entity.create: bad_value: " + tag + "." + key + " requires " + descriptor->type);
        if (std::string_view(descriptor->type) == "string" || std::string_view(descriptor->type) == "asset" ||
            std::string_view(descriptor->type) == "text")
        {
            if (!value.is<std::string>()) throw sol::error("Entity.create: bad_value: " + key + " requires text");
            it->second = EncodeText(value.as<std::string>());
        }
        else it->second = ToFieldText(key, value, it->second);
    }
    std::string why;
    if (!Resource::Authoring::Validate(tag, values, why)) throw sol::error("Entity.create: bad_value: " + why);
    if (!Resource::Authoring::ValidateSubmitted(tag, values, values, why)) throw sol::error("Entity.create: bad_value: " + why);
    return values;
}

sol::table LuaEntityMergeWrite(sol::state_view lua, World& world, Entity e, LuaEntity id,
                               const std::string& tag, const sol::table& fields)
{
    return SetComponent(lua, world, e, id, tag, fields);
}

bool LuaValueToFieldText(const sol::object& v, std::string& out, std::string& why)
{
    switch (v.get_type())
    {
    case sol::type::boolean:
        out = v.as<bool>() ? "1" : "0";
        return true;
    case sol::type::number:
    {
        const double d = v.as<double>();
        if (!std::isfinite(d)) { why = "not a finite number"; return false; }
        out = NumberText(d);
        return true;
    }
    case sol::type::string:
        out = EncodeText(v.as<std::string>());
        return true;
    case sol::type::table:
    {
        const sol::table list = v.as<sol::table>();
        const size_t n = list.size();
        std::string text;
        for (size_t i = 1; i <= n; ++i)
        {
            const sol::object el = list[i];
            if (el.get_type() != sol::type::number || !std::isfinite(el.as<double>()))
            {
                why = "a table must be a list of finite numbers (the vector form a_b_c)";
                return false;
            }
            if (i > 1) text += '_';
            text += NumberText(el.as<double>());
        }
        if (n == 0) { why = "an empty table (a vector is a list of numbers)"; return false; }
        out = std::move(text);
        return true;
    }
    case sol::type::userdata:
        if (v.is<LuaVec3>())
        {
            const LuaVec3 p = v.as<LuaVec3>();
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            {
                why = "a Vec3 of finite numbers";
                return false;
            }
            out = NumberText(p.x) + "_" + NumberText(p.y) + "_" + NumberText(p.z);
            return true;
        }
        [[fallthrough]];
    default:
        why = "a number, a boolean, a string, a list of numbers or a Vec3";
        return false;
    }
}

std::vector<std::string> LuaEntityWritableTags()
{
    std::vector<std::string> out = WritableTags();
    out.emplace_back(kTransformTag);
    std::sort(out.begin(), out.end());
    return out;
}

void RegisterLuaEntityWritable(const char* tag, LuaEntityWarnHook warn)
{
    if (!FindPolicy(tag)) ModuleWritable().push_back({ tag, warn });
}

void SetLuaEntityWriteObserver(LuaEntityWriteObserver observer)
{
    Observer() = std::move(observer);
}

std::vector<std::string> LuaEntityDedicatedTags()
{
    std::vector<std::string> out;
    for (const Dedicated& d : kDedicated) out.emplace_back(d.tag);
    std::sort(out.begin(), out.end());
    return out;
}

void RegisterLuaEntityBindings(sol::state& lua, World& world)
{
    sol::table ent = lua.create_named_table("Entity");
    ent["referenceVersion"] = 1; // SDK refuses older hosts before attempting a write.
    ent["pageVersion"] = 1;
    auto pages = std::make_shared<EntityPages>();
    LuaDoc::Fn(ent, "Entity", "page", [&world, pages](sol::this_state s, sol::table q) -> sol::table {
        sol::state_view L(s);
        return EntityPage(L, world, *pages, q);
    },
        { .doc = "A fixed snapshot of the matching entities, read in bounded pages: Entity.page{filters, projection, subtree?, limit?, maxBytes?} starts one, Entity.page{cursor = c} continues it (docs/guides/entity_query_paging.md).",
          .params = { { .name = "q", .doc = "filters and projection as Entity.find takes them, or {cursor = c}" } },
          .returnsDoc = "{rows, totalMatched, returned, revision, nextCursor, frame, scanned, bytes}",
          .errors = { "stale_cursor", "unsupported_paging" } });

    // Contracts exist independently of live component instances and reflection.
    LuaDoc::Fn(ent, "Entity", "schema", [](sol::this_state s, const std::string& tag) -> sol::object
    {
        sol::state_view L(s);
        const auto& fields = Resource::Authoring::Fields(tag);
        if (fields.empty()) return sol::make_object(L, sol::lua_nil);
        sol::table out = L.create_table(), list = L.create_table();
        out["version"] = 1; out["tag"] = tag; out["operation"] = "patch_existing";
        out["additionalProperties"] = false;
        out["storageDecimals"] = tag == "Transform" ? 6 : 4;
        out["fields"] = list;
        sol::table creation = L.create_table(), defaults = L.create_table();
        for (const auto& [key, value] : Resource::Authoring::Defaults(tag)) defaults[key] = FieldValue(L, key, value);
        creation["defaults"] = defaults;
        creation["validated"] = true;
        creation["scope"] = tag == "Transform" ? "Editor placement and SceneDocument.AddNode; omitted root position uses editor spawn position"
            : "Editor.Create, Editor.AddComponent, Engine.AddComponent (new), SceneDocument.AddNode";
        creation["legacyLoaderValidated"] = false;
        out["creation"] = creation;
        if (tag == "RigidBody") out["effect"] = "ComponentWrite preserves unchanged bodies; changed authored values rebuild on the next physics step. Collider required.";
        if (tag == "AnimationSet")
        {
            out["storageDecimals"] = sol::lua_nil;
            out["storageFormat"] = "float32 with general stream precision; indexed slots 0..n-1";
            out["effect"] = "AnimationClipSystem detects binding/loop/speed/default changes on its next update; compat and clock/pose evidence are required.";
        }
        if (tag.size() > 2 && tag[0] == 'C' && tag[1] == 'v')
        {
            // The Canvas UI components (Cv*).
            out["storageDecimals"] = sol::lua_nil;
            out["storageFormat"] = "float32 with general stream precision (6 significant digits)";
            out["effect"] = "ComponentWrite keeps the runtime state (texture lease, hover / press, the sprite playhead); "
                            "the layout follows on the next UI pass. A .iui block holds the same keys (UI/UIAsset.h).";
        }
        if (tag == "Material")
        {
            out["operation"] = "Editor.MaterialEditorSet / native material_patch";
            out["scope"] = "PBR schema fields; extension keys use the material codec, custom shader reflection is separate";
            out["storageDecimals"] = sol::lua_nil;
            creation["validated"] = false;
            creation["scope"] = "Use Editor.CreateMaterialInstance; Material is not an ECS serializer tag";
            out["effect"] = "MaterialEditSession staged edit, undo and live-overlay notification; save and fresh reload prove persistence";
        }
        int i = 1;
        for (const auto& f : fields)
        {
            sol::table row = L.create_table();
            row["key"] = f.key; row["type"] = f.type; row["arity"] = f.arity;
            row["unit"] = f.unit; row["minimum"] = f.minimum; row["maximum"] = f.maximum;
            row["exclusiveMinimum"] = f.exclusiveMinimum; row["exclusiveMaximum"] = f.exclusiveMaximum;
            row["semantics"] = f.semantics; row["writable"] = true;
            list[i++] = row;
        }
        return sol::make_object(L, out);
    },
        { .doc = "The patch contract of a component - fields, types, ranges, storage precision - for the tags with a hand-validated contract (Transform, LightData, Camera, RigidBody, AnimationSet, Material); nil for the rest, whose keys gen_catalog.py --component <tag> lists (entityKeys).",
          .params = { { .name = "tag", .doc = "the serializer tag, case-sensitive" } },
          .returns = "table?" });

    // Entity.components() -> { readable, dedicated, pseudo, writable }: every serializer tag
    // this build knows; `dedicated` = the ones read by a read-only reader here instead of their
    // serializer (which would stamp a GUID on the referenced entity); `pseudo` = Transform and
    // Authoring; `writable` = LuaEntityWritableTags().
    LuaDoc::Fn(ent, "Entity", "components", [](sol::this_state s) -> sol::table
    {
        sol::state_view L(s);
        std::vector<std::string> readable, dedicated;
        for (const auto& kv : GetComponentRegistry().All())
        {
            readable.push_back(kv.second.tag);
            if (DedicatedReader(kv.second.tag)) dedicated.push_back(kv.second.tag);
        }
        std::sort(readable.begin(), readable.end());
        std::sort(dedicated.begin(), dedicated.end());
        sol::table out = L.create_table();
        out["readable"]  = ListOf(L, readable);
        out["dedicated"] = ListOf(L, dedicated);
        out["pseudo"]    = ListOf(L, { kTransformTag, kAuthoringTag });
        out["writable"]  = ListOf(L, LuaEntityWritableTags());
        return out;
    },
        { .doc = "Every component tag this build reads (as .iscene writes it, case-sensitive), the ones read by a dedicated reader, the Transform pseudo component and the tags Entity.set writes.",
          .returns = "{readable: string[], dedicated: string[], pseudo: string[], writable: string[]}" });

    // Entity.get(id [, select, opts]) -> nil for a dead or stale id; otherwise
    // { id, session, epoch, name, [transient], components = { Tag = fields, ... }, [missing] }.
    // select: "Tag", "Tag.key", or a list of them; default every component.
    LuaDoc::Fn(ent, "Entity", "get", [&world](sol::this_state s, LuaEntity id, sol::variadic_args args)
        -> sol::object
    {
        sol::state_view L(s);
        // Read by position: sol's optional<object> may not consume a nil
        // select argument, otherwise get(id, nil, context) loses the guard.
        sol::optional<sol::table> opts;
        if (args.size() > 1 && args.get<sol::object>(1).get_type() != sol::type::lua_nil)
            opts = args.get<sol::table>(1);
        CheckContext("Entity.get", world, opts);
        Select sel;
        if (args.size() > 0) AddSelect("Entity.get", args.get<sol::object>(0), sel, false);
        if (sel.Empty()) sel.all = true;
        const Entity e = LuaId::ResolveLive(world, id);
        if (e == NullEntity) return sol::make_object(L, sol::lua_nil);
        return EntityRow(L, world, e, sel);
    },
        { .doc = "One entity's components through the .iscene serializers, read-only. Optional arguments: what to read (\"Tag\", \"Tag.key\" or a list; nil = everything) and {session, epoch} to check the id against.",
          .params = { { .name = "id", .doc = "an entity id (generation in the high bits)" } },
          .returns = "EntityRow?",
          .returnsDoc = "{id, name, transient?, components = {Tag = fields}, missing?}; nil when the entity is dead or the id's generation is stale",
          .errors = { "stale_session", "stale_epoch", "unknown_component" } });

    // Entity.find{ name = <case-insensitive substring>, tag = <TagComponent tag>,
    //              component = <serializer tag the entity must have>,
    //              components = "Tag" | {tags}   (read these in full),
    //              fields = "Tag.key" | {...}    (read only these fields),
    //              all = true                    (read every component),
    //              limit = <n, default 10000> }
    //   -> { rows = { {id, session, epoch, name, [components], [missing]}, ... },
    //        count, truncated, scanned }
    // Rows are ascending by entity id. Without components / fields / all a row
    // holds only id, session, epoch, name (and transient). Every row comes from
    // the same frame: this runs inside one Lua call.
    LuaDoc::Fn(ent, "Entity", "find", [&world](sol::this_state s, sol::optional<sol::table> q) -> sol::table
    {
        sol::state_view L(s);
        std::string name, tag, component;
        Select sel;
        size_t limit = 10000;
        if (q)
        {
            name      = Lower(q->get_or<std::string>("name", ""));
            tag       = q->get_or<std::string>("tag", "");
            component = q->get_or<std::string>("component", "");
            AddSelect("Entity.find", q->get<sol::object>("components"), sel, false);
            AddSelect("Entity.find", q->get<sol::object>("fields"), sel, true);
            sel.all   = q->get_or("all", false);
            const double lim = q->get_or("limit", 10000.0);
            if (lim >= 1.0) limit = static_cast<size_t>(lim);
        }
        const ComponentSerializer* mustHave = nullptr;
        if (!component.empty() && !(mustHave = KnownTag(component)) && !IsPseudoTag(component)) UnknownTag("Entity.find", component);

        std::vector<Entity> hits;
        size_t scanned = 0;
        for (Entity e : world.GetEntities())
        {
            if (!world.IsAlive(e)) continue;
            ++scanned;
            if (!name.empty() && Lower(world.GetName(e)).find(name) == std::string::npos) continue;
            if (!tag.empty())
            {
                const TagComponent* tc = world.GetComponent<TagComponent>(e);
                if (!tc || !tc->Has(tag.c_str())) continue;
            }
            // The same presence rule as ReadableComponents: the serializer's `has`.
            if (mustHave && !(mustHave->has && mustHave->has(world, e))) continue;
            if (component == kTransformTag && !world.HasComponent<LocalTransform>(e)) continue;
            hits.push_back(e);
        }
        // GetEntities() is reordered by DestroyEntity (swap-to-back); sort so
        // the same world always answers in the same order.
        std::sort(hits.begin(), hits.end());
        const bool truncated = hits.size() > limit;
        if (truncated) hits.resize(limit);

        sol::table rows = L.create_table();
        for (size_t i = 0; i < hits.size(); ++i)
            rows[i + 1] = EntityRow(L, world, hits[i], sel);
        sol::table out = L.create_table();
        out["rows"]      = rows;
        out["count"]     = hits.size();
        out["truncated"] = truncated;
        out["scanned"]   = scanned;
        return out;
    },
        { .doc = "Entities matching a filter, ascending by id. name = case-insensitive substring, tag = TagComponent tag, component = must have it; components / fields / all choose what each row reads (none: id + name); limit defaults to 10000.",
          .params = { { .name = "q", .doc = "{name?, tag?, component?, components?, fields?, all?, limit?}" } },
          .returns = "EntityQuery",
          .returnsDoc = "{rows, count, truncated, scanned}",
          .errors = { "unknown_component" } });

    // Entity.set: see LuaEntityBindings.h. The opts epoch (Engine.GetWorldEpoch /
    // Entity.census().epoch when the id was read) refuses a write aimed at a replaced world:
    // the id's generation catches a Clear (World::Clear advances every slot's generation), the
    // epoch also catches the editor's Stop, which brings the pre-Play world back with its ids.
    LuaDoc::Fn(ent, "Entity", "set", [&world](sol::this_state s, LuaEntity id, const std::string& tag,
                                     sol::table fields, sol::optional<sol::table> opts) -> sol::table
    {
        sol::state_view L(s);
        CheckContext("Entity.set", world, opts);
        const Entity e = LuaId::ResolveLive(world, id);
        if (e == NullEntity)
            SetError("stale_handle", "entity " + std::to_string(id.raw()) +
                                     " is dead, or its slot was recycled (generation mismatch)");
        sol::table result = SetComponent(L, world, e, id, tag, fields);
        result["id"] = LuaId::Out(world, e);
        result["session"] = EntitySession();
        result["epoch"] = world.Epoch();
        return result;
    },
        { .doc = "Change an EXISTING component - only the keys given, merged over its current values - or the Transform pseudo component (position, rotation xyzw, scale). Allow-listed tags only; a refusal names its reason. In Editor.exe the write is undoable.",
          .params = { { .name = "id", .doc = "an entity id" },
                      { .name = "tag", .doc = "the serializer tag, or \"Transform\"" },
                      { .name = "fields", .doc = "keys as the serializer writes them (gen_catalog.py --component <tag>: entityKeys)" },
                      { .name = "opts", .doc = "{session, epoch}: refuse an id read in another session or before a world reload" } },
          .returnsDoc = "{id, tag, applied, before, after, warnings?}",
          .effect = LuaDoc::Effect::World,
          .errors = { "stale_handle", "stale_epoch", "stale_session", "unknown_component", "unsupported_write", "no_component", "unknown_key", "bad_value" } });

    // Entity.census() -> { entities, pools, structuralHash, epoch }
    // The world's STRUCTURE (World::StructuralHash: every live id with its
    // generation, every non-empty pool's members) - independent of the
    // serializers, so it can witness that a read changed nothing: take one
    // before, read, take one after, compare. The hash is hex text (64 bits do
    // not fit a Lua number exactly).
    LuaDoc::Fn(ent, "Entity", "census", [&world](sol::this_state s) -> sol::table
    {
        sol::state_view L(s);
        size_t pools = 0;
        const uint64_t h = world.StructuralHash(&pools);
        char hex[20];
        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
        size_t alive = 0;
        for (Entity e : world.GetEntities())
            if (world.IsAlive(e)) ++alive;
        sol::table out = L.create_table();
        out["entities"]       = alive;
        out["pools"]          = pools;
        out["structuralHash"] = std::string(hex);
        out["epoch"]          = world.Epoch();
        return out;
    },
        { .doc = "The world's structural fingerprint - every live entity's id and generation and every non-empty pool. Equal before and after proves a step changed no entity or component set.",
          .returns = "{entities: integer, pools: integer, structuralHash: string, epoch: integer}" });
}
