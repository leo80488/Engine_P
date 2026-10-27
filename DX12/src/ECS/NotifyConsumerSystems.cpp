#include "ECS/NotifyConsumerSystems.h"
#include "Gameplay/AttackPlayback.h"
#include "Gameplay/GameplayTime.h"

#include "ECS/ECS.h"
#include "ECS/CommandBuffer.h"
#include "ECS/ExclusiveResourceIds.h"
#include "ECS/FrameContext.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/FollowComponents.h"
#include "ECS/LifetimeComponent.h"
#include "ECS/NotifyTypes.h"
#include "ECS/AnimFXGateComponent.h"    // AnimFXGateComponent / ActiveFXGates
#include "ECS/ParticleComponent.h"
#include "ECS/TrailComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/Components.h"              // MaterialComponent, MeshHandle, VisibilityComponent, RenderLayer, BoundingVolume
#include "ECS/AnimationComponents.h"     // SocketComponent (bone-attach base pose)
#include "ECS/VFXSpawnRequests.h"        // Lane B mailboxes (Tracer / Decal / Afterimage / Mesh)
#include "ECS/CameraStackComponents.h"   // CameraShakeComponent, Camera::kMainChannel
#include "ECS/CameraStackSystem.h"       // Camera::GetOrCreateChannelEntity

#include "Audio/AudioEvents.h"
#include "Resource/VFXPrefab.h"
#include "Resource/MaterialSerializer.h"  // LoadMaterial - .imat-driven VFX look
#include "System/EventBus.h"
#include "System/Check.h"                 // ENGINE_CHECK_ONCE
#include "Physics/PhysicsSystem.h"        // OverlapCapsuleAll (hitbox sweep)
#include "ECS/HierarchyComponents.h"      // Parent / GlobalTransform
#include "ECS/AnimationComponents.h"      // SocketComponent
#include "ECS/PhysicsComponents.h"
#include "ECS/TagComponent.h"             // affiliation filter
#include "Gameplay/GameplayTagsComponent.h"   // gameplay tags: the hitbox filter, StateToggleSystem
#include "System/CommandLine.h"
#include <algorithm>
#include <cmath>
#include "System/Log.h"

#include <algorithm>
#include <DirectXMath.h>
#include <memory>
#include <unordered_map>

// Notify consumers drain animation mailboxes; HitboxSystem also consumes
// Attack::Playback windows against attached weapon colliders.

namespace
{
    // Walk up to the outermost ancestor, bounded like the trigger bubble-up.
    //
    // The notify mailbox lands on the ANIMATED entity (TimelineSystem iterates
    // AnimationComponent), which on a PMX/FBX character is a descendant of the
    // entity gameplay actually holds. Reporting that descendant as the attacker
    // would hand every combat script an id it has to walk up itself.
    Entity ResolveOwnerRoot(World& world, Entity e)
    {
        Entity cur = e;
        for (int depth = 0; depth < 8; ++depth)
        {
            const auto* p = world.GetComponent<Parent>(cur);
            if (!p || p->entity == NullEntity || !world.IsAlive(p->entity)) break;
            cur = p->entity;
        }
        return cur;
    }
    // True under -gametags_legacy_filter (harness builds only): a hitbox ignores its gameplay tag lists.
    bool LegacyTagFilter()
    {
#if WITH_HARNESS
        static const bool s_legacy = Sys::CommandLine::Get().Has("-gametags_legacy_filter");
        return s_legacy;
#else
        return false;
#endif
    }

    // True under -statetoggle_inert (harness builds only): StateToggle notifies grant nothing, as they
    // did before gameplay tags existed.
    bool InertStateToggles()
    {
#if WITH_HARNESS
        static const bool s_inert = Sys::CommandLine::Get().Has("-statetoggle_inert");
        return s_inert;
#else
        return false;
#endif
    }

    // Follow attachments are intentionally not Parent links. Traverse both,
    // validating sockets so an unequipped/orphaned weapon cannot keep hurting.
    Entity WeaponOwner(World& world,Entity e)
    {
        for(int depth=0;depth<32 && world.IsAlive(e);++depth) {
            if(const auto* follow=world.GetComponent<FollowSocketComponent>(e)) {
                const Entity target=follow->target.Resolve(world);
                const auto* sockets=world.GetComponent<SocketComponent>(target);
                if(!sockets)return NullEntity;
                const auto index=follow->socketNameHash?sockets->FindByName(follow->socketNameHash):follow->socketIndex;
                const auto bound=follow->socketNameHash?sockets->count:sockets->AuthoredCount();   // FollowSystem's rule
                if(index>=bound)return NullEntity;
                e=target;
            } else if(const auto* follow=world.GetComponent<FollowEntityComponent>(e)) {
                e=follow->target.Resolve(world);
            } else if(const auto* parent=world.GetComponent<Parent>(e)) {
                if(parent->entity==NullEntity)return e;
                e=parent->entity;
            } else return e;
        }
        return NullEntity;
    }

}

