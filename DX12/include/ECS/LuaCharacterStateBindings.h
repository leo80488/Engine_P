#pragma once

// Lua bindings for character animation, shared by Logic scripts (per-entity) and BT actions
// (asset/ai/*.lua): Character.* (states, possession, actions, the anim driver), Animator.* (runtime
// control of AnimationSetComponent) and Locomotion.* (blend space and transitions).
//
//   Character.AddState(entityId, name, clipPath, opts?)   opts = { loop=true, speed=1.0 }
//     Registers the state (and its AnimationSet alias); the same name overwrites the existing entry.
//   Character.SetState(entityId, name, blendDuration?)
//     Cross-fades toward `name`; blendDuration defaults to the component's defaultBlendDuration and
//     0 snaps. Returns false when the entity has no CharacterState component or the name is not
//     registered.
//   Character.GetState(entityId) -> string | nil   the playing state's name, nil before any is set
//   Character.IsBlending(entityId) -> bool
//
// AnimationSetComponent is controlled through the Animator table:
//   Animator.SetClip(entityId, alias, path, opts?) -> bool
//     opts = { loop=true, speed=1, source="BoneClip", morph="FaceClip",
//              default=false, playOnLoad=false }
//   Animator.RemoveClip / ClearClips / HasClip / GetClips / GetClipInfo
//   Animator.Play(entityId, clipName, blendDuration?, restart?) -> bool
//   Animator.Stop(entityId)
//   Animator.Pause(entityId) / Animator.Resume(entityId)
//   Animator.SetSpeed(entityId, speed) -> bool
//   Animator.GetCurrent/GetTarget(entityId) -> string | nil
//   Animator.IsPlaying/IsPaused/GetTime/SetTime/GetDuration
//   Animator.GetNormalizedTime/SetNormalizedTime
//   Animator.IsBlending(entityId) -> bool

namespace sol { class state; }
class World;
namespace Resource { class AnimationClipSystem; }

void RegisterLuaCharacterStateBindings(sol::state& lua, World& world,
                                       Resource::AnimationClipSystem& animationClips);
