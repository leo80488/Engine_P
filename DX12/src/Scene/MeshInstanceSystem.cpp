#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Scene/MeshInstanceSystem.h"

#include "ECS/AnimationComponents.h"      // SocketComponent (the Mesh's derived sockets)
#include "ECS/Components.h"               // MaterialComponent
#include "ECS/FrameContext.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/PhysicsComponents.h"        // the derived asset collision
#include "ECS/StaticMeshComponents.h"
#include "ECS/TickPhase.h"
#include "Renderer/Renderer.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/MeshLibIdentity.h"
#include "Resource/ModelCollision.h"      // MeshBodySetup / ModelCollisionAt
#include "Resource/ModelSockets.h"        // the model's SO mesh= records
#include "Scene/MeshAssetRegistry.h"
#include "Scene/SceneInstanceLoader.h"    // DeriveMeshSockets
#include "System/CommandLine.h"
#include "System/Log.h"

#include <algorithm>
#include <string>
#include <vector>

// Derive and DestroySections create and destroy entities directly. That is
// only safe in a serial phase; flipping the flag must be a build break, not an
// intermittent heap corruption (the same guard SequencerTickSystem carries).
static_assert(GetPhaseDescriptor(TickPhase::GameplayPostLogic).allowsParallel == false,
              "MeshInstanceSystem performs direct structural ECS changes; "
              "TickPhase::GameplayPostLogic must remain serial.");

namespace
{
    // The owner's authored fields plus the revision of the model file (the Mesh Editor saves it in
    // place): a section derived from the old file has the old value and is rebuilt. Revision 0
    // (every model not re-written this session) gives the plain Signature(), bit for bit.
    uint64_t EffectiveSignature(const StaticMeshComponent& sm, const MeshAssetRegistry* reg)
    {
        const uint64_t sig = sm.Signature();
        const uint32_t rev = reg ? reg->ModelRevision(sm.model) : 0u;
        if (rev == 0) return sig;
        const uint64_t mixed = (sig ^ (uint64_t(rev) * 0x9E3779B97F4A7C15ull)) * 1099511628211ull;
        return mixed ? mixed : 1ull;
    }

    bool IsSectionOf(const MeshSectionOf* s, Entity owner, uint32_t ownerGen)
    {
        return s && s->owner == owner && s->ownerGeneration == ownerGen;
    }

    // @p owner's derived sections, in its Children order.
    void CollectSections(World& w, Entity owner, std::vector<Entity>& out)
    {
        out.clear();
        const Children* ch   = w.GetComponent<Children>(owner);
        auto*           pool = w.GetPool<MeshSectionOf>();
        if (!ch || !pool) return;
        const uint32_t gen = w.GetGeneration(owner);
        for (Entity c : ch->entities)
            if (IsSectionOf(pool->Get(c), owner, gen)) out.push_back(c);
    }

    void Unlink(World& w, Entity e)
    {
        if (const Parent* p = w.GetComponent<Parent>(e))
            if (Children* ch = w.GetComponent<Children>(p->entity))
                ch->entities.erase(std::remove(ch->entities.begin(), ch->entities.end(), e),
                                   ch->entities.end());
    }

    // Destroy one section. Anything parented under it by hand (the Outliner can
    // show sections) moves up to @p heir instead of being orphaned.
    void DestroySection(World& w, Entity s, Entity heir)
    {
        if (Children* sc = w.GetComponent<Children>(s))
        {
            const std::vector<Entity> kids = sc->entities;
            for (Entity k : kids)
            {
                if (!w.IsAlive(k)) continue;
                if (Parent* kp = w.GetComponent<Parent>(k)) kp->entity = heir;
                if (heir != NullEntity && w.IsAlive(heir))
                {
                    if (!w.GetComponent<Children>(heir)) w.AddComponent<Children>(heir, Children{});
                    w.GetComponent<Children>(heir)->entities.push_back(k);
                }
            }
        }
        Unlink(w, s);
        w.DestroyEntity(s);
    }

