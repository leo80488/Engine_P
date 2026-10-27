#pragma once

// Timeline / AnimNotify data layer. Pure data (no system, editor or DX12) in EngineCore, so the
// runtime (TimelineSystem and the consumer systems) and the editor's notify track editor can both
// depend on it.
// Model (docs/design/timeline_system_architecture.md): AnimationComponent advances a clip;
// TimelineComponent says at which clip times the world reacts - NotifyTracks holding point
// Notifies and interval NotifyStates. Each frame TimelineSystem routes every notify whose time lies
// in the clip's prev -> curr window to the matching Pending* mailbox component on the same entity,
// and the consumer systems drain those mailboxes in a later phase (spawn VFX, play audio, open a
// hitbox, ...). Mailbox components keep the hot path strictly ordered; the EventBus carries
// cross-system broadcasts to Lua.

#include "ECS/ECS.h"
#include "Gameplay/GameplayTags.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// NotifyCategory - top-level routing key. Tier 1 categories each map to one dedicated Pending*
// mailbox; StateToggle (Tier 2) and Custom (Tier 3) use generic mailboxes.
enum class NotifyCategory : uint8_t
{
    Hitbox      = 0,   // -> PendingHitboxCommands
    VFX         = 1,   // -> PendingVFXSpawns
    Camera      = 2,   // -> PendingCameraEffects
    Audio       = 3,   // -> PendingAudioPlays
    StateToggle = 4,   // -> PendingStateToggles (generic, tag-driven)
    Custom      = 5,   // -> PendingGenericNotifies (designer / Lua)
    // -> ActiveFXGates (a LEVEL, not a mailbox; see AnimFXGateComponent.h). Only meaningful as a
    // NotifyState: the interval is the payload. A point notify of this category is inert.
    FXGate      = 6,
    Sync        = 7, // graph marker sync; no gameplay mailbox
    IKGate      = 8,   // LEVEL: goalMask, weight, forceLock; evaluated by IKRig Prepare

    COUNT
};

// StringID - hash wrapper around std::string for designer-authored type ids ("OnFootstep",
// "ComboWindowEnd"). Hot paths compare the FNV-1a hash; the string is kept for debugging and
// serialization. Identity is the hash, so two StringIDs built from the same string compare equal;
// hash 0 means invalid (Fnv1a never returns it).
struct StringID
{
    uint64_t    hash = 0;
    std::string str;

    StringID() = default;
    explicit StringID(std::string s) : hash(Fnv1a(s)), str(std::move(s)) {}

    bool IsValid() const { return hash != 0; }

    bool operator==(const StringID& o) const { return hash == o.hash; }
    bool operator!=(const StringID& o) const { return hash != o.hash; }

    static uint64_t Fnv1a(const std::string& s)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ull;
        }
        return h ? h : 1ull;
    }
};

namespace std {
    template <> struct hash<StringID> {
        size_t operator()(const StringID& s) const noexcept { return static_cast<size_t>(s.hash); }
    };
}

// PropertyBag - generic key -> value store for Notify params: the few primitive types a designer
// authors, without a reflection system.
struct PropertyBag
{
    using Value = std::variant<int, float, std::string, DirectX::XMFLOAT3, DirectX::XMFLOAT4>;
    std::unordered_map<std::string, Value> values;

    // ---- Typed accessors: a missing key or another type returns the default ----
    int GetInt(const std::string& k, int def = 0) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<int>(&it->second)) return *v;
        return def;
    }
    void SetInt(const std::string& k, int v) { values[k] = v; }

    float GetFloat(const std::string& k, float def = 0.f) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<float>(&it->second)) return *v;
        return def;
    }
    void SetFloat(const std::string& k, float v) { values[k] = v; }

    std::string GetString(const std::string& k, const std::string& def = {}) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<std::string>(&it->second)) return *v;
        return def;
    }
    void SetString(const std::string& k, std::string v) { values[k] = std::move(v); }

    DirectX::XMFLOAT3 GetVec3(const std::string& k, DirectX::XMFLOAT3 def = {0,0,0}) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<DirectX::XMFLOAT3>(&it->second)) return *v;
        return def;
    }
    void SetVec3(const std::string& k, DirectX::XMFLOAT3 v) { values[k] = v; }

    // Colours and quaternions.
    DirectX::XMFLOAT4 GetVec4(const std::string& k,
                              DirectX::XMFLOAT4 def = {0,0,0,0}) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        if (auto* v = std::get_if<DirectX::XMFLOAT4>(&it->second)) return *v;
        return def;
    }
    void SetVec4(const std::string& k, DirectX::XMFLOAT4 v) { values[k] = v; }
};

