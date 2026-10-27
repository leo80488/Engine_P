#include "ECS/LuaHitboxBindings.h"

#include "ECS/ECS.h"
#include "ECS/SystemRegistry.h"
#include "ECS/NotifyConsumerSystems.h"   // HitboxSystem
#include "ECS/AnimEntityResolve.h"       // Anim::ResolveAnimEntityOrSelf

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"       // LuaEntity + LuaId::* (needs sol first)

void RegisterLuaHitboxBindings(sol::state& state, World& world, SystemRegistry& systemRegistry)
{
    sol::state* lua = &state;
    (*lua)["Hitbox"]["IsActive"]=[&world, &systemRegistry](LuaEntity any,int slot){
        const auto entity=Anim::ResolveAnimEntityOrSelf(world,LuaId::ResolveLive(world,any));
        for(const auto& system:systemRegistry.GetSystems(TickPhase::BoneAttachment))
            if(const auto* hitbox=dynamic_cast<const HitboxSystem*>(system.get()))return hitbox->IsActive(world,entity,slot);
        return false;
    };
}
