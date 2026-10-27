// PreviewStage.cpp - the hidden preview stage's shared mechanics, PreviewStageHost
// and StageClipPlayer; the contracts are in the header.

#include "Editor/PreviewStage.h"

#include "ECS/AnimationComponents.h"        // SkeletonComponent / MeshSkinnedComponent / Animation*
#include "Resource/AnimationClipSystem.h"   // StageClipPlayer
#include "Resource/AnimationResource.h"
#include "Resource/SkeletonInspect.h"       // Resource::FitClip (the bound-bone count)
#include "ECS/CameraStackComponents.h"      // VirtualCameraComponent
#include "ECS/HierarchyComponents.h"        // tags, transforms, Visibility, MeshLibRef
#include "ECS/HierarchyOps.h"               // Hierarchy::DestroySubtree (World::DestroyEntity is FLAT)
// ---- the STRIP list's component types ----
#include "AI/AIComponents.h"                // AIComponent
#include "Audio/AudioComponents.h"          // Audio::AudioSourceComponent
#include "ECS/CharacterControllerComponent.h"
#include "ECS/CharacterIntentComponent.h"   // BrainComponent
#include "ECS/HairShadowCasterComponent.h"
#include "ECS/AnimGraphComponent.h"
#include "ECS/LocomotionComponents.h"       // LocomotionComponent
#include "ECS/PerceptionComponent.h"        // PerceptionComponent / PerceptibleComponent
#include "ECS/PerObjectShadowComponent.h"
#include "ECS/PhysicsComponents.h"          // ColliderComponent / RigidBodyComponent
#include "ECS/PlayerComponent.h"
#include "Nav/NavComponents.h"              // NavAgentComponent
#include "Scripting/ScriptComponent.h"
#include "ECS/CharacterAnimDriver.h"        // CharacterAnimDriverComponent
#include "ECS/HeadTrackComponent.h"
#include "ECS/ParticleComponent.h"          // ParticleEmitterComponent / ParticleRenderComponent
#include "ECS/TrailComponent.h"
#include "Physics/ChainPhysicsSystem.h"     // CapsuleColliderComponent (KEPT)
// ----
#include "ECS/Components.h"                 // MaterialComponent (RequestPreview's payload)
#include "Graphics/SkinningBuffers.h"       // PoseRingBuffer / SkinnedVertexRing caps
#include "RenderGraph/RenderPass/SkinningPass.h"   // SkinningPass::kMaxSkinJobs
#include "Renderer/Preview/PreviewStudioRig.h"     // Preview::kStudioCamera
#include "Renderer/Preview/PreviewTypes.h"         // Preview::ViewDesc
#include "Renderer/Renderer.h"
#include "Resource/PrefabSerializer.h"      // LoadPrefab
#include "Scene/SceneInstanceLoader.h"      // a model .iscn subject
#include "System/Log.h"
#include "System/PathUtil.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Editor
{
namespace
{
    // Remove T from every stage entity that carries it. POOL-driven (the
    // Entities() list intersected with the stage set), never a per-entity Get
    // loop over the stage, and the removal happens after the scan because
    // RemoveComponent swap-erases the pool being iterated.
    template <class T>
    std::uint32_t StripPool(World& w, const std::unordered_set<Entity>& stage)
    {
        auto* p = w.GetPool<T>();
        if (!p) return 0;
        std::vector<Entity> hit;
        for (Entity e : p->Entities())
            if (stage.count(e)) hit.push_back(e);
        for (Entity e : hit) w.RemoveComponent<T>(e);
        return static_cast<std::uint32_t>(hit.size());
    }
}

std::uint32_t StageTagCount(World& w, std::uint8_t owner)
{
    auto* p = w.GetPool<PreviewStageTag>();
    if (!p) return 0;
    std::uint32_t n = 0;
    for (const PreviewStageTag& t : p->Data()) n += t.owner == owner ? 1u : 0u;
    return n;
}

StageSpawn LoadStageSubject(const std::string& path, World& world, Resource::AssetManager& assetMgr,
                            Renderer* renderer, Resource::AnimationClipSystem* animClipSys)
{
    StageSpawn out;
    // A DIFF of the alive set rather than the root's Children: SavePrefab pulls
    // FollowSocket / FollowEntity followers (an equipped weapon, its trail) in
    // as TOP-LEVEL nodes, and a Children walk would leave them untagged,
    // visible in the main view and alive after the stage is torn down.
    std::vector<std::uint8_t> before(world.EntityIdBound(), 0);
    for (Entity e : world.GetEntities())
        if (static_cast<std::size_t>(e) < before.size() && world.IsAlive(e)) before[e] = 1;

    // Not wrapped in RecordSpawn: the stage is not user content and must never come
    // back through Ctrl+Z.
    // A stage (and each preview attachment) is spawned again on every tab switch, host
    // switch and list edit: reuse the skeleton an earlier stage spawn registered rather
    // than add another ~650 KB registry entry.
    SceneInstanceLoader::RegistrationReuseScope reuse;
    Entity root = NullEntity;
    std::string ext = Sys::PathToUtf8(Sys::U8Path(path).extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".iscn")
    {
        // A model, placed the way the viewport drop (and Editor.InstantiatePrefab)
        // places one.
        Resource::MeshLibrary* meshLib = renderer ? renderer->GetMeshLibrary() : nullptr;
        if (!meshLib) return out;
        const auto res = SceneInstanceLoader::Load(Sys::RelativizeAssetPath(path), world, assetMgr, *meshLib, renderer);
        root = res.success ? res.rootEntity : NullEntity;
    }
    else
        root = Resource::LoadPrefab(path, world, assetMgr, renderer, animClipSys);
    if (root == NullEntity) return out;

    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e)) continue;
        const std::size_t i = static_cast<std::size_t>(e);
        if (i < before.size() && before[i]) continue;
        out.ents.push_back(e);
        out.set.insert(e);
    }
    if (!out.set.count(root)) { out.ents.push_back(root); out.set.insert(root); }
    out.root = root;
    return out;
}

