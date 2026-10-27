#pragma once
// Preview.* - the in-game picture-in-picture from Lua.

namespace sol { class state; }
class Renderer;
class World;

namespace Preview
{
    // Registers the global `Preview` table. The closures keep @p renderer and @p world
    // by pointer: both outlive the Lua VM (App declares the VM last).
    void RegisterLuaPreviewBindings(sol::state& state, Renderer& renderer, World& world);
}
