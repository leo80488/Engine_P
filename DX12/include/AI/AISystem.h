#pragma once

// AISystem - loads behaviour trees and ticks them for every AIComponent.
// Runs as AIBTTickSystem in TickPhase::AI, after the Lua script phases (a script's blackboard write
// is seen the same frame) and after AILOD and Perception (a tree reads this frame's sighting); before
// NavAgent, CharacterMotor and the physics step, so a BT-issued move is consumed the same frame.

#include <vector>
#include "ECS/ECS.h"
#include "AI/ActionRegistry.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace sol { class state; }

namespace AI
{
    class BTAsset;

    class AISystem
    {
    public:
        // Last frame's population and how many of them evaluated a tree (read by the AI benchmark).
        uint32_t LastTickCount()  const { return m_ticksThisFrame; }
        uint32_t LastAgentCount() const { return m_agentsThisFrame; }
        AISystem();
        ~AISystem();

        // One-time wiring. Borrows ScriptSystem's sol::state; null disables Lua loading and dispatch.
        void Init(sol::state* lua);

        // Per-frame tick: advances the clock of every enabled agent that has a tree and ticks the due ones,
        // most overdue first, up to the per-frame budget (the anti-convoy notes are in Update,
        // AdvanceAndMeasure and TickEntity).
        void Update(World& world, float dt);

        // Synchronous load: runs the file in the shared sol::state (it returns a table), converts the
        // table to a node tree, assigns NodeIds and caches it by path. Returns nullptr on failure; the
        // failure is cached too, so later calls with the same path return the cached result until
        // CheckHotReload sees the file change.
        std::shared_ptr<BTAsset> AcquireTree(const std::string& path);

        // Re-parses a tree even if cached and replaces the cache entry. Existing AIComponent::tree pointers
        // are not patched: assign the result yourself or rely on CheckHotReload. On failure returns nullptr
        // and leaves the cached entry untouched.
        std::shared_ptr<BTAsset> ReloadTree(const std::string& path);

        // Call once per frame. Re-stats a few watched BT files per frame and re-parses a changed one: on
        // success every AIComponent using that path gets the new tree with its instance state reset; on
        // failure running agents keep the old tree but the cache entry becomes null (unlike ReloadTree), so
        // AcquireTree returns nullptr until the file is fixed. Hot-reload hosts only
        // (Sys::HotReloadEnabled), like ScriptSystem::CheckHotReload.
        void CheckHotReload(World& world);

        ActionRegistry& Actions() { return m_actions; }

        // Sum of Update's dt; the absolute clock for cooldown decorators.
        float Elapsed() const { return m_elapsed; }

    private:
        struct DueAgent { float overdue; Entity entity; };

        void TickEntity(World& world, Entity e, float dt);
        void OnTreeReloaded(World& world, const std::string& path);

        // Reads the file (AssetFS: pak first, then loose disk) and parses it on every call, never from the
        // cache (the caller does that lookup). Registers the file's mtime for CheckHotReload before anything
        // that can fail, so a tree that does not parse is still watched. Returns nullptr and logs on failure.
        std::shared_ptr<BTAsset> ParseFromFile(const std::string& path);

        // Advances one agent's clock without ticking it and returns how overdue it is (< 0: not due or
        // cannot run; a wake request returns a huge value so it sorts first). Adds the agent's arrival rate
        // (dt / tickInterval) to expected, which sizes the per-frame budget in Update. Resolves a
        // deserialized agent's treePath through AcquireTree first.
        float AdvanceAndMeasure(World& world, Entity e, float dt, float& expected);

        sol::state*     m_lua = nullptr;
        ActionRegistry  m_actions;

        // path -> shared tree; every agent using a path aliases one parsed BT.
        std::unordered_map<std::string, std::shared_ptr<BTAsset>> m_loaded;

        // Mtime watch for hot reload (as in ScriptSystem).
        std::unordered_map<std::string, std::filesystem::file_time_type> m_fileTimestamps;
        // Round-robin cursor for the amortised hot-reload sweep.
        std::size_t m_hotReloadCursor = 0;

        float m_elapsed = 0.f;

        // Agents whose tree ticked this frame, and the population (both reset each Update). Kept in every
        // configuration, not only under Tracy: the AI benchmark needs "how many trees ran this frame" to
        // tell a per-agent cost from a convoy spike.
        uint32_t m_ticksThisFrame = 0;
        uint32_t m_agentsThisFrame = 0;

        // Agents whose interval elapsed this frame and how far past it they are; sorted most-overdue-first
        // and cut to the per-frame budget in Update. A member so its allocation is reused every frame.
        std::vector<DueAgent> m_due;
    };
}