// LoadPrefab spawns the USER'S authored character; what a preview must not run
// is stripped in the same serial window as the spawn, so no system ever sees it:
// Jolt colliders / bodies, the character controller, scripts, AI, nav,
// perception, audio, the player tag, per-object and hair shadow opt-ins (the
// hair shadow is a scene-wide fitted map a caster far below would stretch),
// every animation writer beside the panel's clip clock (Locomotion - which
// HasLivePoseSource also counts as a live pose source for the chain solver's
// resetRot - AnimDriver, LocoTransitions, AnimLayer, HeadTrack), the authored
// AnimGraph / params (the graph preview installs its own) and the VFX
// (particle emitters would spray below the scene; trails).
// KEPT: CapsuleColliderComponent - the CHAIN SOLVER's own collider
// (ChainPhysicsSystem.h, MAX_CAPSULES), not Jolt's ColliderComponent.
std::uint32_t StripStage(World& world, const std::unordered_set<Entity>& stage, const char* logPrefix, bool skip,
                         const char* skipFlag)
{
    if (skip)
    {
        LOG_WARNING("%s strip: SKIPPED (%s) - the stage keeps every authored component", logPrefix, skipFlag);
        return 0;
    }
    std::uint32_t n[23] = {};
    n[0]  = StripPool<ColliderComponent>(world, stage);
    n[1]  = StripPool<RigidBodyComponent>(world, stage);
    n[2]  = StripPool<CharacterControllerComponent>(world, stage);
    n[3]  = StripPool<ScriptComponent>(world, stage);
    n[4]  = StripPool<AIComponent>(world, stage);
    n[5]  = StripPool<BrainComponent>(world, stage);
    n[6]  = StripPool<NavAgentComponent>(world, stage);
    n[7]  = StripPool<PerceptionComponent>(world, stage);
    n[8]  = StripPool<PerceptibleComponent>(world, stage);
    n[9]  = StripPool<Audio::AudioSourceComponent>(world, stage);
    n[10] = StripPool<PlayerComponent>(world, stage);
    n[11] = StripPool<PerObjectShadowComponent>(world, stage);
    n[12] = StripPool<HairShadowCasterComponent>(world, stage);
    n[13] = StripPool<LocomotionComponent>(world, stage);
    n[14] = StripPool<CharacterAnimDriverComponent>(world, stage);
    n[15] = StripPool<LocomotionTransitionComponent>(world, stage);
    n[16] = StripPool<AnimLayerComponent>(world, stage);
    n[17] = StripPool<HeadTrackComponent>(world, stage);
    n[18] = StripPool<ParticleEmitterComponent>(world, stage);
    n[19] = StripPool<ParticleRenderComponent>(world, stage);
    n[20] = StripPool<TrailComponent>(world, stage);
    n[21] = StripPool<AnimGraphComponent>(world, stage);
    n[22] = StripPool<AnimGraphParamsComponent>(world, stage);
    std::uint32_t stripped = 0;
    for (std::uint32_t v : n) stripped += v;
    std::uint32_t capsules = 0;
    if (auto* cp = world.GetPool<CapsuleColliderComponent>())
        for (Entity e : cp->Entities()) capsules += stage.count(e) ? 1u : 0u;
    LOG_INFO("%s strip: removed=%u collider=%u rigid=%u cc=%u "
             "script=%u ai=%u brain=%u nav=%u perception=%u perceptible=%u "
             "audio=%u player=%u perObjShadow=%u hairShadow=%u locomotion=%u "
             "animDriver=%u locoTransitions=%u animLayer=%u headTrack=%u "
             "emitter=%u particleRender=%u trail=%u animGraph=%u graphParams=%u keptCapsuleCollider=%u",
             logPrefix, stripped, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8],
             n[9], n[10], n[11], n[12], n[13], n[14], n[15], n[16], n[17],
             n[18], n[19], n[20], n[21], n[22], capsules);
    return stripped;
}

