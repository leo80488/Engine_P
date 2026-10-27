#pragma once

// FacialAnimationComponent - script- and inspector-driven morph-target (blend shape)
// control: auto-blink, partial eye closure, multi-morph expressions, vowel visemes.
// ATTACH TO THE SKELETON ENTITY (the one carrying SkeletonComponent and, when the rig
// has morph targets, MorphComponent). A HARD invariant: SkinnedMeshSubsystem::
// BuildSkinJobs looks it up only on the entity a section's SkeletonRef resolves to
// (its facial-layer write); anywhere else it is a silent no-op. Same placement as
// ChainPhysicsComponent / AnimationSetComponent.
// It does NOT touch MorphComponent::weights[]: three writers own that array
// (AnimationSystem::SampleMorphClip, AnimationClipSystem's fill on clip rebind, the
// editor's manual sliders), and in clip mode it is CLIP-CHANNEL indexed. The facial
// layer produces resolvedOut[] in MESH-SLOT space, and BuildSkinJobs folds it onto
// the morph POOL slice after the clip/manual fill: mode-agnostic and unclobberable.
// Group pipeline, the whole composition model:
//     Authored   (Override)   designer's static base, e.g. jitome (narrowed eyes) 0.3
//                             (a list, not channels)
//     Expression (Override)   Face.SetExpression
//     Viseme     (Override)   Face.SetVowel
//     Manual     (Override)   Face.SetWeight / Face.SetEyeClose
//     Blink      (Max)        auto-blink + Face.Blink        <-- applied LAST
// Groups resolve in that order; within a group a morph name appears at most once, so
// the fold is total and independent of channel allocation order. Channel IDENTITY is
// (group, name): one name may be live in several groups, and Face.SetExpression can
// never steal or clear a channel Face.SetWeight owns. Blink is Max and last so a
// blink still closes fully over a partial squint from SetEyeClose.
// There is deliberately NO per-channel blend mode and no Add mode: two Override
// channels on one slot would resolve by allocation history, which is
// nondeterministic under -fixeddt/-seed.
// Persistence: for a character placed as a PREFAB INSTANCE, settings authored on
// this component in the inspector are SILENTLY DROPPED by Save Scene
// (SceneSerializer skips prefab-instance members). Author them, then Save Prefab
// (.ipfb), which has no such skip.

#include "ECS/ECS.h"

#include <cstdint>
#include <string>
#include <vector>

// Composition group. The numeric order IS the apply order: do not reorder.
enum class FacialGroup : std::uint8_t
{
    Authored   = 0,   // Override
    Expression = 1,   // Override
    Viseme     = 2,   // Override
    Manual     = 3,   // Override
    Blink      = 4,   // Max, applied last
    Count      = 5
};

struct FacialAnimationComponent
{
    // Name buffers are 64 bytes to match SkeletonAsset::morphTargetNames[][64];
    // resolution is an exact UTF-8 byte compare, so any other stride truncates.
    static constexpr int          kNameLen     = 64;
    // Live channels of Expression / Viseme / Manual / Blink together. The Authored list
    // is NOT channels and does not count.
    static constexpr int          kMaxChannels = 32;
    // The Authored list's cap: the morph cap (kMaxMorphTargets, asserted in
    // FacialAnimationSystem.cpp), one entry per morph at most since an entry is keyed
    // by its name.
    static constexpr int          kMaxAuthored = 1024;
    static constexpr int          kVowelCount  = 5;    // a i u e o
    static constexpr std::int16_t kUnresolved  = -1;
    // A name that WAS looked up against the current skeleton asset and is not on it.
    // Distinct from kUnresolved so the per-frame path caches the MISS instead of
    // re-running the linear name scan every frame for a morph the rig does not have
    // (the default blink name on any non-PMX rig). Every consumer tests `slot < 0`, so
    // -2 reads as unresolved everywhere; only the resolve guard in
    // FacialAnimationSystem::Update tells them apart. Facial::InvalidateResolution (a
    // reset to kUnresolved) and an assetIndex change both force a fresh lookup, so a rig
    // swap or a name edit retries.
    static constexpr std::int16_t kResolvedMissing = -2;