void HitboxSystem::Update(World& world, const FrameContext&)
{
    using namespace DirectX;

    struct Weapon {
        Entity entity,owner;
        const ColliderComponent* collider;
        DirectX::XMFLOAT4X4 previous,current;
    };
    std::vector<Weapon> weapons;
    std::unordered_map<Entity,WeaponPose> poses;
    // Keep the last sampled pose even during windup: the opening frame must
    // include the blade's travel, not just its end position.
    world.ForEach<ColliderComponent>([&](Entity e,const ColliderComponent& collider) {
        if(collider.layer!=ColliderComponent::CollisionLayer::AttackBox)return;
        const auto* gt=world.GetComponent<GlobalTransform>(e);
        const Entity owner=WeaponOwner(world,e);
        if(!gt||owner==NullEntity)return;
        WeaponPose pose{world.GetGeneration(e),owner,world.GetGeneration(owner),gt->matrix};
        auto previous=gt->matrix;
        if(auto it=m_weaponPoses.find(e);it!=m_weaponPoses.end() &&
           it->second.generation==pose.generation && it->second.owner==owner &&
           it->second.ownerGeneration==pose.ownerGeneration) previous=it->second.world;
        poses.emplace(e,pose);
        weapons.push_back({e,owner,&collider,previous,gt->matrix});
    });
    m_weaponPoses.swap(poses);

    // Attack clocks publish intervals AFTER animation sampling, before this
    // query. A whole short window crossed in one frame still gets one sweep.
    for(auto& [entity,attacker]:m_active) {
        attacker.attackRunning=false;
        for(auto& slot:attacker.slots)if(slot.attackManaged)slot.active=false;
    }

    // ---- 1. Drain the mailboxes into slot state ----
    world.ForEach<PendingHitboxCommands>([&](Entity e, PendingHitboxCommands& mb) {
        if (mb.commands.empty()) return;

        Attacker& atk = m_active[e];
        const uint32_t gen = world.GetGeneration(e);
        if (atk.generation != gen)
        {
            // Recycled entity id: the previous occupant's open swings are not this character's.
            // Reset rather than inherit.
            atk = Attacker{};
            atk.generation = gen;
        }

        for (const auto& cmd : mb.commands)
        {
            if (cmd.slot >= static_cast<uint32_t>(kMaxSlots))
            {
                ENGINE_CHECK_ONCE(false,
                    "[Hitbox] slot %u is out of range (max %d) - authored on entity %u",
                    cmd.slot, kMaxSlots, static_cast<uint32_t>(e));
                continue;
            }
            Slot& slot = atk.slots[cmd.slot];

            switch (cmd.type)
            {
            case HitboxCmdType::Begin:
                slot.params = cmd.params;
                slot.active = true;
                slot.attackManaged = false;
                slot.weaponCollider = false;
                // A fresh swing clears the dedupe list. Re-opening with the
                // SAME hitGroup deliberately does not: that is how a multi-part
                // attack (two Begin/End windows, one group) still only hits
                // each target once.
                if (slot.dedupeGroup != cmd.params.hitGroup)
                {
                    slot.dedupeGroup = cmd.params.hitGroup;
                    slot.hits.clear();
                    slot.immuneHits.clear();
                }
                // No previous centre: the opening frame tests only where it is,
                // instead of sweeping from wherever the last swing ended.
                slot.hasPrev = false;
                break;

            case HitboxCmdType::End:
                slot.active  = false;
                slot.hasPrev = false;
                break;

            case HitboxCmdType::ChangeShape:
                slot.params.shape       = cmd.params.shape;
                slot.params.radius      = cmd.params.radius;
                slot.params.halfHeight  = cmd.params.halfHeight;
                slot.params.localOffset = cmd.params.localOffset;
                slot.params.socketHash  = cmd.params.socketHash;
                break;

            case HitboxCmdType::ChangeDamage:
                slot.params.damage = cmd.params.damage;
                break;
            }
        }
        mb.commands.clear();
    });

    world.ForEach<Attack::Playback>([&](Entity,Attack::Playback& p) {
        if(!p.active||!world.IsAlive(p.owner)||world.GetGeneration(p.owner)!=p.ownerGeneration)return;
        if(auto it=m_active.find(p.owner);it!=m_active.end())it->second.attackRunning=true;
        if(!p.started||!p.windows)return;
        auto& atk=m_active[p.owner];
        if(atk.generation!=p.ownerGeneration){atk={};atk.generation=p.ownerGeneration;}
        atk.attackRunning=true;
        const auto& d=*p.definition;
        for(size_t i=0;i<d.windows.size();++i)if(p.windows&(1u<<i)) {
            auto& slot=atk.slots[i];slot.active=true;slot.attackManaged=true;
            slot.weaponCollider=d.weaponCollider;slot.weaponTag=d.weaponTag;slot.params={};
            const double begin=d.TimeAt(p.previous),span=d.TimeAt(p.progress)-begin;
            slot.weaponFrom=span>0?float((std::max)(0.0,(d.TimeAt(d.windows[i].start)-begin)/span)):0.f;
            slot.weaponTo=span>0?float((std::min)(1.0,(d.TimeAt(d.windows[i].end)-begin)/span)):1.f;
            slot.params.radius=d.radius;slot.params.halfHeight=d.halfHeight;
            slot.params.shape=d.halfHeight>0?HitboxShapeKind::Capsule:HitboxShapeKind::Sphere;
            slot.params.localOffset={d.offsetX,d.offsetY,d.offsetZ};slot.params.damage=d.damage;
            slot.params.hitGroup=p.instance;slot.params.layerMask=uint16_t(1u<<2); // QueryLayer::Character
            slot.params.requireTags=d.requireTags;slot.params.ignoreTags=d.ignoreTags;
            if(slot.dedupeGroup!=p.instance){slot.dedupeGroup=p.instance;slot.hits.clear();slot.immuneHits.clear();slot.hasPrev=false;}
        }
        p.windows=0;
    });
    if (m_active.empty()) return;

    // ---- 2. Evaluate every open slot ----
    auto& bus = EventBus::Get();
    std::vector<DX12Physics::OverlapHit> hits;
    const GameplayTime::Clocks& clocks = GameplayTime::Current();

    for (auto it = m_active.begin(); it != m_active.end(); )
    {
        const Entity e   = it->first;
        Attacker&    atk = it->second;

        bool anyActive = false;
        for (const Slot& s : atk.slots) anyActive = anyActive || s.active;

        // Prune: dead, recycled, or nothing open. Keeping idle attackers would
        // grow this map for the life of the process.
        if (!world.IsAlive(e) || world.GetGeneration(e) != atk.generation || (!anyActive&&!atk.attackRunning))
        {
            it = m_active.erase(it);
            continue;
        }
        ++it;

        // A frozen attacker (a paused game clock, or its own dilation at 0) swings nothing: its
        // open slots stay open and resume from where they stopped.
        if (GameplayTime::EntityDt(world.GetComponent<TimeDilationComponent>(e), clocks) <= 0.f)
            continue;

        const Entity carrier = ResolveOwnerRoot(world, e);

        for (uint32_t si = 0; si < static_cast<uint32_t>(kMaxSlots); ++si)
        {
            Slot& slot = atk.slots[si];
            if (!slot.active) continue;

            hits.clear();
            if(slot.weaponCollider) {
                for(const auto& weapon:weapons) {
                    if(weapon.owner!=carrier)continue;
                    if(!slot.weaponTag.empty()) {
                        const auto* tags=world.GetComponent<TagComponent>(weapon.entity);
                        if(!tags||!tags->Has(slot.weaponTag.c_str()))continue;
                    }
                    if(m_physics)m_physics->SweepColliderAll(*weapon.collider,weapon.previous,weapon.current,hits,
                        DX12Physics::MakeLayerMask(DX12Physics::Layer::Character) |
                        DX12Physics::MakeLayerMask(DX12Physics::Layer::HurtBox),slot.weaponFrom,slot.weaponTo);
                }
            } else {
                // ---- Place the shape at its socket (else the entity origin) ----
                XMMATRIX socketWorld = XMMatrixIdentity();
                bool placed = false;
                if (slot.params.socketHash != 0)
                {
                    if (const auto* sc = world.GetComponent<SocketComponent>(e))
                    {
                        const uint32_t idx = sc->FindByName(slot.params.socketHash);
                        if (idx != SocketComponent::kNotFound)
                        {
                            socketWorld = XMLoadFloat4x4(&sc->sockets[idx].worldTransform);
                            placed = true;
                        }
                    }
                    if (!placed)
                    {
                        // Never mis-place silently: the hitbox would hit the wrong things.
                        ENGINE_CHECK_ONCE(false,
                            "[Hitbox] entity %u has an authored socket hash %u that no "
                            "SocketComponent provides - the hitbox falls back to the entity "
                            "origin and will hit the wrong things.",
                            static_cast<uint32_t>(e), slot.params.socketHash);
                    }
                }
                if (!placed)
                {
                    if (const auto* gt = world.GetComponent<GlobalTransform>(e))
                        socketWorld = XMLoadFloat4x4(&gt->matrix);
                }

                const XMVECTOR offs    = XMLoadFloat3(&slot.params.localOffset);
                const XMVECTOR centreV = XMVector3Transform(offs, socketWorld);
                XMFLOAT3 centre; XMStoreFloat3(&centre, centreV);

                XMVECTOR dScale, dRot, dTrans;
                XMMatrixDecompose(&dScale, &dRot, &dTrans, socketWorld);
                XMFLOAT4 rot; XMStoreFloat4(&rot, dRot);

                const float radius = (std::max)(0.001f, slot.params.radius);
                const float half   = (slot.params.shape == HitboxShapeKind::Capsule)
                                   ? (std::max)(0.f, slot.params.halfHeight) : 0.f;
                const DX12Physics::LayerMask mask =
                    slot.params.layerMask != 0
                        ? static_cast<DX12Physics::LayerMask>(slot.params.layerMask)
                        : DX12Physics::MakeLayerMask(DX12Physics::Layer::HurtBox);

                // ---- Sub-step the segment travelled since last frame ----
                // Step no further than the shape's radius so nothing thinner than the volume slips between
                // samples; capped at 8 so a teleport does not become hundreds of queries.
                int steps = 1;
                XMVECTOR fromV = centreV;
                if (slot.hasPrev)
                {
                    fromV = XMLoadFloat3(&slot.prevCenter);
                    const float dist = XMVectorGetX(
                        XMVector3Length(XMVectorSubtract(centreV, fromV)));
                    steps = (std::max)(1, static_cast<int>(std::ceil(dist / radius)));
                    steps = (std::min)(steps, 8);
                }

                for (int step = 1; step <= steps; ++step)
                {
                    const float t = static_cast<float>(step) / static_cast<float>(steps);
                    XMFLOAT3 samplePos;
                    XMStoreFloat3(&samplePos, XMVectorLerp(fromV, centreV, t));

                    if (m_physics)
                        m_physics->OverlapCapsuleAll(samplePos, rot, radius, half, hits, mask);

                }
                slot.prevCenter = centre;
                slot.hasPrev = true;
            }

            // Whose swing this counts as. A dead instigator degrades to the carrier rather than
            // dropping the hit: a fireball whose caster died mid-flight still burns what it lands on.
            const Entity blame =
                (slot.params.instigator != NullEntity
                 && world.IsAlive(slot.params.instigator))
                    ? ResolveOwnerRoot(world, slot.params.instigator)
                    : carrier;

            for (const auto& h : hits)
            {
                if (h.entity == NullEntity || h.entity == e) continue;
                const Entity victimRoot = ResolveOwnerRoot(world, h.entity);
                // Excluded against BOTH: the thing carrying the volume (a
                // projectile must not hit itself) and whoever is to blame
                // (it must not hit its own caster either).
                if (victimRoot == carrier || victimRoot == blame) continue;

                // Affiliation gate. Checked on the ROOT, not the collider
                // that was struck: tags describe the character, and a
                // per-part hurtbox rig should not need every limb tagged.
                if (!slot.params.requireTag.empty() || !slot.params.ignoreTag.empty())
                {
                    const auto* tags = world.GetComponent<TagComponent>(victimRoot);
                    if (!slot.params.requireTag.empty()
                        && (!tags || !tags->Has(slot.params.requireTag.c_str())))
                        continue;
                    if (!slot.params.ignoreTag.empty()
                        && tags && tags->Has(slot.params.ignoreTag.c_str()))
                        continue;
                }

                // Gameplay tags, on the root as well: every required tag matched, none of the ignored ones.
                const GameplayTagsComponent* gameplayTags = world.GetComponent<GameplayTagsComponent>(victimRoot);
                if (!LegacyTagFilter() && (!slot.params.requireTags.empty() || !slot.params.ignoreTags.empty()))
                {
                    const auto matches = [&](Gameplay::Tag tag) { return gameplayTags && gameplayTags->Matches(tag); };
                    if (!std::all_of(slot.params.requireTags.begin(), slot.params.requireTags.end(), matches)) continue;
                    if (std::any_of(slot.params.ignoreTags.begin(), slot.params.ignoreTags.end(), matches)) continue;
                }

                // Dedupe on the ROOT: a character whose hurtboxes are three
                // separate collider entities is still one victim. A victim already struck this swing is
                // done; one that was immune is reported once, and can still be struck when it stops being.
                if (std::find(slot.hits.begin(), slot.hits.end(), victimRoot)
                    != slot.hits.end()) continue;
                const bool immune = gameplayTags && (gameplayTags->Matches(Gameplay::Std::IFrame()) ||
                                                     gameplayTags->Matches(Gameplay::Std::Invincible()));
                std::vector<Entity>& reported = immune ? slot.immuneHits : slot.hits;
                if (std::find(reported.begin(), reported.end(), victimRoot) != reported.end()) continue;
                reported.push_back(victimRoot);

                HitboxHitEvent ev;
                ev.result     = immune ? HitResult::Immune : HitResult::Hit;
                ev.attacker   = blame;
                ev.source     = carrier;
                ev.victim     = victimRoot;
                ev.victimPart = h.entity;
                ev.slot     = si;
                ev.hitGroup = slot.params.hitGroup;
                ev.damage   = slot.params.damage;
                ev.point    = h.point;
                ev.normal   = h.normal;
                bus.Publish<HitboxHitEvent>(std::move(ev));
            }

        }
    }
}

