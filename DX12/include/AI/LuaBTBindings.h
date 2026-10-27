#pragma once

// LuaBTBindings - binds BTContext into the shared sol::state, creates the empty Actions /
// Conditions tables that the BT script libraries (asset/ai/*actions.lua) fill, and the BT status
// constants. Call once after ScriptSystem::Initialize, before any AI Lua script runs.

namespace sol { class state; }

namespace AI
{
    void RegisterLuaBTBindings(sol::state& lua);
}
