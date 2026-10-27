#include "Graphics/LuaDDGIBindings.h"

#include "Renderer/Renderer.h"
#include "Graphics/DDGIVolumeManager.h"
#include "Diagnostics/OwnerAccounting.h"   // Diag::DrainCause
#include "System/Log.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <tuple>

void RegisterLuaDDGIBindings(sol::state& state, Renderer& renderer,
                             std::function<void(Diag::DrainCause)> drainFrameWork)
{
    sol::table engineTbl = state["Engine"];

    // DDGI probe watch (relocation diagnostics).
    //   Engine.DDGIWatchProbe(x, y, z [, volumeSlot]) - watch one probe (grid coords) of a DDGI volume;
    //     Engine.DDGIWatchProbe(-1) clears. The next DDGI tick latches the request, so the first capture
    //     of a new target lands a few frames later.
    //   Engine.DDGIWatchedProbe() -> valid, offX, offY, offZ, state, fresh, bfSmooth, capture, sky,
    //     skyDirX, skyDirY, skyDirZ - the latest GPU readback of that probe's ProbeData. state 0 = ACTIVE,
    //     1 = INACTIVE; bfSmooth -1 = never measured; capture is a monotonic counter, at best one per
    //     DDGIVolumeManager::kProbeWatchPeriodFrames frames; sky / skyDir = the sky visibility's zeroth
    //     and first moments.
    // The ddgi_relocation pack drives an obstacle onto a probe and asserts it relocates and returns to
    // the grid once the obstacle leaves.
    engineTbl.set_function("DDGIWatchProbe",
        [&renderer, drainFrameWork](int x, sol::optional<int> y, sol::optional<int> z, sol::optional<int> slot) {
            const int yy = y.value_or(-1), zz = z.value_or(-1);
            if (x >= 0 && (yy < 0 || zz < 0))
            {
                LOG_WARNING("Engine.DDGIWatchProbe: y and z must be >= 0 (got %d, %d) — ignored", yy, zz);
                return;
            }
            const uint32_t sl = slot ? static_cast<uint32_t>((std::max)(*slot, 0)) : 0u;
            // Through the structural mailbox (DDGIVolumeManager's Tick latches the request after the sync point
            // either way).
            renderer.RunOrDeferToSyncPoint(
                [&renderer, x, yy, zz, sl]
                {
                    if (x < 0) renderer.GetDDGIManager().ClearProbeWatch();
                    else       renderer.GetDDGIManager().SetProbeWatch(x, yy, zz, sl);
                },
                [drainFrameWork] { drainFrameWork(Diag::DrainCause::LuaRequest); });
        });
    engineTbl.set_function("DDGIWatchedProbe",
        [&renderer]() -> std::tuple<bool, float, float, float, int, bool, float, int, float, float, float, float> {
            const auto& w = renderer.GetDDGIManager().GetProbeWatch();
            const uint32_t enc = (w.stateRaw >> 8) & 0xFFu;
            const float    bf  = enc ? float(enc - 1u) / 254.0f : -1.0f;
            return { w.valid, w.offset.x, w.offset.y, w.offset.z,
                     int(w.stateRaw & 0x3u), (w.stateRaw & 0x4u) != 0u,
                     bf, int(w.capture),
                     w.skyVisibility, w.skyVisDir.x, w.skyVisDir.y, w.skyVisDir.z };
        });
}