void TagStage(World& world, const std::vector<Entity>& ents, std::uint8_t owner)
{
    for (Entity e : ents)
    {
        if (!world.IsAlive(e)) continue;
        if (!world.HasComponent<SceneTransientTag>(e))
            world.AddComponent<SceneTransientTag>(e, SceneTransientTag{});
        world.AddComponent<PreviewStageTag>(e, PreviewStageTag{ owner });
    }
}

// Clears the MainCamera, ShadowAny and ReflectionProbe view bits (the stage is
// nobody's caster or probe content) and sets SecondaryView. renderInMainPass MUST
// stay true: CullWorldForPreview's filterList tests it BEFORE the view bit, so
// clearing it would hide the stage from the preview too. castShadow = false keeps
// the stage out of the CSM AND makes the WorldCull geometry supply deterministic
// (the retained path's KEEP rule on !castShadow). Every stage entity gets a
// VisibilityComponent, because a missing one reads as "visible in every view".
void ApplyStageViewPolicy(World& world, const std::vector<Entity>& ents, const std::unordered_set<Entity>& set)
{
    std::vector<Entity> missing;
    for (Entity e : ents)
        if (world.IsAlive(e) && !world.HasComponent<VisibilityComponent>(e)) missing.push_back(e);
    for (Entity e : missing) world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
    if (auto* vp = world.GetPool<VisibilityComponent>())
    {
        auto& vents = vp->Entities();
        auto& data  = vp->Data();
        for (std::size_t i = 0; i < vents.size(); ++i)
        {
            if (!set.count(vents[i])) continue;
            VisibilityComponent& v = data[i];
            v.viewMask &= ~(ViewBit::MainCamera | ViewBit::ShadowAny | ViewBit::ReflectionProbe);
            v.viewMask |= ViewBit::SecondaryView;
            v.SetCastShadow(false);
            v.SetRenderInMainPass(true);
        }
    }
}

// Counts the stage's four numbers (instances, skin jobs, bones, verts) and the
// SCENE's side of each, so the refusal is against the LIVE headroom rather than
// the absolute caps: a subject that fits alone but not beside the scene would
// otherwise be admitted and park the scene's own characters at their rest pose
// (the skin-job cap truncates the list at the source). Skin jobs / bones / verts
// are the World's worst case (no per-frame counter exists for bones or verts:
// AnimProfile's bonesSampled and skinnedVerts are declared but never written);
// instances are the last frame's staged total.
const char* MeasureStageBudget(World& world, Renderer& renderer, const std::unordered_set<Entity>& stage,
                               StageBudget& b)
{
    b = StageBudget{};
    if (auto* mp = world.GetPool<MeshLibRef>())
        for (Entity e : mp->Entities()) b.instances += stage.count(e) ? 1u : 0u;
    std::unordered_set<std::uint64_t> sources;
    if (auto* kp = world.GetPool<MeshSkinnedComponent>())
    {
        auto& ents = kp->Entities();
        auto& data = kp->Data();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            if (!stage.count(ents[i]))
            {
                ++b.sceneSkinJobs;
                b.sceneVerts += data[i].vertexCount;
                continue;
            }
            ++b.skinJobs;
            b.verts += data[i].vertexCount;
            if (data[i].sharedSourceKey != 0) sources.insert(data[i].sharedSourceKey);
        }
    }
    b.sources = static_cast<std::uint32_t>(sources.size());
    if (auto* sp = world.GetPool<SkeletonComponent>())
    {
        auto& ents = sp->Entities();
        auto& data = sp->Data();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            if (stage.count(ents[i])) { ++b.skeletons; b.bones += data[i].boneCount; }
            else                      b.sceneBones += data[i].boneCount;
        }
    }
    const Renderer::FrameStats& fs = renderer.GetFrameStats();
    b.sceneInstances = fs.staticInstSlots + fs.dynamicInstances;
    b.capInstances   = renderer.GetInstanceCapacity();
    b.capSkinJobs    = SkinningPass::kMaxSkinJobs;
    b.capBones       = PoseRingBuffer::kMaxBonesPerFrame;
    b.capVerts       = SkinnedVertexRing::kMaxSkinnedVertsPerFrame;
    auto over2 = [](std::uint64_t scene, std::uint64_t subj, std::uint64_t cap) { return scene + subj > cap; };
    return over2(b.sceneInstances, b.instances, b.capInstances) ? "instances (Renderer::m_maxInstances)"
         : over2(b.sceneSkinJobs,  b.skinJobs,  b.capSkinJobs)  ? "skin jobs (SkinningPass::kMaxSkinJobs)"
         : over2(b.sceneBones,     b.bones,     b.capBones)     ? "bones (PoseRingBuffer::kMaxBonesPerFrame)"
         : over2(b.sceneVerts,     b.verts,     b.capVerts)     ? "vertices (SkinnedVertexRing::kMaxSkinnedVertsPerFrame)"
                                                                 : nullptr;
}

