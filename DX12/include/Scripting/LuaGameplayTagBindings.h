#pragma once

// Tags.Add / Remove / Clear / Has / Count / List / Valid / Parent (Gameplay/GameplayTags.h).

#include "ECS/ECS.h"

namespace sol { class state; }

void RegisterLuaGameplayTagBindings(sol::state& lua, World& world);
