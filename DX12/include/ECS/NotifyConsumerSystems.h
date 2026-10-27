#pragma once

// Systems that drain the Pending* mailbox components TimelineSystem writes:
//   Tier 1 (typed, engine subsystem): HitboxSystem, VFXSpawnSystem, CameraEffectSystem, AudioPlaySystem
//   Tier 2 (generic state):           StateToggleSystem
//   Tier 3 (designer / Lua bridge):   GenericNotifyDispatcher (PendingGenericNotifies -> EventBus)
// Each drains its own Pending<Kind> mailbox. All run in TickPhase::BoneAttachment, so they act in
// the frame TimelineSystem (TickPhase::Animation) wrote the mailbox. AudioPlaySystem republishes as
// the EventBus PlaySoundEvent, so AudioSystem needs no notion of notifies. CameraEffectSystem drives
// only the shake (zoom, FOV punch and hit-stop are logged); StateToggleSystem turns StateToggle
// notifies into gameplay tags (Gameplay/GameplayTags.h).
// AnimFXGateSystem is not a drainer: it mirrors ActiveFXGates onto resident weapon FX.

#include "ECS/ISystem.h"
#include "ECS/NotifyTypes.h"        // HitboxParams - HitboxSystem's slot state

#include <string>
#include <unordered_map>
#include <vector>

namespace Resource { struct VFXPrefab; }
namespace DX12Physics { class PhysicsSystem; }

// ==== Editor hooks for live .ivfx iteration (defined in NotifyConsumerSystems.cpp) ====
// The VFXSpawnSystem prefab cache lives for the process; these let the editor pick up a Save
// without a restart and preview an unsaved prefab in place.
void InvalidateVFXPrefabCache(const std::string& path);        // evict one cached prefab (re-save picked up)
void SetVFXPreviewPrefab(const Resource::VFXPrefab& prefab);   // inject an in-memory prefab under the preview key
extern const char* kVFXPreviewAssetKey;                        // spawn assetPath that resolves to the injected prefab

// Access declarations (in the .cpp) let the Scheduler co-batch the drainers: each owns one
// disjoint mailbox type. VFXSpawnSystem is the deliberate exception - creating entities and adding
// components is its core job, so it keeps the opaque default and serializes.

// HitboxSystem - turns authored hitbox notifies and Attack::Playback windows into overlap tests.
// A Begin command opens a slot, End closes it; every frame in between the slot's shape is placed
// at its socket and tested against the world (a window whose attack sets weaponCollider sweeps the
// owner's AttackBox colliders instead).
// Sub-stepping: a swing moves the volume metres per frame, so testing only where it lands tunnels
// through a target in between. Each active slot keeps last frame's centre and samples the segment
// at step <= radius, so nothing thinner than the shape is skipped.
// Dedupe: `hitGroup` names one swing; a target is reported once per group however many frames or
// sub-steps it stays inside, else a 20-frame window deals 20 hits.
// Slot state lives here, not in a component: it must not serialize (a save must not restore a
// half-open hitbox), and adding components mid-tick is a structural change DeclareAccess promises
// not to make. Entries are checked against the entity's generation so a recycled id cannot inherit
// the previous occupant's open swing.
class HitboxSystem            final : public SystemInPhase<TickPhase::BoneAttachment>
{
public:
    explicit HitboxSystem(DX12Physics::PhysicsSystem* physics) : m_physics(physics) {}

    bool IsActive(const World& world,Entity entity,int slot)const {
        auto found=m_active.find(entity);return slot>=0 && slot<kMaxSlots && world.IsAlive(entity) && found!=m_active.end()
            && found->second.generation==world.GetGeneration(entity) && found->second.slots[slot].active;
    }
    const char* GetName() const override { return "HitboxSystem"; }
    void Update(World&, const FrameContext&) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;

private:
    static constexpr int kMaxSlots = 4;

    struct Slot
    {
        bool                active      = false;
        bool                attackManaged = false;
        bool                weaponCollider = false;
        std::string         weaponTag;
        float               weaponFrom=0.f, weaponTo=1.f;
        HitboxParams        params;
        DirectX::XMFLOAT3   prevCenter  = { 0.f, 0.f, 0.f };
        bool                hasPrev     = false;
        uint32_t            dedupeGroup = 0xffffffffu;
        std::vector<Entity> hits;                  // already reported this swing
        std::vector<Entity> immuneHits;            // already reported as immune this swing
    };

    struct Attacker
    {
        uint32_t generation = 0;
        bool attackRunning = false;
        Slot     slots[kMaxSlots];
    };

    struct WeaponPose {
        uint32_t generation=0;
        Entity owner=NullEntity;
        uint32_t ownerGeneration=0;
        DirectX::XMFLOAT4X4 world{};
    };

    DX12Physics::PhysicsSystem*             m_physics = nullptr;
    std::unordered_map<Entity, Attacker>    m_active;

    std::unordered_map<Entity, WeaponPose> m_weaponPoses;
};

class VFXSpawnSystem          final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "VFXSpawnSystem"; }          void Update(World&, const FrameContext&) override; };

class CameraEffectSystem      final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "CameraEffectSystem"; }      void Update(World&, const FrameContext&) override;
          void DeclareAccess(SystemAccessBuilder& b) const override; };

class AudioPlaySystem         final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "AudioPlaySystem"; }         void Update(World&, const FrameContext&) override;
          void DeclareAccess(SystemAccessBuilder& b) const override; };

// StateToggleSystem - the gameplay tags an animation grants. Each frame it rebuilds the `derived` half
// of GameplayTagsComponent on every animated entity's owner root from that entity's ActiveStateToggles:
// the StateToggle intervals open at the clip's time (`open`, TimelineSystem's level) and the tags held
// by point toggles (`held`, from PendingStateToggles, counted down on the entity's game clock). Nothing
// persists between frames, so a clip cancelled mid-interval, a death or a scene change leaves no tag.
class StateToggleSystem       final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "StateToggleSystem"; }
          void PrepareStructure(World& world) override;
          void Update(World&, const FrameContext&) override;
          void DeclareAccess(SystemAccessBuilder& b) const override; };

class GenericNotifyDispatcher final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "GenericNotifyDispatcher"; } void Update(World&, const FrameContext&) override;
          void DeclareAccess(SystemAccessBuilder& b) const override; };

// Mirrors TimelineSystem's ActiveFXGates level output onto the resident
// Trail / ParticleEmitter components of socket-attached effect groups. NOT a
// mailbox drainer: it recomputes an answer rather than consuming events, which
// is what makes a cancelled combo turn the weapon trail off. See
// ECS/AnimFXGateComponent.h for the full rationale.
class AnimFXGateSystem       final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "AnimFXGateSystem"; }        void Update(World&, const FrameContext&) override;
          void DeclareAccess(SystemAccessBuilder& b) const override; };
