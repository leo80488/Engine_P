#pragma once
namespace sol { class state; }
class World;
void RegisterLuaIKBindings(sol::state&,World&);
