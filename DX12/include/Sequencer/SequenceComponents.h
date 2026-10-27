#pragma once

// SequenceComponents - the ECS surface of the cinematic Sequencer.
//   SequencePlayerComponent   authored + transport. Serialized into the scene.
//   SequenceRuntimeComponent  everything derived. Never serialized, never carried across an
//                             Edit<->Play snapshot.
// The runtime half owns two things that must not survive a snapshot restore: a resolved entity
// table (entity ids are recycled) and the pre-animated cache (restoring a stale cache would write
// a dead entity's old components onto whatever now occupies that id).

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"     // AttachmentRef
#include "Sequencer/SequenceAsset.h"

#include <cstdint>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

// SequenceFinishedEvent - published on the EventBus when a sequence ends, naturally or by a skip,
// so gameplay resumes on an edge instead of polling, and Lua gets it through the existing bridge.
struct SequenceFinishedEvent
{
    Entity      player  = NullEntity;
    bool        skipped = false;
    std::string sequencePath;
};

// SeqSpawnedTag - "this entity exists only because a sequence made it".
// The ownership edge that makes orphans collectable. The owner's SequenceRuntimeComponent is reset
// by ComponentCloner, so without this tag a spawned entity would become unreachable and immortal:
// EnterPlay deep-copies the editor world with a live preview's spawned entities in it, and
// ExitPlay restores them with an owner whose runtime state is blank (cameras and fade quads would
// pile up, one set per Play).
// `owner` is the player entity id. SequencerTickSystem's sweep treats an entity as orphaned when
// its owner is dead, has no runtime component, or is no longer armed.
struct SeqSpawnedTag
{
    uint32_t owner = 0;
};

// SequenceRoleBinding - one entry of the CAST.
// The .iseq declares ROLES; the cast lives here, on the player, in the scene, so one cutscene runs
// in two levels, on two prefab instances or on a Lua-supplied set of actors (see "Binding identity"
// in SequenceAsset.h).
struct SequenceRoleBinding
{
    std::string   role;      // must match Seq::Binding::role
    AttachmentRef target;    // GUID-backed, survives save/load and world reload
};

// SequencePlayerComponent - authored config + transport. Serialized.
struct SequencePlayerComponent
{
    enum class SkipPolicy : uint8_t { None = 0, JumpToEnd = 1, StopImmediately = 2 };

    // ---- Authoring (serialized) ----
    std::string sequencePath;
    bool        playOnLoad = false;
    // Overrides the asset's own LOOP flag when `overrideLooping` is set, so a
    // shared cutscene can loop in one placement and play once in another.
    bool        overrideLooping = false;
    bool        looping         = false;
    // Signed playback rate. Negative plays the sequence backwards, which the
    // absolute evaluator handles for free; the one-shot lane fires on section
    // END edges in that direction (see Seq::EvalMode).
    float       rate = 1.f;

    std::vector<SequenceRoleBinding> roleBindings;

    // ---- Gameplay integration ----
    // GameFlow state entered as an OVERLAY on start and returned from on end (e.g. "Cutscene"), so the
    // flow remembers where the cutscene came from. The release returns it only while the flow is still
    // in that state (asked of the flow, never mirrored on this component). Empty = leave the flow
    // alone: a scripted camera move inside gameplay should not seize input.
    // Input belongs to that state: a cutscene that needs the pad declares `input_context` on its own
    // GameFlowStateDefinition and the flow pushes and pops it. Do not add an input context here: two
    // owners of one input stack push a context twice and pop it once, leaving the player unable to
    // move with nothing in the log.
    std::string gameFlowState;

    // What a skip request does. JumpToEnd is a plain seek to the last frame, which the absolute
    // evaluator handles correctly: every actor ends in its authored final pose.
    SkipPolicy skipPolicy = SkipPolicy::JumpToEnd;

    // Written on natural end / skip: GameState key "seq.watched.<path>" (save-persisted), for
    // "don't show me that again".
    bool recordWatched = false;

    // ---- Transport requests (inspector / Lua / another sequence) ----
    // playRequested / stopRequested / seekRequested are EDGE triggers consumed and cleared by
    // SequencerTickSystem each frame; `paused` is a LEVEL the system reconciles against. Same
    // convention as AudioSourceComponent, so the inspector, Lua and a parent sequence drive one API.
    bool    playRequested = false;
    bool    stopRequested = false;
    bool    skipRequested = false;   // edge; resolved per skipPolicy
    bool    paused        = false;
    bool    seekRequested = false;
    int32_t seekTargetFrame = 0;

    // ---- Runtime state (NOT serialized; scrubbed by ComponentCloner) ----
    bool    playing   = false;
    bool    started   = false;    // playOnLoad one-shot guard
    int32_t frame     = 0;        // THE playhead. Integer frames at the asset's frameRate.
    float   subFrame  = 0.f;      // [0,1) accumulator; never used for event tests
    // < 0 means "no previous observation": first sight, or re-armed by a seek.
    // The evaluator reads this as an unconditional discontinuity.
    int32_t prevFrame = -1;
};

// SequenceRuntimeComponent - derived state. Never serialized.
struct SequenceRuntimeComponent
{
    // ---- Pre-animated cache ----
    struct PreAnim
    {
        Entity     entity     = NullEntity;
        uint32_t   generation = 0;
        std::vector<std::pair<std::type_index, std::shared_ptr<void>>> comps;
    };

