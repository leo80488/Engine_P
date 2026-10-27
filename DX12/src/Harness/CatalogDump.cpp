#include "Harness/CatalogDump.h"

// Self-guard as well as the vcxproj's ExcludedFromBuild, so the TU compiles to
// nothing wherever WITH_HARNESS is off: Retail carries no harness.
#ifdef WITH_HARNESS

#include "ECS/ECS.h"
#include "ECS/ExclusiveResourceIds.h"
#include "ECS/ISystem.h"
#include "ECS/SystemRegistry.h"
#include "ECS/TickPhase.h"
#include "Reflection/Reflect.h"
#include "Reflection/ReflectRegistry.h"
#include "RenderGraph/RenderGraph.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/AuthoringSchema.h"
#include "Scripting/LuaDoc.h"
#include "Scripting/LuaEntityBindings.h"
#include "Scripting/ScriptSystem.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/Sha256.h"

#include <sol/sol.hpp>
#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace Harness
{
namespace
{
    using Json = nlohmann::json;

    constexpr int kSchema = 1;

    // ==== identity ====

    const char* ConfigName()
    {
#if defined(_DEBUG)
        return "Debug";
#elif defined(TRACY_ENABLE)
        return "Profile";
#else
        return "Release";      // this TU is harness-only: never Retail
#endif
    }

    std::wstring ExePathW()
    {
        std::wstring buf(1024, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (n == 0) return {};
            if (n < buf.size()) { buf.resize(n); return buf; }
            buf.resize(buf.size() * 2);
        }
    }

    // "struct LightData" / "class UI::UICanvas" -> "LightData" / "UI::UICanvas"
    std::string CppName(std::type_index ti)
    {
        std::string s = ti.name();
        for (const char* p : { "struct ", "class ", "enum " })
        {
            const std::string prefix = p;
            if (s.compare(0, prefix.size(), prefix) == 0) { s.erase(0, prefix.size()); break; }
        }
        return s;
    }

    // ==== lua.json ====

    // The standard library and sol2's own globals: listed by name only.
    bool IsStdGlobal(const std::string& n)
    {
        static const char* const kStd[] = {
            "_G", "_VERSION", "assert", "collectgarbage", "coroutine", "debug", "dofile", "error",
            "getmetatable", "io", "ipairs", "load", "loadfile", "loadstring", "math", "next", "os",
            "package", "pairs", "pcall", "print", "rawequal", "rawget", "rawlen", "rawset", "require",
            "select", "setmetatable", "string", "table", "tonumber", "tostring", "type", "unpack",
            "utf8", "warn", "xpcall",
        };
        for (const char* s : kStd)
            if (n == s) return true;
        return false;
    }

    constexpr size_t kMaxMembers = 4000;

    Json Brief(lua_State* L, int idx)
    {
        Json j;
        const int t = lua_type(L, idx);
        j["type"] = lua_typename(L, t);
        if (t == LUA_TNUMBER)
        {
            if (lua_isinteger(L, idx)) j["value"] = static_cast<long long>(lua_tointeger(L, idx));
            else
            {
                const double d = lua_tonumber(L, idx);
                if (std::isfinite(d)) j["value"] = d;
            }
        }
        else if (t == LUA_TBOOLEAN) j["value"] = lua_toboolean(L, idx) != 0;
        // A function a SCRIPT defined (not a C++ binding): the table holding it
        // grows with whatever scripts ran, so its member list is not the API.
        else if (t == LUA_TFUNCTION && !lua_iscfunction(L, idx)) j["lua"] = true;
        else if (t == LUA_TSTRING)
        {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            j["value"] = std::string(s, std::min<size_t>(len, 160));
        }
        return j;
    }

    Json DescribeValue(lua_State* L, int idx, int depth);

    // Every string-keyed entry of the table at @p tbl into @p members (first
    // writer wins). @p skipMeta drops "__x" keys - the metamethods of a
    // metatable. lua_next is a RAW walk: no __pairs, no __index runs.
    void CollectMembers(lua_State* L, int tbl, Json& members, int depth, bool skipMeta, bool& truncated)
    {
        tbl = lua_absindex(L, tbl);
        lua_pushnil(L);
        while (lua_next(L, tbl) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                size_t len = 0;
                const char* k = lua_tolstring(L, -2, &len);     // a string key: no in-place conversion
                const std::string key(k, len);
                const bool meta = key.size() >= 2 && key[0] == '_' && key[1] == '_';
                if (!(skipMeta && meta) && !members.contains(key))
                {
                    if (members.size() >= kMaxMembers) truncated = true;
                    else members[key] = (depth > 0 && lua_type(L, -1) == LUA_TTABLE)
                                            ? DescribeValue(L, -1, depth - 1) : Brief(L, -1);
                }
            }
            lua_pop(L, 1);
        }
    }

    // A value, and for a table / userdata its members: its own entries, its
    // metatable's __index table (where sol2 usertypes may keep methods) and the
    // metatable's non-metamethod entries. An __index FUNCTION cannot be listed;
    // "indexFunction": true says so instead of pretending the list is complete.
    Json DescribeValue(lua_State* L, int idx, int depth)
    {
        idx = lua_absindex(L, idx);
        Json j = Brief(L, idx);
        const int t = lua_type(L, idx);
        if (t != LUA_TTABLE && t != LUA_TUSERDATA) return j;
        if (!lua_checkstack(L, 8)) return j;
        Json members = Json::object();
        bool truncated = false;
        if (t == LUA_TTABLE)
        {
            CollectMembers(L, idx, members, depth, false, truncated);
            const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
            if (n > 0) j["arrayLength"] = static_cast<long long>(n);
        }
        if (lua_getmetatable(L, idx))
        {
            const int mt = lua_gettop(L);
            lua_pushliteral(L, "__index");
            lua_rawget(L, mt);
            if (lua_type(L, -1) == LUA_TTABLE)
                CollectMembers(L, lua_gettop(L), members, depth, true, truncated);
            else if (lua_type(L, -1) == LUA_TFUNCTION)
                j["indexFunction"] = true;
            lua_pop(L, 1);
            CollectMembers(L, mt, members, depth, true, truncated);
            lua_pop(L, 1);
        }
        if (!members.empty()) j["members"] = std::move(members);
        if (truncated) j["truncated"] = true;
        return j;
    }

    bool StartsWith(const std::string& s, const char* p)
    {
        return s.rfind(p, 0) == 0;
    }

    // A sol2 registry metatable name -> the C++ type it belongs to. sol2 keeps
    // several per type: "sol.T" (value), "sol.T*", "sol.T.user", "sol.const T",
    // "sol.sol::d::u<T >" (unique), "sol.T.<gc>". All of them name T.
    std::string UsertypeName(std::string key)
    {
        if (StartsWith(key, "sol.")) key.erase(0, 4);
        // "\xE2\x99\xBB" is sol2's UTF-8 gc-table suffix (U+267B); escaped
        // because this file is not compiled as UTF-8.
        for (const char* suffix : { ".user", "*", ".\xE2\x99\xBB" })
        {
            const std::string s = suffix;
            if (key.size() > s.size() && key.compare(key.size() - s.size(), s.size(), s) == 0)
            { key.erase(key.size() - s.size()); break; }
        }
        if (StartsWith(key, "const ")) key.erase(0, 6);
        if (StartsWith(key, "sol::d::u<"))
        {
            // "sol::d::u<X<Y> >": drop the wrapper's own closing '>' only.
            key.erase(0, 10);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            if (!key.empty() && key.back() == '>') key.pop_back();
            while (!key.empty() && key.back() == ' ') key.pop_back();
        }
        if (StartsWith(key, "::")) key.erase(0, 2);
        return key;
    }

    // sol2's own entries on every usertype: not API.
    bool IsSolBookkeeping(const std::string& member)
    {
        return member == "__name" || member == "class_cast" || member == "class_check";
    }

    // sol2 keeps a usertype's members in metatables stored in the REGISTRY
    // under "sol.<C++ type>" (plus "*", ".user", ... variants), not in the
    // global table, whose __index is a function. Walk those: C++ type -> the
    // union of their non-metamethod member names.
    Json UsertypeMembers(lua_State* L)
    {
        Json out = Json::object();
        lua_pushvalue(L, LUA_REGISTRYINDEX);
        const int reg = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, reg) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TTABLE)
            {
                size_t len = 0;
                const char* k = lua_tolstring(L, -2, &len);
                const std::string key(k, len);
                if (StartsWith(key, "sol."))
                {
                    // "sol.LuaVec3", "sol.LuaVec3*", "sol.LuaVec3.user" -> "LuaVec3".
                    std::string type = UsertypeName(key);
                    Json& members = out[type];
                    if (members.is_null()) members = Json::object();
                    bool truncated = false;
                    CollectMembers(L, lua_gettop(L), members, 0, true, truncated);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        // Keep only types with members, minus sol2's bookkeeping entries.
        Json kept = Json::object();
        for (auto it = out.begin(); it != out.end(); ++it)
        {
            Json members = Json::object();
            for (auto m = it.value().begin(); m != it.value().end(); ++m)
                if (!IsSolBookkeeping(m.key())) members[m.key()] = m.value();
            if (!members.empty()) kept[it.key()] = std::move(members);
        }
        return kept;
    }

    // A LuaDoc entry (Scripting/LuaDoc.h) as lua.json carries it on its member.
    Json DocJson(const LuaDoc::Entry& e)
    {
        Json j;
        j["kind"]      = e.kind;
        j["signature"] = LuaDoc::Signature(e);
        j["doc"]       = e.doc.doc ? e.doc.doc : "";
        Json params = Json::array();
        const size_t n = (std::max)(e.types.size(), e.doc.params.size());
        for (size_t i = 0; i < n; ++i)
        {
            const bool hasDoc = i < e.doc.params.size();
            Json p;
            p["name"]     = hasDoc && e.doc.params[i].name ? e.doc.params[i].name : "";
            p["type"]     = LuaDoc::ParamType(e, i);
            p["optional"] = (i < e.optional.size() && e.optional[i]) || (hasDoc && e.doc.params[i].optional);
            if (hasDoc && e.doc.params[i].doc && *e.doc.params[i].doc) p["doc"] = e.doc.params[i].doc;
            params.push_back(std::move(p));
        }
        j["params"]  = std::move(params);
        j["returns"] = LuaDoc::ReturnType(e);
        if (e.doc.returnsDoc && *e.doc.returnsDoc) j["returnsDoc"] = e.doc.returnsDoc;
        if (e.variadic) j["variadic"] = true;
        j["effect"]  = LuaDoc::EffectName(e.doc.effect);
        j["context"] = LuaDoc::ContextName(e.doc.context);
        if (!e.doc.errors.empty())
        {
            Json errors = Json::array();
            for (const char* code : e.doc.errors) errors.push_back(code);
            j["errors"] = std::move(errors);
        }
        return j;
    }

    Json DumpLua(ScriptSystem& scripts)
    {
        Json out;
        sol::state* lua = scripts.GetLua();
        if (!lua) { out["error"] = "no Lua state"; return out; }
        lua_State* L = lua->lua_state();
        const int top = lua_gettop(L);
        Json globals = Json::object();
        std::vector<std::string> internal, stdlib;
        lua_pushglobaltable(L);
        const int g = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, g) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                size_t len = 0;
                const char* k = lua_tolstring(L, -2, &len);
                const std::string name(k, len);
                if (IsStdGlobal(name)) stdlib.push_back(name);
                // Engine registries ("__systems") and sol2's own bookkeeping
                // ("sol.LuaVec3.<gc>"): named, never walked.
                else if (StartsWith(name, "__") || StartsWith(name, "sol.")) internal.push_back(name);
                else globals[name] = DescribeValue(L, -1, 1);
            }
            lua_pop(L, 1);
        }
        lua_settop(L, top);

        // A usertype global's `__name` is its registry metatable's name; attach
        // the members found there.
        Json usertypes = UsertypeMembers(L);
        lua_settop(L, top);
        for (auto it = globals.begin(); it != globals.end(); ++it)
        {
            Json& v = it.value();
            if (!v.contains("members") || !v["members"].contains("__name")) continue;
            const Json& nm = v["members"]["__name"];
            if (!nm.contains("value") || !nm["value"].is_string()) continue;
            const std::string raw = nm["value"].get<std::string>();
            if (!StartsWith(raw, "sol.")) continue;
            const std::string type = UsertypeName(raw);
            v["usertype"] = type;
            Json& members = v["members"];
            if (usertypes.contains(type))
                for (auto m = usertypes[type].begin(); m != usertypes[type].end(); ++m)
                    if (!members.contains(m.key())) members[m.key()] = m.value();
            for (const char* b : { "__name", "class_cast", "class_check" }) members.erase(b);
        }

        // LuaDoc: what each member IS, where its binding was registered with a
        // description - else documented: false. An entry with no bound member is an
        // ORPHAN (a misspelt Describe, a binding that moved).
        size_t documented = 0, memberCount = 0;
        std::set<std::string> seen;
        for (auto it = globals.begin(); it != globals.end(); ++it)
        {
            Json& v = it.value();
            if (!v.is_object() || !v.contains("members") || !v["members"].is_object()) continue;
            for (auto m = v["members"].begin(); m != v["members"].end(); ++m)
            {
                if (!m.value().is_object()) continue;
                ++memberCount;
                const std::string name = it.key() + "." + m.key();
                const LuaDoc::Entry* e = LuaDoc::Find(name);
                m.value()["documented"] = e != nullptr;
                if (!e) continue;
                m.value()["doc"] = DocJson(*e);
                seen.insert(name);
                ++documented;
            }
        }
        Json orphans = Json::array();
        for (const LuaDoc::Entry* e : LuaDoc::All())
            if (!seen.count(e->name)) orphans.push_back(e->name);
        out["luaDoc"] = Json{ { "documented", documented }, { "members", memberCount },
                              { "orphans", std::move(orphans) } };

        // The player-settings paths Settings.get / set take (the LuaLS definitions'
        // SettingKey), asked through the bindings themselves so the list is exactly
        // what a script sees.
        Json settings = Json::array();
        {
            const sol::optional<sol::table> st = (*lua)["Settings"];
            const sol::optional<sol::protected_function> list = st ? (*st)["list"] : sol::optional<sol::protected_function>();
            const sol::optional<sol::protected_function> meta = st ? (*st)["meta"] : sol::optional<sol::protected_function>();
            if (list && meta)
            {
                const sol::protected_function_result paths = (*list)();
                if (paths.valid() && paths.get_type() == sol::type::table)
                {
                    const sol::table t = paths;
                    for (size_t i = 1; i <= t.size(); ++i)
                    {
                        const sol::optional<std::string> path = t[i];
                        if (!path) continue;
                        Json row = { { "path", *path } };
                        const sol::protected_function_result m = (*meta)(*path);
                        if (m.valid() && m.get_type() == sol::type::table)
                        {
                            const sol::table mt = m;
                            row["kind"]    = mt.get_or<std::string>("kind", "");
                            row["restart"] = mt.get_or("restart", false);
                        }
                        settings.push_back(std::move(row));
                    }
                }
            }
        }
        lua_settop(L, top);

        std::sort(stdlib.begin(), stdlib.end());
        std::sort(internal.begin(), internal.end());
        out["settings"]  = std::move(settings);
        out["globals"]   = std::move(globals);
        out["usertypes"] = std::move(usertypes);   // by C++ type, including types with no global
        out["internal"]  = internal;
        out["stdlib"]    = stdlib;
        return out;
    }

    // ==== components.json ====

    const char* KindName(Reflect::FieldKind k)
    {
        using K = Reflect::FieldKind;
        switch (k)
        {
        case K::F32: return "f32";     case K::F32x2: return "f32x2";
        case K::F32x3: return "f32x3"; case K::F32x4: return "f32x4";
        case K::I32: return "i32";     case K::U32: return "u32";
        case K::Bool: return "bool";   case K::Enum: return "enum";
        case K::String: return "string";
        case K::Vector: return "vector";
        case K::Custom: return "custom";
        case K::U8: return "u8";       case K::U16: return "u16";
        case K::I64: return "i64";     case K::F64: return "f64";
        case K::Flags: return "flags"; case K::Quat: return "quat";
        case K::Struct: return "struct";
        case K::EntityRef: return "entityRef";
        case K::AssetRef: return "assetRef";
        default: return nullptr;       // pseudo-entries (headers, info, groups)
        }
    }

    const char* WidgetName(Reflect::Widget w)
    {
        using W = Reflect::Widget;
        switch (w)
        {
        case W::Drag: return "drag";     case W::Color: return "color";
        case W::Angle: return "angle";   case W::InputText: return "inputText";
        case W::Combo: return "combo";   case W::Checkbox: return "checkbox";
        default: return nullptr;
        }
    }

    // The field's value in @p obj (a default instance), as JSON; null for a kind without one.
    Json FieldValue(const Reflect::FieldDescriptor& f, const void* obj)
    {
        using K = Reflect::FieldKind;
        const auto* p = static_cast<const unsigned char*>(obj) + f.offset;
        auto floats = [&](int n) {
            Json a = Json::array();
            for (int i = 0; i < n; ++i) a.push_back(reinterpret_cast<const float*>(p)[i]);
            return a;
        };
        auto bits = [&]() -> uint32_t {
            switch (f.enumSize)
            {
            case 1: return *p;
            case 2: return *reinterpret_cast<const uint16_t*>(p);
            case 4: return *reinterpret_cast<const uint32_t*>(p);
            default: return 0;
            }
        };
        switch (f.kind)
        {
        case K::F32: return *reinterpret_cast<const float*>(p);
        case K::F64: return *reinterpret_cast<const double*>(p);
        case K::F32x2: return floats(2);
        case K::F32x3: return floats(3);
        case K::F32x4: case K::Quat: return floats(4);
        case K::I32: return *reinterpret_cast<const int32_t*>(p);
        case K::U32: case K::EntityRef: return *reinterpret_cast<const uint32_t*>(p);
        case K::U8: return *p;
        case K::U16: return *reinterpret_cast<const uint16_t*>(p);
        case K::I64: return *reinterpret_cast<const int64_t*>(p);
        case K::Bool: return *reinterpret_cast<const bool*>(p);
        case K::Enum: case K::Flags: return bits();
        case K::String: case K::AssetRef: return *reinterpret_cast<const std::string*>(p);
        default: return nullptr;
        }
    }

    Json FieldFlagNames(Reflect::FieldFlag flags)
    {
        using F = Reflect::FieldFlag;
        static const std::pair<F, const char*> kNames[] = {
            { F::Runtime, "runtime" }, { F::ReadOnly, "readOnly" }, { F::Advanced, "advanced" },
            { F::Hidden, "hidden" }, { F::NoUndo, "noUndo" }, { F::NoMultiEdit, "noMultiEdit" },
            { F::LuaRead, "luaRead" }, { F::LuaWrite, "luaWrite" }, { F::Overridable, "overridable" },
            { F::Animatable, "animatable" }, { F::ClampHard, "clampHard" },
        };
        Json out = Json::array();
        for (const auto& [flag, name] : kNames)
            if ((static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0) out.push_back(name);
        return out;
    }

    Json DescribeFields(const Reflect::TypeDescriptor& d, int depth)
    {
        const void* defaults = Reflect::DefaultInstance(d);
        Json fields = Json::array();
        for (const Reflect::FieldDescriptor& f : d.fields)
        {
            using K = Reflect::FieldKind;
            if (f.kind == K::Header || f.kind == K::CollapseBegin)
            {
                if (f.label && *f.label) fields.push_back(Json{ { "section", f.label } });
                continue;
            }
            if (f.kind == K::InfoText)
            {
                // REFLECT_INFO: prose about the field above it.
                if (!fields.empty() && fields.back().contains("name") && f.label && *f.label)
                {
                    Json& last = fields.back();
                    last["info"] = last.contains("info") ? last["info"].get<std::string>() + " " + f.label
                                                         : std::string(f.label);
                }
                continue;
            }
            const char* kind = KindName(f.kind);
            if (!kind) continue;
            Json j;
            j["name"]  = f.name ? f.name : "";
            j["label"] = f.label ? f.label : "";
            j["kind"]  = kind;
            if (const char* w = WidgetName(f.widget)) j["widget"] = w;
            if (!(f.minVal == 0.f && f.maxVal == 0.f)) { j["min"] = f.minVal; j["max"] = f.maxVal; }
            if (f.widget == Reflect::Widget::Angle) j["unit"] = "radians (min/max in degrees)";
            if (f.unit) j["unit"] = f.unit;
            if (f.tooltip) j["tooltip"] = f.tooltip;
            if (f.key && *f.key) j["key"] = f.key;
            if (f.legacyKeys) j["legacyKeys"] = f.legacyKeys;
            if (f.category) j["category"] = f.category;
            if (f.hasHard) j["hard"] = Json::array({ f.hardMin, f.hardMax });
            if (f.flags != Reflect::FieldFlag::None) j["flags"] = FieldFlagNames(f.flags);
            if (defaults)
            {
                Json v = FieldValue(f, defaults);
                if (!v.is_null()) j["default"] = std::move(v);
            }
            if (f.dropPayload) j["payload"] = f.dropPayload;
            if (f.kind == K::Flags && f.presets)
            {
                Json presets = Json::array();
                for (std::size_t i = 0; i < f.presetCount; ++i)
                    presets.push_back(Json{ { "value", f.presets[i].value }, { "label", f.presets[i].label },
                                            { "add", f.presets[i].add } });
                j["presets"] = std::move(presets);
            }
            if ((f.kind == K::Enum || f.kind == K::Flags) && f.enumOpts)
            {
                Json opts = Json::array();
                for (std::size_t i = 0; i < f.enumCount; ++i)
                    opts.push_back(Json{ { "value", f.enumOpts[i].value },
                                         { "label", f.enumOpts[i].label ? f.enumOpts[i].label : "" } });
                j["options"] = std::move(opts);
            }
            if ((f.kind == K::Vector || f.kind == K::Struct) && f.elementDesc)
            {
                j["element"] = f.elementDesc->name ? f.elementDesc->name : "";
                if (f.arrayCapacity) j["capacity"] = f.arrayCapacity;
                if (depth > 0) j["elementFields"] = DescribeFields(*f.elementDesc, depth - 1);
            }
            fields.push_back(std::move(j));
        }
        return fields;
    }

    // The keys of "  Tag: k=v k=v\n" lines, and the first line (for an example).
    void SerializedKeys(const std::string& text, std::set<std::string>& keys, std::string& firstLine)
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            if (firstLine.empty())
            {
                const auto b = line.find_first_not_of(' ');
                firstLine = line.substr(b == std::string::npos ? 0 : b, 240);
            }
            std::istringstream tok(line.substr(colon + 1));
            std::string token;
            while (tok >> token)
            {
                const auto eq = token.find('=');
                if (eq != std::string::npos && eq > 0) keys.insert(token.substr(0, eq));
            }
        }
    }

    Json DumpComponents(World& world)
    {
        const auto& all = GetComponentRegistry().All();
        std::vector<std::pair<std::string, std::type_index>> order;
        for (const auto& [ti, cs] : all) order.emplace_back(cs.tag, ti);
        std::sort(order.begin(), order.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        std::vector<Entity> alive;
        for (Entity e : world.GetEntities())
            if (world.IsAlive(e)) alive.push_back(e);
        std::sort(alive.begin(), alive.end());

        const std::vector<std::string> dedicated = LuaEntityDedicatedTags();
        const std::vector<std::string> writable  = LuaEntityWritableTags();
        constexpr size_t kSampleEntities = 64;

        Json list = Json::array();
        size_t reflected = 0;
        for (const auto& [tag, ti] : order)
        {
            const ComponentSerializer& cs = all.at(ti);
            const bool mutates = std::find(dedicated.begin(), dedicated.end(), tag) != dedicated.end();
            Json c;
            c["tag"]     = tag;
            c["cppType"] = CppName(ti);
            if (mutates) c["serializerMutates"] = true;
            // Entity.set accepts it (its post-write handling is known).
            if (std::find(writable.begin(), writable.end(), tag) != writable.end()) c["writable"] = true;
            size_t live = 0, sampled = 0;
            std::set<std::string> keys;
            std::string example;
            for (Entity e : alive)
            {
                if (!cs.has || !cs.has(world, e)) continue;
                ++live;
                // Never run a mutating serializer: its keys come from the source scan.
                if (mutates || !cs.serialize || sampled >= kSampleEntities) continue;
                std::ostringstream ss;
                cs.serialize(world, e, ss);
                SerializedKeys(ss.str(), keys, example);
                ++sampled;
            }
            c["liveEntities"] = live;
            if (!keys.empty()) c["liveKeys"] = std::vector<std::string>(keys.begin(), keys.end());
            if (!example.empty()) c["example"] = example;
            if (const Reflect::RuntimeEntry* r = Reflect::FindRuntime(ti); r && r->desc)
            {
                Json rj;
                rj["label"]  = r->label;
                if (r->desc->defaults) rj["defaultConstructible"] = true;
                rj["type"]   = r->desc->name ? r->desc->name : "";
                rj["fields"] = DescribeFields(*r->desc, 1);
                c["reflect"] = std::move(rj);
                ++reflected;
            }
            list.push_back(std::move(c));
        }
        Json out;
        out["components"]   = std::move(list);
        out["entities"]     = alive.size();
        out["reflected"]    = reflected;
        out["reflectionAvailable"] = Reflect::RuntimeCount() > 0;
        out["authoringSchemas"] = Json::array();
        for (const char* tag : Resource::Authoring::ContractTags())
        {
            Json schema = {{"version", 1}, {"tag", tag}, {"operation", "patch_existing"},
                           {"additionalProperties", false}, {"storageDecimals", std::string_view(tag) == "Transform" ? 6 : 4},
                           {"fields", Json::array()}};
            if (std::string_view(tag) == "AnimationSet" || std::string_view(tag) == "Material" ||
                std::string_view(tag).substr(0, 2) == "Cv")   // float32, general stream precision
                schema["storageDecimals"] = nullptr;
            if (std::string_view(tag) == "Material")
            {
                schema["operation"] = "Editor.MaterialEditorSet / native material_patch";
                schema["scope"] = "PBR fields; custom shader reflection and texture slots use dedicated adapters";
            }
            for (const auto& f : Resource::Authoring::Fields(tag))
                schema["fields"].push_back({{"key", f.key}, {"type", f.type}, {"arity", f.arity},
                    {"unit", f.unit}, {"minimum", f.minimum}, {"maximum", f.maximum},
                    {"exclusiveMinimum", f.exclusiveMinimum}, {"exclusiveMaximum", f.exclusiveMaximum},
                    {"semantics", f.semantics}, {"writable", true}});
            out["authoringSchemas"].push_back(std::move(schema));
        }
        return out;
    }

    // ==== systems.json ====

    const char* ExclusiveName(std::uint64_t id)
    {
        switch (id)
        {
        case ExclusiveRes::kPhysicsWorld:   return "PhysicsWorld";
        case ExclusiveRes::kAudioEngine:    return "AudioEngine";
        case ExclusiveRes::kEventBus:       return "EventBus";
        case ExclusiveRes::kScriptVM:       return "ScriptVM";
        case ExclusiveRes::kRenderer:       return "Renderer";
        case ExclusiveRes::kAnimClipSystem: return "AnimClipSystem";
        case ExclusiveRes::kCameraStack:    return "CameraStack";
        case ExclusiveRes::kPostFxRuntime:  return "PostFxRuntime";
        case ExclusiveRes::kGuidRegistry:   return "GuidRegistry";
        default:                            return nullptr;
        }
    }

    std::vector<std::string> TypeNames(const std::unordered_set<std::type_index>& s)
    {
        std::vector<std::string> v;
        for (const std::type_index& ti : s) v.push_back(CppName(ti));
        std::sort(v.begin(), v.end());
        return v;
    }

    Json DumpSystems(const SystemRegistry& reg, const ScriptSystem& scripts)
    {
        const auto& setNames = reg.GetSystemSetNames();
        Json phases = Json::array();
        size_t total = 0;
        for (size_t p = 0; p < static_cast<size_t>(TickPhase::COUNT); ++p)
        {
            const PhaseDescriptor& d = kPhaseDescriptors[p];
            Json ph;
            ph["phase"]              = d.name;
            ph["fixedTimestep"]      = d.isFixedTimestep;
            ph["allowsParallel"]     = d.allowsParallel;
            ph["requiresMainThread"] = d.requiresMainThread;
            Json systems = Json::array();
            const auto span = reg.GetSystems(static_cast<TickPhase>(p));
            for (size_t i = 0; i < span.size(); ++i)
            {
                const ISystem& s = *span[i];
                SystemAccessBuilder b;
                s.DeclareAccess(b);
                Json sj;
                sj["order"]   = i;
                sj["name"]    = s.GetName() ? s.GetName() : "?";
                sj["runMask"] = static_cast<int>(s.RunMask());
                std::vector<std::string> sets;
                for (const SystemSetId id : reg.GetSystemSets(s))
                {
                    const auto it = setNames.find(id);
                    if (it != setNames.end()) sets.push_back(it->second);
                }
                if (!sets.empty()) sj["sets"] = sets;
                if (reg.IsAlwaysRun(s)) sj["alwaysRun"] = true;
                if (b.IsOpaque()) sj["opaque"] = true;
                if (!b.Reads().empty())  sj["reads"]  = TypeNames(b.Reads());
                if (!b.Writes().empty()) sj["writes"] = TypeNames(b.Writes());
                if (!b.Exclusives().empty())
                {
                    std::vector<std::string> ex;
                    for (std::uint64_t id : b.Exclusives())
                    {
                        const char* n = ExclusiveName(id);
                        ex.push_back(n ? n : "id:" + std::to_string(id));
                    }
                    std::sort(ex.begin(), ex.end());
                    sj["exclusives"] = ex;
                }
                systems.push_back(std::move(sj));
                ++total;
            }
            ph["systems"] = std::move(systems);
            phases.push_back(std::move(ph));
        }
        Json lua = Json::array();
        for (const auto& [script, set] : scripts.GetSystemSetAssignments())
            lua.push_back(Json{ { "script", script }, { "set", set } });
        Json out;
        out["phases"]        = std::move(phases);
        out["systemCount"]   = total;
        out["luaSetMembers"] = std::move(lua);
        return out;
    }

    // ==== passes.json ====

    Json DumpPasses(const RG::RenderGraph& graph)
    {
        Json passes = Json::array();
        size_t index = 0, ran = 0;
        for (const RG::RenderGraph::PassInfo& p : graph.DescribePasses())
        {
            Json j;
            j["order"] = index++;
            j["name"]  = p.name;
            j["ran"]   = p.ran;
            if (!p.shown) j["shown"] = false;   // the graph's view hides it
            if (std::string(p.colorTarget) != "None") j["colorTarget"] = p.colorTarget;
            if (std::string(p.stage) != "Unstaged") j["stage"] = p.stage;
            if (!p.reads.empty())     j["reads"]     = p.reads;
            if (!p.writes.empty())    j["writes"]    = p.writes;
            if (!p.depth.empty())     j["depth"]     = p.depth;
            if (!p.readWrite.empty()) j["readWrite"] = p.readWrite;
            if (!p.manual.empty())    j["manual"]    = p.manual;
            if (!p.scoped.empty())    j["scoped"]    = p.scoped;   // ReadScoped
            if (p.ran) ++ran;
            passes.push_back(std::move(j));
        }
        Json textures = Json::array();
        for (const RG::RenderGraph::TextureInfo& t : graph.DescribeTextures())
        {
            Json j;
            j["name"]   = t.name;
            j["format"] = t.format;
            if (t.width || t.height) { j["width"] = t.width; j["height"] = t.height; }
            if (t.isDepth)  j["depth"] = true;
            if (t.isUAV)    j["uav"] = true;
            if (t.onDemand) { j["onDemand"] = true; j["demanded"] = t.demanded; }
            if (t.imported)   // ImportTexture, or a pass's own import
            {
                j["imported"] = true;
                j["entry"]    = RG::ImportEntryName(t.entry);
                j["tracking"] = RG::ImportTrackingName(t.tracking);
            }
            textures.push_back(std::move(j));
        }
        Json out;
        out["passes"]    = std::move(passes);
        out["ranLastFrame"] = ran;
        out["textures"]  = std::move(textures);
        return out;
    }

    bool WriteJson(const std::string& dir, const char* name, const Json& j, std::string& error)
    {
        const std::string text = j.dump(1, ' ', false, Json::error_handler_t::replace);
        const std::string path = dir + "/" + name;
        if (!Sys::WriteFileBytes(path, text.data(), text.size()))
        {
            error = "could not write " + path;
            return false;
        }
        return true;
    }
}

