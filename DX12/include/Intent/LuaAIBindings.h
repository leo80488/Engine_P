#pragma once

// LuaAIBindings - the AI-facing Lua tables on the engine's sol::state. Published:
// AIBehavior.*, Perception.* (read-only), AIStats.*, AnimStats.*, AILOD.*.
// The strategic goal table `AI.*` (the counterpart to the tactical `Intent.*`) is built
// but not published: nothing consumes AIIntentComponent (see RegisterLuaAIBindings).
//   AI.SetGoal(entityId, goalNameOrId, targetEntity?, gx?, gy?, gz?)
//   AI.ClearGoal(entityId)
//   AI.GetGoal(entityId)                 -> "Idle" / "Attack" / ...
//   AI.GetTarget(entityId)               -> entityId (or 0 when unset)
//   AI.Goal                              -> table { Idle=0, Patrol=1, ... }
// AI.Goal.Attack etc. spare BT leaves magic strings; SetGoal accepts the names too.

// AILOD.*, published under its own table so it cannot resurrect `AI`:
//   AILOD.SetEnabled(bool)               -- stop rewriting tickInterval
//   AILOD.IsEnabled()                    -> bool
//   AILOD.SetTickInterval(eid, seconds)  -- author one agent's cadence
//   AILOD.SetTiers{ combat=, distant=, offscreen=, boss=,
//                   combatRadius=, distantRadius= }

namespace sol { class state; }
class World;
namespace AI { class AILODSystem; class PerceptionSystem; }

namespace Intent
{
    void RegisterLuaAIBindings(sol::state& lua, ::World& world,
                               AI::AILODSystem& lod,
                               AI::PerceptionSystem& perception);
}
