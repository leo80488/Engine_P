#pragma once
// Profiler.* - the engine profiler's runtime surface from Lua.

namespace sol { class state; }
class IGraphicsDevice;

namespace Diag
{
    // Registers the global `Profiler` table. @p gfx is used only by the harness-only
    // Profiler.DebugLeakGpu.
    void RegisterLuaProfilerBindings(sol::state& state, IGraphicsDevice* gfx);
}