// Notify - a point event on a NotifyTrack. `time` is clip-local seconds; `params` carries the
// category-specific payload (damage, slot, asset path, ...); `id` is stable per clip so the
// editor can track a notify across drags and undo.
struct Notify
{
    uint32_t       id       = 0;          // assigned by editor; 0 = unset
    NotifyCategory category = NotifyCategory::Custom;
    float          time     = 0.f;        // clip-local seconds
    PropertyBag    params;

    // Editor display fields (serialized too: cheap and useful)
    std::string    displayName;
    uint32_t       color    = 0xFF80C8FFu; // RRGGBBAA little-endian (ImU32 layout)
};

// NotifyState - an interval event; the consumer observes three phases:
//   - Begin: time crossed `startTime` going forward
//   - Tick : this frame's prev and curr both lie inside (startTime, endTime)
//   - End  : time crossed `endTime` (going forward, or wrap)
// TimelineSystem encodes the phase in the mailbox payload (PendingHitboxCommands::Cmd::type
// carries Begin / End for the hitbox case, etc.).
struct NotifyState
{
    uint32_t       id       = 0;
    NotifyCategory category = NotifyCategory::Custom;
    float          startTime = 0.f;
    float          endTime   = 0.1f;
    PropertyBag    params;

    std::string    displayName;
    uint32_t       color = 0xFF80C8FFu;
};

// NotifyTrack - one named row in the timeline editor. Its category constrains every notify it
// holds (a Hitbox track holds only Hitbox notifies). `muted` disables the whole row at runtime
// without deleting it.
struct NotifyTrack
{
    std::string              name;
    NotifyCategory           category = NotifyCategory::Custom;
    bool                     muted    = false;
    std::vector<Notify>      notifies;
    std::vector<NotifyState> states;
};

// TimelineComponent - an entity's NotifyTracks, bound to its AnimationComponent. `clipDuration`
// mirrors the active clip's length so the editor and the crossing test wrap correctly on looping
// clips. `lastObservedTime` is runtime state: TimelineSystem seeds it from
// AnimationComponent::primaryTime on first sight and updates it every frame.
struct TimelineComponent
{
    std::vector<NotifyTrack> tracks;
    float                    clipDuration     = 1.f;
    float                    lastObservedTime = -1.f;  // <0 = uninitialized; first frame just snaps
    uint32_t                 nextNotifyId     = 1u;    // monotonic editor allocator
};

// ==== Pending* mailbox components ====
// Written by TimelineSystem (or another producer), drained and cleared by the matching consumer
// system in a later phase. One component per engine subsystem: Hitbox commands carry Begin / End /
// Change in one stream, Camera effects Shake / Zoom / HitStop, etc. Producers push back, consumers
// walk front to back and clear the vector. Mailboxes are not auto-cleared between frames when
// their consumer is missing: that would mask a scheduler ordering bug.

// ---- Tier 1: Hitbox ----

enum class HitboxCmdType : uint8_t { Begin, End, ChangeShape, ChangeDamage };

// Sphere or capsule. Box is deliberately absent: Jolt has no cheap oriented-box
// overlap that beats a capsule for a limb or a blade, and every melee volume in
// practice is one of these two.
enum class HitboxShapeKind : uint8_t { Sphere = 0, Capsule = 1 };

