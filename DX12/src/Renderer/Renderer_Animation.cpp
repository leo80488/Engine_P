// Renderer_Animation.cpp - the Renderer's half of animation: the anim pin's owners,
// PrepareAnimationVisibility (which skinned entities animate this frame) and
// TickAnimationChain (sampling, the pose chain and the skinning jobs it feeds).

#include "Diagnostics/Profiler.h"
#include <optional>
#include "System/PathUtil.h"
#include "Renderer/Renderer.h"
#include "Graphics/AnimProfile.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/ParticleSystem.h"

// ECS components + systems
#include "ECS/ECS.h"
#include "ECS/FaceShadingSystem.h"   // FaceShading::Update - head-bone world axes -> the face material's frameParams
#include "Gameplay/AttackPlayback.h" // a swinging skeleton is never culled
#include "Gameplay/EventDelivery.h"   // the legacy arm of the event drain
#include "Gameplay/GameplayTime.h"

// engine systems
#include "System/Log.h"
#include "System/JobDefaults.h"
#include "System/CommandLine.h"
#include "System/Check.h"     // ENGINE_CHECK_ONCE - pool-overflow tripwires
#include "System/TaskSystem.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <DirectXMath.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;


// The anim pin's owners (see the header). Called only from a panel's
// ProcessPendingActions - never while classify reads them.
void Renderer::AddAnimPinOwner(const void* owner, const std::unordered_set<Entity>* s)
{
    if (!owner || !s) return;
    for (AnimPinOwner& o : m_animPinOwners)
        if (o.owner == owner) { o.set = s; return; }
    m_animPinOwners.push_back({ owner, s });
}

void Renderer::RemoveAnimPinOwner(const void* owner)
{
    std::erase_if(m_animPinOwners, [owner](const AnimPinOwner& o) { return o.owner == owner; });
}

bool Renderer::HasAnimPinOwner(const void* owner) const
{
    for (const AnimPinOwner& o : m_animPinOwners)
        if (o.owner == owner) return true;
    return false;
}