namespace
{
    // Give a spawned emitter its .imat look: TrailSystem / ParticleSystem see the
    // MaterialComponent (with a custom shader) and let it drive shader, blend, params and
    // textures. Loads are cached for the process, failed ones too: a typo'd path on a
    // rapid-fire notify would otherwise re-probe pak and disk and log twice per spawn, 10-20
    // times a second. MaterialSourcePath keeps serialization reference-based should the
    // instance ever be saved.
    void AddLookMaterial(World& world, Entity inst, const std::string& materialPath)
    {
        if (materialPath.empty()) return;
        static std::unordered_map<std::string, std::shared_ptr<MaterialComponent>> s_cache;
        auto it = s_cache.find(materialPath);
        if (it == s_cache.end())
        {
            auto loaded = std::make_shared<MaterialComponent>();
            if (!Resource::LoadMaterial(materialPath, *loaded))
            {
                LOG_WARNING("VFXSpawn: material '%s' failed to load — emitter keeps its component-driven look",
                            materialPath.c_str());
                loaded.reset();   // negative-cache the failure
            }
            it = s_cache.emplace(materialPath, std::move(loaded)).first;
        }
        if (!it->second) return;   // known-bad path, already warned once
        world.AddComponent<MaterialComponent>(inst, MaterialComponent(*it->second));
        world.AddComponent<MaterialSourcePath>(inst, MaterialSourcePath{ materialPath });
    }

    // Process-lifetime prefab cache keyed by asset path, failed loads included. Prefabs are a
    // few KB, so loaded ones are kept. File scope (not a function-local static) so the editor
    // hooks below can evict and inject entries.
    std::unordered_map<std::string, std::shared_ptr<Resource::VFXPrefab>> s_vfxCache;

    const Resource::VFXPrefab* GetOrLoadVFXPrefab(const std::string& path)
    {
        auto it = s_vfxCache.find(path);
        if (it != s_vfxCache.end()) return it->second.get();

        auto prefab = std::make_shared<Resource::VFXPrefab>();
        if (!Resource::LoadVFXPrefab(path, *prefab)) {
            // Cache the failed result too: retrying a broken path 60 times a second while a designer
            // fixes the file would spam the log.
            s_vfxCache.emplace(path, prefab);
            return prefab.get();
        }
        auto inserted = s_vfxCache.emplace(path, std::move(prefab));
        return inserted.first->second.get();
    }

