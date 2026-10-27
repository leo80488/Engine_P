#pragma once

// Registers the `Sequence.*` Lua table. Call once, alongside the other
// RegisterLua*Bindings entry points, after the sol state exists.

namespace sol { class state; }
class World;

void RegisterLuaSequenceBindings(sol::state& lua, World& world);
