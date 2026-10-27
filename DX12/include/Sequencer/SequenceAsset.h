#pragma once

// SequenceAsset - the `.iseq` cinematic sequence asset (UE Sequencer's MovieScene equivalent).
// Pure data + text I/O: no imgui, no DX12, no ECS systems, so the runtime evaluator, the editor
// window and the cook tool can all depend on it.
//
// Object graph (mirrors UE):
//   Sequence
//     +- Binding[]      Possessable (a role bound to a scene entity) |
//     |                 Spawnable   (a sequence-owned temporary entity)
//     +- Track[]        type + binding index + mute/lock
//          +- Section[] frame range + row + weight + ease envelope + params
//               +- Channel[]  one keyed scalar curve (role-tagged)
// The evaluator (Seq::Evaluate) writes ABSOLUTE state from the frame every tick and never
// accumulates, so scrubbing is correct by construction.
//
// Time is integer FRAMES, not float seconds. `Sequence::frameRate` is the authoritative time base;
// the playhead is an int32 frame plus a sub-frame accumulator. One-shot dedup is an integer compare
// (no 1e-5 boundary bugs of a half-open float window), an event at frame 96 fires on frame 96 at
// any refresh rate, and a fixed-step render-out can drive the clock without touching the
// evaluator (the reason UE's Sequencer uses FFrameNumber).
//
// .iseq text format (line-based, whitespace-tokenized, '#' comment):
//     ISEQ 1                  # format version
//     MINVER 1                # refuse to load below this reader version
//     FPS 30                  # authoritative time base
//     DUR 300                 # playback range end, FRAMES
//     LOOP 0
//     MAXJUMP 15              # frames; a larger forward step is a seek, not playback
//     RESTORE 1               # RestoreMode: 0 Always / 1 EditorOnly / 2 Never
//     CLOCK 0                 # ClockSource: 0 Unscaled / 1 Scaled / 2 Fixed
//     LAYER 0                 # cross-sequence write arbitration priority
//
//     BIND POSSESS                       # binding index = order of appearance
//     ROLE Hero                          # STABLE IDENTITY - see below
//     GUID 4f9a...                       # authoring-time DEFAULT only
//     CHILD Armature/Spine/Head          # NameComponent chain under the GUID'd root
//
//     BIND SPAWN CAMERA
//     ROLE ShotCamA
//     SP 0 1.6 -3                        # spawn position
//     SR 0 0 0 1                         # spawn rotation (quaternion xyzw)
//     SS 1 1 1                           # spawn scale
//
//     TRACK CAMERACUT -1                 # type + binding index (-1 = master track)
//     NAME Camera%20Cuts
//     SECTION 0 120
//     ROW 0
//     WEIGHT 1
//     EASE 0 0 2 1                       # inFrames outFrames inCurve outCurve
//     PREROLL 0
//     PARAM i binding 1                  # typed PropertyBag entry
//     PARAM s clip asset/Anim/walk.ianim
//     CH 3 0                             # channel: role, defaultValue
//     K 0 0 0 0 2                        # key: frame value tangentIn tangentOut interp
//     K 30 90 0 0 2
// Free-form strings (ROLE / NAME / CHILD / PREFAB / string PARAMs) are percent-encoded, so a space
// or a colon can neither break tokenization nor stop the cook tool's backward path walk
// (tools/package_game.py treats ':' as a path stop byte).
//
// Binding identity is the ROLE, not a GUID. `Binding::defaultGuid` only lets the editor preview;
// the cast lives on the scene's SequencePlayerComponent, so one .iseq runs in two levels, on two
// prefab instances or on a Lua-supplied cast. A scene GUID baked into the asset would weld it to
// one scene.
//
// Forward compatibility: the loader RETAINS what it cannot parse. Every scope keeps an
// `unknownLines` vector that the writer re-emits verbatim at the end of its block, so a newer file
// saved by an older editor loses nothing, and `MINVER` makes a file refuse to load in a reader too
// old to preserve it.

