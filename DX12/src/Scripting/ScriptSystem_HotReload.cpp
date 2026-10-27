// ScriptSystem: script hot reload - polling the tracked files and reloading one by its category.
// Split from ScriptSystem.cpp (file size gate); the rest of the class lives there.

#include "Scripting/ScriptSystem.h"
#include "Scripting/ScriptManifest.h"
#include "Scripting/LuaDiag.h"            // ResetOnce: a reloaded script warns again
#include "System/HostPolicy.h"         // HotReloadEnabled: authoring host or -hotreload only
#include "System/HotReloadReceipt.h"   // [HOTRELOAD] receipts
#include "System/Log.h"
#include "System/PathUtil.h"
#include "Resource/AssetFS.h"

#include <sol/sol.hpp>
#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)
#include "ScriptSystem_Internal.h"   // private to src/Scripting, not on the include path
using namespace ScriptInternal;   // RunScriptAsset / GetInstance*

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// Polls the tracked script files, a slice per frame, and reloads changed ones
// (ReloadChangedScript): a Logic template re-points live instances' metatable
// __index (their per-entity state survives); a System is torn down (OnShutdown)
// and reloaded (OnInit next Update); a Service / UI script is reloaded (callers
// holding the old table keep it until they re-fetch); a global script is re-run;
// an imported module is dropped from the import cache.
void ScriptSystem::CheckHotReload(World& /*world*/)
{
    // Packed Game.exe: nothing to do. Hot reload polls loose disk files, and a packed
    // build reads scripts from game.ipak. Runtime check, not #ifdef WITH_EDITOR: this
    // TU is in EngineCore.lib, compiled once for every exe, so it never sees the
    // per-exe define (System/HostPolicy.h). HotReloadEnabled = the editor host, or a
    // harness Game.exe started with -hotreload (a resident agent session edits a
    // script and checks it without a restart).
    if (!Sys::HotReloadEnabled()) return;

    // The manifest file is boot configuration, not a hot-reloadable script: kind /
    // order / enabled / set edits would need per-entry teardown to apply live. It is
    // kept out of m_fileTimestamps (a non-script path there would fall through every
    // category probe silently); it is polled here every 60 calls and warns once.
    if (m_bootedFromManifest && m_manifestTimestampValid && !m_manifestChangeNotified
        && ++m_manifestStatCounter >= 60)
    {
        m_manifestStatCounter = 0;
        try
        {
            const auto now = fs::last_write_time(Sys::U8Path(Scripting::kScriptManifestPath));
            if (now != m_manifestTimestamp)
            {
                m_manifestChangeNotified = true;
                LOG_WARNING("Script manifest changed on disk — kind/order/enabled/set edits "
                            "apply on the next editor/game start (per-script hot reload is "
                            "unaffected).");
            }
        }
        catch (...)
        {
            // A failed stat after a valid baseline means the file is gone or unreadable: the
            // next start falls back to the folder scan. Same one-shot notice as an edit.
            m_manifestChangeNotified = true;
            LOG_WARNING("Script manifest deleted or unreadable — the next editor/game "
                        "start falls back to the legacy folder scan.");
        }
    }

    // Amortise the sweep: re-stat a slice of the tracked set each frame, so a save is
    // noticed within a few frames and the per-frame syscall cost does not scale with
    // the number of loaded scripts.
    constexpr std::size_t kFilesPerFrame = 8;
    if (m_fileTimestamps.empty()) return;
    if (m_hotReloadCursor >= m_fileTimestamps.size()) m_hotReloadCursor = 0;

    auto       it        = m_fileTimestamps.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(m_hotReloadCursor));
    std::size_t examined = 0;

    // Stat first, reload after: a reload can track a NEW file (a system that
    // imports a module writes m_fileTimestamps[newPath]), and that insert may
    // rehash the map under the live iterator this sweep walks with.
    std::vector<std::string> changed;
    for (; it != m_fileTimestamps.end() && examined < kFilesPerFrame; ++examined)
    {
        const std::string& path = it->first;
        auto&              oldTime = it->second;
        ++it;
        ++m_hotReloadCursor;

        try {
            auto newTime = fs::last_write_time(Sys::U8Path(path));
            if (newTime == oldTime) continue;
            oldTime = newTime;
            changed.push_back(path);
        } catch (...) {}
    }
    for (const std::string& path : changed)
        ReloadChangedScript(path);
}