std::uint32_t DestroyStageEntities(World& world, Renderer* renderer, const std::vector<EntityHandle>& handles,
                                   bool markBuffers)
{
    // ONLY entities whose generation-stamped handle is still valid are ours. A stage
    // id destroyed outside the panel and recycled by CreateEntity names somebody else's
    // entity now - marking or destroying it would delete user content.
    std::unordered_set<Entity> ours;
    ours.reserve(handles.size());
    for (const EntityHandle& h : handles)
        if (world.IsHandleValid(h)) ours.insert(h.entity);
    // Mark every stage entity BEFORE destroying, so the ReleaseOwnedBuffers their
    // OnEntityDestroyed runs frees the skinned buffers instead of parking them on the
    // editor's orphan list for the rest of the session.
    if (renderer && markBuffers)
        for (Entity e : ours) renderer->MarkSkinnedBuffersUnsnapshotted(e);
    // DestroySubtree, NEVER World::DestroyEntity (which is FLAT and would orphan
    // the mesh children and leak their descriptor slots). Destroy every stage
    // entity whose parent is not itself a live stage entity: that covers the root
    // AND the top-level followers LoadPrefab spawned beside it.
    std::uint32_t destroyed = 0;
    for (const EntityHandle& h : handles)
    {
        if (!world.IsHandleValid(h)) continue;   // already gone (a parent's subtree, or outside)
        const Entity e = h.entity;
        const Parent* par = world.GetComponent<Parent>(e);
        const bool parentInStage = par && par->entity != NullEntity && ours.count(par->entity) &&
                                   world.IsAlive(par->entity);
        if (parentInStage) continue;
        destroyed += Hierarchy::DestroySubtree(world, e);
    }
    return destroyed;
}

// A VirtualCameraComponent and NO CameraComponent. ExtractPreviewCameras accepts
// either lens, but App::RefreshMainCamera falls back to "the first entity with a
// CameraComponent" whenever a scene has no ActiveCameraTag - a preview camera with
// one could become the scene's main camera (and Lua's CameraEntity) in such a
// scene. Without VCamPriority / VCamBlend it never joins the VCam stack either.
Entity CreateStageCamera(World& world, const char* name, std::uint8_t owner)
{
    const Entity cam = world.CreateEntity();
    world.SetName(cam, name);
    world.AddComponent<LocalTransform>(cam, LocalTransform{});
    world.AddComponent<GlobalTransform>(cam, GlobalTransform{});
    VirtualCameraComponent vc{};
    vc.fov   = Preview::kStudioCamera.fovY;
    vc.nearZ = kStageCameraNear;
    vc.farZ  = kStageCameraFar;
    world.AddComponent<VirtualCameraComponent>(cam, vc);
    world.AddComponent<SceneTransientTag>(cam, SceneTransientTag{});
    world.AddComponent<PreviewStageTag>(cam, PreviewStageTag{ owner });
    return cam;
}

// ==== PreviewStageHost: one owner's stage, on the functions above ====
namespace
{
    std::string StemOf(const std::string& path)
    {
        const std::size_t slash = path.find_last_of("/\\");
        std::string s = (slash == std::string::npos) ? path : path.substr(slash + 1);
        const std::size_t dot = s.find_last_of('.');
        return (dot == std::string::npos) ? s : s.substr(0, dot);
    }

    // The first SkeletonComponent entity of the stage in the root's depth-first
    // order (the prefab's own skeleton before a follower's); pool order for one
    // that is not under the root (a top-level follower's).
    Entity FirstStageSkeleton(World& world, Entity root, const std::unordered_set<Entity>& set)
    {
        std::vector<Entity> stack{ root };
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (set.count(e) && world.HasComponent<SkeletonComponent>(e)) return e;
            if (const Children* ch = world.GetComponent<Children>(e))
                for (auto it = ch->entities.rbegin(); it != ch->entities.rend(); ++it) stack.push_back(*it);
        }
        if (auto* sp = world.GetPool<SkeletonComponent>())
            for (Entity e : sp->Entities())
                if (set.count(e)) return e;
        return NullEntity;
    }
}

