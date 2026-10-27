#pragma once

// SequenceEval - the evaluator: one (mostly) pure function of the playhead.
// CONTRACT: everything an absolute track writes is a function of `frame` alone, so reaching frame N
// by playing forward and by scrubbing backward produce identical state.
// Every track type belongs to one of three lanes (Seq::EvalMode):
//   Absolute    recomputed from frameF every tick and written unconditionally.
//   RangeState  which sections are live is derived from frameF alone every tick, so enter/exit
//               edges are direction-agnostic by construction: no "which way did time move"
//               bookkeeping, which is where scrubbing usually breaks.
//   OneShot     fires only on a CONTINUOUS crossing in the play direction, and is suppressed
//               entirely when `continuous` is false.
// Anything that is not one of these three is not a track type.

#include "ECS/ECS.h"
#include "ECS/EngineMode.h"
#include "Sequencer/SequenceAsset.h"
#include "Sequencer/SequenceClock.h"   // AdvanceClock / IsDiscontinuous - the playhead contract
#include "Sequencer/SequenceComponents.h"

class Renderer;
namespace Resource { class AnimationClipSystem; class AssetManager; }

namespace Seq
{

struct EvalContext
{
    World*     world    = nullptr;
    Entity     player   = NullEntity;
    Renderer*  renderer = nullptr;
    // Needed only by the Skeletal Animation track, to BIND a clip by alias. The pose itself is
    // driven by writing primaryTime absolutely, which needs only the component.
    Resource::AnimationClipSystem* clips = nullptr;
    // Needed only by SpawnArchetype::FromPrefab.
    Resource::AssetManager* assetMgr = nullptr;

    // ---- the playhead ----
    int32_t frame     = 0;      // integer frame - the ONLY thing event tests use
    float   frameF    = 0.f;    // frame + subFrame - used by curves, never by events
    int32_t prevFrame = -1;     // < 0 = no previous observation
    int32_t dir       = 1;      // +1 forward, -1 reverse (sign of the play rate)

    // False for a seek, a first sight, a loop wrap, a step against the play direction, or a forward
    // jump larger than Sequence::maxForwardJump (a frame hitch, an alt-tab, a breakpoint).
    // Suppresses the entire one-shot lane.
    bool continuous = false;

    float      frameRate = 30.f;
    EngineMode mode      = EngineMode::Playing;
};

// Resolve every binding into rt.bound / rt.boundGen. Cheap on the steady path (a generation
// check); re-resolves through GuidRegistry only when stale. Reports unresolvable roles once per
// play: a silently unbound role is the classic "one actor isn't moving" data bug.
void ResolveBindings(World& world, const Sequence& seq,
                     SequencePlayerComponent& player, SequenceRuntimeComponent& rt);

// Capture the pre-animated state of every entity this sequence will write, then
// stamp the pose-authority claims. Idempotent: a no-op once rt.armed is set.
void ClaimActors(World& world, const Sequence& seq, SequenceRuntimeComponent& rt,
                 Entity player, Resource::AssetManager* assetMgr = nullptr,
                 Renderer* renderer = nullptr,
                 Resource::AnimationClipSystem* clips = nullptr);

// Drop the claims and, per @p restoreMode and @p mode, restore the pre-animated state.
// Safe to call on a half-armed or never-armed runtime.
void ReleaseActors(World& world, const Sequence& seq, SequenceRuntimeComponent& rt,
                   RestoreMode restoreMode, EngineMode mode);

// State the entity-destroy listener snapshots when a LIVE player is destroyed without
// SequencerTickSystem::ReleasePlayer (an outliner delete, a Lua Destroy). The entity is gone by the
// time anything can react, so the listener moves out what the release needs and the next Update
// finishes the job: pose-claim removal, director release, pre-animated restore.
struct OrphanedPlayerRelease
{
    std::shared_ptr<const Sequence>                asset;
    std::vector<Entity>                            bound;
    std::vector<SequenceRuntimeComponent::PreAnim> preAnim;
};
void ReleaseOrphanedPlayerState(World& world, OrphanedPlayerRelease& rec);

// Re-arm every cross-time cursor a seek invalidates and reset temporal rendering history. MUST
// run before evaluating the frame after a discontinuity, or TimelineSystem reads a backward jump
// as a loop wrap and replays every notify in the clip.
void OnDiscontinuity(World& world, const Sequence& seq, SequenceRuntimeComponent& rt,
                     Renderer* renderer, bool quietHistoryReset);

// Evaluate one frame.
void Evaluate(const Sequence& seq, SequenceRuntimeComponent& rt, const EvalContext& ec);


// ---- Direct-manipulation suppression ----
// While the user drags a gizmo on an entity a Transform track drives, the evaluator must not
// write that entity, or it rewrites LocalTransform under the drag every frame and the handle
// never moves (Record mode depends on this). One entity is enough: ImGuizmo manipulates one at a
// time, and the editor clears it when the drag ends.
namespace Suppress
{
    void   Set(Entity e);      // NullEntity clears
    Entity Current();
    bool   IsSuppressed(Entity e);
}

// ---- Time dilation ----
// A STACK, not a setter. ScriptSystem resets its time scale to 1.0 on Lua reload and clear, so a
// TimeDilation track calling SetTimeScale directly would lose its value on a mid-cutscene
// hot-reload, and two overlapping sequences would fight over one float.
// Entries are keyed by owner (the player entity). The strongest slow-down wins: a hit-stop inside
// a slow-motion shot must not run faster than the shot. Resolve() never exceeds 1.0, so a value
// above 1 has no effect.
namespace TimeScale
{
    void  Push(uint32_t ownerId, float value);
    void  Release(uint32_t ownerId);
    float Resolve();          // 1.0 when nothing is pushed
    void  Clear();

