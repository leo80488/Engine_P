#pragma once
namespace sol { class state; }
class World;
class AnimGraphSystem;
void RegisterLuaAnimGraphBindings(sol::state&, World&, AnimGraphSystem&);
