#pragma once

// GameState - a process-global, scene-independent key/value store for gameplay flags, counters
// and a simple item inventory: state that survives scene transitions ("did the player collect the
// gold key?", "is the vault looted?"). The per-entity BlackboardComponent is BT-only, and a
// System-script Lua table is wiped on scene reload (ScriptSystem::ClearAll).
//   * Header-only inline singleton: the function-local static in Get() is one instance across
//     all translation units.
//   * Deliberately NOT cleared by ScriptSystem::ClearAll / world reload. Call ClearAll()
//     (Lua: Engine.ClearState()) at "New Game" to wipe it.
//   * Disk persistence is Gameplay/SaveGame.h (Engine.SaveGame / Engine.LoadGame): the three
//     maps, the graveyard and playtime in a JSON slot file.
// Main thread only, like the rest of the gameplay/ECS layer.

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Gameplay
{

class GameState
{
public:
    static GameState& Get()
    {
        static GameState s_instance;
        return s_instance;
    }

    // ---- Numbers (also back bools) ----
    void   SetNumber(const std::string& key, double v) { m_numbers[key] = v; }
    double GetNumber(const std::string& key, double def = 0.0) const
    {
        auto it = m_numbers.find(key);
        return it != m_numbers.end() ? it->second : def;
    }

    void SetBool(const std::string& key, bool v) { m_numbers[key] = v ? 1.0 : 0.0; }
    bool GetBool(const std::string& key, bool def = false) const
    {
        auto it = m_numbers.find(key);
        return it != m_numbers.end() ? (it->second != 0.0) : def;
    }

    // ---- Strings ----
    void SetString(const std::string& key, std::string v) { m_strings[key] = std::move(v); }
    std::string GetString(const std::string& key, const std::string& def = "") const
    {
        auto it = m_strings.find(key);
        return it != m_strings.end() ? it->second : def;
    }

    // ---- Inventory (named integer counters) ----
    // Returns the NEW count after applying delta; a negative delta consumes, and the count is
    // clamped at 0.
    int AddItem(const std::string& item, int delta)
    {
        int n = m_items[item] + delta;
        if (n < 0) n = 0;
        m_items[item] = n;
        return n;
    }
    int  GetItem(const std::string& item) const
    {
        auto it = m_items.find(item);
        return it != m_items.end() ? it->second : 0;
    }
    bool HasItem(const std::string& item, int atLeast = 1) const
    {
        return GetItem(item) >= atLeast;
    }

    // ---- Existence / removal ----
    bool Has(const std::string& key) const
    {
        return m_numbers.count(key) || m_strings.count(key) || m_items.count(key);
    }
    void Erase(const std::string& key)
    {
        m_numbers.erase(key);
        m_strings.erase(key);
        m_items.erase(key);
    }
    void ClearAll()
    {
        m_numbers.clear();
        m_strings.clear();
        m_items.clear();
        m_destroyedPersistents.clear();
        m_playtime = 0.0;
    }

    // ---- Destroyed-persistents graveyard ----
    // Guid hex strings of PersistentTag entities destroyed at RUNTIME, recorded by the World destroy
    // listener Gameplay::InstallPersistenceHooks installs (World::Clear fires no listeners, so scene
    // switches never graveyard anything). The save persists it; ApplySavedEntities destroys matching
    // scene-authored entities after load so "picked up" things stay gone. ClearAll wipes it.
    void MarkPersistentDestroyed(const std::string& guidHex)   { m_destroyedPersistents.insert(guidHex); }
    void UnmarkPersistentDestroyed(const std::string& guidHex) { m_destroyedPersistents.erase(guidHex); }
    const std::unordered_set<std::string>& DestroyedPersistents() const { return m_destroyedPersistents; }
    void ReplaceDestroyedPersistents(std::unordered_set<std::string> s)
    {
        m_destroyedPersistents = std::move(s);
    }

    // ---- Playtime ----
    // Wall-clock seconds while the sim is advancing: ticked with REAL dt by ScriptTimerSystem, which
    // is RunMask::Play (editor Edit mode does not count). Persisted by SaveGame; double so 40+ hour
    // saves keep precision.
    void   AddPlaytime(double seconds) { if (seconds > 0.0) m_playtime += seconds; }
    void   SetPlaytime(double seconds) { m_playtime = seconds < 0.0 ? 0.0 : seconds; }
    double GetPlaytime() const         { return m_playtime; }

    // ---- Whole-store snapshot ----
    // Everything the store holds, by value: the editor's Play takes one on entry and puts it back
    // on Stop, so a played session's flags, items, graveyard and playtime never reach the edit
    // world (PlayModeSession).
    struct Snapshot
    {
        std::unordered_map<std::string, double>      numbers;
        std::unordered_map<std::string, std::string> strings;
        std::unordered_map<std::string, int>         items;
        std::unordered_set<std::string>              destroyedPersistents;
        double                                       playtime = 0.0;
    };
    Snapshot Capture() const
    {
        return { m_numbers, m_strings, m_items, m_destroyedPersistents, m_playtime };
    }
    void Restore(Snapshot s)
    {
        m_numbers              = std::move(s.numbers);
        m_strings              = std::move(s.strings);
        m_items                = std::move(s.items);
        m_destroyedPersistents = std::move(s.destroyedPersistents);
        m_playtime             = s.playtime;
    }

    // ---- Bulk access for the save-game writer (Gameplay/SaveGame.cpp) ----
    const std::unordered_map<std::string, double>&      Numbers() const { return m_numbers; }
    const std::unordered_map<std::string, std::string>& Strings() const { return m_strings; }
    const std::unordered_map<std::string, int>&         Items()   const { return m_items;   }
    void ReplaceAll(std::unordered_map<std::string, double>      numbers,
                    std::unordered_map<std::string, std::string> strings,
                    std::unordered_map<std::string, int>         items)
    {
        m_numbers = std::move(numbers);
        m_strings = std::move(strings);
        m_items   = std::move(items);
    }

private:
    GameState() = default;

    std::unordered_map<std::string, double>      m_numbers;
    std::unordered_map<std::string, std::string> m_strings;
    std::unordered_map<std::string, int>         m_items;
    std::unordered_set<std::string>              m_destroyedPersistents;
    double                                       m_playtime = 0.0;
};

} // namespace Gameplay