bool PreviewStageHost::Spawn(World& world, Renderer& renderer, Resource::AssetManager& assetMgr,
                             Resource::AnimationClipSystem* animClipSys, const std::string& subject, std::string& why)
{
    if (Up()) return true;
    StageSpawn sp = LoadStageSubject(subject, world, assetMgr, &renderer, animClipSys);
    if (sp.root == NullEntity)
    {
        why = "'" + subject + "' did not load (see the log)";
        return false;
    }
    m_root = sp.root;
    m_ents = std::move(sp.ents);
    m_set  = std::move(sp.set);
    m_stripped = StripStage(world, m_set, m_desc.logPrefix, false, "");
    TagStage(world, m_ents, m_desc.owner);
    world.SetName(m_root, std::string(m_desc.rootPrefix) + StemOf(subject));
    m_handles.clear();
    m_handles.reserve(m_ents.size());
    for (Entity e : m_ents) m_handles.push_back(world.MakeHandle(e));
    m_rootH = world.MakeHandle(m_root);
    // The origin slot; the authored rotation and scale stay.
    if (LocalTransform* lt = world.GetComponent<LocalTransform>(m_root)) lt->translation = { 0.f, OriginY(), 0.f };
    ApplyStageViewPolicy(world, m_ents, m_set);

    m_skel = FirstStageSkeleton(world, m_root, m_set);
    if (m_skel == NullEntity)
    {
        TearDown(&renderer, &world, "no skeleton");
        why = "'" + subject + "' has no skeleton";
        return false;
    }
    m_skelH = world.MakeHandle(m_skel);
    StageBudget b;
    if (const char* over = MeasureStageBudget(world, renderer, m_set, b))
    {
        TearDown(&renderer, &world, "over budget");
        why = std::string("the scene plus the subject exceed the per-frame budget for ") + over;
        return false;
    }
    m_pin.clear();
    if (auto* sk = world.GetPool<SkeletonComponent>())
        for (Entity e : sk->Entities())
            if (m_set.count(e)) m_pin.insert(e);
    ++m_spawns;
    LOG_INFO("%s stage spawned root=%u entities=%u origin=(0,%.0f,0) stripped=%u spawns=%u skeletons=%zu "
             "skinned=%u subject='%s'", m_desc.logPrefix, static_cast<unsigned>(m_root),
             static_cast<unsigned>(m_ents.size()), OriginY(), m_stripped, m_spawns, m_pin.size(), b.skinJobs,
             subject.c_str());
    return true;
}

void PreviewStageHost::TearDown(Renderer* renderer, World* world, const char* reason)
{
    if (world && m_root != NullEntity)
    {
        const std::uint32_t n = DestroyStageEntities(*world, renderer, m_handles, /*markBuffers=*/true);
        LOG_INFO("%s stage destroyed: %u entit%s (root %u) reason=%s", m_desc.logPrefix, n, n == 1 ? "y" : "ies",
                 static_cast<unsigned>(m_root), reason);
    }
    // Through the handle: a recycled id is not our camera.
    if (world && m_cam != NullEntity && world->IsHandleValid(m_camH)) Hierarchy::DestroySubtree(*world, m_cam);
    ReleaseView(renderer, reason);
    Unpin(renderer);
    ForgetEntities();
}

void PreviewStageHost::ForgetEntities()
{
    m_root = m_skel = m_cam = NullEntity;
    m_rootH = m_skelH = m_camH = NullEntityHandle;
    m_ents.clear();
    m_handles.clear();
    m_set.clear();
    m_pin.clear();   // the ids will be recycled: a stale pin would animate whatever claims them
}

bool PreviewStageHost::Validate(World& world)
{
    if (m_root == NullEntity) return true;
    bool anyDead = false;
    for (const EntityHandle& h : m_handles)
        if (!world.IsHandleValid(h)) { anyDead = true; break; }
    if (!anyDead) return true;
    std::vector<Entity>       ents;
    std::vector<EntityHandle> handles;
    std::uint32_t dead = 0;
    for (std::size_t i = 0; i < m_handles.size(); ++i)
    {
        if (world.IsHandleValid(m_handles[i]))
        {
            ents.push_back(m_ents[i]);
            handles.push_back(m_handles[i]);
            continue;
        }
        // The id may already be somebody else's: out of every set NOW, before the
        // pin can read it this frame.
        m_set.erase(m_ents[i]);
        m_pin.erase(m_ents[i]);
        ++dead;
    }
    m_ents.swap(ents);
    m_handles.swap(handles);
    const bool alive = world.IsHandleValid(m_rootH) && world.IsHandleValid(m_skelH);
    LOG_WARNING("%s %u stage entit%s destroyed outside the panel%s", m_desc.logPrefix, dead,
                dead == 1 ? "y was" : "ies were", alive ? "" : " - the root / skeleton with them");
    return alive;
}

