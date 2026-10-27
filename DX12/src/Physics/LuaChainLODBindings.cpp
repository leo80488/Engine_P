#include "Physics/LuaChainLODBindings.h"

#include "Physics/ChainPhysicsSystem.h"

#include <sol/sol.hpp>

#include <algorithm>

// The system lives on the Renderer; App hands it over. Same contract as AILOD: OFF by default,
// because a per-frame quality rewrite silently invalidates any measurement taken over it.
void RegisterLuaChainLODBindings(sol::state& state, ChainPhysicsSystem* chain)
{
    sol::state* lua = &state;
    if (chain)
    {
        sol::table ct = lua->create_table();
        ct.set_function("SetEnabled", [chain](bool on) { chain->lodEnabled = on; });
        ct.set_function("IsEnabled",  [chain]() -> bool { return chain->lodEnabled; });
        ct.set_function("SetTiers", [chain](sol::table t)
        {
            auto grab = [&t](const char* k, float& dst)
            {
                sol::optional<float> v = t[k];
                if (v) dst = *v;
            };
            grab("tier1",      chain->lodTiers.tier1Distance);
            grab("tier2",      chain->lodTiers.tier2Distance);
            grab("hysteresis", chain->lodTiers.hysteresis);
            // The shadow-only profile (ChainPhysicsSystem::LodTiers): 0 = off.
            if (sol::optional<int> v = t["shadowIterations"]) chain->lodTiers.shadowOnlyIterations = (std::max)(0, *v);
            if (sol::optional<int> v = t["shadowSubsteps"])   chain->lodTiers.shadowOnlySubCap     = (std::max)(1, *v);
        });
        (*lua)["ChainLOD"] = ct;
    }
}