    struct AuthoredEntry
    {
        std::string  name;                  // UTF-8, exact-matched (<= kNameLen - 1 bytes)
        float        weight = 0.0f;         // [0,1]
        std::int16_t slot   = kUnresolved;  // RUNTIME: its mesh slot, kResolvedMissing = a cached miss
    };

    // One live channel. Identity is (group, name); channels[k]'s name is channelName[k].
    struct Channel
    {
        float        target   = 0.0f;   // [0,1]
        float        current  = 0.0f;   // [0,1], eased toward target
        float        blendSec = 0.0f;   // 0 = instant
        std::int16_t slot     = kUnresolved;   // mesh morph-target slot, -1 = unresolved
        std::uint8_t group    = 0;      // FacialGroup
        std::uint8_t state    = 0;      // 0 free, 1 live, 2 releasing (freed at current==0)
    };

    // One slot of the facial layer, in MESH-SLOT space.
    struct Resolved
    {
        std::uint16_t slot   = 0;
        float         weight = 0.0f;
        // 1 = only the Blink group (Max) wrote this slot: BuildSkinJobs takes the MAX with
        // what the morph clip / manual weights put there, as the fold does with the other
        // groups, so a half-closed clip eye is never opened by a blink on its way up.
        // 0 = an Override group owns it: its weight replaces the clip's.
        std::uint8_t  maxOnly = 0;
        // Face tracking. alpha < 1 only for a slot the Tracked layer reached first:
        // BuildSkinJobs blends (1 - alpha) * pool + alpha * weight, so a fading track hands
        // the morph back to the clip under it. floor is where a Blink lands on such a slot,
        // so a recovering auto-blink still closes the eye fully. A slot nothing tracks keeps
        // alpha = 1, floor = 0: the pool write is the plain weight, bit for bit. Appended
        // last so the aggregate initialization stays valid.
        float         alpha   = 1.0f;
        float         floor   = 0.0f;
    };

    // One tracked morph, in mesh-slot space.
    struct Tracked
    {
        std::uint16_t slot   = 0;
        float         weight = 0.0f;
        float         alpha  = 0.0f;   // rule mask x morph weight x authority
    };

    // ==== AUTHORED: serialized (tag "FacialAnimation"), reflected, prefab-copied ====
    // Master enable. False = the system clears resolvedOut and does nothing else, so
    // the clip / manual weights show through untouched.
    bool enabled = true;

    bool autoBlinkEnabled = true;

    // Default is the PMX/MMD blink morph "mabataki", stored as UTF-8 bytes.
    char blinkMorph[kNameLen] = "\xe3\x81\xbe\xe3\x81\xb0\xe3\x81\x9f\xe3\x81\x8d";

    float blinkIntervalMinSec = 2.5f;
    float blinkIntervalMaxSec = 6.0f;
    float blinkCloseSec       = 0.08f;
    float blinkHoldSec        = 0.05f;
    float blinkOpenSec        = 0.15f;
    float blinkMaxWeight      = 1.0f;

    // Vowel morphs for Face.SetVowel("a".."o"). Defaults: the PMX kana a i u e o.
    char visemeMorph[kVowelCount][kNameLen] = {
        "\xe3\x81\x82", "\xe3\x81\x84", "\xe3\x81\x86",
        "\xe3\x81\x88", "\xe3\x81\x8a"
    };

    // Persistent static expression set in the inspector (Facial Animation > Authored
    // Channels, and the Morph panel's sliders): a SPARSE list, one entry per morph NAME
    // set, so dozens of body sliders fit. Not channels: the fold emits them FIRST,
    // straight from here, each through its own cached slot. Write through
    // Facial::SetAuthored, or reset an entry's slot to kUnresolved when renaming it in
    // place.
    std::vector<AuthoredEntry> authored;

