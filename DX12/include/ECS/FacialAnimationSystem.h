#pragma once

// FacialAnimationSystem - resolves FacialAnimationComponent channels into a
// mesh-slot-indexed facial layer (resolvedOut) once per frame.
// Call order inside Renderer::TickAnimationChain:
//   1. AnimationSystem::Update()        - clips, incl. SampleMorphClip -> MorphComponent
//   2. FaceTrackingSystem::Resolve()    - writes each binding's Tracked input
//   3. FacialAnimationSystem::Update()  - THIS (folds the Tracked input of this frame)
//   4. FootIK / IK / HeadTrack / ChainPhysics / LocalToWorld
//   ...
//   n. SkinnedMeshSubsystem::BuildSkinJobs() - folds resolvedOut onto the morph pool
// The hard requirements are "after FaceTrackingSystem::Resolve, before BuildSkinJobs,
// same call"; sitting next to the clip sample is only for readability.
// Attachment: the component has an effect only on the SkeletonComponent carrier
// (BuildSkinJobs reads it off the entity a section's SkeletonRef resolves to). This
// system warns once per process when it finds one elsewhere, then skips it.
// Stateless across frames: every per-entity value lives ON the component, so no
// world-clear / entity-destroy teardown and no subsystem-side map keyed by
// assetIndex.
// PERFORMS NO STRUCTURAL WORLD MUTATION (no Add/RemoveComponent, no entity
// creation): BuildSkinJobs caches pool pointers once and AnimationSystem iterates
// the same pools in this phase. It needs none either: BuildSkinJobs claims the morph
// pool slice a facial write lands in unconditionally, before any MorphComponent is
// consulted, and resize() zero-fills it.
// ALLOCATION-FREE per frame: everything it touches is inline storage on the
// component plus one bounds-checked SkeletonAsset reference.

#include "ECS/ECS.h"
#include "ECS/FacialAnimationComponent.h"
#include "Resource/SkeletonAsset.h"

#include <cstdint>
#include <unordered_set>

class FacialAnimationSystem
{
public:
    // Unlike HeadTrackSystem, takes no AnimationSystem&: the facial layer never touches
    // poses, only morph weights.
    explicit FacialAnimationSystem(SkeletonRegistry& skeletons) : m_skeletons(skeletons) {}

    // dt        - animation delta. 0 in Edit / paused: the blink timer and the easing
    //             integrator do not advance, so an in-flight BLEND freezes where it was
    //             (pause must not change what is on screen). An INSTANT channel
    //             (blendSeconds <= 0, every Edit-mode writer) still lands on its target
    //             and resolvedOut is still rebuilt, so inspector edits show the same frame.
    // activeSet - when non-null (anim culling on, the default) only entities in the
    //             anim-visible set update. Culled characters keep their last resolvedOut
    //             (BuildSkinJobs does not consult activeSet): a frozen face, the same
    //             staleness culled HeadTrack heads get.
    void Update(World& world, float dt,
                const std::unordered_set<Entity>* activeSet = nullptr);

    // Editor A/B toggle. When false, Update() ZEROES every component's resolvedOut and
    // returns, so every character shows exactly the clip/manual morph fill. A bare
    // early-out is not enough: resolvedOut lives on the component and BuildSkinJobs
    // folds it regardless of this flag, so the last facial layer would stay forever.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // The editor's "Blink now" and Face.Blink both funnel here, so the trigger rule
    // (copy the durations, reset the phase) lives in one place. Safe at dt == 0, but the
    // machine does not advance then, so a caller that wants the closed eye to SHOW in
    // Edit must also call Facial::ForceBlinkWeight. A duration <= 0 takes the
    // component's authored value.
    static void TriggerBlink(FacialAnimationComponent& fa,
                             float closeSec, float holdSec, float openSec, float peak);

private:
    SkeletonRegistry& m_skeletons;
    bool              m_enabled = true;
};