#include "ECS/Guid.h"
#include "ECS/NotifyTypes.h"   // PropertyBag - typed variant with a generic editor

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Seq
{

// Reader version. Bump when the reader gains the ability to preserve
// something it previously dropped; a file whose MINVER exceeds this is
// refused rather than silently degraded.
inline constexpr int32_t kFormatVersion = 1;

// ==== Channel - one keyed scalar curve (UE's FMovieSceneFloatChannel) ====
// Vectors and quaternions are N sibling Channels distinguished by `role`, never a packed float4
// key, so every component has its own tangents.
enum class Interp : uint8_t
{
    Constant = 0,   // hold this key's value until the next key
    Linear   = 1,
    Cubic    = 2,   // cubic Hermite using tangentOut of this key / tangentIn of the next
};

struct Key
{
    int32_t frame      = 0;
    float   value      = 0.f;
    float   tangentIn  = 0.f;                 // value units per FRAME (Cubic only)
    float   tangentOut = 0.f;
    Interp  interp     = Interp::Cubic;       // interp of the SEGMENT STARTING at this key
};

struct Channel
{
    uint16_t         role         = 0;        // see namespace Role; meaning is track-type-local
    float            defaultValue = 0.f;      // used when `keys` is empty
    std::vector<Key> keys;                    // INVARIANT: sorted ascending by frame, unique

    // Stateless: binary search + segment interp. Clamps outside [first, last].
    // `frameF` is fractional so a sub-frame accumulator produces smooth motion
    // without giving up integer-frame event semantics.
    float Evaluate(float frameF) const;

    // Call after any authoring mutation. Stable-sorts by frame and collapses
    // duplicates (last one wins, matching "drop a key on top of a key").
    void SortAndDedupe();

    bool IsEmpty() const { return keys.empty(); }
};

// Channel roles. Plain uint16 constants rather than a global enum, so adding a track type never
// edits a shared enumeration.
namespace Role
{
    inline constexpr uint16_t LocX = 0, LocY = 1, LocZ = 2;
    inline constexpr uint16_t RotX = 3, RotY = 4, RotZ = 5;      // Euler DEGREES at authoring time
    inline constexpr uint16_t ScaleX = 6, ScaleY = 7, ScaleZ = 8;

    inline constexpr uint16_t Scalar0 = 16, Scalar1 = 17, Scalar2 = 18, Scalar3 = 19;

    inline constexpr uint16_t Weight    = 32;   // PostProcess blend / Fade alpha / generic section weight
    inline constexpr uint16_t AnimTime  = 33;   // SkeletalAnim / CameraMotion / Director master time (SECONDS)
    inline constexpr uint16_t Volume    = 34;
    inline constexpr uint16_t Pitch     = 35;
    inline constexpr uint16_t TimeScale = 36;
    inline constexpr uint16_t Fov       = 37;   // degrees
}

// ==== Section - a frame range on a track, with its own weight and ease envelope ====
// One flat struct rather than a class hierarchy, so the .iseq grammar stays a single production
// and a Section is cheap to copy for undo snapshots.
enum class EaseCurve : uint8_t { Linear = 0, EaseIn = 1, EaseOut = 2, EaseInOut = 3 };

struct Section
{
    int32_t   startFrame   = 0;
    int32_t   endFrame     = 30;     // exclusive

    // Frames evaluated BEFORE startFrame with the event lane suppressed, so chain physics / cloth /
    // animation have settled by the time the section is on screen.
    int32_t   preRoll      = 0;

    int32_t   row          = 0;      // overlap lane; a track that uses one live section takes the lowest row
    float     weight       = 1.f;    // authored master weight
    int32_t   easeInFrames = 0;
    int32_t   easeOutFrames= 0;
    EaseCurve easeInCurve  = EaseCurve::EaseOut;
    EaseCurve easeOutCurve = EaseCurve::EaseIn;
    bool      active       = true;

    // Per-track-type payload. A PropertyBag rather than untyped scalars: it is typed, round-trips
    // through NotifyIO, and NotifyEditorRegistry::DrawGenericPropertyBag inspects any track type that
    // has no panel of its own.
    PropertyBag params;

    std::vector<Channel> channels;

    // Lines this reader did not understand, re-emitted verbatim so a newer file survives an older
    // editor's Save (see the file header).
    std::vector<std::string> unknownLines;

    bool    Contains(int32_t f) const { return f >= startFrame && f < endFrame; }
    int32_t DurationFrames()    const { return endFrame - startFrame; }

    // Authored weight * ease envelope; 0 outside [startFrame, endFrame).
    // Pure function of frameF: no state, no dt.
    float EvaluateWeight(float frameF) const;

    // Frame relative to the section start. Negative inside the preRoll region.
    float LocalFrame(float frameF) const { return frameF - static_cast<float>(startFrame); }

    // First channel with the given role, or nullptr.
    const Channel* Find(uint16_t role) const;
    Channel*       Find(uint16_t role);

    // Channel value at frameF, or `fallback` when the role is absent.
    float ChannelValue(uint16_t role, float frameF, float fallback) const;
};

// ==== Track ====
enum class TrackType : uint8_t
{
    // ---- master tracks (binding == -1) ----
    CameraCut = 0,
    Shot,
    SubSequence,
    Fade,
    TimeDilation,
    Marker,          // pure annotation; also the ruler's snap source
    SceneLoad,

    // ---- object tracks (binding >= 0) ----
    Transform,
    SkeletalAnim,
    CameraMotion,
    Director,        // drives MotionDirectorComponent::masterTime
    Property,
    MaterialParam,
    Visibility,
    PostProcess,
    Audio,
    VFX,
    CameraShake,
    Subtitle,
    Event,

    COUNT
};

// How the evaluator treats a track. Every track type MUST declare one: the classification is the
// correctness contract, and a wrong label means nobody tests the case that breaks.
enum class EvalMode : uint8_t
{
    Absolute,    // recomputed from frameF every tick; forward and backward must agree bit-for-bit
    RangeState,  // the live-section set comes from frameF alone; enter/exit is direction-agnostic
    OneShot,     // fires only on a CONTINUOUS crossing in the play direction; fully suppressed on a seek
};

// Static per-type metadata. Free functions rather than members so TrackType
// stays a plain enum usable in switch statements and serialization.
EvalMode    TrackEvalMode(TrackType t);
bool        TrackIsMaster(TrackType t);          // true => binding must be -1
const char* TrackTypeName(TrackType t);          // canonical .iseq token, e.g. "CAMERACUT"
bool        TrackTypeFromName(const char* s, TrackType& out);
const char* TrackTypeLabel(TrackType t);         // human label, e.g. "Camera Cut"

struct Track
{
    TrackType   type    = TrackType::Transform;
    int32_t     binding = -1;        // index into Sequence::bindings; -1 = master track
    std::string name;                // display name; Property/Material tracks also use it as the target key
    uint32_t    targetId = 0;        // HashTarget(name), resolved once on load; never serialized
    bool        mute     = false;
    bool        solo     = false;    // any solo in the sequence mutes every non-solo track
    bool        locked   = false;

    std::vector<Section>     sections;
    std::vector<std::string> unknownLines;

    // Sections sorted by (row, startFrame). Authoring keeps this invariant.
    void SortSections();
};

// ==== Binding - UE's Possessable vs Spawnable ====
enum class BindingKind : uint8_t { Possessable = 0, Spawnable = 1 };

// Built-in Spawnable recipes. FromPrefab uses prefabPath; the rest are entity templates the
// runtime constructs directly (sequencer plumbing with no .ipfb).
enum class SpawnArchetype : uint8_t
{
    FromPrefab = 0,
    Camera,             // VirtualCamera + CameraPose + VCamPriority + VCamBlend + transforms
    PostProcessVolume,  // PostProcessVolumeComponent, isGlobal
    FadeQuad,           // UICanvas + UIRect (fullscreen) + UIImage with EMPTY texturePath -> white
    SubtitleText,       // UICanvas + UIRect + UIText
    AudioSource,        // AudioSourceComponent
};

const char* SpawnArchetypeName(SpawnArchetype a);
bool        SpawnArchetypeFromName(const char* s, SpawnArchetype& out);

struct Binding
{
    BindingKind kind = BindingKind::Possessable;

    // THE identity. Stable across scenes, prefab instances and cast overrides.
    std::string role;

    // ---- Possessable ----
    // Authoring-time default only. The runtime prefers the cast carried by
    // SequencePlayerComponent::roleBindings; this is what the editor falls
    // back to so a freshly-opened .iseq previews without ceremony.
    ECS::Guid   defaultGuid = ECS::kInvalidGuid;

    // Prefab / .iscn descendants have no GUID the asset can name (each prefab instance derives its
    // own), so a possessable addresses the GUID'd ROOT plus a '/'-separated NameComponent chain,
    // resolved by a breadth-first subtree walk (the root -> descendant indirection ResolveSkinnedAnim
    // in CameraStackSystem.cpp also resolves).
    std::string childPath;

    // ---- Spawnable ----
    SpawnArchetype    archetype  = SpawnArchetype::FromPrefab;
    std::string       prefabPath;
    DirectX::XMFLOAT3 spawnPos   { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4 spawnRot   { 0.f, 0.f, 0.f, 1.f };
    DirectX::XMFLOAT3 spawnScale { 1.f, 1.f, 1.f };

    std::vector<std::string> unknownLines;

    bool IsSpawnable() const { return kind == BindingKind::Spawnable; }
};

// ==== Sequence - the .iseq asset ====

// What a sequence does to the state it touched when it stops.
// Restoring in Edit mode protects the scene: sequencer writes bypass the editor's undo stack and
// scene-dirty marking, so without a restore they would change the scene permanently with no undo
// entry and no save prompt.
enum class RestoreMode : uint8_t
{
    Always     = 0,   // always restore pre-animated state on Stop
    EditorOnly = 1,   // restore in Edit (protects the scene), leave posed in Play
    Never      = 2,   // a gameplay cutscene that intentionally leaves actors where it ended
};

// Where the sequence clock gets its delta.
enum class ClockSource : uint8_t
{
    Unscaled = 0,   // raw frame delta: a cutscene ignores gameplay slow-mo
    Scaled   = 1,   // affected by the global time scale
    Fixed    = 2,   // exactly one sequence frame per tick (step / render-out)
};

struct Sequence
{
    int32_t     durationFrames = 300;
    float       frameRate      = 30.f;    // AUTHORITATIVE time base, not a display hint
    bool        looping        = false;

    // A forward jump larger than this is a seek, not playback. Without it a
    // single frame hitch (alt-tab, shader compile, breakpoint) fires every
    // one-shot in the skipped span in one frame.
    int32_t     maxForwardJump = 15;

    RestoreMode restoreMode = RestoreMode::EditorOnly;
    ClockSource clockSource = ClockSource::Unscaled;

    // Cross-sequence write arbitration: players evaluate in ascending layer, so where two sequences
    // drive one entity the higher layer writes last and wins. Entity creation order cannot decide it:
    // it changes across a scene save/load (LoadScene re-creates every row in file order).
    int32_t     layer = 0;

    std::vector<Binding> bindings;
    std::vector<Track>   tracks;

    std::vector<std::string> unknownLines;

    // ---- derived helpers ----
    float   FrameToSeconds(float f) const { return frameRate > 0.f ? f / frameRate : 0.f; }
    float   SecondsToFrame(float s) const { return s * frameRate; }
    float   DurationSeconds()       const { return FrameToSeconds(static_cast<float>(durationFrames)); }
    bool    AnySolo()               const;
    // A track contributes iff it is not muted and (no solo exists, or it is solo).
    bool    TrackAudible(const Track& t) const;
    // -1 when the role is absent.
    int32_t FindBindingByRole(const std::string& role) const;

    // Re-derive every cached, non-serialized field (Track::targetId, sort
    // invariants). Called by the parser; call it after programmatic edits too.
    void    Normalize();
};

// ==== .iseq I/O ====
// Same five-function shape as Resource/VFXPrefab.h.

// Read through AssetFS (game.ipak first, then loose disk) and parse. Returns
// false and logs on I/O failure or a MINVER refusal; `out` is always written
// (a failed load yields a default-constructed sequence, so the runtime is a
// no-op rather than a crash).
bool LoadSequence(const std::string& path, Sequence& out);

// Parse from an in-memory blob, no I/O; LoadSequence is this plus an AssetFS read. Lets the
// editor parse unsaved buffers and the round-trip self-test run without disk. Returns false only
// on a MINVER refusal.
bool ParseSequenceText(const std::string& text, Sequence& out);

// The exact inverse of the parser, including verbatim re-emission of every
// retained unknown line. A parse->build->parse->build is a text fixpoint.
std::string BuildSequenceText(const Sequence& seq);

// Write BuildSequenceText to loose disk (creates parent dirs). AssetFS is
// read-only, so this always targets the project tree, never the pak.
bool SaveSequence(const std::string& path, const Sequence& seq);

// Round-trip oracle over the shipped sample assets, plus a synthetic
// unknown-directive preservation check. Logs PASS/FAIL per case.
bool SequenceSelfTest();

// FNV-1a 32. Never returns 0, so 0 is a usable "unset" sentinel.
uint32_t HashTarget(const char* s) noexcept;
inline uint32_t HashTarget(const std::string& s) noexcept { return HashTarget(s.c_str()); }

// Percent-encoding used for every free-form string in the format. Exposed
// because the editor needs the same rules when it previews raw text.
std::string PctEncode(const std::string& s);
std::string PctDecode(const std::string& s);

// Ease evaluation, shared by Section::EvaluateWeight and the editor's handle
// rendering. `p` is clamped to [0,1].
float ApplyEase(EaseCurve c, float p);

} // namespace Seq