    // ==== RUNTIME: not serialized, scrubbed by ComponentCloner (see below) ====
    Channel channels[kMaxChannels]{};
    // channels[k]'s name, kept in this parallel array so the per-frame integration pass
    // never touches the 64-byte name blocks (names are read only on the cold re-resolve
    // path).
    char    channelName[kMaxChannels][kNameLen]{};

    // Resolution cache validity. Re-resolve when this differs from the entity's
    // SkeletonComponent::assetIndex; per channel ALSO re-resolve when slot is
    // kUnresolved (never looked up) or >= skelAsset.morphTargetCount (as
    // HeadTrackSystem does). A kResolvedMissing slot is a CACHED miss, NOT retried until
    // the asset index changes or InvalidateResolution runs. NEVER serialized: a stale
    // slot from another rig is worse than a rescan.
    // SkeletonRegistry is append-only (Register is a push_back), so an index never
    // names another asset in-process; the guard is still needed because a world reload
    // re-registers the same .iskel at a NEW index, and a channel's NAME can change under
    // the cache.
    std::uint32_t resolvedAssetIndex = 0xFFFFFFFFu;

    // Cached mesh slot for blinkMorph, plus the name it was resolved from. The blink
    // state machine's output is NOT a channel (recomputed every tick), so it needs its
    // own one-entry cache. The name copy makes an inspector edit of blinkMorph
    // self-healing (HeadTrackSystem uses the same "cached index OR name changed" pair).
    std::int16_t blinkSlot = kUnresolved;
    char         blinkResolvedName[kNameLen]{};

    // ---- Blink state machine ----
    // 0 idle, 1 closing, 2 hold, 3 opening.
    std::uint8_t blinkPhase   = 0;
    float        blinkT       = 0.0f;   // seconds inside the current phase
    float        blinkIdleT   = 0.0f;   // seconds since the last blink ended
    float        blinkNextAt  = 0.0f;   // idle seconds to wait before the next blink
    // Durations of the blink CURRENTLY running, copied from the authored fields at
    // trigger time so Face.Blink(e, {closeTime=...}) can drive a one-off fast blink
    // without reconfiguring auto-blink.
    float        blinkCloseCur = 0.0f;
    float        blinkHoldCur  = 0.0f;
    float        blinkOpenCur  = 0.0f;
    float        blinkPeakCur  = 1.0f;

    // The machine's output for THIS frame, in [0,1]. Recomputed by
    // FacialAnimationSystem::Update whenever dt > 0 and left ALONE when dt <= 0, so the
    // editor's "Blink now" (Facial::ForceBlinkWeight) can stamp it and the closed eye
    // shows in Edit, where blinkT never advances.
    float        blinkWeightCur = 0.0f;

    // xorshift32 state. Seeded on the first tick from
    //     h = uint32(entity) * 2654435761u; h ^= h >> 15; h ^= uint32(Determinism::Seed())
    // (the Knuth mix AISystem uses: hashed from the entity id, NOT random, so a
    // fixed-seed run replays identically). Forced to 1 when the result is 0: 0 is a
    // fixed point of xorshift32 and NullEntity is 0. Raw sequential ids would also give
    // a near-arithmetic ramp in the first outputs (a crowd blinking in unison), hence
    // the mix.
    std::uint32_t rng = 0;

    // False after construction, after a Play/Stop snapshot round trip and after a
    // deserialize. On false the system re-seeds the RNG, rolls the first blink
    // countdown, drops every resolved SLOT (the authored entries' too), then sets it.
    // It deliberately does NOT wipe channels[] or the blink state machine: Face.* runs in
    // GameplayLogic, BEFORE the system, so an EnsureComp-then-SetWeight (or
    // EnsureComp-then-Face.Blink, or the editor's "Blink now" on a freshly added
    // component) in the same frame would lose its write. That state is cleared by the
    // ComponentCloner below (Play/Stop, undo) and a fresh deserialize, both of which
    // land the NSDMI constants.
    bool runtimeSeeded = false;