std::vector<EntityHandle> PreviewStageHost::Adopt(World& world, StageSpawn&& extra)
{
    std::vector<EntityHandle> out;
    if (!Up() || extra.root == NullEntity) return out;
    StripStage(world, extra.set, m_desc.logPrefix, false, "");
    TagStage(world, extra.ents, m_desc.owner);
    ApplyStageViewPolicy(world, extra.ents, extra.set);
    out.reserve(extra.ents.size());
    for (Entity e : extra.ents)
    {
        const EntityHandle h = world.MakeHandle(e);
        out.push_back(h);
        m_ents.push_back(e);
        m_handles.push_back(h);
        m_set.insert(e);
        if (world.HasComponent<SkeletonComponent>(e)) m_pin.insert(e);   // its own skeleton animates too
    }
    return out;
}

void PreviewStageHost::Drop(World& world, Renderer* renderer, const std::vector<EntityHandle>& handles)
{
    if (handles.empty()) return;
    // The (id, generation) pairs going - taken BEFORE the destroy recycles anything.
    auto key = [](const EntityHandle& h) { return (std::uint64_t(h.entity) << 32) | std::uint64_t(h.generation); };
    std::unordered_set<std::uint64_t> gone;
    for (const EntityHandle& h : handles) gone.insert(key(h));
    DestroyStageEntities(world, renderer, handles, /*markBuffers=*/true);
    std::vector<Entity>       ents;
    std::vector<EntityHandle> kept;
    for (std::size_t i = 0; i < m_handles.size(); ++i)
    {
        if (gone.count(key(m_handles[i])))
        {
            m_set.erase(m_ents[i]);
            m_pin.erase(m_ents[i]);
            continue;
        }
        ents.push_back(m_ents[i]);
        kept.push_back(m_handles[i]);
    }
    m_ents.swap(ents);
    m_handles.swap(kept);
}

void PreviewStageHost::Pin(Renderer& renderer)
{
    renderer.AddAnimPinOwner(this, &m_pin);
    m_pinned = true;
}

void PreviewStageHost::Unpin(Renderer* renderer)
{
    if (renderer && m_pinned) renderer->RemoveAnimPinOwner(this);
    m_pinned = false;
}

bool PreviewStageHost::EnsureView(Renderer& renderer)
{
    if (m_view != ~0u) return true;
    if (m_viewTries++ % 60u != 0u) return false;   // none was free: ask again once a second
    m_view = renderer.AcquirePreviewView(/*worldCull=*/true);
    m_askedW = m_askedH = 0;
    m_sharingLane = false;
    if (m_view == ~0u) return false;
    m_viewTries = 0;
    LOG_INFO("%s acquired view %u (WorldCull)", m_desc.logPrefix, m_view);
    return true;
}

void PreviewStageHost::ReleaseView(Renderer* renderer, const char* reason)
{
    if (m_view != ~0u && renderer)
    {
        renderer->SetPreviewCameraEntity(m_view, NullEntity);
        renderer->ReleasePreviewView(m_view);
        LOG_INFO("%s released view %u (%s)", m_desc.logPrefix, m_view, reason);
    }
    m_view = ~0u;
    m_viewTries = 0;
    m_askedW = m_askedH = m_renderW = m_renderH = 0;
    m_sharingLane = false;
}

void PreviewStageHost::NegotiateSize(Renderer& renderer, std::uint32_t w, std::uint32_t h)
{
    if (m_view == ~0u || w == 0 || h == 0) return;
    if (w == m_askedW && h == m_askedH && !m_sharingLane) return;
    const std::uint32_t one = m_view;
    if (renderer.RequestPreviewWorldCullLaneSize(w, h, &one, 1u))
    {
        m_renderW = w;
        m_renderH = h;
        m_sharingLane = false;
    }
    else
    {
        std::uint32_t lw = 0, lh = 0;
        renderer.GetPreviewWorldCullSize(lw, lh);
        const bool have = lw != 0 && lh != 0;
        if (have && (!m_sharingLane || m_renderW != lw || m_renderH != lh))
            LOG_INFO("%s sharing lane size %ux%u (wanted %ux%u; another view holds the WorldCull lane)",
                     m_desc.logPrefix, lw, lh, w, h);
        m_renderW = have ? lw : w;
        m_renderH = have ? lh : h;
        m_sharingLane = have;
    }
    m_askedW = w;
    m_askedH = h;
}