struct HitboxParams
{
    // ---- Attachment ----
    // Which socket on the attacker the volume rides; 0 = the entity's own GlobalTransform (a
    // body-check, an AoE stomp). It gives `localOffset` its space. SocketSystem refreshes sockets in
    // TickPhase::Animation, before TickPhase::BoneAttachment consumes this, so the transform read is
    // always this frame's. Bare hands and a sword are the same path: a socket on the hand bone or on
    // the blade, no dummy entity.
    uint32_t          socketHash  = 0;             // SocketComponent::HashName
    DirectX::XMFLOAT3 localOffset = { 0, 0, 0 };   // offset in SOCKET space

    // ---- Shape ----
    HitboxShapeKind   shape       = HitboxShapeKind::Sphere;
    float             radius      = 0.5f;
    float             halfHeight  = 0.f;           // Capsule: half the segment, caps excluded; axis = socket local Y

    // ---- Gameplay ----
    // Who is responsible for the volume when that is not its carrier; NullEntity = the carrier
    // (melee). Needed for volumes that outlive their swing on their own entity (a fireball, a
    // lingering AoE, a thrown weapon): without it the system would credit the projectile rather than
    // the caster and, worse, hit the caster, since a projectile's root is not the caster's.
    Entity            instigator  = NullEntity;

    // ---- Affiliation filter ----
    // Whose side a target is on: a different axis from the physics layer, which says what kind of
    // volume it is. Do not encode sides as layers ("PlayerHurtBox" / "EnemyHurtBox"): there are only 8
    // global layers, and PvP, a charmed ally, a prop that takes melee or friendly fire would force a
    // new layer scheme. With layer = kind and tag = side those are just different tag strings.
    //   requireTag  non-empty: the victim must carry this tag (TagComponent)
    //   ignoreTag   non-empty: a victim carrying this tag is skipped
    // Both empty (the default) hits anything the layer mask found. Tags are checked on the victim's
    // root.
    std::string       requireTag;
    std::string       ignoreTag;
    // Gameplay tags (Gameplay/GameplayTags.h), also checked on the victim's root: it must match every
    // tag of requireTags and none of ignoreTags, a tag matching the tags below it ("Team" matches a
    // victim holding "Team.Enemy"). Empty = no condition.
    std::vector<Gameplay::Tag> requireTags;
    std::vector<Gameplay::Tag> ignoreTags;

    float             damage      = 10.f;
    uint32_t          hitGroup    = 0;             // dedupe key: same group = same swing
    // Which physics layers count as a target. 0 = "use the system default",
    // resolved to the HurtBox layer by HitboxSystem so authored data does not
    // have to know the bit values.
    uint16_t          layerMask   = 0;
};

struct PendingHitboxCommands
{
    struct Cmd {
        HitboxCmdType type    = HitboxCmdType::Begin;
        uint32_t      slot    = 0;                 // multiple hitboxes per entity
        HitboxParams  params;
    };
    std::vector<Cmd> commands;
};

// ---- Tier 1: VFX ----

enum class VFXAttachMode : uint8_t { Detached, AttachToBone, FollowEntity };

struct PendingVFXSpawns
{
    struct Spawn {
        std::string       assetPath;               // VFX prefab, e.g. "FX/sword_trail.ivfx"
        VFXAttachMode     mode      = VFXAttachMode::Detached;
        int32_t           boneIndex = -1;          // valid when mode == AttachToBone
        DirectX::XMFLOAT3 localPos  = { 0, 0, 0 };
        float             duration  = 1.0f;        // <=0 = use asset default
    };
    std::vector<Spawn> spawns;
};

// ---- Tier 1: Camera ----

enum class CameraEffectType : uint8_t { Shake, Zoom, FOVPunch, HitStop };

struct CameraEffectParams
{
    // Shake
    float shakeAmplitude = 0.2f;
    float shakeFrequency = 20.f;
    // Zoom / FOVPunch
    float fovDelta       = 0.f;       // degrees
    // HitStop
    float duration       = 0.05f;
    float timeScale      = 0.05f;     // 0 = freeze
};

