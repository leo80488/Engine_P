#pragma once

// The damage pipeline: how a hit becomes a change of Health, in one fixed order, with a recorded reason
// for every hit that changed nothing. Data and order are C++, the policy is Lua's (a pre-hook).
//
//   target alive?            -> NoTarget
//   already dead?            -> Dead
//   immune (State.IFrame / State.Invincible, or the hit said so)   -> Immune
//   the same hit group already applied to this target by this instigator   -> SameHitGroup
//   pre-hook (Lua: Damage.OnBefore) may veto -> Blocked / Vetoed, or change amount / type
//   amount > 0?              -> NoAmount
//   the target has no Health -> Unmanaged (a hit that landed, but there is nothing to take it off)
//   resistance               amount x (1 - Resist_<type>); nothing left -> Resisted
//   Health                   applied = min(amount, Health); overkill = the rest
//   events                   Damaged (always, with the reason) and Died (Health reached zero, once)
//
// A HitboxHit becomes a Spec: the pipeline subscribes to the event and runs its queue as a stage of
// EventDelivery's drain, so the Damaged / Died events reach Lua with the HitboxHit that caused them.
// Consumers that mean "a hit landed" (HitStop, quests) read Damaged and test Landed(reason): a hit that
// was blocked, vetoed, immune or on a corpse is not one.
// Every outcome but Unmanaged is logged as "[DAMAGE]" and kept in a ring (Recent / Last).
// Main thread.

#include "ECS/ECS.h"
#include "Gameplay/AttributeSet.h"
#include "Gameplay/GameplayTags.h"

#include <DirectXMath.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Gameplay::Damage
{
    enum class Reason : uint8_t
    {
        Applied,        // Health went down
        Unmanaged,      // a hit that landed on a target with no Health: nothing to apply
        Immune,         // the target held State.IFrame / State.Invincible
        Blocked,        // the pre-hook vetoed it as a block / parry
        Vetoed,         // the pre-hook vetoed it
        Dead,           // the target had no Health left
        SameHitGroup,   // this instigator's hit group had already applied to the target
        NoTarget,       // the target is not alive
        Resisted,       // the resistance took all of it
        NoAmount,       // nothing to apply (amount zero or negative)
    };

    const char* ReasonName(Reason reason);
    // The hit got through: it changed Health, or there was no Health to change.
    inline bool Landed(Reason reason) { return reason == Reason::Applied || reason == Reason::Unmanaged; }

    // What is asked of the pipeline.
    struct Spec
    {
        Entity      target     = NullEntity;
        Entity      instigator = NullEntity;
        Entity      source     = NullEntity;   // what carried the volume (a projectile); the instigator for a melee swing
        Entity      part       = NullEntity;   // the collider struck
        std::string type       = "Physical";
        float       amount     = 0.f;
        uint32_t    hitGroup   = 0;            // 0 = no group: every spec applies
        std::vector<Tag> tags;
        bool        fromHit    = false;        // made from a HitboxHit
        bool        immune     = false;        // the hit already reported the target immune
        uint32_t    slot       = 0;
        DirectX::XMFLOAT3 point  = { 0, 0, 0 };
        DirectX::XMFLOAT3 normal = { 0, 1, 0 };
    };

    struct Result
    {
        Reason      reason     = Reason::NoTarget;
        float       requested  = 0.f;   // the amount after the hook, before the resistance
        float       applied    = 0.f;
        float       overkill   = 0.f;
        float       health     = 0.f;   // Health after (0 when the target has none)
        bool        died       = false;
        std::string detail;             // the hook's own words for a veto
    };

    // What the pre-hook decided.
    struct Verdict
    {
        bool        veto   = false;
        Reason      reason = Reason::Vetoed;   // Blocked or Vetoed
        std::string detail;
    };
    // May change @p spec's amount, type and tags, or veto it through @p verdict.
    using PreHook = std::function<void(World&, Spec& spec, Verdict& verdict)>;
    // The one hook the script layer installs (it fans out to its Damage.OnBefore functions); an empty
    // function removes it.
    void SetPreHook(PreHook hook);
    bool InPreHook();

    // Runs @p spec through the pipeline now and publishes its events. Refused from inside the pre-hook
    // (a Result of reason Vetoed).
    Result Apply(World& world, Spec spec);

    // Subscribes to HitboxHit and adds the stage that turns the queued hits into specs (called once by
    // EventDelivery::Install).
    void Install();

    // ---- The events ----
    struct DamagedEvent
    {
        Entity      target     = NullEntity;
        Entity      instigator = NullEntity;
        Entity      source     = NullEntity;
        Entity      part       = NullEntity;
        std::string type;
        float       requested  = 0.f;
        float       applied    = 0.f;
        float       overkill   = 0.f;
        float       health     = 0.f;
        Reason      reason     = Reason::Applied;
        std::string detail;
        uint32_t    hitGroup   = 0;
        bool        fromHit    = false;
        DirectX::XMFLOAT3 point  = { 0, 0, 0 };
        DirectX::XMFLOAT3 normal = { 0, 1, 0 };
    };

    // ---- Why a hit did nothing ----
    struct Record
    {
        uint64_t    seq        = 0;   // the n-th outcome of the run
        Entity      target     = NullEntity;
        Entity      instigator = NullEntity;
        std::string type;
        float       requested  = 0.f;
        float       applied    = 0.f;
        float       overkill   = 0.f;
        Reason      reason     = Reason::Applied;
        std::string detail;
        uint32_t    hitGroup   = 0;
    };
    // The last outcomes, newest first, at most @p count (64 are kept).
    std::vector<Record> Recent(size_t count);
    // The newest outcome for @p target; false when there is none.
    bool Last(Entity target, Record& out);
    // Counts since the run began: outcomes of that reason.
    uint64_t Count(Reason reason);
    uint64_t DiedCount();
    // Forgets the ring and the counts (a test between scenes).
    void ResetDiagnostics();

    // True when the reverse arm @p mode is asked for with -damage_reverse=<mode>[,<mode>...] (harness
    // builds only): "resist" skips the resistance, "group" the hit-group memory, "died" announces Died
    // on every hit that leaves Health at zero, "hitstop" reports every refused hit to Damaged consumers
    // as applied (the old HitStop that reacted to every HitboxHit).
    bool Reverse(const char* mode);
}