void PreviewStageHost::EnsureCamera(World& world)
{
    // The HANDLE decides - a recycled id is somebody else's entity.
    if (m_cam != NullEntity && world.IsHandleValid(m_camH)) return;
    m_cam  = CreateStageCamera(world, m_desc.cameraName, m_desc.owner);
    m_camH = world.MakeHandle(m_cam);
}

void PreviewStageHost::PlaceCamera(World& world, const float eye[3], const float forward[3])
{
    using namespace DirectX;
    if (m_cam == NullEntity || !world.IsHandleValid(m_camH)) return;
    const XMVECTOR f   = XMVector3Normalize(XMVectorSet(forward[0], forward[1], forward[2], 0.f));
    const XMVECTOR upW = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    const XMVECTOR r   = XMVector3Normalize(XMVector3Cross(upW, f));   // LH: up x fwd
    const XMVECTOR u   = XMVector3Cross(f, r);
    XMMATRIX m;
    m.r[0] = r;
    m.r[1] = u;
    m.r[2] = f;
    m.r[3] = XMVectorSet(eye[0], eye[1], eye[2], 1.f);
    // ExtractPreviewCameras reads position = row 3, forward = row 2, up = row 1.
    if (GlobalTransform* gt = world.GetComponent<GlobalTransform>(m_cam)) XMStoreFloat4x4(&gt->matrix, m);
    if (LocalTransform* lt = world.GetComponent<LocalTransform>(m_cam))
    {
        lt->translation = { eye[0], eye[1], eye[2] };
        XMStoreFloat4(&lt->rotation, XMQuaternionNormalize(XMQuaternionRotationMatrix(m)));
        lt->scale = { 1.f, 1.f, 1.f };
    }
}

void PreviewStageHost::Request(Renderer& renderer)
{
    if (m_view == ~0u || m_cam == NullEntity || m_renderW == 0 || m_renderH == 0) return;
    // PUSHED EVERY FRAME: ExtractPreviewCameras reads it on the game half, and the
    // Renderer zeroes camera identities on a world clear.
    renderer.SetPreviewCameraEntity(m_view, m_cam);
    Preview::ViewDesc d{};
    d.w      = m_renderW;
    d.h      = m_renderH;
    d.source = Preview::Source::WorldCull;
    d.tickIntervalFrames = 1;
    // The scene's own sky: a WorldCull view is a second camera under the scene's lighting.
    d.features   = Preview::Feature::kSkybox;
    d.background = Preview::Background::Skybox;
    renderer.RequestPreview(m_view, d, MaterialComponent{});
}

// ==== StageClipPlayer: a stage's clip transport (see the header) ====
void StageClipPlayer::Select(Resource::AnimationClipSystem* acs, const std::string& path,
                             const std::string& sourceClip)
{
    const std::string rel = path.empty() ? std::string() : Sys::RelativizeAssetPath(path);
    if (m_handle.IsValid() && acs) acs->ReleaseClip(m_handle);
    m_handle       = Resource::AnimHandle{};
    m_path         = rel;
    m_source       = sourceClip;
    m_clipName.clear();
    m_applyPending = true;   // Update re-registers (or, with no clip, removes) the alias
    m_bound        = false;
    m_infoValid    = false;
    m_duration     = 0.f;
    m_fps          = 30.f;
    m_frames = m_channels = m_boundBones = 0;
    // The playhead is the caller's (a picker starts it at 0 playing; a script may set
    // both with the clip): it wraps / clamps once the new clip's duration is known.
    if (acs && !m_path.empty()) m_handle = acs->AcquireClip(m_path);
    LOG_INFO("%s clip selected: '%s' source='%s'", m_logPrefix.c_str(), m_path.empty() ? "(rest pose)" : m_path.c_str(),
             m_source.c_str());
}

void StageClipPlayer::Release(Resource::AnimationClipSystem* acs)
{
    if (m_handle.IsValid() && acs) acs->ReleaseClip(m_handle);
    m_handle = Resource::AnimHandle{};
    m_infoValid = false;
}

void StageClipPlayer::StepFrames(int n)
{
    m_playing = false;
    m_t += static_cast<double>(n) / static_cast<double>(m_fps > 0.f ? m_fps : 30.f);
}