bool WriteCatalogDump(const CatalogSources& src, const std::string& utf8Dir, std::string& error)
{
    try
    {
        const Json lua        = DumpLua(src.scripts);
        const Json components = DumpComponents(src.world);
        const Json systems    = DumpSystems(src.systems, src.scripts);
        const Json passes     = DumpPasses(src.graph);

        const std::wstring exe = ExePathW();
        std::uint64_t bytes = 0;
        // The same digest tools/scenario_run.py records as exeSha256.
        const std::string sha = Sys::Sha256FileHex(Sys::WideToUtf8(exe), &bytes);

        Json m;
        m["schema"]      = kSchema;
        m["generator"]   = "-dump_catalog";
        m["host"]        = Sys::IsEditorHost() ? "Editor" : "Game";
        m["exe"]         = Json{ { "path", Sys::WideToUtf8(exe) }, { "config", ConfigName() },
                                 { "sha256", sha }, { "bytes", bytes } };
        m["frame"]       = src.frame;
        m["scene"]       = Sys::CommandLine::Get().Get("-scene");
        m["commandLine"] = Sys::WideToUtf8(GetCommandLineW());
        m["files"]       = Json{ { "lua", "lua.json" }, { "components", "components.json" },
                                 { "systems", "systems.json" }, { "passes", "passes.json" } };
        m["counts"]      = Json{
            { "luaGlobals", lua.contains("globals") ? lua["globals"].size() : 0 },
            { "components", components["components"].size() },
            { "reflected",  components["reflected"] },
            { "systems",    systems["systemCount"] },
            { "passes",     passes["passes"].size() },
            { "textures",   passes["textures"].size() } };

        // The manifest LAST: a directory with a manifest is a complete dump.
        return WriteJson(utf8Dir, "lua.json", lua, error)
            && WriteJson(utf8Dir, "components.json", components, error)
            && WriteJson(utf8Dir, "systems.json", systems, error)
            && WriteJson(utf8Dir, "passes.json", passes, error)
            && WriteJson(utf8Dir, "manifest.json", m, error);
    }
    catch (const std::exception& e)
    {
        error = std::string("exception: ") + e.what();
        return false;
    }
}
} // namespace Harness

#endif // WITH_HARNESS