    // Installed by App. Called only when the resolved value CHANGES, so a sequence with no
    // TimeDilation track never touches the global.
    void  SetSink(void (*fn)(float));
}

// ---- Editor preview ----
namespace Preview
{
    // While true, EditorCameraController::EnsureEditorCamera does NOT re-arm the editor free-cam, so
    // the sequencer's shot camera owns the viewport. Because it disables the only camera the user can
    // drive, it is force-cleared on every mode change and world swap instead of relying on opt-in
    // call sites.
    void SetOwnsViewport(bool v);
    bool OwnsViewport();
}

// ---- Asset cache ----
// Shared and reference-counted, so an editor Save that drops the cache entry cannot dangle a
// player that is mid-preview.
std::shared_ptr<const Sequence> AcquireSequence(const std::string& path);
void                            InvalidateSequenceCache(const std::string& path);
void                            ClearSequenceCache();

// ---- Live authoring ----
// Installs an UNSAVED, in-memory document as the cached copy of @p path, so the Sequencer window
// and its preview player evaluate the same document (otherwise the player shows whatever was
// last read from disk). Bumps SequenceCacheRevision so a live player re-acquires it. The publish
// is revoked when the preview closes, so an unsaved edit cannot leak into a level's own
// SequencePlayerComponent after the session ends.
void PublishSequenceEdit(const std::string& path, const Sequence& seq);

// Bumped by every publish, invalidate and clear. A live player stores the value it last acquired
// at and re-acquires only when they differ: one integer compare per player per frame on the steady
// path instead of a map lookup. Never 0: 0 is the "never acquired" value on a fresh runtime
// component.
uint32_t SequenceCacheRevision();

// Hash of everything ClaimActors / ReleaseActors bake in: which entities are touched, which get a
// pose-authority claim, and which Spawnables exist. A content edit that keeps it is hot-swapped
// UNDER a live player without tearing the cast down (dragging a key must not respawn the shot
// camera, which churns the VCam stack and resets temporal history every drag frame); only a change
// forces a re-arm.
// EXCLUDES spawnPos / spawnRot / spawnScale: they are drag-edited continuously, so the hot-swap
// path writes them onto the already-spawned entity instead (ApplySpawnTransforms).
uint64_t ArmSignature(const Sequence& seq);

// Push each Spawnable's authored spawn transform onto the entity it already spawned. Called only
// on a hot-swap; a Transform track evaluated later in the same tick still wins.
void ApplySpawnTransforms(World& world, const Sequence& seq,
                          const SequenceRuntimeComponent& rt);

} // namespace Seq
