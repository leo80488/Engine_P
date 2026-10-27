#include "Platform/LuaPlatformBindings.h"

#include "Platform/BackgroundPolicy.h"
#include "Platform/PlatformServices.h"
#include "Scripting/LuaDoc.h"

#include <sol/sol.hpp>

void Plat::RegisterLuaPlatformBindings(sol::state& lua)
{
    sol::table p = lua.create_named_table("Platform");
    LuaDoc::Fn(p, "Platform", "name", []() -> std::string { return Services().Name(); },
        { .doc = "The store services in use: \"local\" (achievements and stats in the player's folder), or a "
                 "store's (\"steam\")." });
    LuaDoc::Fn(p, "Platform", "unlock", [](const std::string& id) -> bool { return Services().UnlockAchievement(id); },
        { .doc = "Unlock the achievement the game calls id; false when it was unlocked already.",
          .params = { { .name = "id" } }, .effect = LuaDoc::Effect::Disk });
    LuaDoc::Fn(p, "Platform", "isUnlocked", [](const std::string& id) -> bool { return Services().IsAchievementUnlocked(id); },
        { .doc = "Whether the achievement id is unlocked.", .params = { { .name = "id" } } });
    LuaDoc::Fn(p, "Platform", "unlocked", [](sol::this_state ts) -> sol::table
        {
            sol::table t = sol::state_view(ts).create_table();
            int i = 0;
            for (const std::string& id : Services().UnlockedAchievements()) t[++i] = id;
            return t;
        },
        { .doc = "Every unlocked achievement's id." });
    LuaDoc::Fn(p, "Platform", "resetAchievement", [](const std::string& id) -> bool { return Services().ResetAchievement(id); },
        { .doc = "Lock the achievement id again (a test's or a developer's reset); false when it was not unlocked.",
          .params = { { .name = "id" } }, .effect = LuaDoc::Effect::Disk });
    LuaDoc::Fn(p, "Platform", "setStat", [](const std::string& id, double value) -> bool { return Services().SetStat(id, value); },
        { .doc = "Set the stat id (a number kept across launches).",
          .params = { { .name = "id" }, { .name = "value" } }, .effect = LuaDoc::Effect::Disk });
    LuaDoc::Fn(p, "Platform", "stat", [](const std::string& id) -> double { return Services().GetStat(id); },
        { .doc = "The stat id's value (0 when never set).", .params = { { .name = "id" } } });
    LuaDoc::Fn(p, "Platform", "setPresence", [](const std::string& key, const std::string& value)
        { Services().SetPresence(key, value); },
        { .doc = "Set what the player's friends see (a store's rich presence; logged by the local services).",
          .params = { { .name = "key" }, { .name = "value" } } });
    LuaDoc::Fn(p, "Platform", "saveFolder", []() -> std::string { return Services().SaveFolder(); },
        { .doc = "The folder save files go to (the one a store syncs to its cloud)." });
    // What the game does while the person is not looking (Platform/BackgroundPolicy.h).
    LuaDoc::Fn(p, "Platform", "setBackgroundPolicy", [](sol::table config)
        {
            BackgroundPolicy& policy = CurrentBackgroundPolicy();
            policy.throttleMinimized = config.get_or("throttleMinimized", policy.throttleMinimized);
            policy.throttleOccluded  = config.get_or("throttleOccluded",  policy.throttleOccluded);
            policy.muteOnFocusLoss   = config.get_or("muteOnFocusLoss",   policy.muteOnFocusLoss);
            policy.pauseOnFocusLoss  = config.get_or("pauseOnFocusLoss",  policy.pauseOnFocusLoss);
        },
        { .doc = "Set what the game does while the person is not looking; fields left out keep their value. "
                 "throttleMinimized / throttleOccluded (default true): hold the frame loop to about 10 Hz while the window is minimized / "
                 "covered. muteOnFocusLoss (default true): silence the audio while the application is not the foreground. "
                 "pauseOnFocusLoss (default false): pause through GameFlow (a pause of its own that stacks with the others) "
                 "while the application is not the foreground. Never applies in the editor, nor to an automated run.",
          .params = { { .name = "config", .type = "table" } }, .effect = LuaDoc::Effect::World });
    LuaDoc::Fn(p, "Platform", "backgroundPolicy", [](sol::this_state ts) -> sol::table
        {
            const BackgroundPolicy& policy = CurrentBackgroundPolicy();
            sol::table t = sol::state_view(ts).create_table();
            t["throttleMinimized"] = policy.throttleMinimized;
            t["throttleOccluded"]  = policy.throttleOccluded;
            t["muteOnFocusLoss"]   = policy.muteOnFocusLoss;
            t["pauseOnFocusLoss"]  = policy.pauseOnFocusLoss;
            return t;
        },
        { .doc = "The background policy in effect: throttleMinimized, throttleOccluded, muteOnFocusLoss, pauseOnFocusLoss." });
}
