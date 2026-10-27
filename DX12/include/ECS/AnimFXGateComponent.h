#pragma once

// AnimFXGateComponent - "this effect is armed by the animation, not by hand".
//
// A weapon prefab on a hand socket carries its trail / spark emitters with it, and those are
// pure motion reactors (the trail samples whenever it moves further than minSampleDistance,
// the sparks fire whenever the blade tip crosses minSwingSpeed), so walking with the weapon
// drawn smears a ribbon. The effect must be live only during the attack that earns it.
//
// LEVEL-triggered, not edge-triggered: a NotifyState whose End edge disables the trail leaves
// the weapon glowing forever, because TimelineSystem's Path A fires a clip's tracks only while
// `lastNotifyClip == primaryClip` - a cancelled combo, a stagger or death mid-swing switches
// the clip and the End edge is never evaluated. So the gate is recomputed every frame:
//   TimelineSystem   writes ActiveFXGates on the ANIMATING entity: the FXGate intervals whose
//                    [startTime, endTime] contains primaryTime on the CURRENT clip.
//   AnimFXGateSystem reads it back through the attachment chain and mirrors it onto
//                    TrailComponent::enabled / ParticleEmitterComponent::enabled.
// A clip switch changes the answer the same frame; "off" is the resting state and needs no
// event.
//
// Authoring:
//   1. Put ONE AnimFXGateComponent on the weapon prefab's root. With includeFollowers
//      (default) it drives every Trail / ParticleEmitter in the root's subtree AND every
//      entity FollowEntity-bound to it - how a prefab's VFX children are wired (followers
//      with parent = -1, not real children).
//   2. On the attack .ianim, add a track with category FXGate and a NotifyState interval
//      spanning the swing. Its `group` param must match the component's `group`, or leave
//      BOTH empty for "any attack arms it".
// `forceOn` keeps the effects live regardless of animation, for tuning the look in the
// editor.

#include <cstdint>
#include <string>
#include <vector>

namespace AnimFX
{
    // FNV-1a over the group name. 0 is reserved as the "ungrouped / matches
    // anything" sentinel, so an empty name can never collide with a real one.
    inline uint64_t HashGroup(const std::string& s)
    {
        if (s.empty()) return 0ull;
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
        return h ? h : 1ull;
    }
}

// AnimFXGateComponent - authored, serialized. Placed on the entity that owns the effect
// group (normally a weapon prefab root).
struct AnimFXGateComponent
{
    // Must match the FXGate notify's `group` param. Empty on EITHER side means
    // "match anything", so the zero-config case (one weapon, one attack track)
    // needs no naming at all. Serialized percent-encoded, so spaces are legal.
    std::string group;

    // Drive the whole effect group, not just this entity: descendants through
    // Children, plus everything FollowEntity-bound to any member (transitive).
    bool includeFollowers = true;

    // Ignore the animation and keep the group live. For look-dev: with the gate installed,
    // dragging the weapon around in the editor produces nothing until an attack clip opens a
    // gate, which reads as "my trail broke".
    bool forceOn = false;

    // ---- Runtime only (not serialized) ----
    // The gate writes TrailComponent::enabled / ParticleEmitterComponent::enabled only when the
    // answer CHANGES, so the inspector checkboxes stay usable between swings instead of being
    // stomped every frame.
    bool lastApplied = false;
    bool hasApplied  = false;

    // A gate spanning the entire clip never has a falling edge at loop wrap.
    // Track playback separately so each pass can restart one-shot material
    // clocks and ribbon history even while the gate remains open.
    uint32_t lastDriver = ~0u;
    uint32_t lastClip = ~0u;
    float lastTime = 0.f;
    bool hasPlayback = false;

    bool ObservePlayback(uint32_t driver, uint32_t clip, float time)
    {
        const bool restarted = hasPlayback &&
            (driver != lastDriver || clip != lastClip || time < lastTime - 0.00001f);
        lastDriver = driver;
        lastClip = clip;
        lastTime = time;
        hasPlayback = true;
        return restarted;
    }
};

// ActiveFXGates - per-frame LEVEL output of TimelineSystem, written on the ANIMATING entity
// (the one with AnimationComponent). Unlike the Pending* mailboxes in NotifyTypes.h this is
// not a queue: it is cleared and refilled every frame, and an empty list means "no gate is
// open right now".
struct ActiveFXGates
{
    std::vector<uint64_t> groups;         // AnimFX::HashGroup of each open gate
    bool                  matchesAny = false;  // an open gate with an empty group

    bool IsOpen(uint64_t groupHash) const
    {
        if (matchesAny) return true;              // gate names nothing -> arms all
        if (groupHash == 0ull) return !groups.empty();  // component names nothing -> any gate
        for (const uint64_t h : groups)
            if (h == groupHash) return true;
        return false;
    }
};
