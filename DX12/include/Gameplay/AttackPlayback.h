#pragma once
#include "Gameplay/AttackDefinition.h"
#include "ECS/ECS.h"
namespace sol { class state; }
namespace Resource { class AnimationClipSystem; }
namespace Attack {
// Runtime-only; owned by the animator entity, with the gameplay root preserved.
struct Playback {
    std::shared_ptr<const Definition> definition;
    std::string alias;
    Entity owner=NullEntity;
    uint32_t ownerGeneration=0,instance=0;
    double elapsed=0;
    float previous=0,progress=0,duration=1;
    uint8_t windows=0;
    bool active=false,started=false;
};
bool Advance(World&,Entity,const std::string& alias,float duration,float dt,float& time);
void RegisterLua(sol::state&,World&);
}
