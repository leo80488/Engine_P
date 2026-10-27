#include "Scripting/BootScripts.h"

#include "Harness/ExitCodes.h"
#include "Module/ModuleRegistry.h"   // a script module the project leaves off
#include "Resource/AssetFS.h"
#include "Scripting/ScriptManifest.h"
#include "Scripting/ScriptSystem.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"
#include "System/Log.h"

#include <string>
#include <vector>

// A refusal posts PostQuitMessage; the caller must not return early on it, so App::Run's
// orderly tail still saves the PSO cache.
bool Scripting::RunBootScripts(ScriptSystem& scriptSystem)
{
    bool bootScriptsRefused = false;
    {
        auto runLegacyBootScan = [&scriptSystem]()
        {
            // Migration shim: globals were never part of the folder scan, so this branch loads the
            // BT libraries itself, or a no-manifest boot registers zero Actions/Conditions and every
            // behaviour tree dies silently with exit 0. Order matters: base first, then extensions
            // that append (enemy_actions deliberately wins the Actions.ChaseCamera collision), bench
            // last (Bench*-prefixed names only). Must run after RegisterLuaBTBindings
            // (InitLuaBindings) and the Nav module's RegisterLua.
            // TODO(Lua_Script_Manifest_Implementation_Plan M6): retire this shim with the folder scan.
            constexpr const char* kBTScripts[] = {
                "asset/ai/actions.lua",
                "asset/ai/enemy_actions.lua",
                "asset/ai/bench_actions.lua",
            };
            for (const char* p : kBTScripts)
                if (Module::Registry::Get().IsScriptEnabled(p))   // the AIActions script module
                    scriptSystem.AddGlobalScript(p);
            // Scan loads services/*.lua, systems/*.lua, ui/*.lua; Logic
            // templates load lazily on first ScriptComponent sight.
            scriptSystem.ScanScriptDirectory("asset/scripts");
        };

        // A packed non-editor boot trusts only the pak: a loose manifest beside a shipped install
        // must not inject scripts through the AssetFS disk fallback.
        auto& afs = ::Resource::AssetFS::Get();
        const bool pakOnly = !Sys::IsEditorHost() && afs.IsMounted();
        const bool haveManifest = pakOnly
            ? afs.HasInPakExact(Scripting::kScriptManifestPath)
            : afs.Exists(Scripting::kScriptManifestPath);

        // An attended editor degrades per entry so a typo does not kill the dev loop. Everything
        // else (Game.exe, or any host under -unattended: harness packs run Editor.exe too) refuses
        // before executing a single chunk: an automated run on a partial registry is a false PASS.
        const bool refuseMode = !Sys::IsEditorHost()
                             || Sys::CommandLine::Get().Has("-unattended");

        if (!haveManifest)
        {
            LOG_INFO("ScriptSystem: no %s — legacy folder scan (directory implies kind)",
                     Scripting::kScriptManifestPath);
            runLegacyBootScan();
        }
        else
        {
            Scripting::ScriptManifest bootManifest;
            std::vector<std::string> manifestErrors;
            const bool usable = Scripting::LoadScriptManifest(
                Scripting::kScriptManifestPath, pakOnly, bootManifest, manifestErrors);
            for (const std::string& err : manifestErrors)
                LOG_ERROR("ScriptManifest: %s", err.c_str());
            // A script module the project leaves off (game.json "modules"): its entries load as
            // disabled ones (logged by LoadBootScripts); a package does not even carry the files.
            for (Scripting::BootScriptEntry& e : bootManifest.entries)
                if (e.enabled && !Module::Registry::Get().IsScriptEnabled(e.path))
                {
                    e.enabled = false;
                    LOG_INFO("[MODULES] boot script %s: its module is off in this project", e.path.c_str());
                }

            if (!usable)
            {
                // FILE-LEVEL hard error (unreadable / parse / version). Never
                // silently pretend manifest mode wasn't selected.
                if (refuseMode)
                {
                    LOG_ERROR("Script manifest present but unusable — refusing to boot. "
                              "Fix or delete %s.", Scripting::kScriptManifestPath);
                    PostQuitMessage(Harness::PreconditionFail);
                    bootScriptsRefused = true;
                }
                else
                {
                    LOG_ERROR("Script manifest present but unusable — falling back to the "
                              "LEGACY FOLDER SCAN so the editor stays workable. Boot-script "
                              "kinds/order come from directories until the manifest is fixed.");
                    runLegacyBootScan();
                }
            }
            else if (!manifestErrors.empty() && refuseMode)
            {
                // Entry-level errors in an unattended / packaged run: refuse before LoadBootScripts,
                // so a refused boot executes no chunk's top-level side effects.
                LOG_ERROR("Script manifest has %zu invalid entr%s — refusing to boot "
                          "before running any script.",
                          manifestErrors.size(), manifestErrors.size() == 1 ? "y" : "ies");
                PostQuitMessage(Harness::PreconditionFail);
                bootScriptsRefused = true;
            }
            else
            {
                // Attended editor: invalid entries were already dropped by the
                // parser (and logged above); boot the survivors.
                scriptSystem.LoadBootScripts(bootManifest);
            }
        }
    }
    return bootScriptsRefused;
}
