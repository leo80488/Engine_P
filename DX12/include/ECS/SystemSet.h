#pragma once

// Phase answers "when does this system run?"; SystemSet answers "under which game-flow conditions
// is it enabled?". The two scheduler dimensions are independent on purpose.

#include "System/StringHash.h"

#include <string_view>

using SystemSetId = StringHash;

// The shared set vocabulary.
//
// These are set names, not state names, which keeps the engine content-agnostic: which capability
// bucket PhysicsStepSystem belongs to is engine knowledge; which buckets MainMenu turns on is
// game knowledge and stays in Lua.
// Constants rather than literals at each call site: a set name is matched by hash, and a typo in
// either place that must agree (C++ registration, Lua state definition) fails silently - the
// system never runs once a flow state is active. A compile-checked symbol removes half of that.
namespace Sets
{
// Anything that advances the simulation: AI, navigation, player control, the
// character motor, the physics step, the animation driver.
inline constexpr std::string_view kGameplaySimulation   = "GameplaySimulation";
// Game logic written in Lua plus the C++ systems that service it: script
// timers, entity Logic dispatch, projectiles, lifetime expiry.
inline constexpr std::string_view kGameplayScripts      = "GameplayScripts";
// Turning simulation state into what you see: the skinned animation chain,
// notify timelines, dissolve lerps.
inline constexpr std::string_view kGameplayPresentation = "GameplayPresentation";
// Notify consumers and one-shot effects: hitboxes, state toggles, camera
// effects, VFX spawning.
inline constexpr std::string_view kGameplayFX           = "GameplayFX";
// Diegetic audio driven by the world (3D emitters, collision SFX). Not the music bus - a pause
// menu that kills its own BGM is a bug, not a feature.
inline constexpr std::string_view kGameplayAudio        = "GameplayAudio";
// Sequencer and the motion directors it drives.
inline constexpr std::string_view kCinematics           = "Cinematics";
// World / screen UI canvas layout. Separate from every bucket above on purpose: a pause menu is a
// state that keeps this and drops the rest.
inline constexpr std::string_view kGameplayUI           = "GameplayUI";
} // namespace Sets