void Renderer::PrepareAnimationVisibility(World& world, FrameIndex frame)
{
    // Owner-only, CPU pool reads; every classify job joins before returning.
    // No upload slot is selected, reset, or written here.
    m_animationVisibilityFrame.reset();
    m_animationVisibilityWorld = nullptr;
    if (!m_skin.IsInitialised()) return;
    // Animation culling tests the PREVIOUS frame's frustums (the view is rebuilt in
    // BeginFrame, after this): an entity entering the frustum shows a 1-frame-stale pose.
    if (m_skin.IsAnimCullingEnabled())
    {
        // Static-named zone (csvexport-friendly) covering classify + merge,
        // so captures can track the culling cost across path changes.
        ZoneScopedN("AnimVisibilityCull");
        Anim::StageTimer _tCull;

        m_skin.GetAnimVisibleSet().clear();
        m_skin.GetAnimShadowOnlySet().clear();

        // Skeleton roots usually lack WorldAabb - merge the children's AABBs (root as fallback).
        // Iterate the SkeletonComponent pool's dense array (avoids ~22k hash lookups on Bistro).
        auto* pSkel      = world.GetPool<SkeletonComponent>();
        auto* pWorldAabb = world.GetPool<WorldAabb>();
        auto* pChildren  = world.GetPool<Children>();
        const size_t skelN = pSkel ? pSkel->Data().size() : 0;
        const auto&  skelEnts = EntitiesOf(pSkel);
        // A vector-valued conditional would COPY the live pool even when
        // bound to const auto&. These read-only jobs join before pool mutation.
        const SkeletonComponent* skelData = pSkel ? pSkel->Data().data() : nullptr;

        // Per-skeleton verdict, written to disjoint slots so the classify step can fan out to
        // TaskSystem workers (everything it touches - pools, the view's boundingFrustum,
        // m_shadowFrustum, AabbVs6Planes - is a pure read). The unordered_set insert is NOT
        // thread-safe, so visible entities land in the set in the serial merge below.
        enum : uint8_t { kSkip = 0, kCulled, kNoAabb, kFrustumPass, kShadowPass };
        // The pinned owners this frame reads: every one, or with -anim_pin_single (the reverse
        // arm) only the first.
        static const bool s_pinSingle = Sys::CommandLine::Get().Has("-anim_pin_single");
        const std::size_t pinOwners = s_pinSingle ? (std::min)(m_animPinOwners.size(), std::size_t(1))
                                                  : m_animPinOwners.size();
        auto* pPlayback = world.GetPool<Attack::Playback>();
        auto classify = [&](size_t si) -> uint8_t
        {
            const SkeletonComponent* skel = &skelData[si];
            if (skel->assetIndex == kInvalidAnimHandle) return kSkip;
            const Entity e = skelEnts[si];

            // A skeleton mid-attack animates wherever it is: its weapon sweeps the posed blade,
            // and a culled pose would hit, or miss, by a stale frame.
            if (pPlayback)
                if (const Attack::Playback* swing = pPlayback->Get(e); swing && swing->active)
                    return kFrustumPass;

            // A PINNED skeleton animates regardless of the main frustum and the shadow cascades: a
            // panel's hidden stage sits km below the scene and would otherwise be kCulled, i.e. drawn
            // in its bind pose (LocalToWorldSystem stamps poseByteOffset=~0u before the active set
            // skip). This lambda runs on ParallelFor workers when skelN >= 16, so the lookup must stay
            // a pure read: unordered_set::count is a const query, and the owners and their sets are
            // only rewritten by the panels' ProcessPendingActions, which never overlaps
            // PrepareAnimationVisibility. No owner = one size compare.
            for (std::size_t k = 0; k < pinOwners; ++k)
                if (m_animPinOwners[k].set->count(e)) return kFrustumPass;

            // Build merged AABB from skeleton root + its children.
            using namespace DirectX;
            XMVECTOR vmin = XMVectorSet( FLT_MAX,  FLT_MAX,  FLT_MAX, 0);
            XMVECTOR vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0);
            bool hasAabb = false;

            auto mergeAabb = [&](const WorldAabb* wa) {
                if (!wa) return;
                vmin = XMVectorMin(vmin, XMVectorSet(wa->min.x, wa->min.y, wa->min.z, 0));
                vmax = XMVectorMax(vmax, XMVectorSet(wa->max.x, wa->max.y, wa->max.z, 0));
                hasAabb = true;
            };

            // Check root entity's own AABB
            if (pWorldAabb) mergeAabb(pWorldAabb->Get(e));

            // Merge children AABBs (mesh sub-entities)
            const Children* ch = pChildren ? pChildren->Get(e) : nullptr;
            if (ch)
            {
                for (Entity child : ch->entities)
                    if (pWorldAabb) mergeAabb(pWorldAabb->Get(child));
            }

            if (!hasAabb) return kNoAabb;

            XMFLOAT3 lo, hi;
            XMStoreFloat3(&lo, vmin);
            XMStoreFloat3(&hi, vmax);

            BoundingBox bb;
            bb.Center  = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
            bb.Extents = { (hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f };

            // Camera frustum test with BoundingFrustum::Intersects, not the fast 6-plane path
            // (suspected of a startup flicker through ExtractFrustumPlanes).
            if (FrameViewWrite().view.boundingFrustum.Intersects(bb))
                return kFrustumPass;

            // Shadow frustums: shadow casters need animation too. Bounded by kCasterCascades, not
            // kCascadeCount: cascade 3 is the ultra-far terrain cascade (ShadowSystem::kFarCascadeFar =
            // 2000 m) and ShadowPass skips its whole packet loop (terrainOnlyCascade), so a skeleton
            // admitted only by cascade 3 would animate for a cascade that never draws it. The AABB fast
            // reject mirrors ShadowFrustumCompute::IntersectsAny.
            if (!m_animShadowHoldout && m_shadowFrustum.IsValid())
            {
                for (int sc = 0; sc < ShadowFrustumCompute::kCasterCascades; ++sc)
                {
                    if (!m_shadowFrustum.GetAABB(sc).Intersects(bb)) continue;
                    if (ShadowFrustumCompute::AabbVs6Planes(bb, m_shadowFrustum.GetPlanes(sc).planes))
                        return static_cast<uint8_t>(kShadowPass | (sc << 4));
                }
            }

            return kCulled;
        };

        thread_local std::vector<uint8_t> t_verdict;
        t_verdict.resize(skelN);
        uint8_t* verdictData = t_verdict.data(); // capture caller storage, not worker TLS
        constexpr size_t kParallelCullThreshold = 16;   // below this, dispatch costs more than the tests
        if (skelN >= kParallelCullThreshold)
        {
            TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(skelN),
                [&](uint32_t si) { verdictData[si] = classify(si); });
        }
        else
        {
            for (size_t si = 0; si < skelN; ++si)
                t_verdict[si] = classify(si);
        }

        // Serial merge: visible-set inserts + debug counters.
        int totalSkeletons = 0;
        int noAabbCount    = 0;
        int frustumPass    = 0;
        int shadowPass     = 0;
        int culledCount    = 0;
        std::uint32_t cascadeHits[4] = { 0, 0, 0, 0 };
        for (size_t si = 0; si < skelN; ++si)
        {
            // The classify lambda packs the admitting cascade into the high
            // nibble for kShadowPass; everything else uses the low nibble only.
            const uint8_t v  = static_cast<uint8_t>(t_verdict[si] & 0x0F);
            const int     sc = t_verdict[si] >> 4;
            if (v == kSkip) continue;
            ++totalSkeletons;
            switch (v)
            {
            case kNoAabb:      m_skin.GetAnimVisibleSet().insert(skelEnts[si]); ++noAabbCount;  break;
            case kFrustumPass: m_skin.GetAnimVisibleSet().insert(skelEnts[si]); ++frustumPass;  break;
            case kShadowPass:  m_skin.GetAnimVisibleSet().insert(skelEnts[si]);
                               m_skin.GetAnimShadowOnlySet().insert(skelEnts[si]);
                               ++shadowPass;
                               if (sc >= 0 && sc < 4) ++cascadeHits[sc];
                               break;
            default:           ++culledCount;                                                   break;
            }
        }
        {
            auto& prof = Anim::AnimProfile::Get();
            prof.cullMs    = _tCull.Ms();
            prof.skeletons = static_cast<std::uint32_t>(totalSkeletons);
            // What survived the cull IS the animation population - the difference between this and
            // `skeletons` is what culling saved.
            prof.animated  = static_cast<std::uint32_t>(
                                 m_skin.GetAnimVisibleSet().size());
            prof.cullNoAabb  = static_cast<std::uint32_t>(noAabbCount);
            prof.cullFrustum = static_cast<std::uint32_t>(frustumPass);
            for (int i = 0; i < 4; ++i) prof.cullShadowCascade[i] = cascadeHits[i];
            prof.shadowHoldout = m_animShadowHoldout;
            prof.cullShadow  = static_cast<std::uint32_t>(shadowPass);
            prof.cullSkipped = static_cast<std::uint32_t>(culledCount);
        }
    }
    else
    {
        // Culling off: every skeleton animates, and the profile must say so
        // rather than reporting a stale count from the last frame it ran.
        auto* pSkelAll = world.GetPool<SkeletonComponent>();
        auto& prof = Anim::AnimProfile::Get();
        prof.cullMs    = 0.f;
        prof.skeletons = pSkelAll ? static_cast<std::uint32_t>(pSkelAll->Data().size()) : 0u;
        prof.animated  = prof.skeletons;
    }

    m_animationVisibilityWorld = &world;
    m_animationVisibilityEpoch = world.Epoch();
    m_animationVisibilityFrame = frame;
