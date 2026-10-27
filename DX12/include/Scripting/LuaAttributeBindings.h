#pragma once

// Attr.Init / Get / GetBase / Set / SetCurrent / Add / Has / List / Remove / Profile / ResistName
// (Gameplay/AttributeSet.h).

#include "ECS/ECS.h"

namespace sol { class state; }

void RegisterLuaAttributeBindings(sol::state& lua, World& world);
