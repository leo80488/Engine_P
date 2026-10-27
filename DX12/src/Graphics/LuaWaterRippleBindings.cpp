#include "Graphics/LuaWaterRippleBindings.h"

#include "Renderer/Renderer.h"
#include "Graphics/WaterRippleSystem.h"
#include "Diagnostics/OwnerAccounting.h"   // Diag::DrainCause

#include <sol/sol.hpp>

void RegisterLuaWaterRippleBindings(sol::state& state, Renderer& renderer,
                                    std::function<void(Diag::DrainCause)> drainFrameWork)
{
    sol::table engineTbl = state["Engine"];

    // Engine.RippleAt(x, z, radius, strength) - spawns a ripple on whichever ripple-enabled
    // WaterComponent contains the world XZ point (ignored if none). radius in world metres; strength is
    // the height impulse (negative = a dip).
    engineTbl.set_function("RippleAt",
        [&renderer, drainFrameWork](float x, float z, float radius, float strength)
        {
            // Through the structural mailbox (WaterRippleSystem::Sync consumes the queue in the scene build,
            // after the sync point).
            renderer.RunOrDeferToSyncPoint(
                [&renderer, x, z, radius, strength]
                {
                    if (auto* rs = renderer.GetWaterRippleSystem())
                        rs->AddRipple(x, z, radius, strength);
                },
                [drainFrameWork] { drainFrameWork(Diag::DrainCause::LuaRequest); });
        });
}