#if WITH_HARNESS
    if (Sys::CommandLine::Get().Has("-materialcpuverify"))
        LOG_INFO("[ANIM_VISIBILITY_PREP] frame=%llu match=1",
            static_cast<unsigned long long>(frame));
#endif
}

void Renderer::TickAnimationChain(World& world, FrameIndex frame, float dt)
{
    Anim::StageTimer _tWall;
    // App acquired the slot before this phase. Descriptor reset must precede
    // BuildSkinJobs and Afterimage patches, including on static-only scenes.
    ENGINE_CHECK_ONCE(!m_gfx.IsSlotKeyedByFrame()
                      || m_gfx.GetUploadSlot()
                         == static_cast<uint32_t>(frame % RHI::kFramesInFlight),
        "[SPLITCHECK][SLOTPARITY] frame %llu rotates upload rings to slot %u, but the "
        "game side claimed slot %llu — AcquireUploadSlot was skipped or ran with a "
        "different frame number.",
        static_cast<unsigned long long>(frame), m_gfx.GetUploadSlot(),
        static_cast<unsigned long long>(frame % RHI::kFramesInFlight));


    m_meshMgr.GetDescriptorHeap().BeginFrame(m_gfx.GetUploadSlot());
    if (!m_skin.IsInitialised()) return;

    const bool prepared = m_animationVisibilityFrame && *m_animationVisibilityFrame == frame &&
        m_animationVisibilityWorld == &world && m_animationVisibilityEpoch == world.Epoch();
#if WITH_HARNESS
    if (prepared && Sys::CommandLine::Get().Has("-materialcpuverify"))
    {
        ZoneScopedN("Anim.VisibilityPrepassOracle");
        const auto visible = m_skin.GetAnimVisibleSet();
        const auto shadow = m_skin.GetAnimShadowOnlySet();
        // Recompute here, at the consumer, to catch any animation-driver mutation since the
        // prepass that would change the classification. Verification only.
        PrepareAnimationVisibility(world, frame);
        if (visible != m_skin.GetAnimVisibleSet() || shadow != m_skin.GetAnimShadowOnlySet())
            throw std::runtime_error("Animation visibility prepass mismatch");
        LOG_INFO("[ANIM_VISIBILITY_REUSE] frame=%llu match=1",
            static_cast<unsigned long long>(frame));
    }
#endif
    if (!prepared) PrepareAnimationVisibility(world, frame);
    m_animationVisibilityFrame.reset();
    const std::unordered_set<Entity>* animActivePtr = m_skin.IsAnimCullingEnabled()
        ? &m_skin.GetAnimVisibleSet() : nullptr;

    // All rings use the slot actually acquired by App, including DXGI fallback.
    Anim::StageTimer _tProlog;
    const uint32_t frameSlot = m_gfx.GetUploadSlot();
    m_skin.GetPoseRingBuffer().BeginFrame(frameSlot);
    // Rotate ring BEFORE any custom-material packing happens this frame.
    m_customMatCbvRing.BeginFrame(frameSlot);
    m_customMatSrvRing.BeginFrame(frameSlot);
    m_skin.GetVertexRing().BeginFrame(frameSlot);
    Anim::AnimProfile::Get().prologueMs = _tProlog.Ms();
    // No character-state tick here: Character.SetState/AddState go through the Animator alias
    // path (AnimationClipSystem mailbox), CharacterStateComponent data migrates into
    // AnimationSetComponent in ResolveAnimationSets, and gait/action selection is
    // CharacterAnimDriverSystem's.
    // Sampling resolves each entity's dt from both of the frame's clocks (TimeDilationComponent);
    // dt is the game one, which the stages after it share.
    const GameplayTime::Clocks& clocks = GameplayTime::Current();
    ENGINE_CHECK_ONCE(clocks.game == dt,
        "[GAMETIME] TickAnimationChain got dt %.6f but the published game clock is %.6f - "
        "GameplayTime::Publish was skipped for this frame.", dt, clocks.game);
    { ZoneScopedN("Anim.Sample"); PROF_SCOPE("Anim.Sample"); Anim::StageTimer _t;
    m_skin.GetAnimGraphSystem()->Prepare(world, clocks);
    m_skin.GetAnimationSystem()->Update(world, clocks, animActivePtr);
    m_skin.GetAnimGraphSystem()->Publish(world);
      Anim::AnimProfile::Get().sampleMs = _t.Ms(); }
    // The modules' BeforeFacial stages (a tracked face writes each binding's Tracked morph list
    // for the Facial fold right below).
    //
    // Facial morph layer: resolves FacialAnimationComponent channels (auto-blink, expressions,
    // vowel visemes) into a mesh-slot-indexed resolvedOut[] that BuildSkinJobs (later in this
    // function) folds onto the morph pool. After the clip sample, so it composes on top of a
    // morph clip instead of racing it, and before BuildSkinJobs (the hard requirement). Both
    // are straight-line statements in this function on one thread; that, plus
    // RendererAnimationChainSystem being opaque to the Scheduler, is the whole ordering
    // guarantee. It touches no pose, so its place relative to IK / HeadTrack is free.
    for (IAnimChainHook* hook : m_skin.GetChainHooks()) hook->BeforeFacial(world, dt, animActivePtr);
    { ZoneScopedN("Anim.Facial"); PROF_SCOPE("Anim.Facial"); Anim::StageTimer _t;
    if (auto* fas = m_skin.GetFacialAnimationSystem())
        fas->Update(world, dt, animActivePtr);
      Anim::AnimProfile::Get().facialMs = _t.Ms(); }
    m_skin.GetIKRigSystem()->SetEvaluationEnabled(m_skin.GetIKSystem()->IsEnabled());
    m_skin.GetIKRigSystem()->SetRelevance(FrameViewWrite().view.cameraPosition,
        m_skin.IsAnimCullingEnabled()?&m_skin.GetAnimShadowOnlySet():nullptr);
    {Anim::StageTimer timer;
     m_skin.GetIKRigSystem()->Prepare(world,dt,m_skin.GetPhysicsSystem(),animActivePtr);
     Anim::AnimProfile::Get().ikRigPrepareMs=timer.Ms();}

    { Anim::StageTimer _t;
    m_skin.GetIKSystem()->Update(world, animActivePtr);
      Anim::AnimProfile::Get().ikMs = _t.Ms(); }
    {Anim::StageTimer timer;
     m_skin.GetIKRigSystem()->Solve(world,dt,animActivePtr);
     Anim::AnimProfile::Get().ikRigSolveMs=timer.Ms();}
    Anim::AnimProfile::Get().ikRigMs=Anim::AnimProfile::Get().ikRigPrepareMs+Anim::AnimProfile::Get().ikRigSolveMs;
    m_skin.GetAnimationSystem()->ApplyPostIKGrants(world);
    // Head-look constraint: swing each HeadTrackComponent's head bone toward the active
    // camera (clamped) on top of the final animated pose. Before chain physics so
    // hair/accessories react to the head turn, and before the world-matrix bake below so the
    // turn reaches the GPU this frame.
    { ZoneScopedN("Anim.HeadTrack"); PROF_SCOPE("Anim.HeadTrack"); Anim::StageTimer _t;
    if (auto* hts = m_skin.GetHeadTrackSystem())
        hts->Update(world, dt, animActivePtr);
      Anim::AnimProfile::Get().headTrackMs = _t.Ms(); }
    // The modules' AfterHeadTrack stages (a tracked face's bones): after HeadTrack, which writes
    // the head absolutely on a clip-less skeleton and gives way to the head's authority, and before
    // chain physics, so hair follows. Every bone written is journaled and put back by the next
    // AnimationSystem::Update.
    for (IAnimChainHook* hook : m_skin.GetChainHooks()) hook->AfterHeadTrack(world, animActivePtr);
    { Anim::StageTimer _t;
      // The counters are per-frame totals the system accumulates across every island, so they
      // are cleared here, not inside it.
      Anim::AnimProfile::Get().chainEntities  = 0;
      Anim::AnimProfile::Get().chainTier0 = 0;
      Anim::AnimProfile::Get().chainTier1 = 0;
      Anim::AnimProfile::Get().chainTier2 = 0;
      Anim::AnimProfile::Get().chainSubsteps = 0;
      Anim::AnimProfile::Get().chainShadowOnly = 0;
      // The preview lane's share, same lifetime.
      Anim::AnimProfile::Get().previewChainEntities = 0;
      Anim::AnimProfile::Get().previewChainTier0 = 0;
      Anim::AnimProfile::Get().previewChainTier1 = 0;
      Anim::AnimProfile::Get().previewChainTier2 = 0;
      Anim::AnimProfile::Get().previewChainSubsteps = 0;
      // Camera for the chain distance LOD. The view is one frame stale here (rebuilt later in
      // the frame); irrelevant for a distance tier.
      m_skin.GetChainPhysicsSystem()->SetCameraPosition(FrameViewWrite().view.cameraPosition);
      m_skin.GetChainPhysicsSystem()->SetShadowOnlySet(
          m_skin.IsAnimCullingEnabled() ? &m_skin.GetAnimShadowOnlySet() : nullptr);
    { PROF_SCOPE("Anim.Chain");   // the island jobs on the workers carry this name
    m_skin.GetChainPhysicsSystem()->Update(world, dt, animActivePtr); }
      Anim::AnimProfile::Get().chainMs = _t.Ms(); }
    // An editor preview's temporary pose: after the chains, before LocalToWorld (only a
    // preview stage carries one: one pool lookup).
    m_skin.GetAnimationSystem()->ApplyPreviewPoseOverrides(world);
    { ZoneScopedN("Anim.LocalToWorld"); PROF_SCOPE("Anim.LocalToWorld"); Anim::StageTimer _t;
    m_skin.GetLocalToWorldSystem()->Update(world, m_skin.GetPoseRingBuffer(), animActivePtr);
      Anim::AnimProfile::Get().localToWorldMs = _t.Ms(); }
    { ZoneScopedN("Anim.Socket"); PROF_SCOPE("Anim.Socket"); Anim::StageTimer _t;
    m_skin.GetSocketSystem()->Update(world);
      Anim::AnimProfile::Get().socketMs = _t.Ms(); }
    // Flat-binding followers update post-Socket to see fresh bone transforms.
    { ZoneScopedN("Anim.Follow"); PROF_SCOPE("Anim.Follow"); Anim::StageTimer _t;
    m_skin.GetFollowSystem()->Update(world);
      Anim::AnimProfile::Get().followMs = _t.Ms(); }

    // NPR SDF face shadow: push each face's head-bone world forward/right axes into the face
    // MaterialComponent's frameParams (HeadForward/HeadRight @ b8, and the slot's frame record
    // for the deferred toon). Runs here (pose live, before BuildRenderScene) so both are fresh
    // this frame.
    { ZoneScopedN("Anim.FaceShading"); PROF_SCOPE("Anim.FaceShading"); Anim::StageTimer _t;
    FaceShading::Update(world, m_skin.GetPoseRingBuffer(), m_skin.GetSkeletonRegistry());
      Anim::AnimProfile::Get().faceMs = _t.Ms(); }

    // Decal lifetimes tick after transform/follow so expired entities are gone before upload.
    Anim::StageTimer _tTail;
    { PROF_SCOPE("Anim.Tail");
    m_decalLifetimeSystem.Update(world, dt);

    // Gameplay events are not delivered here: EventDelivery drains outside the renderer. Only the
    // -eventdelivery_legacy reverse arm delivers from this point.
    Gameplay::EventDelivery::DrainFromAnimationChain(); }
    Anim::AnimProfile::Get().tailMs = _tTail.Ms();

    // ---- Skeleton-level bone AABB merge (parallel per skeleton) ----
    Anim::StageTimer _tAabb;
    {
        ZoneScopedN("Anim.BoneAabb"); PROF_SCOPE("Anim.BoneAabb");
        // 256: an .iscn-imported FBX character is a node tree, so its skinned parts are
        // grandchildren of the skeleton entity and arrive via the SkeletonRef sweep below rather
        // than the Children walk - 27 parts for one reference rig, plus whatever attachments the
        // direct-children path adds.
        constexpr int kMaxAABBChildren = 256;
        struct AABBJob {
            const XMFLOAT4X4*      matrices;
            const SkeletonAsset*   asset;
            uint32_t               boneCount;
            const GlobalTransform* gt;
            WorldAabb*             rootAabb;
            WorldAabb* childAabbs[kMaxAABBChildren];
            int        childCount;
        };
        // Retain slots, but rebuild every active job before dispatch. Inactive
        // slots may contain stale pointers and are NEVER dispatched/read.
        // Avoid copying the unused tail of 256 child pointers per skeleton.
        thread_local std::vector<AABBJob> t_aabbJobs;
        size_t activeAABBJobs = 0;

        // Every skinned mesh entity points at its skeleton owner through SkeletonRef, at any depth.
        // The Children walk below reaches the flat PMX layout (mesh entities are direct children of
        // the root) but not the .iscn layout (root -> node entity -> mesh entity, where the node
        // entities carry no WorldAabb). One pass over the pool, grouped by owner.
        thread_local std::unordered_map<Entity, std::vector<WorldAabb*>> t_retainedAABBGroups;
        auto& skinnedByOwner = t_retainedAABBGroups;
        // Retain bucket/vector capacity, never reuse a previous frame's pointers.
        for (auto& [owner, bounds] : skinnedByOwner) bounds.clear();
        {
            auto* pSkelRefPool = world.GetPool<SkeletonRef>();
            auto* pWorldAabbP  = world.GetPool<WorldAabb>();
            if (pSkelRefPool && pWorldAabbP)
            {
                const auto& refEnts = EntitiesOf(pSkelRefPool);
                const auto& refData = pSkelRefPool->Data();
                for (size_t i = 0; i < refData.size() && i < refEnts.size(); ++i)
                {
                    if (refData[i].entity == NullEntity) continue;
                    if (WorldAabb* wa = pWorldAabbP->Get(refEnts[i]))
                        skinnedByOwner[refData[i].entity].push_back(wa);
                }
            }
        }

        // Drop absent owners so scratch cannot grow across scene reloads/churn.
        for (auto it = skinnedByOwner.begin(); it != skinnedByOwner.end(); )
            if (it->second.empty()) it = skinnedByOwner.erase(it);
            else ++it;
#if WITH_HARNESS
        if (Sys::CommandLine::Get().Has("-boneaabbprepverify"))
        {
            ZoneScopedN("Anim.BoneAabb.PrepareOracle");
            std::unordered_map<Entity, std::vector<WorldAabb*>> legacy;
            if (auto* refs = world.GetPool<SkeletonRef>())
            {
                const auto& entities = EntitiesOf(refs);
                for (size_t i = 0; i < refs->Data().size() && i < entities.size(); ++i)
                    if (refs->Data()[i].entity != NullEntity)
                        if (auto* wa = world.GetComponent<WorldAabb>(entities[i]))
                            legacy[refs->Data()[i].entity].push_back(wa);
            }
            bool match = legacy == skinnedByOwner;
            size_t checked = 0;
            for (const auto& [owner, bounds] : legacy) checked += bounds.size();
            if (!ENGINE_CHECK(match, "Bone AABB preparation grouping mismatch"))
                throw std::runtime_error("Bone AABB preparation grouping mismatch");
            static thread_local uint32_t t_checks = 0;
            if (t_checks++ % 60 == 0)
                LOG_INFO("[BONE_AABB_PREP] refs=%zu match=1", checked);
        }
#endif

        // Iterate the SkeletonComponent pool directly (no hash lookups); cache the pool pointers.
        auto* pSkel2      = world.GetPool<SkeletonComponent>();
        auto* pGlobalXf2  = world.GetPool<GlobalTransform>();
        auto* pWorldAabb2 = world.GetPool<WorldAabb>();
        auto* pChildren2  = world.GetPool<Children>();
        const size_t skelN2 = pSkel2 ? pSkel2->Data().size() : 0;
        const auto&  skelEnts2 = EntitiesOf(pSkel2);
        const SkeletonComponent* skelData2 = pSkel2 ? pSkel2->Data().data() : nullptr;
        const uint32_t poseWriteHead = m_skin.GetPoseRingBuffer().GetWriteHead();
        if (t_aabbJobs.size() < skelN2) t_aabbJobs.resize(skelN2);
        for (size_t si = 0; si < skelN2; ++si)
        {
            const Entity e = skelEnts2[si];
            const SkeletonComponent* skel = &skelData2[si];
            if (skel->boneCount == 0) continue;
            if (skel->poseByteOffset == ~0u) continue;
            const uint32_t boneEndBytes = skel->poseByteOffset + skel->boneCount * 64;
            if (boneEndBytes > poseWriteHead * 64) continue;

            const XMFLOAT4X4* matrices = m_skin.GetPoseRingBuffer().ReadMapped(skel->poseByteOffset);
            if (!matrices) continue;

            AABBJob& job = t_aabbJobs[activeAABBJobs++];
            job.matrices  = matrices;
            job.boneCount = skel->boneCount;
            job.asset     = (skel->assetIndex != kInvalidSkeletonIndex)
                ? &m_skin.GetSkeletonRegistry().Get(skel->assetIndex) : nullptr;
            job.gt        = pGlobalXf2  ? pGlobalXf2->Get(e)  : nullptr;
            job.rootAabb  = pWorldAabb2 ? pWorldAabb2->Get(e) : nullptr;
            job.childCount = 0;

            // Skinned parts first: they MUST get the merged box (the culler tests them), so a full
            // child budget must not drop them.
            if (auto sit = skinnedByOwner.find(e); sit != skinnedByOwner.end())
            {
                for (WorldAabb* wa : sit->second)
                {
                    if (!ENGINE_CHECK_ONCE(job.childCount < kMaxAABBChildren,
                            "Renderer: skinned AABB child budget exhausted (%d) on entity %u "
                            "via SkeletonRef — remaining parts keep their bind-pose box and "
                            "can be wrongly culled. Raise kMaxAABBChildren in Renderer.cpp.",
                            kMaxAABBChildren, static_cast<uint32_t>(e)))
                        break;
                    job.childAabbs[job.childCount++] = wa;
                }
            }

            const Children* ch = pChildren2 ? pChildren2->Get(e) : nullptr;
            if (ch)
            {
                for (Entity child : ch->entities)
                {
                    // A silent truncation would be confusing: the skinned bounds stop growing to cover the
                    // remaining children, so a fully on-screen attachment is frustum culled and pops while
                    // nothing near the culling code looks wrong. Cheap _ONCE tripwire; the clamp stays.
                    if (!ENGINE_CHECK_ONCE(job.childCount < kMaxAABBChildren,
                            "Renderer: skinned AABB child budget exhausted "
                            "(%d) on entity %u — its remaining children do not "
                            "expand the bounds and can be wrongly culled. "
                            "Raise kMaxAABBChildren in Renderer.cpp.",
                            kMaxAABBChildren, static_cast<uint32_t>(e)))
                        break;
                    // Skip anything the SkeletonRef sweep already queued (the
                    // flat PMX layout has the mesh entities in BOTH lists).
                    const SkeletonRef* cref = world.GetComponent<SkeletonRef>(child);
                    if (cref && cref->entity == e) continue;
                    WorldAabb* cwa = pWorldAabb2 ? pWorldAabb2->Get(child) : nullptr;
                    if (cwa) job.childAabbs[job.childCount++] = cwa;
                }
            }
        }

        auto processAABB = [](const AABBJob& j)
        {
            XMVECTOR vmin = XMVectorSet( FLT_MAX,  FLT_MAX,  FLT_MAX, 0);
            XMVECTOR vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0);

            // j.matrices are SKIN matrices (inverseBindPose x localToModel - see AnimationSystem's pose
            // build), NOT bone model transforms, so everything merged below starts in bind-model space:
            // rest boxes are trusted only when the asset stamped them model-space
            // (ISKEL_FLAG_BONE_AABB_MODEL -> hasBoneAABBs), and joint positions are the bindPose
            // translations, mapped to the posed model by the skin matrix. Do not read matrices[b]._41..
            // as bone positions: at rest pose every skin matrix is identity, which collapses an idle
            // character's hull to the model origin and frustum-culls the whole character.
            if (j.asset && j.asset->hasBoneAABBs)
            {
                for (uint32_t b = 0; b < j.boneCount; ++b)
                {
                    const auto& ra = j.asset->boneRestAABBs[b];
                    if (ra.localMin.x > ra.localMax.x) continue;
                    const XMMATRIX boneMat = XMLoadFloat4x4(&j.matrices[b]);
                    const float lx[2] = { ra.localMin.x, ra.localMax.x };
                    const float ly[2] = { ra.localMin.y, ra.localMax.y };
                    const float lz[2] = { ra.localMin.z, ra.localMax.z };
                    for (int iz = 0; iz < 2; ++iz)
                    for (int iy = 0; iy < 2; ++iy)
                    for (int ix = 0; ix < 2; ++ix)
                    {
                        XMVECTOR w = XMVector3TransformCoord(
                            XMVectorSet(lx[ix], ly[iy], lz[iz], 1.f), boneMat);
                        vmin = XMVectorMin(vmin, w);
                        vmax = XMVectorMax(vmax, w);
                    }
                }
            }

            auto contributed = [&]() {
                XMFLOAT3 a, b;
                XMStoreFloat3(&a, vmin);
                XMStoreFloat3(&b, vmax);
                return a.x <= b.x && a.y <= b.y && a.z <= b.z;
            };

            // Joint-hull path: skeletons with no usable rest boxes (hasBoneAABBs false) and the
            // every-rest-box-degenerate case. The bindPose translation is the joint in bind-model space;
            // the skin matrix carries it to the posed model, so this hull is correct in every pose,
            // rest included (where it equals the bind-pose joint hull).
            if (!contributed())
            {
                if (j.asset)
                {
                    // Real joints only: a virtual bone sits on its `to` joint anyway - it adds nothing, and is
                    // no joint of the mesh.
                    const uint32_t joints = (std::min)(j.boneCount, j.asset->RealBoneCount());
                    for (uint32_t b = 0; b < joints; ++b)
                    {
                        const XMFLOAT4X4& bp = j.asset->bindPose[b];
                        XMVECTOR posed = XMVector3TransformCoord(
                            XMVectorSet(bp._41, bp._42, bp._43, 1.f),
                            XMLoadFloat4x4(&j.matrices[b]));
                        vmin = XMVectorMin(vmin, posed);
                        vmax = XMVectorMax(vmax, posed);
                    }
                }
                else
                {
                    // No asset (unregistered skeleton): the raw skin-matrix translations are the only data
                    // left. Meaningless as positions at rest (all zero) - the degeneracy guard below turns that
                    // into "write nothing".
                    for (uint32_t b = 0; b < j.boneCount; ++b)
                    {
                        XMVECTOR bp = XMVectorSet(
                            j.matrices[b]._41, j.matrices[b]._42, j.matrices[b]._43, 0);
                        vmin = XMVectorMin(vmin, bp);
                        vmax = XMVectorMax(vmax, bp);
                    }
                }
                if (!contributed()) return;   // nothing knowable - write nothing

                // The joint hull is the positions of the bones and nothing
                // else: it has no skin on it, so a shoulder, a hand or a hem
                // sits OUTSIDE it. Culling against it unpadded would clip
                // silhouettes at the frustum edge. Expand proportionally (the
                // rig's own size is the only scale reference available here)
                // rather than by a fixed metre value, which would be wrong at
                // both ends for a 0.1-scaled PMX rig and a full-size one.
                XMVECTOR ext  = XMVectorScale(XMVectorSubtract(vmax, vmin), 0.5f);
                XMVECTOR grow = XMVectorScale(ext, 0.25f);
                vmin = XMVectorSubtract(vmin, grow);
                vmax = XMVectorAdd     (vmax, grow);
            }

            // Degeneracy guard: a hull with ~zero extent (single joint, or a pathological pose source)
            // says nothing about where the skin is, and padded to a centimeter box it culls idle
            // characters. Leave the load-time bind-pose AABB alone instead (TransformSystem re-derives
            // the children's WorldAabb from their meshlib LocalAabb every frame).
            {
                XMFLOAT3 a, b;
                XMStoreFloat3(&a, vmin);
                XMStoreFloat3(&b, vmax);
                const float maxExt = (std::max)({ b.x - a.x, b.y - a.y, b.z - a.z });
                if (!(maxExt > 1e-4f)) return;   // also rejects NaN
            }

            const float kPad = 0.05f;
            vmin = XMVectorSubtract(vmin, XMVectorReplicate(kPad));
            vmax = XMVectorAdd     (vmax, XMVectorReplicate(kPad));

            if (j.gt)
            {
                const XMMATRIX worldMat = XMLoadFloat4x4(&j.gt->matrix);
                XMFLOAT3 lo, hi; XMStoreFloat3(&lo, vmin); XMStoreFloat3(&hi, vmax);
                const float cx[2]={lo.x,hi.x}, cy[2]={lo.y,hi.y}, cz[2]={lo.z,hi.z};
                vmin = XMVectorSet( FLT_MAX,  FLT_MAX,  FLT_MAX, 0);
                vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0);
                for (int iz=0;iz<2;++iz) for (int iy=0;iy<2;++iy) for (int ix=0;ix<2;++ix)
                {
                    XMVECTOR w = XMVector3TransformCoord(
                        XMVectorSet(cx[ix],cy[iy],cz[iz],1.f), worldMat);
                    vmin = XMVectorMin(vmin, w);
                    vmax = XMVectorMax(vmax, w);
                }
            }

            XMFLOAT3 rmin, rmax;
            XMStoreFloat3(&rmin, vmin);
            XMStoreFloat3(&rmax, vmax);

            if (j.rootAabb) { j.rootAabb->min = rmin; j.rootAabb->max = rmax; }
            for (int c = 0; c < j.childCount; ++c)
            { j.childAabbs[c]->min = rmin; j.childAabbs[c]->max = rmax; }
        };

        const AABBJob* jobs = t_aabbJobs.data();
        if (activeAABBJobs > 1)
        {
            TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(activeAABBJobs),
                [&](uint32_t i) { processAABB(jobs[i]); });
        }
        else if (activeAABBJobs != 0)
        {
            processAABB(t_aabbJobs[0]);
        }
    }

    Anim::AnimProfile::Get().boneAabbMs = _tAabb.Ms();

    // These two end the chain. _tWall must stop after the LAST statement (wallMs, at the end),
    // or the profile under-reports the system against the scheduler's measure.
    {
        ZoneScopedN("Anim.SkinMatrix"); PROF_SCOPE("Anim.SkinMatrix");
        Anim::StageTimer _t;
        m_skin.GetSkinMatrixSystem()->Update(world, m_skin.GetPoseRingBuffer(), m_skin.GetVertexRing());
        Anim::AnimProfile::Get().skinMatrixMs = _t.Ms();
    }
    {
        ZoneScopedN("Anim.BuildSkinJobs"); PROF_SCOPE("Anim.BuildSkinJobs");
        Anim::StageTimer _t;
        m_skin.BuildSkinJobs(world, m_meshMgr);
        Anim::AnimProfile::Get().skinJobsMs = _t.Ms();
    }

    // Pose writes land in PoseRingBuffer's CPU shadow (cached RAM); this is the single point
    // that pushes them into the UPLOAD mapping the GPU reads. It MUST stay after the last
    // MapSlice writer (SkinMatrixSystem above and the prev-pose copy inside BuildSkinJobs are
    // the only two) and before the render graph runs SkinningPass. See PoseRingBuffer::m_shadow
    // for why the ring is staged rather than written through.
    {
        ZoneScopedN("Anim.PoseFlush"); PROF_SCOPE("Anim.PoseFlush");
        Anim::StageTimer _t;
        m_skin.GetPoseRingBuffer().FlushToGPU();
        Anim::AnimProfile::Get().poseFlushMs = _t.Ms();
    }

    Anim::AnimProfile::Get().wallMs = _tWall.Ms();
}
