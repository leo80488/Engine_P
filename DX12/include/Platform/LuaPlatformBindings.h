#pragma once

// Platform/LuaPlatformBindings.h - the Platform Lua table over Plat::Services(): achievements, stats,
// presence, the save folder (Platform/PlatformServices.h).

namespace sol { class state; }

namespace Plat
{
    void RegisterLuaPlatformBindings(sol::state& lua);
}