    std::string SlotLabel(const MeshAssetRegistry::Section& s, uint32_t k)
    {
        return s.slot.empty() ? "section " + std::to_string(k) : s.slot;
    }

    // Drops the mesh sockets an earlier Derive gave the owner (its own sockets and a model's
    // skeleton sockets stay; so does the component, which is not saved while it holds only
    // derived ones).
    void DropDerivedSockets(World& w, Entity owner)
    {
        if (SocketComponent* sc = w.GetComponent<SocketComponent>(owner))
            sc->ReplaceDerived(nullptr, 0, SocketComponent::kMeshSocket);
    }

    // Drops the Collider + RigidBody an earlier Derive gave the owner.
    void DropDerivedCollision(World& w, Entity owner)
    {
        if (!w.HasComponent<DerivedColliderTag>(owner)) return;
        w.RemoveComponent<ColliderComponent>(owner);
        w.RemoveComponent<RigidBodyComponent>(owner);
        w.RemoveComponent<DerivedColliderTag>(owner);
    }
}

MeshInstanceSystem::MeshInstanceSystem(Renderer* renderer)
    : m_renderer(renderer)
{
    m_disabled = Sys::CommandLine::Get().Has("-staticmesh_no_derive");
    if (m_disabled)
        LOG_WARNING("[STATICMESH] -staticmesh_no_derive: StaticMesh sections are not derived - "
                    "every StaticMesh owner draws nothing");
}

void MeshInstanceSystem::DestroySections(World& world, Entity owner)
{
    std::vector<Entity> sections;
    CollectSections(world, owner, sections);
    for (Entity s : sections)
        if (world.IsAlive(s)) DestroySection(world, s, owner);
}