// One changed script, with its receipt: which kind of script it was, whether
// the reload threw or logged an error, and from which frame the new code runs.
void ScriptSystem::ReloadChangedScript(const std::string& path)
{
    // Any reload invalidates the dispatch cache, over-broad on purpose (a module edit
    // bumps it too): reloads are rare and the refill is one table walk per row.
    ++m_luaDispatchGen;

    const std::uint64_t receipt = HotReload::Open("lua", { path });
    HotReload::Start(receipt);
    std::vector<CapturedLogLine> captured;
    const char* category = "untracked";
    bool ok = true;
    try
    {
        Logger::ScopedContext capture(Logger::CurrentSource(), Logger::CurrentRequestId(),
                                      &captured, 32);
        category = ReloadByCategory(path, ok);
    }
    catch (const std::exception& e)
    {
        ok = false;
        captured.push_back(CapturedLogLine{ LogLevel::Error, e.what() });
    }
    catch (...)
    {
        ok = false;
        captured.push_back(CapturedLogLine{ LogLevel::Error, "unknown exception" });
    }

    HotReload::Receipt rc;
    for (const CapturedLogLine& l : captured)
        if (l.level == LogLevel::Error) rc.errors.push_back(l.message);
    const std::string cat = category;
    if (!ok || !rc.errors.empty())
    {
        rc.code    = "lua_error";
        rc.failed  = 1;
        rc.message = "the " + cat + " script " + path + " did not reload" +
                     (cat == "logic" ? " - live instances keep the previous template" : std::string());
    }
    else if (cat == "untracked")
    {
        rc.code    = "restart_required";
        rc.failed  = 1;
        rc.message = path + " is tracked but no reload rule applies to it - restart to run the new version";
    }
    else
    {
        // CheckHotReload runs AFTER this frame's Update (ScriptLogicSystem), so
        // the NEXT frame's Update is the first to run the new code (a global
        // script re-ran just now, but its handlers are called from there on).
        // A module runs again only at the next Engine.Import of it.
        rc.ok             = 1;
        rc.effectiveFrame = Logger::CurrentFrame() + 1;
        rc.message = cat == "module"
            ? "module " + path + " dropped from the import cache - the next Engine.Import of it runs the new version"
            : "the " + cat + " script " + path + " runs its new version from frame " +
              std::to_string(rc.effectiveFrame);
    }
    rc.detailJson = "{\"category\":\"" + cat + "\"}";
    HotReload::Close(receipt, rc);
}

const char* ScriptSystem::ReloadByCategory(const std::string& path, bool& ok)
{
    // Imported module? Drop the cached value so the next Engine.Import re-runs the
    // file. Checked before the template branch; a path is only ever one of these.
    {
        sol::table modules = (*m_lua)["__modules"];
        if (modules.valid())
        {
            sol::object cached = modules[path];
            if (cached.valid() && cached.get_type() != sol::type::lua_nil)
            {
                LOG_INFO("ScriptSystem: hot-reload module [%s]", path.c_str());
                modules[path] = sol::lua_nil;
                return "module";
            }
        }
    }

    // Logic template?
    if (m_logicTemplates.find(path) != m_logicTemplates.end())
    {
        LOG_INFO("ScriptSystem: hot-reload logic template [%s]", path.c_str());
        if (!LoadLogicTemplate(path))
        {
            ok = false;
            return "logic";
        }
        // Re-point every live instance's metatable.__index at the new template (an
        // entity may have several slots bound to it). Per-slot self.entity / self.slot /
        // cached fields are untouched.
        sol::table newTmpl = (*m_lua)["__logic_templates"][path];
        for (auto& [entity, st] : m_states)
        {
            for (std::size_t i = 0; i < st.slots.size(); ++i)
            {
                if (st.slots[i].path != path) continue;
                // A successful reload is the recovery path for a slot
                // parked by a previous OnUpdate throw: un-park it so
                // fixing the script actually revives the entity.
                st.slots[i].faulted = false;
                st.slots[i].lastError.clear();
                sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(entity), i);
                if (!inst.valid()) continue;
                sol::table mt = m_lua->create_table();
                mt["__index"] = newTmpl;
                inst[sol::metatable_key] = mt;
            }
        }
        return "logic";
    }

    // Global script (BT action library)? Whole-file re-run redefines
    // its Actions.*/Conditions.* handlers in place.
    if (std::find(m_globalScripts.begin(), m_globalScripts.end(), path)
        != m_globalScripts.end())
    {
        LOG_INFO("ScriptSystem: hot-reload global script [%s]", path.c_str());
        auto result = RunScriptAsset(*m_lua, path);
        if (!result.valid())
        {
            sol::error err = result;
            LOG_ERROR("ScriptSystem: global script reload error [%s]: %s",
                      path.c_str(), err.what());
            ok = false;
        }
        // Re-arm the warn-once memory so every save is a fresh diagnosis: otherwise an
        // error whose site already warned stays silent after the edit and reads as fixed.
        LuaDiag::ResetOnce();
        return "global";
    }

    // System?
    for (const auto& sys : m_systems)
    {
        if (sys.path != path) continue;
        LOG_INFO("ScriptSystem: hot-reload system [%s]", path.c_str());
        if (!LoadSystem(path)) ok = false;
        return "system";
    }

    // Service?
    for (const auto& [name, svc] : m_services)
    {
        if (svc.path != path) continue;
        LOG_INFO("ScriptSystem: hot-reload service [%s]", path.c_str());
        if (!LoadService(path)) ok = false;
        return "service";
    }

    // UI script?
    for (const auto& [name, ui] : m_uiScripts)
    {
        if (ui.path != path) continue;
        LOG_INFO("ScriptSystem: hot-reload ui-script [%s]", path.c_str());
        if (!LoadUIScript(path)) ok = false;
        return "ui";
    }
    return "untracked";
}