    // Compose two LocalTransforms (spawn-command offset x prefab emitter local) the way
    // TransformSystem propagates Parent -> child: scale multiplies, translation is rotated by the
    // parent rotation. The spawned emitter lands where a Parent/Children hierarchy would put it
    // without rigging one.
    LocalTransform Compose(const LocalTransform& a, const LocalTransform& b)
    {
        using namespace DirectX;
        const XMMATRIX m = b.ToMatrix() * a.ToMatrix();
        XMVECTOR s, r, t;
        XMMatrixDecompose(&s, &r, &t, m);

        LocalTransform out;
        XMStoreFloat3(&out.translation, t);
        XMStoreFloat4(&out.rotation,    r);
        XMStoreFloat3(&out.scale,       s);
        return out;
    }

    // Bake a world matrix into a LocalTransform for Detached mode: the spawned emitter has no
    // parent, so its local is world space.
    LocalTransform BakeFromMatrix(const DirectX::XMFLOAT4X4& world,
                                  const LocalTransform&      prefabLocal)
    {
        using namespace DirectX;
        const XMMATRIX m = prefabLocal.ToMatrix() * XMLoadFloat4x4(&world);
        XMVECTOR s, r, t;
        XMMatrixDecompose(&s, &r, &t, m);

        LocalTransform out;
        XMStoreFloat3(&out.translation, t);
        XMStoreFloat4(&out.rotation,    r);
        XMStoreFloat3(&out.scale,       s);
        return out;
    }

    // Push a fully-resolved Lane-B request onto the spawner's mailbox (get or
    // create the component). Renderer::BeginFrame drains it the same frame.
    template <typename MB, typename Req>
    void PushReq(World& world, Entity owner, Req&& r)
    {
        auto* mb = world.GetComponent<MB>(owner);
        if (!mb) { world.AddComponent<MB>(owner, {}); mb = world.GetComponent<MB>(owner); }
        if (mb) mb->reqs.push_back(std::forward<Req>(r));
    }

    // Best-known world pose to attach a VFX instance to. Bone-attach uses the spawner's live
    // socket transform (refreshed this frame in the Animation phase) so the instance - a trail
    // especially - samples the right place on its first frame instead of the origin.
    // FollowEntity / Detached fall back to the spawner's root world.
    DirectX::XMFLOAT4X4 GetAttachBaseWorld(World& world, Entity spawner,
                                           const PendingVFXSpawns::Spawn& s)
    {
        DirectX::XMFLOAT4X4 base;
        DirectX::XMStoreFloat4x4(&base, DirectX::XMMatrixIdentity());
        if (s.mode == VFXAttachMode::AttachToBone)
            if (auto* sc = world.GetComponent<SocketComponent>(spawner))
            {
                // A raw index names one of the spawner's OWN sockets (FollowSystem's
                // rule): a model's derived ones are not addressed by position.
                const uint32_t idx = (s.boneIndex >= 0) ? static_cast<uint32_t>(s.boneIndex) : 0u;
                if (idx < sc->AuthoredCount()) return sc->sockets[idx].worldTransform;
            }
        if (auto* gt = world.GetComponent<GlobalTransform>(spawner))
            base = gt->matrix;
        return base;
    }

    // Shared Lane-A spawn path. Creates the instance entity, lets the caller
    // attach the type-specific backend component(s), wires attach mode +
    // lifetime, and seeds GlobalTransform from the spawn pose. Returns the
    // instance. Every component-driven VFX type reuses this so the attach /
    // lifetime policy lives in exactly one place.
    template <typename AddBackend>
    Entity SpawnEntityVFX(World& world, Entity spawner,
                          const PendingVFXSpawns::Spawn&   s,
                          const LocalTransform&            cmdOffset,
                          const Resource::VFXEmitterSpec&  spec,
                          const DirectX::XMFLOAT4X4&       baseWorld,
                          float duration, AddBackend&& addBackend)
    {
        using namespace DirectX;
        const Entity inst = world.CreateEntity();
        addBackend(inst);

        const LocalTransform composed = Compose(cmdOffset, spec.localTransform);

        // Seed GlobalTransform with the spawn pose: FollowSystem / TransformSystem only refresh it
        // next frame, and a zero matrix would flash the VFX at the origin (and streak a trail in
        // from world zero).
        GlobalTransform gt;
        XMStoreFloat4x4(&gt.matrix, composed.ToMatrix() * XMLoadFloat4x4(&baseWorld));
        world.AddComponent<GlobalTransform>(inst, gt);

        // Runtime only: notify-fired VFX must never be written into a .iscene. Previewing a clip in
        // edit mode (TimelineSystem and this system run under the default SystemRunMask::Always)
        // spawns real entities into the authoring world; without this tag File > Save Scene would
        // bake them in for good, and immortal, since LifetimeComponent has no serializer.
        world.AddComponent<SceneTransientTag>(inst, {});

        if (duration > 0.f) {
            LifetimeComponent lt; lt.remaining = duration; lt.total = duration;
            world.AddComponent<LifetimeComponent>(inst, lt);
        }

        switch (s.mode)
        {
        // `composed` is the attach-space offset: FollowSystem composes the follower's LocalTransform
        // against the socket / target world matrix.
        case VFXAttachMode::AttachToBone:
        {
            FollowSocketComponent fs;
            fs.target.BindAndStamp(world, spawner);
            fs.socketIndex = (s.boneIndex >= 0) ? static_cast<uint32_t>(s.boneIndex) : 0u;
            world.AddComponent<FollowSocketComponent>(inst, fs);
            world.AddComponent<LocalTransform>(inst, composed);
            break;
        }
        case VFXAttachMode::FollowEntity:
        {
            FollowEntityComponent fe;
            fe.target.BindAndStamp(world, spawner);
            world.AddComponent<FollowEntityComponent>(inst, fe);
            world.AddComponent<LocalTransform>(inst, composed);
            break;
        }
        case VFXAttachMode::Detached:
        default:
            world.AddComponent<LocalTransform>(inst, BakeFromMatrix(baseWorld, composed));
            break;
        }
        return inst;
    }