    // ---- Sub-sequences ----
    struct ChildPlayer { uint64_t key = 0; Entity entity = NullEntity; };

    // ---- Asset and resolved bindings ----
    // Shared, immutable, reference-counted - NOT a raw pointer into the cache: the editor invalidates
    // that cache on Save, and a raw pointer would dangle when the user saves while previewing (same
    // reason s_vfxCache holds shared_ptr).
    std::shared_ptr<const Seq::Sequence> asset;
    std::string                          loadedPath;   // cache-validity key
    // Preloads of the asset's prefab spawnables (Engine.PreloadPrefab's), taken with the asset and
    // released with it: the last holder releases them. Opaque here (SequencerSystem.cpp makes it).
    std::shared_ptr<void>                spawnablePreloads;

    // Value of Seq::SequenceCacheRevision() when `asset` was acquired. 0 means "never acquired" (the
    // revision counter starts at 1), so a fresh component takes the acquire path once.
    // The path alone is NOT a sufficient validity key: an editor Save erases the cache entry without
    // changing the path, and the player would keep evaluating the shared_ptr it already holds.
    uint32_t assetRevision = 0;

    // Seq::ArmSignature(*asset) when the cast was armed. A content edit that keeps it is hot-swapped
    // in place; one that changes it forces a release + re-claim, because the touched set, the
    // pose-authority set or the Spawnable set moved under the live player.
    uint64_t armSignature = 0;

    // Per-binding resolved entity, parallel to Seq::Sequence::bindings.
    // `boundGen` catches a recycled entity id.
    std::vector<Entity>   bound;
    std::vector<uint32_t> boundGen;
    bool                  bindingsDiagnosed = false;   // once-per-play unresolved report
    // FNV over the player's role table (names + target GUIDs) at the last ResolveBindings: a cast edit
    // mid-play invalidates the cached rows (which otherwise persist on the alive + generation fast
    // path). 0 = never.
    uint64_t              castStamp = 0;

    // ---- Pre-animated cache ----
    // Whole-component granularity: per-field restore leaves a half-restored component when two tracks
    // drive one component, and World::CloneEntityComponents is the type-erased primitive.
    std::vector<PreAnim> preAnim;
    bool                 armed = false;   // preAnim captured, actors claimed

    // ---- Entities the sequence creates ----
    // Entities this sequence CREATED (Spawnables), parallel to Sequence::bindings; NullEntity for
    // possessables. Spawned when the sequence arms and destroyed when it releases, NOT per section
    // usage range: a camera popping in and out mid-scrub would churn the VCam stack and reset temporal
    // history on every drag frame.
    std::vector<Entity> spawned;

    // Canvas root for the Fade track's full-screen quad. The sequence creates it when it arms, iff it
    // has a Fade track, so a Fade track never needs a hand-added FadeQuad spawnable and never silently
    // does nothing.
    Entity fadeQuad = NullEntity;

    // ---- Sub-sequences ----
    // Child players this sequence created, keyed by (track, section). Range-state: created when their
    // section becomes live, destroyed when it stops being live, so a scrub past a shot tears its child
    // down exactly as playing past it does.
    std::vector<ChildPlayer> children;

    // Recursion guard. A .iseq that references itself (directly or through a
    // chain) would otherwise spawn players forever inside one frame.
    int32_t depth = 0;

    // ---- Range-state bookkeeping ----
    // Reserved for the (trackIdx << 32 | sectionIdx) keys live at the last evaluation. Nothing fills or
    // reads it: range-state tracks re-derive their live sections from the frame every tick, and
    // sub-sequences keep theirs in `children` (same keys). Seeks and hot-swaps clear it.
    std::vector<uint64_t> activeKeys;

    void ResetDerived()
    {
        bound.clear();
        boundGen.clear();
        bindingsDiagnosed = false;
        activeKeys.clear();
    }
};

// ==== Edit<->Play snapshot hooks ====
// SequencePlayerComponent keeps its authored fields (path, cast, playOnLoad) and drops all
// transport state; otherwise stopping PIE would restore a queued Play edge or a mid-cutscene
// playhead into the editor world, and the next Play would resume a cutscene nobody started.
template <>
struct ComponentCloner<SequencePlayerComponent>
{
    static SequencePlayerComponent Clone(const SequencePlayerComponent& src)
    {
        SequencePlayerComponent dst = src;   // authored fields + cast
        dst.playRequested   = false;
        dst.stopRequested   = false;
        dst.skipRequested   = false;
        dst.seekRequested   = false;
        dst.seekTargetFrame = 0;
        dst.paused          = false;
        dst.playing         = false;
        dst.started         = false;
        dst.frame           = 0;
        dst.subFrame        = 0.f;
        dst.prevFrame       = -1;
        return dst;
    }
};

// The runtime half is ENTIRELY derived. Cloning it would copy a resolved
// entity table and a pre-animated cache into a world where those entity ids
// mean something else; restoring that cache would then write one entity's old
// components onto a different entity.
template <>
struct ComponentCloner<SequenceRuntimeComponent>
{
    static SequenceRuntimeComponent Clone(const SequenceRuntimeComponent&)
    {
        return SequenceRuntimeComponent{};
    }
};
