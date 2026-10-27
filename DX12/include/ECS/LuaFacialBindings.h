#pragma once

// Lua bindings for FacialAnimationComponent: the `Face.*` table (morph targets / blend shapes:
// auto-blink, partial eye closure, multi-morph expressions, vowel visemes) and `Morph.*` (the same
// Manual-layer calls for shape keys that are not a face, plus clip / pool readers).
//
// Every entry point resolves the owner to the SkeletonComponent CARRIER
// (Facial::ResolveSkeletonCarrier), the only entity BuildSkinJobs reads the component from, so the
// character ROOT and the skeleton entity both work. An entity with no skeleton under it, or whose
// rig has not streamed yet, returns false/nil and NEVER gets the component created.
//
//   Face.SetWeight(e, name, weight, blendDuration?) -> bool
//     group Manual (Override); weight clamped to [0,1]; blendDuration defaults to 0 = instant, and
//     an instant write lands at call time so a same-frame Face.GetWeight is exact.
//   Face.GetWeight(e, name) -> number | nil
//     The Manual channel's current value (author intent, not the composed result); nil when no
//     Manual channel of that name exists.
//   Face.ClearWeight(e, name, blendDuration?) -> bool
//   Face.ClearAll(e, blendDuration?) -> bool
//     Fades Manual + Expression + Viseme; authored rows and auto-blink are untouched.
//   Face.SetExpression(e, { [name] = weight, ... } | "name", blendDuration?) -> bool
//     group Expression; a model preset name expands into its targets; any Expression channel
//     absent from the table fades out. Never touches Manual / Viseme / Blink.
//   Face.ClearExpression(e, blendDuration?) -> bool
//   Face.GetPresets(e) -> { [preset] = { [target] = weight } }
//   Face.SetVowel(e, "a"|"i"|"u"|"e"|"o", amount, blendDuration?) -> bool
//     group Viseme, over the component's visemeMorph[]; setting one vowel fades the other four; an
//     unknown vowel key -> false.
//   Face.SetEyeClose(e, amount, blendDuration?) -> bool
//     SetWeight on blinkMorph (group Manual, Override). Blink is the MAX layer ABOVE it, so a blink
//     still closes fully over a partial squint and SetEyeClose(e, 0) really reopens.
//   Face.Blink(e, opts?) -> bool
//     One blink now; opts = SetAutoBlink's timing keys (closeTime, holdTime, openTime, maxWeight),
//     all seconds, all optional.
//   Face.SetAutoBlink(e, enabled, opts?) -> bool
//     opts = { morph, minInterval, maxInterval, closeTime, holdTime, openTime, maxWeight }. Clamps
//     only to >= 0 and maxInterval >= minInterval (sub-second test cadences stay possible), and
//     clamps the pending countdown into the new range so a shortened interval applies at once.
//   Face.GetMorphNames(e) -> table        1-based array; EMPTY table, never nil.
//   Face.HasMorph(e, name) -> bool        exact UTF-8 byte compare.
//   Face.GetResolved(e) -> table          1-based array of { name, slot, weight, alpha, floor }.
//   Face.GetResolved(e, name) -> number | nil
//     The final composed facial layer, LAST frame's output: setters run in GameplayLogic,
//     FacialAnimationSystem in Animation.
//
// The binding layer only resolves entities, marshals arguments and diagnoses (LuaDiag, always
// LOG_WARNING so negative tests pass -maxerrors=0); the work is in Facial::
// (ECS/FacialAnimationSystem.h).

#include "ECS/ECS.h"   // Entity

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace sol { class state; }
class World;
class SkeletonRegistry;
class MorphClipLibrary;

// What BuildSkinJobs last wrote into the morph pool for a skeleton entity, as
// (skeleton morph slot, weight) rows (SkinnedMeshSubsystem::ReadPoolWeights);
// false when nothing is known for it yet.
using MorphPoolReader = std::function<bool(Entity, std::vector<std::pair<std::uint16_t, float>>&)>;

// @p morphClips (optional): names a bound morph clip's channels for
// Morph.GetClipWeights; without it a clip-driven component reads as empty.
// @p readPool (optional): Morph.GetPoolWeights' source; without it that reads as empty.
void RegisterLuaFacialBindings(sol::state& lua, World& world,
                               SkeletonRegistry& skeletons,
                               const MorphClipLibrary* morphClips = nullptr,
                               MorphPoolReader readPool = {});