    // Spawn ONE emitter (the per-type dispatch). Shared by the immediate path and the
    // startDelay-deferred path so both behave the same.
    void SpawnOneEmitter(World& world, Entity spawner,
                         const PendingVFXSpawns::Spawn& s,
                         const LocalTransform& cmdOffset,
                         const Resource::VFXEmitterSpec& spec,
                         const DirectX::XMFLOAT4X4& baseWorld,
                         float dur)
    {
        // A non-positive lifetime adds NO LifetimeComponent, and nothing else ever destroys a VFX entity:
        // right for a deliberately immortal ambient emitter, a leak on a one-shot. Warn, do not clamp.
        if (dur <= 0.f)
            LOG_WARNING("VFX '%s': an emitter resolves to no lifetime (dur <= 0) — "
                        "the spawned entity will never be destroyed. Intended for a "
                        "permanent ambient emitter; otherwise set DUR/DURATION > 0.",
                        s.assetPath.c_str());

        switch (spec.type)
        {
        // ---- Lane A: component-driven, pure ECS ----
        case Resource::VFXEmitterType::Particle:
            SpawnEntityVFX(world, spawner, s, cmdOffset, spec, baseWorld, dur,
                [&](Entity inst){
                    world.AddComponent<ParticleRenderComponent>(inst, spec.particleRender);
                    world.AddComponent<ParticleEmitterComponent>(inst, spec.particle);
                    AddLookMaterial(world, inst, spec.materialPath);
                });
            break;

        case Resource::VFXEmitterType::Trail:
            SpawnEntityVFX(world, spawner, s, cmdOffset, spec, baseWorld, dur,
                [&](Entity inst){
                    world.AddComponent<TrailComponent>(inst, spec.trail);
                    AddLookMaterial(world, inst, spec.materialPath);
                });
            break;

        case Resource::VFXEmitterType::Beam:
            SpawnEntityVFX(world, spawner, s, cmdOffset, spec, baseWorld, dur,
                [&](Entity inst){
                    world.AddComponent<BeamComponent>(inst, spec.beam.beam);
                    MaterialComponent mc;
                    mc.useCustomShader  = true;
                    mc.customShaderPath = spec.beam.shaderPath;
                    mc.userBlendMode    = spec.beam.additive ? BlendMode::Additive : BlendMode::Opaque;
                    world.AddComponent<MaterialComponent>(inst, std::move(mc));
                });
            break;

        // ---- Lane B: Renderer-owned backends via mailbox ----
        case Resource::VFXEmitterType::Tracer:
        {
            using namespace DirectX;
            const LocalTransform composed = Compose(cmdOffset, spec.localTransform);
            const XMMATRIX worldM = composed.ToMatrix() * XMLoadFloat4x4(&baseWorld);
            XMFLOAT4X4 wf; XMStoreFloat4x4(&wf, worldM);

            PendingTracerSpawns::Req r;
            r.start = { wf._41, wf._42, wf._43 };
            XMVECTOR dirW = XMVector3Normalize(
                XMVector3TransformNormal(XMLoadFloat3(&spec.tracer.direction), worldM));
            XMStoreFloat3(&r.end, XMLoadFloat3(&r.start) + dirW * spec.tracer.length);
            r.color            = spec.tracer.color;
            r.width            = spec.tracer.width;
            r.lifetime         = spec.tracer.lifetime;
            r.noiseTexBindless = spec.tracer.noiseTexBindless;
            r.noiseTexPath     = spec.tracer.noiseTexPath;
            PushReq<PendingTracerSpawns>(world, spawner, std::move(r));
            break;
        }

        case Resource::VFXEmitterType::Decal:
        {
            using namespace DirectX;
            const LocalTransform composed = Compose(cmdOffset, spec.localTransform);
            const XMMATRIX worldM = composed.ToMatrix() * XMLoadFloat4x4(&baseWorld);
            XMFLOAT4X4 wf; XMStoreFloat4x4(&wf, worldM);

            PendingDecalSpawns::Req r;
            r.materialName    = spec.decal.materialName;
            r.position        = { wf._41, wf._42, wf._43 };
            XMStoreFloat3(&r.normal, XMVector3Normalize(XMVectorSet(wf._31, wf._32, wf._33, 0.f)));
            r.size            = { spec.decal.sizeX, spec.decal.sizeY };
            r.depth           = spec.decal.depth;
            r.lifetime        = (spec.durationOverride >= 0.f) ? spec.durationOverride : spec.decal.lifetime;
            r.fadeOutDuration = spec.decal.fadeOutDuration;
            r.rollZ           = spec.decal.rollZ;
            r.tintOverride    = spec.decal.tintOverride;
            PushReq<PendingDecalSpawns>(world, spawner, std::move(r));
            break;
        }

        case Resource::VFXEmitterType::Afterimage:
        {
            PendingAfterimageSpawns::Req r;
            r.target   = world.MakeHandle(spawner);
            r.lifetime = (spec.durationOverride >= 0.f) ? spec.durationOverride : spec.afterimage.lifetime;
            r.color    = spec.afterimage.color;
            PushReq<PendingAfterimageSpawns>(world, spawner, std::move(r));
            break;
        }

        // ---- Mesh: hybrid - Lane-A entity, Lane-B GPU mesh resolve ----
        case Resource::VFXEmitterType::Mesh:
        {
            const Entity inst = SpawnEntityVFX(world, spawner, s, cmdOffset, spec, baseWorld, dur,
                [&](Entity e){
                    world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
                    world.AddComponent<RenderLayer>(e, RenderLayer{});
                    world.AddComponent<BoundingVolume>(e, BoundingVolume{});
                    world.AddComponent<MaterialComponent>(e, MaterialComponent{});
                    if (!spec.mesh.materialPath.empty())
                        world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ spec.mesh.materialPath });
                    world.AddComponent<MeshSourcePath>(e, MeshSourcePath{ spec.mesh.meshPath });
                });
            PendingMeshVFXSpawns::Req r;
            r.entity       = world.MakeHandle(inst);
            r.meshPath     = spec.mesh.meshPath;
            r.sectionKey   = spec.mesh.sectionKey;
            r.materialPath = spec.mesh.materialPath;
            r.castShadow   = spec.mesh.castShadow;
            PushReq<PendingMeshVFXSpawns>(world, spawner, std::move(r));
            break;
        }

        default: break;
        }
    }

    // startDelay-deferred emitters, fired N seconds after the notify. File scope (not a member) to
    // avoid header coupling. The generation-checked handle drops an entry whose spawner died; the
    // world-epoch check in VFXSpawnSystem::Update drops the whole queue when the world changes.
    struct DeferredEmitter
    {
        EntityHandle              spawner;
        Resource::VFXEmitterSpec  spec;
        PendingVFXSpawns::Spawn   cmd;
        float                     remaining = 0.f;
        float                     dur       = 0.f;
    };
    std::vector<DeferredEmitter> s_deferredEmitters;
}

// ==== Editor hooks for live .ivfx iteration (declared in the header) ====
const char* kVFXPreviewAssetKey = "__vfx_preview__";

void InvalidateVFXPrefabCache(const std::string& path)
{
    s_vfxCache.erase(path);   // next spawn re-reads the file (a Save is picked up)
}

void SetVFXPreviewPrefab(const Resource::VFXPrefab& prefab)
{
    // Inject an in-memory (possibly unsaved) prefab under the preview key so the
    // editor can fire it through the normal spawn path without touching disk.
    s_vfxCache[kVFXPreviewAssetKey] = std::make_shared<Resource::VFXPrefab>(prefab);
}

