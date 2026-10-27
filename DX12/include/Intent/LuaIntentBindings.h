#pragma once

// LuaIntentBindings - `Intent.*` on the engine's sol::state, the tactical-intent
// producer interface: BT actions and cutscenes write here; NavAgentSystem reads.
//   Intent.MoveTo(entityId, x, y, z)       -> bool
//   Intent.Stop(entityId)                  -> bool
//   Intent.LookAt(entityId, x, y, z)       -> bool  (facingMode = FaceTarget)
//   Intent.FaceEntity(entityId, targetId)  -> bool  (track a moving entity)
//   Intent.ClearLookAt(entityId)           -> bool  (back to FaceMovement)
//   Intent.HasMoveTarget(entityId)         -> bool
//   Intent.SetSpeedScale(entityId, scale)  -> bool
//   Intent.Wake(entityId)                  -> bool  (tick the BT next frame)
//   Intent.RequestStop(entityId)           -> bool  (cutscene escape hatch: zeroes
//                                                    CCC velocity next step)
// MoveTo / LookAt / FaceEntity / SetSpeedScale add a NavAgentComponent on first touch,
// so scripts need not AddComponent. The strategic `AI.*` goal layer
// (Intent/LuaAIBindings.cpp) is built but not published.

namespace sol { class state; }
class World;   // global ::World

namespace Intent
{
    void RegisterLuaIntentBindings(sol::state& lua, ::World& world);
}
