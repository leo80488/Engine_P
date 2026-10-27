#pragma once
// ChainLOD.* - chain-physics distance LOD from Lua.

namespace sol { class state; }
class ChainPhysicsSystem;

// Registers the global `ChainLOD` table. Registers nothing when @p chain is null (the Renderer
// has no chain system to bind).
void RegisterLuaChainLODBindings(sol::state& state, ChainPhysicsSystem* chain);