void VFXSpawnSystem::Update(World& world, const FrameContext& ctx)
{
    // The pending emitters belong to the world they were queued in: drop the whole deferred
    // queue whenever the world epoch moves (a Clear, or the editor's Stop restoring the pre-Play
    // world, whose handles validate again - IsHandleValid alone would give a false positive).
    static uint32_t s_lastEpoch = 0;
    if (world.Epoch() != s_lastEpoch) { s_deferredEmitters.clear(); s_lastEpoch = world.Epoch(); }

    // ---- startDelay pass: fire emitters whose delay has elapsed ----
    // Re-resolves the attach pose from the spawner's current world (a delayed emitter lands where
    // the character is now). An entry whose spawner died mid-delay is dropped via the
    // generation-checked handle rather than spawning an orphan.
    if (!s_deferredEmitters.empty())
    {
        const float dt = ctx.deltaTime;
        for (auto& d : s_deferredEmitters)
        {
            d.remaining -= dt;
            if (d.remaining > 0.f) continue;
            if (!world.IsHandleValid(d.spawner)) continue;
            const Entity sp = d.spawner.entity;
            const DirectX::XMFLOAT4X4 bw = GetAttachBaseWorld(world, sp, d.cmd);
            LocalTransform co; co.translation = d.cmd.localPos;
            SpawnOneEmitter(world, sp, d.cmd, co, d.spec, bw, d.dur);
        }
        s_deferredEmitters.erase(
            std::remove_if(s_deferredEmitters.begin(), s_deferredEmitters.end(),
                [&](const DeferredEmitter& d){ return d.remaining <= 0.f || !world.IsHandleValid(d.spawner); }),
            s_deferredEmitters.end());
    }

    // Collect first, then spawn. ForEach holds the PendingVFXSpawns pool
    // view; AddComponent / CreateEntity inside the callback would corrupt
    // iteration when the new entity itself also gets a PendingVFXSpawns
    // somewhere down the line.
    struct Pending {
        Entity                       spawner;
        PendingVFXSpawns::Spawn      spawn;
    };
    std::vector<Pending> queue;

    world.ForEach<PendingVFXSpawns>([&](Entity e, PendingVFXSpawns& mb) {
        for (auto& s : mb.spawns) queue.push_back({ e, std::move(s) });
        mb.spawns.clear();
    });

    for (const auto& q : queue)
    {
        const Entity spawner = q.spawner;
        const auto&  s       = q.spawn;

        if (s.assetPath.empty()) continue;
        const auto* prefab = GetOrLoadVFXPrefab(s.assetPath);
        if (!prefab || prefab->emitters.empty()) continue;

        // Best-known attach base pose (socket world for bone-attach, else root world), shared by
        // every emitter of this spawn command.
        const DirectX::XMFLOAT4X4 baseWorld = GetAttachBaseWorld(world, spawner, s);

        // Spawn-cmd-level local offset (designer-authored at the notify
        // params level); composed over each prefab emitter's own offset.
        LocalTransform cmdOffset;
        cmdOffset.translation = s.localPos;

        for (const Resource::VFXEmitterSpec& spec : prefab->emitters)
        {
            // Per-emitter effective lifetime: explicit override > spawn-cmd
            // duration > prefab default. (<= 0 means "no LifetimeComponent".)
            const float dur =
                (spec.durationOverride >= 0.f) ? spec.durationOverride :
                (s.duration > 0.f)             ? s.duration :
                                                 prefab->defaultDuration;

            // Honour startDelay: a positive delay queues the emitter for a
            // later frame (fired by the deferred pass at the top of Update, which
            // re-resolves the attach pose so a delayed spark lands where the
            // blade is THEN). startDelay == 0 spawns now. The dur<=0 leak warning
            // lives in SpawnOneEmitter so both paths emit it.
            if (spec.startDelay > 0.f)
            {
                s_deferredEmitters.push_back(
                    { world.MakeHandle(spawner), spec, s, spec.startDelay, dur });
                continue;
            }
            SpawnOneEmitter(world, spawner, s, cmdOffset, spec, baseWorld, dur);
        }
    }
}

void CameraEffectSystem::Update(World& world, const FrameContext& ctx)
{
    // Layer one shake request onto the component. The notify amplitude is trauma (0..1) and
    // stacks additively, so several hits in one frame add up; the falloff makes the shake last
    // about `duration` seconds.
    auto applyShake = [](CameraShakeComponent& shake,
                         float trauma, float frequency, float falloffPerSec)
    {
        shake.trauma = std::min(1.f, shake.trauma + trauma);
        if (frequency > 0.f)
            shake.frequency = frequency;
        shake.falloffPerSec = falloffPerSec;
    };

    world.ForEach<PendingCameraEffects>([&](Entity e, PendingCameraEffects& mb) {
        for (const auto& ef : mb.effects)
        {
            switch (ef.type)
            {
            case CameraEffectType::Shake:
            {
                // The same trauma-based shake Lua Camera.AddShake drives. CameraShakeComponent lives on the
                // Main channel's LiveCamera entity; CameraShakeTickSystem (PreRender, after this
                // BoneAttachment phase) turns trauma^2 into additive position / rotation noise on the
                // resolved pose, in the same frame as the notify.
                const float trauma = std::clamp(ef.params.shakeAmplitude, 0.f, 1.f);
                const float freq   = ef.params.shakeFrequency;
                const float falloff = (ef.params.duration > 0.01f)
                    ? std::max(0.1f, trauma / ef.params.duration)
                    : 1.5f;

                // Fast path: the channel entity and its shake component exist -> in-place write, covered
                // by Write<CameraShakeComponent>.
                const Entity ch = Camera::FindChannelEntity(world, Camera::kMainChannel);
                CameraShakeComponent* shake = (ch != NullEntity)
                    ? world.GetComponent<CameraShakeComponent>(ch)
                    : nullptr;
                if (shake)
                {
                    applyShake(*shake, trauma, freq, falloff);
                }
                else if (ctx.commandBuffer)
                {
                    // First shake after a world (re)load: the channel entity or its shake component does not
                    // exist yet. Creating either is structural (CreateEntity / AddComponent) and illegal from a
                    // co-batched worker, so defer it to the phase-boundary flush - still this frame, before the
                    // PreRender CameraShakeTickSystem consumes the trauma.
                    ctx.commandBuffer->Defer(
                        [applyShake, trauma, freq, falloff](World& w)
                    {
                        const Entity ch2 =
                            Camera::GetOrCreateChannelEntity(w, Camera::kMainChannel);
                        if (!w.HasComponent<CameraShakeComponent>(ch2))
                            w.AddComponent<CameraShakeComponent>(ch2, CameraShakeComponent{});
                        if (auto* s = w.GetComponent<CameraShakeComponent>(ch2))
                            applyShake(*s, trauma, freq, falloff);
                    });
                }
            } break;

            case CameraEffectType::Zoom:
            case CameraEffectType::FOVPunch:
                // TODO: Zoom / FOVPunch need a persistent additive-FOV component plus a small decay system
                // applied after CameraResolveSystem, which overwrites LiveCamera.fov every frame (a one-shot
                // nudge here would be wiped).
                LOG_INFO("[Camera] FOV effect kind=%d fovDelta=%.2f dur=%.2f — not yet wired "
                         "(needs an additive-FOV layer like CameraShakeComponent)",
                         static_cast<int>(ef.type), ef.params.fovDelta, ef.params.duration);
                break;

            case CameraEffectType::HitStop:
            {
                // A timed request on the game clock, composed with Time.RequestScale (the slowest
                // wins) and expiring on real time. Deferred to the command-buffer flush: the
                // request list is the main thread's, and this system may run on a worker.
                const float scale = (std::max)(0.f, ef.params.timeScale);
                const float seconds = (std::max)(0.f, ef.params.duration);
                if (ctx.commandBuffer)
                    ctx.commandBuffer->Defer([scale, seconds](World&)
                    {
                        if (!GameplayTime::RequestTimedScale(scale, seconds))
                            LOG_WARNING("[Camera] HitStop scale=%.2f dur=%.2f dropped: no time-scale "
                                        "service is installed", scale, seconds);
                    });
            } break;
            }
        }
        mb.effects.clear();
    });
}

