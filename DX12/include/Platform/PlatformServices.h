#pragma once

// Platform/PlatformServices.h - what a store platform gives a game: achievements and stats, a save
// folder it syncs, rich presence. One implementation is active at a time (Plat::SetServices); the
// default is the local one: achievements and stats in the player's folder, saves where they always were,
// no presence. A store's implementation (Steam: the Steamworks SDK, which needs a partner account and
// is not vendored) registers itself from its module at process start. Lua: the Platform table.

#include <memory>
#include <string>
#include <vector>

class IPlatformServices
{
public:
    virtual ~IPlatformServices() = default;

    // "local", "steam", ...
    virtual const char* Name() const = 0;
    // Once a frame on the main thread: a store's callbacks.
    virtual void Update() {}

    // ---- Achievements: an id the game names, unlocked once for good ----
    // False when @p id was already unlocked or could not be recorded.
    virtual bool                     UnlockAchievement(const std::string& id) = 0;
    virtual bool                     IsAchievementUnlocked(const std::string& id) const = 0;
    virtual std::vector<std::string> UnlockedAchievements() const = 0;
    // Locks it again: a test's, a developer's reset. False when it was not unlocked.
    virtual bool                     ResetAchievement(const std::string& id) = 0;

    // ---- Stats: a number per id the game names (kills, distance), kept across launches ----
    virtual bool   SetStat(const std::string& id, double value) = 0;
    virtual double GetStat(const std::string& id) const = 0;

    // ---- Rich presence: what the player's friends see ("In the dojo") ----
    virtual void SetPresence(const std::string& key, const std::string& value) = 0;

    // ---- Saves ----
    // The folder save files go to: the one a store syncs to its cloud.
    virtual std::string SaveFolder() const = 0;
};

namespace Plat
{
    // The active implementation: the local one until something sets another.
    IPlatformServices& Services();
    // Replaces the active implementation (a store's module at process start); nullptr goes back to the
    // local one.
    void SetServices(std::unique_ptr<IPlatformServices> services);
}
