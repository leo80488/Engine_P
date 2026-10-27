#pragma once
// Engine.DDGIWatchProbe / Engine.DDGIWatchedProbe - the DDGI probe-relocation watch
// (Graphics/DDGIVolumeManager.h).

#include <cstdint>
#include <functional>

namespace sol { class state; }
class Renderer;
namespace Diag { enum class DrainCause : std::uint8_t; }   // Diagnostics/OwnerAccounting.h

// Appends to the `Engine` table. @p drainFrameWork is App's (the Diag::DrainCause names what the
// drain is charged to): a sync-point call that cannot be deferred drains the frame work first.
void RegisterLuaDDGIBindings(sol::state& state, Renderer& renderer,
                             std::function<void(Diag::DrainCause)> drainFrameWork);
