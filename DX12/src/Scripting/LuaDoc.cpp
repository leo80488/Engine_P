#include "Scripting/LuaDoc.h"

#include <algorithm>
#include <map>

namespace LuaDoc
{
namespace
{
    // Ordered by name, so help("Entity") and the catalog list members stably.
    std::map<std::string, Entry>& Registry()
    {
        static std::map<std::string, Entry> s_instance;
        return s_instance;
    }
}

void Register(Entry entry)
{
    std::string key = entry.name;
    Registry()[std::move(key)] = std::move(entry);
}

void Describe(std::string name, const char* kind, Doc doc, std::vector<std::string> types, std::string returns)
{
    Entry e;
    e.name     = std::move(name);
    e.kind     = kind ? kind : "function";
    e.doc      = std::move(doc);
    e.optional.assign(types.size(), false);
    e.types    = std::move(types);
    e.returns  = std::move(returns);
    Register(std::move(e));
}

const Entry* Find(const std::string& name)
{
    const auto it = Registry().find(name);
    return it == Registry().end() ? nullptr : &it->second;
}

std::vector<const Entry*> All()
{
    std::vector<const Entry*> out;
    out.reserve(Registry().size());
    for (const auto& [name, e] : Registry()) out.push_back(&e);
    return out;
}

const char* EffectName(Effect e)
{
    switch (e)
    {
    case Effect::Read:     return "read";
    case Effect::World:    return "world";
    case Effect::Settings: return "settings";
    case Effect::Disk:     return "disk";
    case Effect::Process:  return "process";
    case Effect::Job:      return "job";
    }
    return "read";
}

const char* ContextName(Context c)
{
    switch (c)
    {
    case Context::Any:     return "any";
    case Context::Update:  return "update";
    case Context::Editor:  return "editor";
    case Context::Harness: return "harness";
    }
    return "any";
}

std::string ParamType(const Entry& e, std::size_t i)
{
    if (i < e.doc.params.size() && e.doc.params[i].type) return e.doc.params[i].type;
    if (i < e.types.size()) return e.types[i];
    return "any";
}

std::string ReturnType(const Entry& e)
{
    return e.doc.returns ? std::string(e.doc.returns) : e.returns;
}

std::string Signature(const Entry& e)
{
    // A property is its value's type; a function is fun(...): ret.
    if (e.kind != "function") return ReturnType(e).empty() ? std::string("any") : ReturnType(e);
    const std::size_t n = std::max(e.types.size(), e.doc.params.size());
    std::string s = "fun(";
    for (std::size_t i = 0; i < n; ++i)
    {
        if (i) s += ", ";
        const char* name = i < e.doc.params.size() && e.doc.params[i].name && *e.doc.params[i].name
                         ? e.doc.params[i].name : nullptr;
        std::string type = ParamType(e, i);
        const bool opt = (i < e.optional.size() && e.optional[i]) ||
                         (i < e.doc.params.size() && e.doc.params[i].optional);
        // A deduced "T?" says optional already; LuaCATS puts the ? on the name.
        if (opt && !type.empty() && type.back() == '?') type.pop_back();
        s += (name ? std::string(name) : "arg" + std::to_string(i + 1)) + (opt ? "?" : "") + ": " + type;
    }
    if (e.variadic) s += std::string(n ? ", " : "") + "...: any";
    s += ")";
    const std::string ret = ReturnType(e);
    if (!ret.empty()) s += ": " + ret;
    return s;
}

void RegisterHelp(lua_State* L)
{
#if defined(WITH_HARNESS)
    if (!L) return;
    sol::state_view lua(L);

    // help("Entity.set") -> {name, kind, signature, doc, params, returns, effect,
    // context, errors}; help("Entity") -> {documented = {names}}; anything else
    // -> nil, why. What a member that is bound but not registered here does is
    // in docs/design/Lua_API_Reference.md; whether it exists at all:
    // python tools/gen_catalog.py --lua <name>.
    lua.set_function("help", [](sol::this_state s, const std::string& name)
        -> std::tuple<sol::object, sol::object>
    {
        sol::state_view v(s);
        if (const Entry* e = Find(name))
        {
            sol::table t = v.create_table();
            t["name"]      = e->name;
            t["kind"]      = e->kind;
            t["signature"] = Signature(*e);
            t["doc"]       = std::string(e->doc.doc ? e->doc.doc : "");
            sol::table params = v.create_table();
            const std::size_t n = std::max(e->types.size(), e->doc.params.size());
            for (std::size_t i = 0; i < n; ++i)
            {
                sol::table p = v.create_table();
                const bool hasDoc = i < e->doc.params.size();
                p["name"]     = std::string(hasDoc && e->doc.params[i].name ? e->doc.params[i].name : "");
                p["type"]     = ParamType(*e, i);
                p["optional"] = (i < e->optional.size() && e->optional[i]) || (hasDoc && e->doc.params[i].optional);
                p["doc"]      = std::string(hasDoc && e->doc.params[i].doc ? e->doc.params[i].doc : "");
                params[i + 1] = p;
            }
            t["params"]     = params;
            t["returns"]    = ReturnType(*e);
            t["returnsDoc"] = std::string(e->doc.returnsDoc ? e->doc.returnsDoc : "");
            t["effect"]     = std::string(EffectName(e->doc.effect));
            t["context"]    = std::string(ContextName(e->doc.context));
            sol::table errors = v.create_table();
            for (std::size_t i = 0; i < e->doc.errors.size(); ++i) errors[i + 1] = std::string(e->doc.errors[i]);
            t["errors"] = errors;
            return { t, sol::lua_nil };
        }
        const std::string prefix = name + ".";
        sol::table names = v.create_table();
        int n = 0;
        for (const Entry* e : All())
            if (e->name.compare(0, prefix.size(), prefix) == 0) names[++n] = e->name;
        if (n > 0)
        {
            sol::table t = v.create_table();
            t["documented"] = names;
            return { t, sol::lua_nil };
        }
        return { sol::lua_nil, sol::make_object(v, "undocumented: '" + name + "' is not registered with LuaDoc "
                                                    "(whether it exists: python tools/gen_catalog.py --lua " + name + ")") };
    });
#else
    (void)L;
#endif
}
}