void AudioPlaySystem::Update(World& world, const FrameContext&)
{
    auto& bus = EventBus::Get();

    world.ForEach<PendingAudioPlays>([&](Entity e, PendingAudioPlays& mb) {
        for (const auto& p : mb.plays)
        {
            // Bridge to the audio path: publish the PlaySoundEvent AudioSystem subscribes to. No
            // VoiceHandle is kept: fire-and-forget is the right default for animation-driven SFX.
            Audio::PlaySoundEvent ev;
            ev.entity   = EntityHandle{ e, 0u };  // generation unused here
            ev.clipPath = p.clipPath;
            ev.volume   = p.volume;
            ev.pitch    = p.pitch;
            switch (p.category) {
                case AudioCategoryTag::SFX:   ev.bus = Audio::BusType::SFX;   break;
                case AudioCategoryTag::Voice: ev.bus = Audio::BusType::Voice; break;
                case AudioCategoryTag::Foley: ev.bus = Audio::BusType::SFX;   break;
                case AudioCategoryTag::Music: ev.bus = Audio::BusType::Music; break;
            }
            ev.is3D = (p.attachBone >= 0);
            bus.Publish<Audio::PlaySoundEvent>(std::move(ev));
        }
        mb.plays.clear();
    });
}

void StateToggleSystem::PrepareStructure(World& world)
{
    // Update adds these components; World::Clear drops the pools, so they are made here, every phase.
    world.EnsurePool<ActiveStateToggles>();
    world.EnsurePool<GameplayTagsComponent>();
}

void StateToggleSystem::Update(World& world, const FrameContext&)
{
    const bool inert = InertStateToggles();
    const GameplayTime::Clocks& clocks = GameplayTime::Current();

    // Point toggles reach the animated entity's holds. A tag switched on again replaces its earlier hold.
    world.ForEach<PendingStateToggles>([&](Entity e, PendingStateToggles& mb) {
        if (!mb.toggles.empty() && !inert)
        {
            auto* active = world.GetComponent<ActiveStateToggles>(e);
            if (!active)
            {
                world.AddComponent<ActiveStateToggles>(e, {});
                active = world.GetComponent<ActiveStateToggles>(e);
            }
            for (const auto& toggle : mb.toggles)
            {
                auto& held = active->held;
                held.erase(std::remove_if(held.begin(), held.end(),
                                          [&](const ActiveStateToggles::Held& h) { return h.tag == toggle.tag; }),
                           held.end());
                if (toggle.enable)
                    held.push_back({ toggle.tag, toggle.duration > 0.f, toggle.duration });
            }
        }
        mb.toggles.clear();
    });

    // The timed holds run down on the entity's own game clock (a pause or its dilation stops them).
    world.ForEach<ActiveStateToggles>([&](Entity e, ActiveStateToggles& active) {
        const float dt = GameplayTime::EntityDt(world.GetComponent<TimeDilationComponent>(e), clocks);
        auto& held = active.held;
        for (auto& h : held)
            if (h.timed) h.remaining -= dt;
        held.erase(std::remove_if(held.begin(), held.end(),
                                  [](const ActiveStateToggles::Held& h) { return h.timed && h.remaining <= 0.f; }),
                   held.end());
    });

    // What is held right now becomes the owner root's derived tags: rebuilt from nothing every frame,
    // so a tag whose source is gone is gone with it.
    world.ForEach<GameplayTagsComponent>([](Entity, GameplayTagsComponent& tags) { tags.derived.ClearAll(); });
    if (inert) return;
    world.ForEach<ActiveStateToggles>([&](Entity e, ActiveStateToggles& active) {
        if (active.open.empty() && active.held.empty()) return;
        const Entity root = ResolveOwnerRoot(world, e);
        auto* tags = world.GetComponent<GameplayTagsComponent>(root);
        if (!tags)
        {
            world.AddComponent<GameplayTagsComponent>(root, {});
            tags = world.GetComponent<GameplayTagsComponent>(root);
        }
        for (Gameplay::Tag tag : active.open) tags->derived.Add(tag);
        for (const auto& h : active.held) tags->derived.Add(h.tag);
    });
}

// ==== AnimFXGateSystem - arm / disarm resident weapon FX from the animation ====
namespace
{
    // Depth caps on the attach walk. Real chains are 2-3 links
    // (character -> weapon root -> trail); these only stop a corrupt graph
    // from spinning.
    constexpr int    kMaxDriverHops   = 16;
    constexpr size_t kMaxGroupMembers = 256;

    // Walk from an effect entity up to the entity whose animation drives it. The hop order
    // mirrors Follow::ResolveAttachParentWorld (socket, then follow target, then real parent)
    // because that is the chain the transform travels down.
    Entity ResolveAnimDriver(World& world, Entity e)
    {
        Entity cur = e;
        for (int hop = 0; hop < kMaxDriverHops; ++hop)
        {
            if (world.HasComponent<AnimationComponent>(cur))
                return cur;

            Entity next = NullEntity;
            if (auto* fs = world.GetComponent<FollowSocketComponent>(cur))
                next = fs->target.Resolve(world);
            if (next == NullEntity)
            {
                if (auto* fe = world.GetComponent<FollowEntityComponent>(cur))
                    next = fe->target.Resolve(world);
            }
            if (next == NullEntity)
            {
                if (const Parent* p = world.GetComponent<Parent>(cur))
                    next = p->entity;
            }
            if (next == NullEntity || next == cur)
                return NullEntity;
            cur = next;
        }
        return NullEntity;
    }

    void ApplyGateToEntity(World& world, Entity e, bool on, bool restart)
    {
        if (auto* tc = world.GetComponent<TrailComponent>(e))
        {
            if (tc->enabled != on || (on && restart))
            {
                tc->enabled = on;
                // Re-arming would splice the new arc onto whatever is still inside maxAge from the previous
                // swing - a straight bar across the world between the two poses. Force a fresh ribbon.
                if (on) tc->restartRequested = true;
            }
        }
        if (auto* ec = world.GetComponent<ParticleEmitterComponent>(e))
        {
            // ParticleSystem already clears hasPrevTip while disabled, so the
            // Trail-shape swing chord re-seeds by itself on the way back up.
            ec->enabled = on;
            if (on && restart) ec->hasPrevTip = false;
        }
    }

    // The effect group: the gate entity, its descendants, and everything FollowEntity-bound to
    // any member, transitively (at most kMaxGroupMembers; the root alone when includeFollowers is
    // false). The follower edge is load-bearing: a VFX child inside a .ipfb is a follower with
    // parent = -1, so a Children-only walk would miss the trail.
    void ApplyGateToGroup(World& world, Entity root, bool on, bool restart, bool includeFollowers,
                          const std::unordered_map<Entity, std::vector<Entity>>& followersOf)
    {
        std::vector<Entity> queue{ root };
        for (size_t head = 0; head < queue.size() && queue.size() < kMaxGroupMembers; ++head)
        {
            const Entity cur = queue[head];
            ApplyGateToEntity(world, cur, on, restart);
            if (!includeFollowers) break;

            if (const Children* ch = world.GetComponent<Children>(cur))
                for (const Entity c : ch->entities)
                    if (world.IsAlive(c) &&
                        std::find(queue.begin(), queue.end(), c) == queue.end())
                        queue.push_back(c);

            if (auto it = followersOf.find(cur); it != followersOf.end())
                for (const Entity f : it->second)
                    if (world.IsAlive(f) &&
                        std::find(queue.begin(), queue.end(), f) == queue.end())
                        queue.push_back(f);
        }
    }
}