unsigned MeshInstanceSystem::Derive(World& world, Entity owner, Renderer& renderer)
{
    DestroySections(world, owner);
    DropDerivedCollision(world, owner);
    DropDerivedSockets(world, owner);
    StaticMeshComponent* sm = world.GetComponent<StaticMeshComponent>(owner);
    if (!sm) return 0;

    const uint64_t sig   = EffectiveSignature(*sm, renderer.GetMeshAssets());
    const uint32_t epoch = world.Epoch();
    auto fail = [&](const std::string& why, bool quiet) -> unsigned
    {
        // Said once per authored value, not once per epoch.
        if (!quiet && (sm->failedSignature != sig || sm->status != why))
            LOG_WARNING("[STATICMESH] '%s' (entity %u) draws nothing: %s",
                        world.GetName(owner).c_str(), owner, why.c_str());
        sm->status          = why;
        sm->failedSignature = sig;
        sm->failedEpoch     = epoch;
        return 0u;
    };

    // A component just added in the Inspector names nothing yet: not a fault.
    if (sm->model.empty()) return fail("no model assigned", true);
    MeshAssetRegistry* reg = renderer.GetMeshAssets();
    if (!reg) return fail("no mesh registry (no mesh library)", false);

    const MeshAssetRegistry::Model& model = reg->GetModel(sm->model, epoch);
    bool byName = false;
    const MeshAssetRegistry::Mesh* mesh = MeshAssetRegistry::Find(model, sm->meshKey, sm->meshName, &byName);
    if (!mesh)
    {
        if (!model.error.empty()) return fail(model.error, false);
        if (sm->meshKey == 0 && sm->meshName.empty()) return fail("no mesh chosen", true);
        return fail("the model '" + sm->model + "' has no mesh " + Resource::MeshKeyToHex(sm->meshKey) +
                    (sm->meshName.empty() ? std::string() : " ('" + sm->meshName + "')"), false);
    }

    // What every section starts from: the owner's placement (TransformPropagate
    // recomposes it this frame - this phase runs before it - but a section read
    // before that sees the right pose, not the origin) and its visibility.
    const GlobalTransform* ogt  = world.GetComponent<GlobalTransform>(owner);
    const GlobalTransform  seed = ogt ? *ogt : GlobalTransform{};
    VisibilityComponent vis{};
    if (const VisibilityComponent* ov = world.GetComponent<VisibilityComponent>(owner))
    {
        vis.viewMask    = ov->viewMask;
        vis.flags       = ov->flags;
        vis.renderLayer = ov->renderLayer;
    }
    if (!world.GetComponent<Children>(owner)) world.AddComponent<Children>(owner, Children{});
    // An owner on an editor panel's preview stage (a Skeleton Editor attachment, a chain
    // preview's prop): its sections belong to the stage too, so like the owner they stay out of
    // the Outliner, viewport picking and Copy.
    const PreviewStageTag* ownerStage = world.GetComponent<PreviewStageTag>(owner);
    const bool             onStage    = ownerStage != nullptr;
    const PreviewStageTag  stageTag   = onStage ? *ownerStage : PreviewStageTag{};

    const uint32_t ownerGen = world.GetGeneration(owner);
    const uint32_t count    = static_cast<uint32_t>(mesh->sections.size());
    std::vector<Entity> made;
    made.reserve(count);
    for (uint32_t k = 0; k < count; ++k)
    {
        const MeshAssetRegistry::Section& sec = mesh->sections[k];
        const Entity s = world.CreateEntity();
        world.SetName(s, mesh->name + " [" + SlotLabel(sec, k) + "]");

        // The component set SceneInstanceLoader gives a mesh part, so every
        // renderer path draws this exactly like one.
        world.AddComponent<LocalTransform>(s, LocalTransform{});
        world.AddComponent<GlobalTransform>(s, seed);
        MeshLibRef ref;
        ref.libHandle = mesh->library;
        ref.meshId    = sec.entry;
        world.AddComponent<MeshLibRef>(s, ref);
        LocalAabb la;
        la.min = sec.aabbMin;
        la.max = sec.aabbMax;
        world.AddComponent<LocalAabb>(s, la);
        WorldAabb wa;
        wa.min = sec.aabbMin;
        wa.max = sec.aabbMax;
        world.AddComponent<WorldAabb>(s, wa);
        world.AddComponent<VisibilityComponent>(s, vis);
        world.AddComponent<RenderLayer>(s, RenderLayer{});
        world.AddComponent<Parent>(s, Parent{ owner });
        world.AddComponent<MeshSourcePath>(s, MeshSourcePath{ mesh->libraryPath });

        // Material: the owner's override for this slot, else the model's default.
        const StaticMeshComponent::SlotOverride* ovr = sm->FindOverride(sec.key);
        const std::string matPath = ovr ? ovr->material : sec.defaultMaterial;
        if (!matPath.empty())
        {
            MaterialComponent          mc{};
            Resource::MaterialLoadInfo info;
            if (!Resource::LoadMaterial(matPath, mc, &info))
                LOG_WARNING("[STATICMESH] '%s' slot '%s': cannot load material '%s' - drawn with defaults",
                            world.GetName(owner).c_str(), SlotLabel(sec, k).c_str(), matPath.c_str());
            world.AddComponent<MaterialComponent>(s, mc);
            world.AddComponent<MaterialSourcePath>(s, MaterialSourcePath{ matPath });
            MaterialSpawnDefault def;
            def.path = matPath;
            world.AddComponent<MaterialSpawnDefault>(s, std::move(def));
            ApplyMaterialLoadInfo(world, s, info);
        }
        else
            world.AddComponent<MaterialComponent>(s, MaterialComponent{});

        MeshSectionOf so;
        so.owner           = owner;
        so.ownerGeneration = ownerGen;
        so.index           = k;
        so.count           = count;
        so.sectionKey      = sec.key;
        so.ownerSignature  = sig;
        world.AddComponent<MeshSectionOf>(s, so);
        world.AddComponent<SceneTransientTag>(s, SceneTransientTag{});
        if (onStage) world.AddComponent<PreviewStageTag>(s, stageTag);
        made.push_back(s);
    }
    Children* ch = world.GetComponent<Children>(owner);
    ch->entities.insert(ch->entities.end(), made.begin(), made.end());

    // The Mesh's asset collision (Resource/ModelCollision.h): the body prims become one MeshAsset
    // collider on the owner (the placed object, one body) unless the user gave the owner its own
    // Collider or RigidBody, which win; each own-library section trimesh goes on the section that
    // draws it. The owner's are tagged derived (never serialized); sections are transient anyway.
    if (sm->collision == StaticMeshComponent::Collision::Asset)
        if (const Resource::MeshBodySetup* setup = Resource::ModelCollisionAt(sm->model)->Find(mesh->key))
            if (setup->complexity != Resource::MeshBodySetup::Complexity::None)
            {
                ColliderComponent base{};
                base.layer     = static_cast<ColliderComponent::CollisionLayer>(setup->layer);
                base.isTrigger = setup->trigger;
                RigidBodyComponent rb{};
                rb.motion = RigidBodyComponent::Motion::Static;
                if (setup->friction >= 0.f) rb.friction = setup->friction;
                if (setup->restitution >= 0.f) rb.restitution = setup->restitution;
                if (setup->HasBody() && !world.HasComponent<ColliderComponent>(owner) &&
                    !world.HasComponent<RigidBodyComponent>(owner))
                {
                    ColliderComponent col = base;
                    col.shape        = ColliderComponent::Shape::MeshAsset;
                    col.assetModel   = sm->model;
                    col.assetMeshKey = mesh->key;
                    world.AddComponent<ColliderComponent>(owner, col);
                    world.AddComponent<RigidBodyComponent>(owner, rb);
                    world.AddComponent<DerivedColliderTag>(owner, DerivedColliderTag{});
                }
                std::vector<uint64_t> keys;
                for (const MeshAssetRegistry::Section& sec : mesh->sections) keys.push_back(sec.key);
                const std::vector<uint64_t> secs = setup->SectionTrimeshes(keys);
                for (uint32_t k = 0; k < count && k < made.size(); ++k)
                {
                    const MeshAssetRegistry::Section& sec = mesh->sections[k];
                    if (std::find(secs.begin(), secs.end(), sec.key) == secs.end()) continue;
                    ColliderComponent col = base;
                    col.shape         = ColliderComponent::Shape::Mesh;
                    col.meshLibPath   = mesh->libraryPath;
                    col.meshLibMeshId = sec.entry;
                    world.AddComponent<ColliderComponent>(made[k], col);
                    world.AddComponent<RigidBodyComponent>(made[k], rb);
                }
            }

    // The Mesh's sockets (`SO mesh=<key>`), derived onto the owner (SocketSystem: offset x its
    // world); the model file's revision is in the signature, so a Mesh Editor save derives them
    // again.
    SceneInstanceLoader::DeriveMeshSockets(world, owner, *Resource::ModelSocketsAt(sm->model), mesh->key, sm->model);
    sm = world.GetComponent<StaticMeshComponent>(owner);   // a SocketComponent may have been added
    if (!sm) return count;

    // `sm` is still valid: nothing above touched the StaticMeshComponent pool.
    sm->failedSignature = 0;
    sm->failedEpoch     = 0;
    if (byName)
    {
        sm->status = "resolved by name: the model has no key " + Resource::MeshKeyToHex(sm->meshKey) + " any more";
        LOG_WARNING("[STATICMESH] '%s' (entity %u): mesh key %s is gone from '%s'; resolved by the name '%s'",
                    world.GetName(owner).c_str(), owner, Resource::MeshKeyToHex(sm->meshKey).c_str(),
                    sm->model.c_str(), sm->meshName.c_str());
    }
    else
        sm->status.clear();
    return count;
}

void MeshInstanceSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    if (!m_renderer || m_disabled) return;

    // 1. Sections whose owner is gone: deleted, its component removed, or its id reused by another
    // entity. An undo that removes an owner recorded before its sections existed leaves exactly
    // these behind.
    std::vector<Entity> orphans;
    std::vector<Entity> lostOwners;   // alive, their StaticMeshComponent removed
    if (auto* pool = world.GetPool<MeshSectionOf>())
    {
        auto*       owners = world.GetPool<StaticMeshComponent>();
        const auto& ents   = pool->Entities();
        const auto& data   = pool->Data();
        for (size_t i = 0; i < data.size(); ++i)
        {
            const Entity o     = data[i].owner;
            const bool   alive = world.IsAlive(o) && world.GetGeneration(o) == data[i].ownerGeneration;
            const bool   ok    = alive && owners && owners->Has(o);
            if (ok) continue;
            orphans.push_back(ents[i]);
            if (alive && std::find(lostOwners.begin(), lostOwners.end(), o) == lostOwners.end()) lostOwners.push_back(o);
        }
    }
    for (Entity e : orphans)
        if (world.IsAlive(e)) DestroySection(world, e, NullEntity);
    // The Mesh's sockets go with its sections: the owner no longer places that Mesh, so nothing
    // may keep following them.
    for (Entity o : lostOwners) DropDerivedSockets(world, o);

    // 2. Owners whose sections are not what their fields give.
    auto* owners = world.GetPool<StaticMeshComponent>();
    if (!owners || owners->Data().empty())
    {
        if (!orphans.empty())
            LOG_INFO("[STATICMESH] removed %zu orphaned section(s)", orphans.size());
        return;
    }
    const uint32_t      epoch   = world.Epoch();
    auto*               secPool = world.GetPool<MeshSectionOf>();
    auto*               visPool = world.GetPool<VisibilityComponent>();
    std::vector<Entity> stale;
    std::vector<Entity> sections;
    {
        const auto& ents = owners->Entities();
        const auto& data = owners->Data();
        for (size_t i = 0; i < data.size(); ++i)
        {
            const Entity               owner = ents[i];
            const StaticMeshComponent& sm    = data[i];
            const uint64_t             sig   = EffectiveSignature(sm, m_renderer->GetMeshAssets());
            CollectSections(world, owner, sections);
            if (sections.empty())
            {
                if (sm.failedSignature != sig || sm.failedEpoch != epoch) stale.push_back(owner);
                continue;
            }
            bool current = true;
            for (Entity s : sections)
            {
                const MeshSectionOf* so = secPool->Get(s);
                if (so->ownerSignature != sig || so->count != sections.size()) { current = false; break; }
            }
            if (!current) { stale.push_back(owner); continue; }

            // Visibility follows the owner (inheritedHidden is TransformPropagate's).
            if (const VisibilityComponent* ov = visPool ? visPool->Get(owner) : nullptr)
                for (Entity s : sections)
                    if (VisibilityComponent* sv = visPool->Get(s))
                        if (sv->viewMask != ov->viewMask || sv->flags != ov->flags ||
                            sv->renderLayer != ov->renderLayer)
                        {
                            sv->viewMask    = ov->viewMask;
                            sv->flags       = ov->flags;
                            sv->renderLayer = ov->renderLayer;
                        }
        }
    }

    unsigned derived = 0, sectionsMade = 0, failed = 0;
    for (Entity owner : stale)
    {
        if (!world.IsAlive(owner)) continue;
        const unsigned n = Derive(world, owner, *m_renderer);
        ++derived;
        sectionsMade += n;
        if (n == 0) ++failed;
    }
    if (derived || !orphans.empty())
        LOG_INFO("[STATICMESH] derived %u owner(s) -> %u section(s), %u drawing nothing; removed %zu "
                 "orphaned section(s)", derived, sectionsMade, failed, orphans.size());
}
