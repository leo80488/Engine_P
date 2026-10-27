#include "Module/ModuleRegistry.h"

#include "Resource/AssetFS.h"
#include "Scripting/LuaDoc.h"   // Engine.hasModule / Engine.modules, described as they are bound
#include "System/Log.h"

#include <algorithm>
#include <map>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace Module
{
namespace
{
    const char* KindName(Kind k)
    {
        switch (k)
        {
        case Kind::Cpp:     return "cpp";
        case Kind::Script:  return "script";
        case Kind::Content: return "content";
        }
        return "?";
    }
}

Registry& Registry::Get()
{
    static Registry s_registry;
    return s_registry;
}

void Registry::Declare(Decl decl)
{
    if (Find(decl.name))
    {
        LOG_ERROR("[MODULES] %s is declared twice", decl.name.c_str());
        return;
    }
    for (const std::string& tag : decl.components)
        m_componentOwner.emplace(tag, decl.name);
    Entry e;
    e.decl    = std::move(decl);
    e.enabled = e.decl.enabledByDefault;   // until Configure reads the project (a tool never does)
    m_entries.push_back(std::move(e));
}

void Registry::Link(std::string_view name, Factory factory)
{
    if (Entry* e = Find(name))
        e->factory = factory;
    else
        LOG_ERROR("[MODULES] %.*s is linked but not declared", static_cast<int>(name.size()), name.data());
}

Registry::Entry* Registry::Find(std::string_view name)
{
    for (Entry& e : m_entries)
        if (e.decl.name == name) return &e;
    return nullptr;
}

const Registry::Entry* Registry::Find(std::string_view name) const
{
    for (const Entry& e : m_entries)
        if (e.decl.name == name) return &e;
    return nullptr;
}

void Registry::Instantiate()
{
    for (Entry& e : m_entries)
        if (e.factory && !e.instance)
            e.instance = e.factory();
}

void Registry::OnProcessStart()
{
    Instantiate();
    for (Entry& e : m_entries)
        if (e.instance) e.instance->OnProcessStart();
}

void Registry::RegisterTypes(ComponentSerializerRegistry& components)
{
    if (m_typesRegistered) return;
    m_typesRegistered = true;
    Instantiate();
    for (Entry& e : m_entries)
        if (e.instance) e.instance->RegisterTypes(components);
}

void Registry::Configure()
{
    std::string text;
    std::string choices;
    if (Resource::AssetFS::Get().ReadFileText("game.json", text))
    {
        const nlohmann::json gj = nlohmann::json::parse(text, nullptr, false);
        if (!gj.is_discarded() && gj.is_object() && gj.contains("modules"))
            choices = gj["modules"].dump();
    }
    ConfigureFrom(choices);
}

void Registry::ConfigureFrom(std::string_view choicesJson)
{
    m_configured = true;
    std::map<std::string, bool, std::less<>> choice;
    if (!choicesJson.empty())
    {
        const nlohmann::json mods = nlohmann::json::parse(choicesJson, nullptr, false);
        if (mods.is_discarded() || !mods.is_object())
            LOG_ERROR("[MODULES] game.json \"modules\" is not an object: every module takes its default");
        else
            for (auto it = mods.begin(); it != mods.end(); ++it)
            {
                if (!it.value().is_boolean())
                {
                    LOG_ERROR("[MODULES] game.json \"modules\".%s is not true / false", it.key().c_str());
                    continue;
                }
                if (!Find(it.key()))
                    LOG_WARNING("[MODULES] game.json names %s, which this build does not know", it.key().c_str());
                choice[it.key()] = it.value().get<bool>();
            }
    }

    for (Entry& e : m_entries)
    {
        const auto it = choice.find(e.decl.name);
        e.enabled = it != choice.end() ? it->second : e.decl.enabledByDefault;
    }
    // Dependencies: the declared order is dependencies first, so one pass in reverse pulls in a
    // whole chain - except a dependency the project turned off. Then whatever needs a module that
    // stays off is off too, to a fixed point (A needs B needs C, C turned off: B, then A).
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
    {
        if (!it->enabled) continue;
        for (const std::string& dep : it->decl.dependsOn)
        {
            const auto c = choice.find(dep);
            if (Entry* d = Find(dep); d && (c == choice.end() || c->second))
                d->enabled = true;
        }
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (Entry& e : m_entries)
        {
            if (!e.enabled) continue;
            for (const std::string& dep : e.decl.dependsOn)
            {
                const Entry* d = Find(dep);
                if (!d || d->enabled) continue;
                LOG_ERROR("[MODULES] %s needs %s, which is off (game.json): %s stays off",
                          e.decl.name.c_str(), dep.c_str(), e.decl.name.c_str());
                e.enabled = false;
                changed = true;
                break;
            }
        }
    }
    LOG_INFO("[MODULES] %s", Describe().c_str());
}

std::vector<Registry::Entry*> Registry::Active()
{
    if (!m_configured) Configure();
    Instantiate();
    std::vector<Entry*> out;
    for (Entry& e : m_entries)
        if (e.enabled && e.instance) out.push_back(&e);
    return out;
}

void Registry::Initialize(Context& ctx)
{
    for (Entry* e : Active()) e->instance->Initialize(ctx);
}

void Registry::RegisterRenderFeatures(RenderFeatureRegistry& features)
{
    for (Entry* e : Active()) e->instance->RegisterRenderFeatures(features);
}

void Registry::OnRendererReady(Context& ctx)
{
    for (Entry* e : Active()) e->instance->OnRendererReady(ctx);
}

void Registry::RegisterLua(Context& ctx)
{
    for (Entry* e : Active()) e->instance->RegisterLua(ctx);
}

void Registry::RegisterSystems(Context& ctx)
{
    for (Entry* e : Active()) e->instance->RegisterSystems(ctx);
}

void Registry::OnDeviceChange()
{
    for (Entry* e : Active()) e->instance->OnDeviceChange();
}

void Registry::Shutdown()
{
    std::vector<Entry*> active = Active();
    for (auto it = active.rbegin(); it != active.rend(); ++it) (*it)->instance->Shutdown();
}

bool Registry::IsDeclared(std::string_view name) const
{
    return Find(name) != nullptr;
}

bool Registry::IsLinked(std::string_view name) const
{
    const Entry* e = Find(name);
    return e && e->factory;
}

bool Registry::IsEnabled(std::string_view name) const
{
    const Entry* e = Find(name);
    return e && e->enabled && (e->decl.kind != Kind::Cpp || e->factory);
}

bool Registry::IsScriptEnabled(std::string_view path) const
{
    for (const Entry& e : m_entries)
        for (const std::string& s : e.decl.scripts)
            if (s == path) return IsEnabled(e.decl.name);
    return true;
}

const std::string* Registry::OwnerOfComponentTag(const std::string& tag) const
{
    const auto it = m_componentOwner.find(tag);
    return it != m_componentOwner.end() ? &it->second : nullptr;
}

std::vector<Registry::Status> Registry::Statuses() const
{
    std::vector<Status> out;
    out.reserve(m_entries.size());
    for (const Entry& e : m_entries)
        out.push_back({ e.decl.name, e.decl.kind, e.factory != nullptr, IsEnabled(e.decl.name) });
    return out;
}

std::string Registry::Describe() const
{
    size_t linked = 0, enabled = 0;
    std::string list;
    for (const Status& s : Statuses())
    {
        linked  += s.linked ? 1 : 0;
        enabled += s.enabled ? 1 : 0;
        list += " " + s.name + "(" + KindName(s.kind) + "," + (s.enabled ? "on" : "off");
        if (s.kind == Kind::Cpp && !s.linked) list += ",not linked";
        list += ")";
    }
    return "declared=" + std::to_string(m_entries.size()) + " linked=" + std::to_string(linked) +
           " enabled=" + std::to_string(enabled) + list;
}

void RegisterLuaBindings(sol::state& lua)
{
    // The Engine table ScriptSystem::RegisterBindings created.
    sol::table engine = lua["Engine"];
    LuaDoc::Fn(engine, "Engine", "hasModule",
        [](const std::string& name) { return Registry::Get().IsEnabled(name); },
        { .doc = "Whether the module is enabled in this project and present in this build.",
          .params = { { "name", "a module name, as game.json \"modules\" spells it" } } });
    LuaDoc::Fn(engine, "Engine", "modules",
        [](sol::this_state ts)
        {
            sol::state_view lv(ts);
            sol::table out = lv.create_table();
            int i = 0;
            for (const Registry::Status& s : Registry::Get().Statuses())
            {
                sol::table row = lv.create_table();
                row["name"]    = s.name;
                row["kind"]    = KindName(s.kind);
                row["linked"]  = s.linked;
                row["enabled"] = s.enabled;
                out[++i] = row;
            }
            return out;
        },
        { .doc = "Every module this build knows: {name, kind, linked, enabled}, dependencies first.",
          .returns = "{name: string, kind: string, linked: boolean, enabled: boolean}[]" });
}
}
