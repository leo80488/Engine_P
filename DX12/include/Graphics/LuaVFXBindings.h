#pragma once
// VFX.* - visual-effect requests from Lua: SpawnAfterimage, the skinned-pose ghost trail of
// Graphics/AfterimageSystem.h.

#include <cstdint>
#include <functional>

namespace sol { class state; }
class World;
class Renderer;
namespace Diag { enum class DrainCause : std::uint8_t; }   // Diagnostics/OwnerAccounting.h

// Registers the global `VFX` table. @p drainFrameWork is App's (the Diag::DrainCause names what the
// drain is charged to): a sync-point call that cannot be deferred drains the frame work first.
void RegisterLuaVFXBindings(sol::state& state, World& world, Renderer& renderer,
                            std::function<void(Diag::DrainCause)> drainFrameWork);