void AnimFXGateSystem::Update(World& world, const FrameContext&)
{
    auto* gatePool = world.GetPool<AnimFXGateComponent>();
    if (!gatePool || gatePool->Entities().empty())
        return;

    // Reverse index target -> followers, built once per frame. FollowEntity
    // only stores the forward edge, and every gate would otherwise rescan the
    // whole pool per member.
    std::unordered_map<Entity, std::vector<Entity>> followersOf;
    if (auto* fePool = world.GetPool<FollowEntityComponent>())
    {
        const auto& ents = fePool->Entities();
        auto&       data = fePool->Data();
        for (size_t i = 0; i < ents.size(); ++i)
        {
            const Entity t = data[i].target.Resolve(world);
            if (t != NullEntity) followersOf[t].push_back(ents[i]);
        }
    }

    // Snapshot the roster: ApplyGateToGroup touches only the Trail / Particle pools, but
    // ResolveAnimDriver resolves AttachmentRefs; do not hold a pool reference across that, it
    // can dangle.
    const std::vector<Entity> gateEnts = gatePool->Entities();

    for (const Entity e : gateEnts)
    {
        AnimFXGateComponent* gate = world.GetComponent<AnimFXGateComponent>(e);
        if (!gate) continue;

        bool on = gate->forceOn;
        bool restart = false;
        if (!on)
        {
            const Entity driver = ResolveAnimDriver(world, e);
            // No driver (detached / owner destroyed) or no open gate -> off.
            if (driver != NullEntity)
            {
                if (const auto* anim = world.GetComponent<AnimationComponent>(driver))
                    restart = gate->ObservePlayback(driver, anim->primaryClip, anim->primaryTime);
                if (const auto* gates = world.GetComponent<ActiveFXGates>(driver))
                    on = gates->IsOpen(AnimFX::HashGroup(gate->group));
            }
            else
                gate->hasPlayback = false;
        }
        else
            gate->hasPlayback = false;

        // Apply on a gate edge or a new playback pass. Full-clip intervals
        // stay open across wrap, but their one-shot shader clocks must reset.
        // Steady playback must not restart the ribbon every frame.
        if (gate->hasApplied && gate->lastApplied == on && !(on && restart))
            continue;

        ApplyGateToGroup(world, e, on, restart, gate->includeFollowers, followersOf);

        // Re-fetch: ApplyGateToGroup adds no component, but the pointer above predates a possible
        // Resolve-driven pool touch.
        if (AnimFXGateComponent* g2 = world.GetComponent<AnimFXGateComponent>(e))
        {
            g2->lastApplied = on;
            g2->hasApplied  = true;
        }
    }
}

void AnimFXGateSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Reads the level output + the attach graph, writes the two emitter
    // enables and its own latch. Declared Write on TrailComponent /
    // ParticleEmitterComponent so no phase-mate can read them mid-flip.
    b.Read<ActiveFXGates>()
     .Read<AnimationComponent>()
     // Write, not Read: AttachmentRef::Resolve refreshes its entity cache in
     // place, so the walk mutates these components even though it only asks
     // them questions.
     .Write<FollowSocketComponent>()
     .Write<FollowEntityComponent>()
     .Read<Parent>()
     .Read<Children>()
     .Write<AnimFXGateComponent>()
     .Write<TrailComponent>()
     .Write<ParticleEmitterComponent>();
}

// CustomNotifyEvent lives in ECS/NotifyTypes.h so the script layer can
// subscribe (ScriptSystem::RegisterCppEventBridges routes it to Lua's
// OnAnimEvent + the "AnimNotify" LuaBus channel).
void GenericNotifyDispatcher::Update(World& world, const FrameContext&)
{
    auto& bus = EventBus::Get();
    world.ForEach<PendingGenericNotifies>([&](Entity e, PendingGenericNotifies& mb) {
        // Move the mailbox out and clear it before publishing. `mb` references the dense pool array
        // and World::ForEach caches the entity / data pointers and the count outside its loop, so a
        // subscriber that grows the pool would dangle both `mb` and the iteration. This does not rely
        // on EventBus::Publish only enqueueing.
        auto notifies = std::move(mb.notifies);
        mb.notifies.clear();
        for (auto& n : notifies) {
            CustomNotifyEvent ev;
            ev.entity = e;
            ev.typeId = std::move(n.typeId);
            ev.params = std::move(n.params);
            // Keep one event/bus broadcast. The script bridge queues the
            // original animated recipient and its distinct character owner.
            if (ev.params.GetInt("_animGraph", 0))
                ev.params.SetInt("_animRoot", static_cast<int>(ResolveOwnerRoot(world, e)));
            bus.Publish<CustomNotifyEvent>(std::move(ev));
        }
    });
}

// ==== DeclareAccess - lets the Scheduler co-batch the mailbox drainers ====
// Each system owns exactly one Pending* mailbox type (drained and cleared in place; ForEach
// never creates pools). The EventBus publishers share ExclusiveRes::kEventBus because EventBus
// is main-thread-only: Publish push_backs per channel and can insert into the bus-wide channel
// map. VFXSpawnSystem has no declaration on purpose: spawning (CreateEntity, AddComponent,
// prefab file I/O, a static cache) is structural and must stay opaque and serialized.

void HitboxSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    b.Write<Attack::Playback>();
    b.Write<PendingHitboxCommands>();
    // Read-only placement inputs. SocketSystem wrote these in
    // TickPhase::Animation, two phases earlier, so they are this frame's.
    b.Read<SocketComponent>();
    b.Read<GlobalTransform>();
    b.Read<Parent>();
    b.Read<ColliderComponent>();
    b.Read<FollowSocketComponent>();
    b.Read<FollowEntityComponent>();
    b.Read<TagComponent>();
    b.Read<GameplayTagsComponent>();   // the victim's tags: the filter and the immunity
    b.Read<TimeDilationComponent>();   // a frozen attacker sweeps nothing
    // Hits go out on the bus, which is main-thread-only (as for GenericNotifyDispatcher and
    // AudioPlaySystem). This pulls HitboxSystem out of the parallel drainer batch: correctness
    // over batching. Slot state lives on the system, so nothing structural happens here.
    b.ExclusiveResource(ExclusiveRes::kEventBus);
    // The weapon sweep and capsule overlap are Jolt queries (SweepColliderAll, OverlapCapsuleAll)
    // -> kPhysicsWorld by engine convention.
    b.ExclusiveResource(ExclusiveRes::kPhysicsWorld);
}

void CameraEffectSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    b.Write<PendingCameraEffects>();
    b.Read<CameraStackChannelSingleton>();   // FindChannelEntity scan
    b.Write<CameraShakeComponent>();         // in-place trauma fast path
}

void AudioPlaySystem::DeclareAccess(SystemAccessBuilder& b) const
{
    b.Write<PendingAudioPlays>();
    b.ExclusiveResource(ExclusiveRes::kEventBus);
}

void StateToggleSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    b.Write<PendingStateToggles>();
    b.Write<ActiveStateToggles>();     // the holds, and the open intervals it reads
    b.Write<GameplayTagsComponent>();  // the owner root's derived tags
    b.Read<TimeDilationComponent>();   // a held tag runs down on the entity's own clock
    b.Read<Parent>();                  // the owner root
}

void GenericNotifyDispatcher::DeclareAccess(SystemAccessBuilder& b) const
{
    b.Read<Parent>();
    b.Write<PendingGenericNotifies>();
    b.ExclusiveResource(ExclusiveRes::kEventBus);
}