// ==== Facial: the component-level helper API ====
// The surface the Lua `Face` table and the editor postDraw wrap: every function works
// on ONE FacialAnimationComponent, touches no World, allocates nothing and makes no
// structural mutation. Callers own entity resolution
// (Anim::ResolveAnimEntityOrSelf, then the SkeletonComponent carrier) and the
// skeleton bounds check (SkeletonRegistry::Get has none).
// Channel identity is (group, name): SetChannel on (Manual, "a") never disturbs
// (Expression, "a").
namespace Facial
{
    // Exact UTF-8 byte compare against SkeletonAsset::morphTargetNames. Returns
    // FacialAnimationComponent::kUnresolved (-1) when absent. The name table is
    // truncated at import to SkeletonAsset::MAX_MORPH_TARGETS, which is
    // kMaxMorphTargets (shaders/morph_limits.hlsli) like MorphComponent::MAX, so
    // name-addressable and GPU slots are the same set. A rig over the cap loses names
    // off the end; IsNameTableTruncated() tells "typo" from "past the cap".
    std::int16_t FindMorphSlot(const SkeletonAsset& skel, const char* name);
    bool         IsNameTableTruncated(const SkeletonAsset& skel);

    // Copy a UTF-8 name into a 64-byte component buffer: truncates, always
    // NUL-terminates, never writes past the buffer. Use it for blinkMorph / visemeMorph
    // so the editor and the serializer agree.
    void CopyName(char* dst64, const char* src);

    // Create or update the (group, name) channel.
    //   weight       clamped to [0,1]
    //   blendSeconds <= 0 snaps current to target AT CALL TIME, so a same-frame
    //                GetChannelWeight is exact (a documented Lua contract).
    // Returns false ONLY when all kMaxChannels channels are live; it never evicts one.
    // Fully faded releasing channels are reclaimed first.
    bool SetChannel(FacialAnimationComponent& fa, FacialGroup group,
                    const char* name, float weight, float blendSeconds);

    // Fade the (group, name) channel to 0; it frees itself once it arrives.
    // blendSeconds <= 0 frees it immediately. False when no such channel exists.
    bool ClearChannel(FacialAnimationComponent& fa, FacialGroup group,
                      const char* name, float blendSeconds);

    // Fade out every channel of one group.
    void ClearGroup(FacialAnimationComponent& fa, FacialGroup group, float blendSeconds);

    // Fade out Manual + Expression + Viseme. Authored rows and auto-blink survive
    // (this is Face.ClearAll).
    void ClearScripted(FacialAnimationComponent& fa, float blendSeconds);

    // Replace the whole Expression group in one call: every table entry becomes or
    // updates an Expression channel, and every Expression channel NOT named in this pass
    // fades out. Call BeginExpression, then SetChannel(..Expression..) per entry, then
    // EndExpression: the brackets make "absent from the table fades out" work without
    // an allocation.
    void BeginExpression(FacialAnimationComponent& fa);
    void EndExpression(FacialAnimationComponent& fa, float blendSeconds);

    // Current (eased) value of a (group, name) channel. False when absent.
    bool GetChannelWeight(const FacialAnimationComponent& fa, FacialGroup group,
                          const char* name, float* outWeight);

    // The composed, post-fold weight for a mesh slot: LAST frame's Update output
    // (setters run in GameplayLogic, this system in Animation). False when the slot is
    // not in this frame's resolvedOut.
    bool GetResolvedWeight(const FacialAnimationComponent& fa, std::uint16_t slot,
                           float* outWeight);

    // "a"|"i"|"u"|"e"|"o" (case-insensitive) -> 0..4, or -1.
    int VowelIndex(const char* key);

    // Set one vowel over fa.visemeMorph[vowelIndex] and fade the other four.
    // vowelIndex must be in [0, kVowelCount). Returns false on a full channel table.
    bool SetVowel(FacialAnimationComponent& fa, int vowelIndex,
                  float amount, float blendSeconds);

    // Editor / Edit-mode helper. The blink state machine is frozen at dt <= 0, so
    // TriggerBlink alone shows nothing there; this stamps the machine's output directly
    // and the next dt > 0 tick overwrites it. Pass 0 to open.
    void ForceBlinkWeight(FacialAnimationComponent& fa, float weight);

    // Drop every cached name -> slot resolution (blink slot, all channels, every
    // authored entry). The editor calls this after changing ANY morph name so it shows
    // on the next tick; the system does it itself when the skeleton asset index changes
    // or a cached slot falls outside the asset.
    void InvalidateResolution(FacialAnimationComponent& fa);

    // The Authored list, keyed by NAME: set an entry's weight (clamped to [0,1]; <= 0
    // removes it, so the list holds only what the designer set; a new entry resolves on
    // the next tick). False for an empty name, or a new name on a full list
    // (kMaxAuthored).
    bool  SetAuthored(FacialAnimationComponent& fa, const char* name, float weight);
    // The entry's weight; -1 when the name has none.
    float GetAuthored(const FacialAnimationComponent& fa, const char* name);
}
