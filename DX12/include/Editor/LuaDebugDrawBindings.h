#pragma once
// Debug.SetDraw, the editor half: the editor's debug-draw category toggles. Game.exe keeps
// the stub Diag::RegisterLuaDebugBindings registers (always false).

namespace sol { class state; }
class DebugDrawSystem;

namespace Editor
{
    // Replaces `Debug.SetDraw` (the table must exist: call after RegisterLuaDebugBindings).
    // @p debugDraw is App's and outlives the Lua VM.
    void RegisterLuaDebugDrawBindings(sol::state& state, DebugDrawSystem& debugDraw);
}
