#pragma once

#include <cstdint>

// Lua bindings for CharacterControllerComponent, PlayerComponent and the camera, registered by
// App::InitLuaBindings:
//
//   Character.*  CharacterControllerComponent: ground state, drive velocity, jumps, teleport,
//                capsule, momentum ramps, move lock (dash / knockback).
//   Player.*     PlayerComponent: walk/run speeds, air control, turn rate, facing mode, camera
//                entity, MakePlayer.
//   Camera.*     the active-camera tag and the camera stack (PushVCam / PopVCam / HardCutTo /
//                AddShake).
//
// The getters and setters expect the matching component: when it is missing they log a warning and
// return defaults / false instead of auto-attaching, so a script typo never spawns components on an
// unrelated entity.

namespace sol { class state; }
class World;

void RegisterLuaPlayerBindings(sol::state& lua, World& world);

// Engine.GetPlayer: appends to the `Engine` table. @p cameraEntity is App's main-camera entity, read
// at call time (the fallback when no entity carries PlayerComponent).
using Entity = std::uint32_t;   // ECS/ECS.h
void RegisterLuaPlayerEngineBindings(sol::state& state, World& world, const Entity& cameraEntity);
