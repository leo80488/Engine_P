#pragma once

// TimelineSystem: cross-time dispatcher for AnimNotify-style events.
//
// Paths A and B fire into the same Pending* mailboxes and a consumer cannot tell them apart; Path D
// does too, with _animGraph = 1 in the params. Path C writes the ActiveFXGates level output instead.
//   Path A (primary, Unreal AnimSequence style): notify tracks authored on the clip. For each entity
//     whose AnimationComponent has a valid primaryClip, the clip's notifyTracks (from the
//     ClipLibrary) are evaluated over [prevNotifyTime, primaryTime] with the per-entity cursor on
//     the AnimationComponent; one authored set drives every entity that plays the clip.
//   Path B (per-entity override): tracks on a TimelineComponent with its own lastObservedTime
//     cursor, for instance-specific events; new authoring goes on the clip.
//   Path C: the FXGate level pass (ActiveFXGates: which gates are open right now).
//   Path D: the AnimGraphSystem's notifies, plus its graph events for an entity whose pose the graph
//     owns; such an entity skips Path A.
//
// TickPhase::Animation, registered after the system that advances AnimationComponent::primaryTime,
// so the window is the latest one. The consumers (Hitbox / VFX / Camera / Audio / StateToggle /
// GenericNotify) run in TickPhase::BoneAttachment and drain the mailboxes the same frame.
//   - Looping clips wrap on duration: the test covers (prev, end] U [0, curr] when curr < prev.
//   - Muted tracks are skipped.
//   - First sight / clip switch snapshots the time without firing, so opening a clip does not
//     replay every notify at once.
//   - Paused animations do not fire (the editor preview's pause semantics).

#include "ECS/ISystem.h"

class ClipLibrary;
class AnimGraphSystem;

class TimelineSystem final : public SystemInPhase<TickPhase::Animation>
{
public:
    // @p clips is the engine's ClipLibrary (owned by the Renderer's skinning subsystem), stored by
    // reference: the library outlives the system, and only its contents change across world reloads,
    // which is safe because lookups are guarded by index < Count(). @p graph is optional.
    explicit TimelineSystem(const ClipLibrary& clips, AnimGraphSystem* graph = nullptr) : m_clips(clips), m_graph(graph) {}

    const char* GetName() const override { return "TimelineSystem"; }

    void OnRegister(World& world) override;
    void PrepareStructure(World& world) override;
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;

private:
    const ClipLibrary& m_clips;
    AnimGraphSystem* m_graph = nullptr;
};
