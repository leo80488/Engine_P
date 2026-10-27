#include "Graphics/LuaVFXBindings.h"

#include "ECS/ECS.h"
#include "Renderer/Renderer.h"
#include "Graphics/AfterimageSystem.h"
#include "Diagnostics/OwnerAccounting.h"   // Diag::DrainCause

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"         // LuaEntity + LuaId::* (needs sol first)

#include <DirectXMath.h>

// VFX.SpawnAfterimage(entity, lifetime, r, g, b) - pushes a skinned-pose ghost-trail snapshot into
// the renderer's afterimage pool (HDR colour).
void RegisterLuaVFXBindings(sol::state& state, World& world, Renderer& renderer,
                            std::function<void(Diag::DrainCause)> drainFrameWork)
{
    sol::state* lua = &state;
    sol::table vfx = lua->create_named_table("VFX");
    vfx.set_function("SpawnAfterimage",
        [&world, &renderer, drainFrameWork](LuaEntity entity, float lifetime, float r, float g, float b)
        {
            // Generation-checked: a stale id would snapshot whatever entity recycled the slot - a ghost trail of
            // the wrong character.
            const Entity e = LuaId::ResolveLive(world, entity);
            if (e == NullEntity) return;
            // Through the structural mailbox: the id resolves now, the spawn lands at this frame's sync point
            // before BeginFrame consumes it, FIFO with the deferred destroys, so a destroy later this frame still
            // drops it.
            renderer.RunOrDeferToSyncPoint(
                [&renderer, e, lifetime, r, g, b]
                {
                    if (auto* sys = renderer.GetAfterimageSystem())
                        sys->Spawn(e, lifetime, DirectX::XMFLOAT4{ r, g, b, 1.0f });
                },
                [drainFrameWork] { drainFrameWork(Diag::DrainCause::LuaRequest); });
        });
}