void StageClipPlayer::RefreshInfo(Resource::AnimationClipSystem* acs, const SkeletonAsset* skel)
{
    if (!acs || m_path.empty()) return;
    // Given back (a Release) while the selection survived: taken again.
    if (!m_handle.IsValid()) m_handle = acs->AcquireClip(m_path);
    const std::uint32_t skelBones = skel ? skel->boneCount : 0u;
    if ((m_infoValid && m_infoSkelBones == skelBones) || !acs->IsReady(m_handle)) return;
    m_infoValid     = true;
    m_infoSkelBones = skelBones;
    const Resource::AnimationResource* res = acs->GetResource(m_handle);
    const Resource::AnimClipData* clip = nullptr;
    if (res)
        for (const Resource::AnimClipData& c : res->clips)
            if (m_source.empty() || m_source == c.name) { clip = &c; break; }
    if (!clip)
    {
        m_duration = 0.f;
        m_frames = m_channels = m_boundBones = 0;
        LOG_WARNING("%s clip '%s' has no bone clip%s", m_logPrefix.c_str(), m_path.c_str(),
                    m_source.empty() ? "" : (" named '" + m_source + "'").c_str());
        return;
    }
    m_clipName = clip->name;
    m_duration = clip->duration;
    m_fps      = clip->frameRate > 0.f ? clip->frameRate : 30.f;
    m_frames   = clip->frameCount;
    m_channels = static_cast<std::uint32_t>(clip->channels.size());
    // The bones it drives - playback's binding (Resource::FitClip: exact names).
    m_boundBones = skel ? Resource::FitClip(*clip, *skel).boundBones : 0u;
}

void StageClipPlayer::Update(World& world, Resource::AnimationClipSystem* acs, Entity animEnt,
                             const SkeletonAsset* skel, double dt)
{
    if (!acs || animEnt == NullEntity || !world.IsAlive(animEnt)) return;
    AnimationComponent* anim = world.GetComponent<AnimationComponent>(animEnt);
    if (m_path.empty())
    {
        // THE REST POSE: no clip on the stage - an authored default clip stopped too,
        // so AnimationSystem writes the skeleton's rest pose.
        if (!m_alias.empty()) acs->RemoveAnimatorClip(world, animEnt, m_alias);
        m_alias.clear();
        if (anim && anim->primaryClip != kInvalidAnimHandle) acs->StopAnimator(world, animEnt);
        m_applyPending = false;
        m_bound = false;
        return;
    }
    if (m_applyPending)
    {
        // TWO steps: QueueAnimatorPlay resolves a NAME through the alias table, so the
        // path's alias goes in first; the old one goes BEFORE the new is queued
        // (removing the current clip stops the animator - a queued request with it).
        m_applyPending = false;
        if (!m_alias.empty()) acs->RemoveAnimatorClip(world, animEnt, m_alias);
        m_alias.clear();
        std::string stem = m_path;
        if (const std::size_t s = stem.find_last_of("/\\"); s != std::string::npos) stem = stem.substr(s + 1);
        if (const std::size_t d = stem.find_last_of('.'); d != std::string::npos) stem = stem.substr(0, d);
        const std::string alias = m_aliasPrefix + stem;
        const bool registered = acs->SetAnimatorClip(world, animEnt, alias, m_path, /*looping=*/true, /*speed=*/1.f,
                                                     m_source);
        const bool queued = registered && acs->QueueAnimatorPlay(world, animEnt, alias, /*blend=*/0.f, /*restart=*/false);
        if (registered) m_alias = alias;
        LOG_INFO("%s clip: alias='%s' path='%s' source='%s' aliasRegistered=%d queued=%d entity=%u",
                 m_logPrefix.c_str(), alias.c_str(), m_path.c_str(), m_source.c_str(), registered ? 1 : 0,
                 queued ? 1 : 0, static_cast<unsigned>(animEnt));
    }
    RefreshInfo(acs, skel);

    // ---- the transport: arithmetic on t only ----
    const double dur = (m_infoValid && m_duration > 0.f) ? m_duration : 0.0;
    if (m_playing && std::isfinite(dt) && dt > 0.0) m_t += dt * static_cast<double>(m_speed);
    if (dur > 0.0)
    {
        if (m_looping)
        {
            m_t = std::fmod(m_t, dur);
            if (m_t < 0.0) m_t += dur;
        }
        else
        {
            if (m_t >= dur) { m_t = dur; if (m_speed > 0.f) m_playing = false; }
            if (m_t <= 0.0) { m_t = 0.0; if (m_speed < 0.f) m_playing = false; }
        }
    }
    else if (!std::isfinite(m_t) || m_t < 0.0)
        m_t = 0.0;

    // ---- the per-frame clock write ----
    if (anim)
    {
        anim->externalTime   = true;
        anim->paused         = false;
        anim->primaryTime    = static_cast<float>(m_t);
        anim->prevNotifyTime = -1.f;
    }
    // Is OUR clip the stage's current one (the alias bound, its clip loaded)?
    const AnimatorTransitionComponent* tr = world.GetComponent<AnimatorTransitionComponent>(animEnt);
    m_bound = anim && tr && !m_alias.empty() && tr->currentClipName == m_alias &&
              anim->primaryClip != kInvalidAnimHandle;
}
} // namespace Editor