    // ---- This frame's facial layer, in MESH-SLOT space ----
    // Rebuilt EVERY Update(), including dt == 0, except for a culled character (outside
    // the active set), which keeps its last layer: a frozen face. One entry per distinct
    // slot after the group fold: resolvedOut[0 .. resolvedCount) are this frame's (the
    // vector keeps its capacity, so the warm per-frame path does not allocate). Consumed
    // by BuildSkinJobs' facial write. Weights are already clamped to [0,1]: the CPU is
    // the ONLY clamp in the pipeline (Skin.cs.hlsl has no upper bound and only tests
    // w > 0.001f).
    std::vector<Resolved> resolvedOut;
    std::uint32_t         resolvedCount = 0;
    // Slot -> its index in resolvedOut this frame (-1 none; an entry is trusted only
    // when it points below resolvedCount at that same slot, so it is never reset).
    std::vector<std::int32_t> resolvedIndexOfSlot;

    // ---- Face tracking's input to the fold ----
    // Written by FaceTrackingSystem::Resolve right before Update folds it, in mesh-slot
    // space; empty when nothing tracks this face. The Tracked layer sits after Authored
    // and before Expression: a script's expression and the hot keys still win.
    std::vector<Tracked> tracked;
    // Set while face tracking owns the eyelids (FaceTrackingComponent::suppressAutoBlink,
    // a source that tracks the lids, morph authority above 0.5 with hysteresis):
    // auto-blink starts no blink and one in flight fades out, or the tracked blink and
    // the automatic one would close the eyes twice.
    bool trackingHoldsBlink = false;
};

// ==== ComponentCloner<FacialAnimationComponent> ====
// Play/Stop and undo capture this component by VALUE through ComponentCloner
// (ECS.h). Without this specialization entering Play would snapshot whatever
// half-closed eye the editor left and Stop would restore it, together with a
// blinkNextAt from another time origin and resolvedOut slots resolved against a
// possibly different skeleton asset. The scrub writes CONSTANTS only, so
// Clone(Clone(x)) == Clone(x), which World::AdoptState relies on. Never add
// kPlainCopy; must live in THIS header (ODR).
template <>
struct ComponentCloner<FacialAnimationComponent>
{
    static FacialAnimationComponent Clone(const FacialAnimationComponent& src)
    {
        FacialAnimationComponent d = src;   // authored fields survive verbatim
        d.runtimeSeeded      = false;
        d.rng                = 0;
        for (FacialAnimationComponent::AuthoredEntry& a : d.authored)
            a.slot = FacialAnimationComponent::kUnresolved;
        d.blinkPhase         = 0;
        d.blinkT             = 0.0f;
        d.blinkIdleT         = 0.0f;
        d.blinkNextAt        = 0.0f;
        d.blinkCloseCur      = 0.0f;
        d.blinkHoldCur       = 0.0f;
        d.blinkOpenCur       = 0.0f;
        d.blinkPeakCur       = 1.0f;
        d.blinkWeightCur     = 0.0f;
        d.blinkSlot          = FacialAnimationComponent::kUnresolved;
        d.blinkResolvedName[0] = '\0';
        d.resolvedAssetIndex = 0xFFFFFFFFu;
        d.resolvedCount      = 0;
        for (int i = 0; i < FacialAnimationComponent::kMaxChannels; ++i)
        {
            d.channels[i]       = FacialAnimationComponent::Channel{};
            d.channelName[i][0] = '\0';
        }
        d.resolvedOut.clear();
        d.resolvedIndexOfSlot.clear();
        d.tracked.clear();
        d.trackingHoldsBlink = false;
        return d;
    }
};
