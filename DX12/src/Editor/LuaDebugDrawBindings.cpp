#include "Editor/LuaDebugDrawBindings.h"

#include "Editor/DebugDrawSystem.h"

#include <sol/sol.hpp>

#include <string>
#include <utility>

void Editor::RegisterLuaDebugDrawBindings(sol::state& state, DebugDrawSystem& debugDraw)
{
    sol::table dbg = state["Debug"];
    dbg.set_function("SetDraw", [&debugDraw](const std::string& name, bool on) -> bool
    {
        static const std::pair<const char*, DebugCategory> kNames[] = {
            { "collision", DebugCategory::Collision }, { "navsoup", DebugCategory::NavSoup },
            { "navmesh", DebugCategory::NavMesh }, { "aabb", DebugCategory::AABB },
            { "frustum", DebugCategory::Frustum }, { "capsules", DebugCategory::Capsules },
            { "probes", DebugCategory::ReflectionProbes }, { "ddgi", DebugCategory::DDGIVolumes },
            { "ppvolumes", DebugCategory::PostProcessVolumes }, { "decals", DebugCategory::Decals },
            { "lightranges", DebugCategory::LightRanges }, { "fog", DebugCategory::FogVolumes },
            { "physics", DebugCategory::Physics },
        };
        for (const auto& [n, cat] : kNames)
            if (name == n)
            {
                DebugCatSet(debugDraw.categoryMask, cat, on);
                if (on) debugDraw.masterEnabled = true;
                return true;
            }
        return false;
    });
}