struct PendingCameraEffects
{
    struct Effect {
        CameraEffectType   type = CameraEffectType::Shake;
        CameraEffectParams params;
    };
    std::vector<Effect> effects;
};

// ---- Tier 1: Audio ----

enum class AudioCategoryTag : uint8_t { SFX, Voice, Foley, Music };

struct PendingAudioPlays
{
    struct Play {
        std::string      clipPath;                 // resolved by AudioSystem
        AudioCategoryTag category   = AudioCategoryTag::SFX;
        float            volume     = 1.f;
        float            pitch      = 1.f;
        int32_t          attachBone = -1;          // <0 = play 2D
    };
    std::vector<Play> plays;
};

// ---- Tier 2: Generic state toggles ----

// A point StateToggle: StateToggleSystem holds the tag (enable) for `duration` seconds of game time, or
// until a toggle of the same tag switches it off. A StateToggle NotifyState (an interval) is not here:
// TimelineSystem reads it as a level (ActiveStateToggles::open), so a clip cancelled inside it leaves
// nothing held. The tag is a dotted gameplay name (ECS/StateToggle.h).
struct PendingStateToggles
{
    struct Toggle {
        Gameplay::Tag tag;
        bool          enable   = true;
        float         duration = 0.f;   // 0 = persist until next toggle; >0 = auto-clear timer
    };
    std::vector<Toggle> toggles;
};

// ---- Tier 3: Custom designer / Lua notifies ----

struct PendingGenericNotifies
{
    struct Notify {
        StringID    typeId;
        PropertyBag params;
    };
    std::vector<Notify> notifies;
};

// CustomNotifyEvent - published on the EventBus by GenericNotifyDispatcher for every
// Custom-category notify (clip- or TimelineComponent-authored) and the engine-fired "clip_end".
// Public so the script layer can subscribe: ScriptSystem bridges it to Lua
// OnAnimEvent(self, name, payload) and the "AnimNotify" LuaBus channel.
struct CustomNotifyEvent
{
    Entity      entity = NullEntity;
    StringID    typeId;
    PropertyBag params;
};

// What HitboxSystem knows of the victim at the moment of the overlap: Immune when its root holds
// State.IFrame or State.Invincible (Gameplay::Std). Whether health drops is still the combat
// code's decision.
enum class HitResult : uint8_t { Hit, Immune };

// HitboxHitEvent - one attacker / victim pair, published by HitboxSystem the first time a
// hitGroup overlaps a target. It reports a hit, not damage: whether health drops is a gameplay
// decision (blocking, friendly fire, armour), so `damage` is the authored number for a
// combat system to interpret. Nothing in the animation pipeline may write health.
// Bridged to Lua as the "HitboxHit" LuaBus channel.
struct HitboxHitEvent
{
    // Who gets the credit / blame: the instigator's root when one is set and alive, else the root
    // of the entity the volume rode.
    Entity   attacker = NullEntity;
    // The entity the volume was actually attached to: the character for a
    // melee swing, the projectile for a spawned one. Lets gameplay find the
    // fireball that landed, not just who cast it.
    Entity   source   = NullEntity;
    Entity   victim   = NullEntity;   // the victim's ROOT, one per character
    // The collider actually struck. Equals `victim` for a single-volume character; with per-part
    // hurtboxes it is the only way to tell a head hit from a leg hit, since dedupe collapses to the
    // root.
    Entity   victimPart = NullEntity;
    uint32_t slot     = 0;
    uint32_t hitGroup = 0;
    float    damage   = 0.f;
    // One Immune report per victim per swing, and the swing can still Hit it once the immunity ends.
    HitResult result  = HitResult::Hit;
    DirectX::XMFLOAT3 point  = { 0, 0, 0 };   // world-space contact point
    DirectX::XMFLOAT3 normal = { 0, 1, 0 };   // world-space, points at the attacker
};
