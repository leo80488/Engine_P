#include "Anim/IK/Kinematics.h"
#include "Anim/Core/PoseOps.h"
#include "Anim/Core/AnimPoseSource.h"
#include "ECS/AnimationSampleFrames.h"
#include "ECS/AnimationSystem.h"
#include "Gameplay/AttackPlayback.h"
#include "Gameplay/GameplayTime.h"
#include "ECS/LocomotionComponents.h"
#include "Resource/HumanoidProfileBuilder.h"  // lazy profile for humanoid bone masks
#include "System/TaskSystem.h"
#include "System/WeightedTaskPlan.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <utility>
#include "Graphics/AnimProfile.h"
#include <chrono>
#include <atomic>

// Toggle: 0 silent, 1 log the locomotion blend-space resolve (change-triggered
// + 1 Hz heartbeat). Without it a blend space that resolves to nothing, or one
// frozen because animDt is 0, looks like a correctly wired one.
#ifndef LOCO_DEBUG
#define LOCO_DEBUG 0
#endif

using namespace DirectX;

// ==== Helpers ====
using Anim::PoseOps::Lerp3;
using Anim::PoseOps::Slerp4;

static float AdvancePlaybackTime(float time, float dt, float speed,
                                 float duration, bool looping)
{
    if (duration <= 0.f) return 0.f;
    time += dt * speed;
    if (looping)
    {
        time = std::fmod(time, duration);
        if (time < 0.f) time += duration;
        return time;
    }
    return std::clamp(time, 0.f, duration);
}

#if LOCO_DEBUG
// The locomotion resolve's heartbeat: a line when the blend's pick count changes, else once a
// second. It compares LAST frame's sampled pose of a leg bone with the skeleton's REST rotation: if
// they differ and move between lines, the pose really is animating, and anything still standing in
// bind pose is a skinning / SkeletonRef problem, not an animation one.
static void LogLocomotionHeartbeat(Entity e, float dt, const AnimationComponent& anim,
                                   const LocomotionComponent& loco,
                                   const std::vector<AnimationSystem::LocalPose>* lastPose,
                                   const SkeletonAsset* skeleton)
{
    static std::unordered_map<Entity, std::pair<int, int>> s_prev; // {outCount, tick}
    auto& st = s_prev[e];
    const bool changed = st.first != loco.outCount;
    if (!changed && ++st.second < 60) return;
    st.first  = loco.outCount;
    st.second = 0;

    char poseInfo[160] = " pose=<none>";
    if (lastPose && skeleton)
    {
        int32_t leg = -1;
        for (uint32_t b = 0; b < skeleton->boneCount && leg < 0; ++b)
            if (std::strcmp(skeleton->boneNames[b], "\xE5\xB7\xA6\xE8\xB6\xB3") == 0) // left foot (MMD bone name)
                leg = static_cast<int32_t>(b);
        if (leg >= 0 && static_cast<size_t>(leg) < lastPose->size())
        {
            XMVECTOR sc, rot, tr;
            XMMatrixDecompose(&sc, &rot, &tr, XMLoadFloat4x4(&skeleton->restPoseLocal[leg]));
            XMFLOAT4 rest; XMStoreFloat4(&rest, rot);
            const XMFLOAT4& cur = (*lastPose)[leg].rot;
            const float dot = std::fabs(cur.x * rest.x + cur.y * rest.y
                                      + cur.z * rest.z + cur.w * rest.w);
            snprintf(poseInfo, sizeof(poseInfo),
                     " legBone=%d rot=(%.3f,%.3f,%.3f,%.3f) vsRest=%.1fdeg",
                     leg, cur.x, cur.y, cur.z, cur.w,
                     XMConvertToDegrees(2.f * std::acos(std::min(dot, 1.f))));
        }
    }

    LOG_INFO("[Loco ent=%u] dt=%.4f paused=%d enabled=%d n=%d "
             "out=%d speed(in=%.2f sm=%.2f) dir(in=%.0f sm=%.0f) "
             "phase=%.3f locoW=%.2f clip0=%u@%.2f%s",
             e, dt, anim.paused ? 1 : 0, loco.enabled ? 1 : 0,
             loco.sampleCount, loco.outCount,
             loco.speed, loco.smoothedSpeed,
             loco.dirDeg, loco.smoothedDir,
             loco.phase, loco.locoWeight,
             loco.outCount > 0 ? loco.outClip[0]   : ~0u,
             loco.outCount > 0 ? loco.outWeight[0] : 0.f,
             poseInfo);
}
#endif


// Build a TRS matrix from a LocalPose (row-major, matching DirectX convention)
static XMFLOAT4X4 LocalPoseToMatrix(const AnimationSystem::LocalPose& p)
{
    XMMATRIX S = XMMatrixScaling(p.scl.x, p.scl.y, p.scl.z);
    XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&p.rot));
    XMMATRIX T = XMMatrixTranslation(p.pos.x, p.pos.y, p.pos.z);
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, S * R * T);
    return out;
}

// Multiply two row-major XMFLOAT4X4 matrices: a x b.
static XMFLOAT4X4 Mul4x4(const XMFLOAT4X4& a, const XMFLOAT4X4& b)
{
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMLoadFloat4x4(&a) * XMLoadFloat4x4(&b));
    return out;
}

// Virtual-bone switch: -1 = not decided yet (the command line is read on first use;
// static initialisation runs before it is parsed). Read by the sampling jobs on the
// workers: relaxed atomic, a frame that sees the old value poses with the old setting.
static std::atomic<int>& VirtualBoneMode()
{
    static std::atomic<int> s_mode{ -1 };
    return s_mode;
}

void AnimationSystem::SetVirtualBonesEnabled(bool on)
{
    VirtualBoneMode().store(on ? 1 : 0, std::memory_order_relaxed);
}

bool AnimationSystem::VirtualBonesEnabled()
{
    int m = VirtualBoneMode().load(std::memory_order_relaxed);
    if (m < 0)
    {
        m = Sys::CommandLine::Get().Has("-vb_off") ? 0 : 1;
        int expected = -1;
        VirtualBoneMode().compare_exchange_strong(expected, m, std::memory_order_relaxed);
        m = VirtualBoneMode().load(std::memory_order_relaxed);   // a SetVirtualBonesEnabled that won the race
    }
    return m != 0;
}

// The VIRTUAL bones' poses (Resource/ModelVirtualBones.h). A virtual bone's model
// transform is its `to` bone's in THIS pose (sampled, blended, layered, pre-IK grants
// applied, IK not yet), expressed under its `from` parent, so LocalToWorld puts it
// exactly on `to`. Only from / to and their ancestors are composed, by
// LocalToWorldSystem's rule (a root keeps its REST translation: root motion is
// stripped there too). Memoised per call.
static void EvaluateVirtualBones(const SkeletonAsset& s, AnimationSystem::LocalPose* pose, uint32_t boneCount)
{
    thread_local std::vector<XMFLOAT4X4> t_model;
    thread_local std::vector<uint32_t>   t_stamp;   // model[b] is this call's when stamp[b] == gen
    thread_local std::vector<uint32_t>   t_chain;
    thread_local uint32_t                t_gen = 0;
    if (t_model.size() < boneCount) { t_model.resize(boneCount); t_stamp.resize(boneCount, 0u); }
    if (++t_gen == 0) { std::fill(t_stamp.begin(), t_stamp.end(), 0u); t_gen = 1; }

    auto localOf = [&](uint32_t b) -> XMMATRIX
    {
        const AnimationSystem::LocalPose& lp = pose[b];
        XMFLOAT3 pos = lp.pos;
        if (s.parentIndex[b] < 0)
        {
            XMVECTOR sc, r, t;
            XMMatrixDecompose(&sc, &r, &t, XMLoadFloat4x4(&s.restPoseLocal[b]));
            XMStoreFloat3(&pos, t);
        }
        return XMMatrixScaling(lp.scl.x, lp.scl.y, lp.scl.z) * XMMatrixRotationQuaternion(XMLoadFloat4(&lp.rot)) *
               XMMatrixTranslation(pos.x, pos.y, pos.z);
    };
    auto modelOf = [&](uint32_t b) -> XMMATRIX
    {
        // Up to the first bone this call composed (or past a root), then down.
        t_chain.clear();
        int32_t c = static_cast<int32_t>(b);
        while (c >= 0 && static_cast<uint32_t>(c) < boneCount && t_stamp[static_cast<uint32_t>(c)] != t_gen &&
               t_chain.size() <= boneCount)
        {
            t_chain.push_back(static_cast<uint32_t>(c));
            c = s.parentIndex[c];
        }
        XMMATRIX parent = (c >= 0 && static_cast<uint32_t>(c) < boneCount) ? XMLoadFloat4x4(&t_model[static_cast<uint32_t>(c)])
                                                                            : XMMatrixIdentity();
        for (std::size_t i = t_chain.size(); i-- > 0;)
        {
            const uint32_t x = t_chain[i];
            const XMMATRIX m = s.parentIndex[x] >= 0 ? XMMatrixMultiply(localOf(x), parent) : localOf(x);
            XMStoreFloat4x4(&t_model[x], m);
            t_stamp[x] = t_gen;
            parent   = m;
        }
        return XMLoadFloat4x4(&t_model[b]);
    };
    for (const SkeletonAsset::VirtualBoneDef& vb : s.virtualBones)
    {
        if (vb.index >= boneCount || vb.from >= boneCount || vb.to >= boneCount) continue;
        const XMMATRIX mTo = modelOf(vb.to);
        const XMMATRIX mFrom = modelOf(vb.from);
        AnimationSystem::LocalPose& out = pose[vb.index];
        // `from` animated to a zero scale has no inverse (a NaN pose, then a NaN skin):
        // the virtual bone rests this frame.
        XMVECTOR det;
        const XMMATRIX inv = XMMatrixInverse(&det, mFrom);
        XMVECTOR sc, r, t;
        const float d = XMVectorGetX(det);
        if (!(std::fabs(d) > 1e-12f) || !XMMatrixDecompose(&sc, &r, &t, XMMatrixMultiply(mTo, inv)) ||
            XMVector3IsNaN(t) || XMVector3IsInfinite(t) || XMVector4IsNaN(r) || XMVector3IsNaN(sc))
        {
            out.pos = vb.restPos;
            out.rot = vb.restRot;
            out.scl = vb.restScl;
            continue;
        }
        XMStoreFloat3(&out.pos, t);
        XMStoreFloat4(&out.rot, XMQuaternionNormalize(r));
        XMStoreFloat3(&out.scl, sc);
    }
}

// Off: every virtual bone at its rest local, written each frame - the pose buffer
// persists across frames, so skipping it would leave the last evaluated pose, not
// the rest the switch promises.
static void RestVirtualBones(const SkeletonAsset& s, AnimationSystem::LocalPose* pose, uint32_t boneCount)
{
    for (const SkeletonAsset::VirtualBoneDef& vb : s.virtualBones)
    {
        if (vb.index >= boneCount) continue;
        pose[vb.index].pos = vb.restPos;
        pose[vb.index].rot = vb.restRot;
        pose[vb.index].scl = vb.restScl;
    }
}

// ==== AnimationSystem - base-pose source predicates ====
// One definition each (see the header), shared by the pass-1 gate, the pass-3
// rest-pose-fallback skip and ChainPhysicsSystem.
bool AnimationSystem::LocoIsPoseSource(const LocomotionComponent*       loco,
                                       const AnimPoseOverrideComponent* poseOverride)
{
    // A pose-authority claim (Sequencer) suppresses locomotion as a pose source
    // entirely (see the pass-1 call site).
    const bool poseOwnedExternally =
        poseOverride && (poseOverride->layers & AnimPoseOverrideComponent::Base) != 0;
    return !poseOwnedExternally
        && loco && loco->outCount > 0
        && loco->locoWeight > 0.001f;
}

bool AnimationSystem::LayerIsPoseSource(const AnimLayerComponent* layers) const
{
    if (!layers) return false;
    for (int li = 0; li < layers->layerCount &&
                     li < AnimLayerComponent::MAX_LAYERS; ++li)
    {
        const AnimLayer& L = layers->layers[li];
        // Mirrors the two skips the layer-blend loop itself performs, so an
        // entity can never be admitted here and then blend nothing.
        if (L.clip == ~0u || L.weight <= 0.001f) continue;
        if (L.clip >= m_clips.Count())           continue;
        if (m_clips.Get(L.clip).frameCount == 0) continue;
        return true;
    }
    return false;
}

bool AnimationSystem::HasLivePoseSource(World& world, Entity e) const
{
    // An AnimLayer only reaches the blend loop through the AnimationComponent
    // pass, so an entity without an AnimationComponent has no live source at
    // all no matter what other components it carries.
    const auto* anim = world.GetComponent<AnimationComponent>(e);
    if (!anim) return false;
    if (m_poseSource && m_poseSource->OwnsPose(e)) return true;
    // Mirror pass 1's actual skips, not just the handle test: ClipLibrary::Get is an
    // unchecked index and pass 1 also drops a zero-frame clip. Otherwise a stale or
    // malformed primaryClip reports a live pose source nothing re-establishes, and
    // ChainPhysics skips its rest re-base and composes onto its own previous output.
    if (anim->primaryClip != kInvalidAnimHandle
        && anim->primaryClip < m_clips.Count()
        && m_clips.Get(anim->primaryClip).frameCount != 0)
        return true;
    if (LocoIsPoseSource(world.GetComponent<LocomotionComponent>(e),
                         world.GetComponent<AnimPoseOverrideComponent>(e)))
        return true;
    return LayerIsPoseSource(world.GetComponent<AnimLayerComponent>(e));
}

// ==== AnimationSystem ====
bool AnimationSystem::BuildBoneMask(const SkeletonAsset& sk, uint8_t mode, const std::string& root, std::vector<float>& weights) {
    return Anim::PoseOps::BuildBoneMask(sk, mode, root, weights);
}
void AnimationSystem::SampleClip(const ClipAsset& clip, float time, uint32_t bones, LocalPose* out, bool looping) const {
    Anim::PoseOps::SampleClip(clip, time, bones, out, looping);
}

void AnimationSystem::EnsureCacheSize(Entity e)
{
    if (e >= m_localPoseCache.size())
    {
        m_localPoseCache.resize(static_cast<size_t>(e) + 1);
        m_lastClipCache.resize(static_cast<size_t>(e) + 1, ~0u);
    }
}

void AnimationSystem::Update(World& world, const GameplayTime::Clocks& clocks,
                              const std::unordered_set<Entity>* activeSet)
{
    // Last frame's temporary preview pose comes off first: this frame samples (or
    // leaves at rest) the un-posed pose.
    if (!m_overrideRestore.empty()) RestorePreviewPoseOverrides(world);

    // Phase 1 (serial): collect animated entities and advance time; morph sampling
    // and the time advance write AnimationComponent.
    struct AnimJob {
        Entity e;
        AnimationComponent*  anim;
        SkeletonComponent*   skel;
        LocomotionComponent* loco   = nullptr; // optional: blend-space base pose
        AnimLayerComponent*  layers = nullptr; // optional: masked overlay layers
        LocomotionTransitionComponent* trans = nullptr; // optional: start/stop/pivot
        // Resolved once in phase 1 by LocoIsPoseSource (with the AnimPoseOverrideComponent
        // suppression) and carried here: phase 2 must not re-derive it, and must not touch
        // the component pools.
        bool locoActive = false;
        bool graph = false;
        bool secondaryLooping = true;
        float dt = 0.f;   // this entity's GameplayTime::EntityDt
    };
    struct alignas(64) LaneTiming { long long ns = 0; };
    struct Scratch {
        std::vector<AnimJob> jobs;
        std::vector<uint64_t> costs;
        Sys::WeightedTaskPlan plan;
        std::vector<LaneTiming> timings;
        bool active = false;
    };
    // Update is owner-only and joins every sampler before return. Retain
    // allocation capacity but erase borrowed component pointers on all exits.
    thread_local Scratch t_scratch;
    if (t_scratch.active) throw std::logic_error("Recursive animation sampling");
    t_scratch.active = true;
    struct ReleaseScratch {
        Scratch& value;
        ~ReleaseScratch() { value.jobs.clear(); value.active = false; }
    } release{t_scratch};
    auto& animJobs = t_scratch.jobs;
    const bool weighted = !Sys::CommandLine::Get().Has("-framepreparelegacy");

    // Iterate the AnimationComponent pool directly: animated entities are a tiny
    // fraction of the world, and walking world.GetEntities() would cost ~22k
    // GetComponent hash lookups per frame to find them.
    auto* pAnim = world.GetPool<AnimationComponent>();
    auto* pSkelA = world.GetPool<SkeletonComponent>();
    auto* pLoco  = world.GetPool<LocomotionComponent>();
    auto* pLayer = world.GetPool<AnimLayerComponent>();
    auto* pTrans = world.GetPool<LocomotionTransitionComponent>();
    auto* pTransition = world.GetPool<AnimatorTransitionComponent>();
    // Pose-authority claims (Sequencer). See AnimPoseOverrideComponent for why
    // AnimationComponent::externalTime alone is not enough.
    auto* pPoseOverride = world.GetPool<AnimPoseOverrideComponent>();
    auto* pDilation = world.GetPool<TimeDilationComponent>();
    const bool legacyClock = GameplayTime::LegacyClock();
    const size_t animPoolN = pAnim ? pAnim->Data().size() : 0;
    const auto&  animEnts  = EntitiesOf(pAnim);
    animJobs.reserve(animPoolN);
    for (size_t ai = 0; ai < animPoolN; ++ai)
    {
        const Entity e = animEnts[ai];
        // Culled (outside the active set): the clocks below still advance, because attack
        // windows, cross-fades and notify times are gameplay state; only the sampling is skipped.
        const bool sampled = !activeSet || activeSet->find(e) != activeSet->end();
        if (!sampled && legacyClock) continue;
        const float dt = GameplayTime::EntityDt(pDilation ? pDilation->Get(e) : nullptr, clocks);

        auto* anim = &pAnim->Data()[ai];
        auto* skel = pSkelA ? pSkelA->Get(e) : nullptr;
        if (!skel || skel->assetIndex == kInvalidAnimHandle) continue;

        if (m_poseSource && m_poseSource->OwnsPose(e)) {
            // A graph advances its own time while it evaluates, so a culled graph stays where it
            // is; its attack clocks advance in AnimGraphSystem::Prepare, which ignores culling.
            if (!sampled) continue;
            const auto& asset = m_skeletons.Get(skel->assetIndex);
            EnsureCacheSize(e);
            auto& pose = m_localPoseCache[e];
            if (pose.size() != asset.boneCount) {
                pose.resize(asset.boneCount);
                Anim::PoseOps::RefPose(asset, pose.data());
            }
            anim->primaryClip = anim->secondaryClip = kInvalidAnimHandle;
            animJobs.push_back({e, anim, skel, nullptr, nullptr, nullptr, false, true, true, dt});
            continue;
        }

        // ---- Locomotion blend-space resolve + phase advance (serial) ----
        // 1D speed axis or 2D polar 8-way; up to MAX_BLEND phase-synced picks.
        LocomotionComponent* loco = pLoco ? pLoco->Get(e) : nullptr;
        if (loco)
            Anim::PoseOps::ResolveBlendSpace(loco, m_clips, dt, anim->paused);

#if LOCO_DEBUG
        // Reaching this line already proves the entity has a valid skeleton (culled or not), so
        // a MISSING heartbeat is just as diagnostic as its contents.
        if (loco && loco->sampleCount > 0)
            LogLocomotionHeartbeat(e, dt, *anim, *loco,
                                   e < m_localPoseCache.size() ? &m_localPoseCache[e] : nullptr,
                                   skel->assetIndex < m_skeletons.Count()
                                       ? &m_skeletons.Get(skel->assetIndex) : nullptr);
#endif
        // A pose-authority claim (Sequencer) suppresses locomotion as a pose source
        // entirely. Otherwise the sequenced pose is only cross-faded in at
        // (1 - locoWeight) on every character with a LocomotionComponent, and the shot
        // looks right in Edit (where CharacterAnimDriverTickSystem does not run) and
        // wrong in Play.
        const AnimPoseOverrideComponent* poseOverride =
            pPoseOverride ? pPoseOverride->Get(e) : nullptr;
        const bool poseOwnedExternally =
            poseOverride && (poseOverride->layers & AnimPoseOverrideComponent::Base) != 0;

        const bool locoActive = LocoIsPoseSource(loco, poseOverride);

        // Fetched HERE rather than at the layer time-advance block below,
        // because the base-pose gate on the next line has to know about it.
        AnimLayerComponent* layers = pLayer ? pLayer->Get(e) : nullptr;
        const bool layerActive = LayerIsPoseSource(layers);

        // Base pose must come from somewhere: locomotion, the primary clip, or an
        // animation layer. Keep `layerActive` here: the layer blend runs in the parallel
        // pass below, which only sees entities pushed into animJobs, so a layer-only
        // character would silently do nothing.
        if (!locoActive && anim->primaryClip == kInvalidAnimHandle && !layerActive)
            continue;

        // ClipLibrary::Get is an unchecked index: a stale handle left over from a
        // world reload or a failed load would read out of bounds here. Treat an
        // out-of-range handle exactly like kInvalidAnimHandle.
        if (anim->primaryClip != kInvalidAnimHandle
            && anim->primaryClip < m_clips.Count())
        {
            const ClipAsset& clip = m_clips.Get(anim->primaryClip);
            if (clip.frameCount == 0)
            {
                // Mirror the gate above, layer term included: pass 3 skips this entity too
                // (clipWrotePose only checks the handle), so dropping a layer-driven entity here
                // leaves nothing writing its pose.
                if (!locoActive && !layerActive) continue;  // no pose source at all
            }
            // Advance time (writes the component: serial only). With externalTime set the clock
            // belongs to its owner (MotionDirector, Sequencer, editor preview stage): sample
            // primaryTime as-is, no self-advance / loop / clamp.
            // This clock runs even while locomotion owns the visible pose: CharacterAnimDriver
            // polls primaryTime to finish a one-shot layered over the blend space, and the
            // MorphComponent sampling below reads the same clock (PMX facial morphs).
            else if (!anim->externalTime && !anim->paused)
            {
                auto* tr=pTransition?pTransition->Get(e):nullptr;
                if(!tr||tr->active||!Attack::Advance(world,e,tr->currentClipName,clip.duration,dt,anim->primaryTime))
                    anim->primaryTime = AdvancePlaybackTime(
                        anim->primaryTime, dt, anim->speed, clip.duration, anim->looping);
            }
        }

        // Name-based Animator.Play cross-fade, used only by the authored AnimationSet /
        // Animator API (CharacterStateSystem has its own transition path).
        // The tick runs even while locomotion owns the base pose (blendWeight has no visual
        // effect there): gated on !locoActive, elapsed never advances, IsBlending() stays
        // true and QueueAnimatorPlay's dedupe swallows re-Plays. The promoted clip takes
        // over when locomotion stops.
        AnimatorTransitionComponent* transition =
            pTransition ? pTransition->Get(e) : nullptr;
        // A transition that was in flight when a pose owner took over must NOT
        // keep integrating: on reaching blend == 1 the promotion below writes
        // primaryClip AND primaryTime, which would silently overwrite the
        // owner's absolute pose mid-shot. Freeze it instead; the owner's
        // pre-animated cache restores `active` when it releases the actor.
        if (transition && transition->active && poseOwnedExternally)
        {
            // Deliberately skipped, not cleared: clearing would stop the release path from
            // resuming a legitimate gameplay blend.
        }
        else if (transition && transition->active)
        {
            if (anim->secondaryClip == kInvalidAnimHandle ||
                anim->secondaryClip >= m_clips.Count())
            {
                anim->secondaryClip = kInvalidAnimHandle;
                anim->blendWeight   = 0.f;
                transition->targetClipName.clear();
                transition->active = false;
            }
            else
            {
                const ClipAsset& targetClip = m_clips.Get(anim->secondaryClip);
                if (!anim->paused)
                {
                    if(!Attack::Advance(world,e,transition->targetClipName,targetClip.duration,dt,anim->secondaryTime))
                        anim->secondaryTime = AdvancePlaybackTime(
                            anim->secondaryTime, dt, transition->targetSpeed,
                            targetClip.duration, transition->targetLooping);
                    transition->elapsed += dt;
                }

                // A cross-fade advances only by `elapsed += dt`. While the animation clock is
                // frozen (editor Stopped / Paused feeds animDt 0, or this animator is paused) a
                // switch requested now would sit at blendWeight 0 forever, the new clip never
                // promoted ("Play does nothing"). So a blend that has not STARTED snaps to the
                // target; one already in progress keeps its weight and resumes on unfreeze.
                const bool blendCannotStart =
                    (dt <= 0.f || anim->paused) && transition->elapsed <= 0.f;
                const float t = (transition->duration <= 1e-4f || blendCannotStart)
                    ? 1.f
                    : std::clamp(transition->elapsed / transition->duration, 0.f, 1.f);
                anim->blendWeight = t;
                if (t >= 1.f)
                {
                    anim->primaryClip   = anim->secondaryClip;
                    anim->primaryTime   = anim->secondaryTime;
                    anim->secondaryClip = kInvalidAnimHandle;
                    anim->secondaryTime = 0.f;
                    anim->blendWeight   = 0.f;
                    anim->speed         = transition->targetSpeed;
                    anim->looping       = transition->targetLooping;

                    transition->currentClipName = transition->targetClipName;
                    transition->targetClipName.clear();
                    transition->duration = 0.f;
                    transition->elapsed  = 0.f;
                    transition->active   = false;
                }
            }
        }

        // ---- Locomotion transition clock + cross-fade weight (serial) ----
        // The driver only ARMS a transition (sets `active`); the clock, the in/out fade and
        // the auto-release live here so a paused or dt=0 frame freezes them like every
        // other animation clock.
        LocomotionTransitionComponent* trans = pTrans ? pTrans->Get(e) : nullptr;
        if (trans)
        {
            if (trans->cooldownTimer > 0.f)
                trans->cooldownTimer = std::max(0.f, trans->cooldownTimer - dt);

            const bool armed = trans->enabled && trans->active >= 0
                            && trans->active < trans->count;
            if (!armed)
            {
                trans->weight    = 0.f;
                trans->active    = -1;
                trans->releasing = false;   // don't leak into the next arm
            }
            else
            {
                LocoTransition& T = trans->transitions[trans->active];
                const ClipAsset* tc = (T.clip != ~0u) ? &m_clips.Get(T.clip) : nullptr;
                if (!tc || tc->frameCount == 0)
                {
                    trans->active    = -1;   // unresolved / bad clip: drop it
                    trans->weight    = 0.f;
                    trans->releasing = false;
                }
                else
                {
                    if (!anim->paused) trans->time += dt * std::max(T.speed, 0.f);

                    if (T.looping)
                    {
                        // A STATE, not a one-shot: wrap instead of ending. Held only while the driver
                        // re-asserts the loopHeld heartbeat each frame; any frame without it releases,
                        // so it never sticks at full weight.
                        if (tc->duration > 1e-4f)
                            trans->time = std::fmod(trans->time, tc->duration);
                        if (!trans->loopHeld) trans->releasing = true;
                    }
                    else
                    {
                        // Effective length: the authored cap, else the whole clip.
                        const float playLen = (T.maxDuration > 1e-3f)
                            ? std::min(T.maxDuration, tc->duration) : tc->duration;

                        // Release early enough that blendOut finishes with the clip.
                        const float release = std::max(0.f, playLen - trans->releaseLead);
                        if (trans->time >= release) trans->releasing = true;
                    }

                    const float rate = trans->releasing
                        ? (trans->blendOut > 1e-3f ? dt / trans->blendOut : 1.f)
                        : (trans->blendIn  > 1e-3f ? dt / trans->blendIn  : 1.f);
                    const float goal = trans->releasing ? 0.f : 1.f;
                    trans->weight += std::clamp(goal - trans->weight, -rate, rate);
                    trans->weight  = std::clamp(trans->weight, 0.f, 1.f);

                    // Fully faded out, or the clip ran off the end: disarm. The `weight <= 0` test
                    // comes first: hitting the raw duration mid-fade would be a hard cut. A looping
                    // transition never hits the duration test.
                    if ((trans->releasing && trans->weight <= 0.001f)
                        || (!T.looping && trans->time >= tc->duration))
                    {
                        trans->active = -1;
                        trans->weight = 0.f;
                        trans->releasing = false;
                        trans->cooldownTimer = trans->cooldown;
                    }
                }
            }

            // Consume the heartbeat: the driver has to re-assert it next frame
            // or the loop above releases itself.
            trans->loopHeld = false;
        }

        // ---- Layer time advance (serial) + lazy humanoid profile for masks ----
        // `layers` was fetched above, next to the base-pose gate that needs it.
        if (layers)
        {
            bool needProfile = false;
            for (int li = 0; li < layers->layerCount &&
                             li < AnimLayerComponent::MAX_LAYERS; ++li)
            {
                AnimLayer& L = layers->layers[li];
                if (L.clip == ~0u) continue;
                const ClipAsset& lc = m_clips.Get(L.clip);
                if (lc.frameCount == 0) continue;
                if (L.playing && !anim->paused)
                {
                    L.time += dt * L.speed;
                    // A single-frame or malformed clip has duration 0 and fmod(x, 0) is NaN: the
                    // NaN pose reaches the skin matrix and WorldAabb, and SceneBVH::BuildRecursive
                    // access-violates ((int)NaN == INT_MIN). Same guard as the morph pass.
                    if (L.looping && lc.duration > 1e-6f)
                        L.time = std::fmod(L.time, lc.duration);
                    else
                        L.time = std::min(L.time, lc.duration);
                    // A negative layer speed drives time below zero; SampleClip
                    // does (uint32_t)(time * frameRate), and a negative float to
                    // unsigned is undefined. Wrap instead of clamping so reverse
                    // playback of a looping layer still loops.
                    if (L.time < 0.f)
                        L.time = (L.looping && lc.duration > 1e-6f)
                               ? L.time + lc.duration * std::ceil(-L.time / lc.duration)
                               : 0.f;
                }
                if (L.maskMode == static_cast<uint8_t>(BoneMaskMode::UpperBody) ||
                    L.maskMode == static_cast<uint8_t>(BoneMaskMode::LowerBody))
                    needProfile = true;
            }
            // Humanoid role masks need a valid profile. Build it HERE (serial):
            // sampleEntity runs in parallel and skeletons are shared assets.
            if (needProfile)
            {
                SkeletonAsset& sa = m_skeletons.Get(skel->assetIndex);
                if (!sa.humanoid.IsValid() && !sa.humanoid.userEdited)
                    Resource::HumanoidProfileBuilder::Build(sa);
            }
        }

        // Clocks are advanced; a culled entity keeps its last pose and morph weights.
        if (!sampled) continue;

        EnsureCacheSize(e);

        // Init rest pose if needed (serial: may resize the inner vector).
        std::vector<LocalPose>& poseVec = m_localPoseCache[e];
        uint32_t& lastClip = m_lastClipCache[e];
        const uint32_t baseClipId = locoActive ? loco->outClip[0] : anim->primaryClip;

        // Layer-only entity: nothing rewrites the base pose (SampleClip and the
        // locomotion sample are the only full-pose writers) and the layer blend LERPs onto
        // poseVec, so rest is re-stamped every frame. Without it a layer at weight 0.3
        // compounds toward the layer pose within about a second.
        const bool baseFromRest = !locoActive
                               && anim->primaryClip == kInvalidAnimHandle;

        const bool needsInit = (poseVec.size() != skel->boneCount)
                             || (lastClip != baseClipId)
                             || baseFromRest;
        poseVec.resize(skel->boneCount);
        if (needsInit)
        {
            const SkeletonAsset& skelAsset = m_skeletons.Get(skel->assetIndex);
            for (uint32_t b = 0; b < skel->boneCount; ++b)
            {
                XMVECTOR sc, rot, tr;
                XMMatrixDecompose(&sc, &rot, &tr,
                                  XMLoadFloat4x4(&skelAsset.restPoseLocal[b]));
                XMStoreFloat3(&poseVec[b].pos, tr);
                XMStoreFloat4(&poseVec[b].rot, rot);
                XMStoreFloat3(&poseVec[b].scl, sc);
            }
            lastClip = baseClipId;
        }

        // Sample morph weights into MorphComponent (serial for ECS safety).
        auto* morphComp = world.GetComponent<MorphComponent>(e);
        if (morphComp && morphComp->primaryMorphClip != kInvalidAnimHandle
                      && morphComp->primaryMorphClip < m_morphClips.Count())
        {
            const MorphClipAsset& mc = m_morphClips.Get(morphComp->primaryMorphClip);
            SampleMorphClip(mc, anim->primaryTime, anim->looping, morphComp);
        }

        animJobs.push_back({ e, anim, skel, loco, layers, trans, locoActive, false,
            transition && transition->active ? transition->targetLooping : anim->looping, dt });
    }

    // Phase 2 (parallel): sample keyframes, cross-fade blend, pre-IK grants.
    // Each entity writes only its own m_localPoseCache[e] slot.
    auto sampleEntity = [this](const AnimJob& j)
    {
        std::vector<LocalPose>& poseVec = m_localPoseCache[j.e];
        thread_local std::vector<LocalPose> t_tmpPose;

        if (j.graph)
            m_poseSource->EvaluatePose(j.e, m_skeletons.Get(j.skel->assetIndex), poseVec.data(),
                                       j.skel->boneCount, j.anim->paused ? 0.f : j.dt);
        else {
        // Phase 1's answer, not a re-derivation (see AnimJob::locoActive).
        const bool locoActive = j.locoActive;

        // Weight of the ANIMATOR pose in the final base pose: locomotion and the
        // Animator cross-fade (the driver fades locoWeight down while a one-shot action
        // plays).
        const float animW = locoActive
            ? 1.f - std::clamp(j.loco->locoWeight, 0.f, 1.f)
            : 1.f;
        const bool animatorUsable =
            j.anim->primaryClip != kInvalidAnimHandle
            && m_clips.Get(j.anim->primaryClip).frameCount > 0;

        // Samples the Animator's primary clip + its cross-fade into `dst`.
        auto sampleAnimator = [this, &j](std::vector<LocalPose>& dst)
        {
            const ClipAsset& clip = m_clips.Get(j.anim->primaryClip);
            SampleClip(clip, j.anim->primaryTime, j.skel->boneCount, dst.data(), j.anim->looping);

            // Cross-fade blend
            if (j.anim->secondaryClip != kInvalidAnimHandle && j.anim->blendWeight > 0.001f)
            {
                const ClipAsset& sec = m_clips.Get(j.anim->secondaryClip);
                if (sec.frameCount > 0)
                {
                    // Sampling/blending does not yield or recursively sample an
                    // animator, so this lane's scratch is not live across a wait.
                    // Reset ALL bones: SampleClip may write fewer than boneCount.
                    thread_local std::vector<LocalPose> t_secPose;
                    t_secPose.assign(j.skel->boneCount, LocalPose{});
                    SampleClip(sec, j.anim->secondaryTime, j.skel->boneCount, t_secPose.data(), j.secondaryLooping);

                    const float w = j.anim->blendWeight;
                    for (uint32_t b = 0; b < j.skel->boneCount; ++b)
                    {
                        dst[b].pos = Lerp3(dst[b].pos, t_secPose[b].pos, w);
                        dst[b].rot = Slerp4(dst[b].rot, t_secPose[b].rot, w);
                        dst[b].scl = Lerp3(dst[b].scl, t_secPose[b].scl, w);
                    }
                }
            }
        };

        if (locoActive)
        {
            // Base pose = locomotion blend: up to MAX_BLEND phase-synced picks, heaviest
            // first, blended incrementally:
            //   pose = p0; acc = w0; then pose = blend(pose, p_i, w_i / (acc + w_i)).
            const ClipAsset& c0 = m_clips.Get(j.loco->outClip[0]);
            j.anim->activeLegDrive = static_cast<uint8_t>(c0.legDriveMode);
            SampleClip(c0, j.loco->outTime[0], j.skel->boneCount, poseVec.data());

            float acc = j.loco->outWeight[0];
            for (int p = 1; p < j.loco->outCount; ++p)
            {
                const float wp = j.loco->outWeight[p];
                if (wp <= 0.001f) continue;
                const ClipAsset& cp = m_clips.Get(j.loco->outClip[p]);
                t_tmpPose.resize(j.skel->boneCount);
                SampleClip(cp, j.loco->outTime[p], j.skel->boneCount, t_tmpPose.data());
                const float t = wp / (acc + wp);
                for (uint32_t b = 0; b < j.skel->boneCount; ++b)
                {
                    poseVec[b].pos = Lerp3(poseVec[b].pos, t_tmpPose[b].pos, t);
                    poseVec[b].rot = Slerp4(poseVec[b].rot, t_tmpPose[b].rot, t);
                    poseVec[b].scl = Lerp3(poseVec[b].scl, t_tmpPose[b].scl, t);
                }
                acc += wp;
            }
        }

        if (animatorUsable && animW > 0.001f)
        {
            // Mirror the clip's leg drive mode for IKSystem (retargeted FK clips must not
            // have their baked legs re-solved by the MMD CCD). The primary clip wins during a
            // cross-fade, and over locomotion once it is the dominant half of the
            // loco / animator cross-fade.
            if (!locoActive || animW >= 0.5f)
                j.anim->activeLegDrive = static_cast<uint8_t>(
                    m_clips.Get(j.anim->primaryClip).legDriveMode);

            if (!locoActive)
            {
                sampleAnimator(poseVec);
            }
            else
            {
                thread_local std::vector<LocalPose> t_animPose;
                t_animPose.resize(j.skel->boneCount);
                sampleAnimator(t_animPose);
                for (uint32_t b = 0; b < j.skel->boneCount; ++b)
                {
                    poseVec[b].pos = Lerp3(poseVec[b].pos, t_animPose[b].pos, animW);
                    poseVec[b].rot = Slerp4(poseVec[b].rot, t_animPose[b].rot, animW);
                    poseVec[b].scl = Lerp3(poseVec[b].scl, t_animPose[b].scl, animW);
                }
            }
        }

        // ---- Locomotion transition (start / stop / pivot / turn) ----
        // A one-shot sampled OVER the base pose, so it works on top of the blend space,
        // the Animator, or a mix. Phase 1 owns its clock / weight; here it is only
        // sampled and blended.
        if (j.trans && j.trans->active >= 0 && j.trans->weight > 0.001f)
        {
            const LocoTransition& T = j.trans->transitions[j.trans->active];
            if (T.clip != ~0u)
            {
                const ClipAsset& tc = m_clips.Get(T.clip);
                if (tc.frameCount > 0)
                {
                    // A transition clip is authored from the same retarget bake
                    // as the loop clips, so its leg-drive mode governs while it
                    // is the dominant half.
                    if (j.trans->weight >= 0.5f)
                        j.anim->activeLegDrive = static_cast<uint8_t>(tc.legDriveMode);

                    thread_local std::vector<LocalPose> t_transPose;
                    t_transPose.resize(j.skel->boneCount);
                    SampleClip(tc, j.trans->time, j.skel->boneCount, t_transPose.data(), T.looping);

                    const float w = j.trans->weight;
                    for (uint32_t b = 0; b < j.skel->boneCount; ++b)
                    {
                        poseVec[b].pos = Lerp3(poseVec[b].pos, t_transPose[b].pos, w);
                        poseVec[b].rot = Slerp4(poseVec[b].rot, t_transPose[b].rot, w);
                        poseVec[b].scl = Lerp3(poseVec[b].scl, t_transPose[b].scl, w);
                    }
                }
            }
        }

        } // end of the non-graph base pose

        const SkeletonAsset& skelAsset = m_skeletons.Get(j.skel->assetIndex);

        // ---- Masked animation layers on top of the base pose ----
        // Per-bone weight = layer.weight * mask[bone]; masks are cached per
        // (skeleton, mode, customRoot) in the layer. Phase 1 ensured the role-mask profile
        // serially; building the MASK here is safe, it writes only this entity's component.
        if (!j.graph && j.layers)
        {
            for (int li = 0; li < j.layers->layerCount &&
                             li < AnimLayerComponent::MAX_LAYERS; ++li)
            {
                AnimLayer& L = j.layers->layers[li];
                if (L.clip == ~0u || L.weight <= 0.001f) continue;
                const ClipAsset& lc = m_clips.Get(L.clip);
                if (lc.frameCount == 0) continue;

                if (L.maskSkel != j.skel->assetIndex ||
                    L.maskModeCached != L.maskMode ||
                    L.maskRootCached != L.customRootBone ||
                    L.maskW.size() != j.skel->boneCount)
                {
                    const bool resolved = BuildBoneMask(skelAsset, L.maskMode,
                                                        L.customRootBone, L.maskW);
                    if (!resolved && !L.maskWarned)
                    {
                        LOG_WARNING("AnimLayer: entity %u layer %d mask '%s' (mode %u) "
                                    "did not resolve to a bone - falling back to FULL BODY, "
                                    "so this layer now overrides the whole character",
                                    j.e, li,
                                    L.maskMode == static_cast<uint8_t>(BoneMaskMode::CustomRoot)
                                        ? L.customRootBone.c_str() : "<humanoid Spine>",
                                    static_cast<unsigned>(L.maskMode));
                        L.maskWarned = true;
                    }
                    // Cache only a successful mask: Upper / LowerBody seed from the humanoid profile,
                    // which is built lazily and can be hand-edited later, so a failed build must retry
                    // (one pass over the bones); maskWarned keeps the log warn-once.
                    if (resolved)
                    {
                        L.maskSkel       = j.skel->assetIndex;
                        L.maskModeCached = L.maskMode;
                        L.maskRootCached = L.customRootBone;
                        L.maskWarned     = false;
                    }
                }

                t_tmpPose.resize(j.skel->boneCount);
                SampleClip(lc, L.time, j.skel->boneCount, t_tmpPose.data(), L.looping);
                for (uint32_t b = 0; b < j.skel->boneCount; ++b)
                {
                    const float w = L.weight * L.maskW[b];
                    if (w <= 0.001f) continue;
                    poseVec[b].pos = Lerp3(poseVec[b].pos, t_tmpPose[b].pos, w);
                    poseVec[b].rot = Slerp4(poseVec[b].rot, t_tmpPose[b].rot, w);
                    poseVec[b].scl = Lerp3(poseVec[b].scl, t_tmpPose[b].scl, w);
                }
            }
        }
        if (!j.graph) {
        const bool locoActive = j.locoActive;
        const float animW = locoActive ? 1.f - std::clamp(j.loco->locoWeight, 0.f, 1.f) : 1.f;
        const bool animatorUsable = j.anim->primaryClip != kInvalidAnimHandle
            && j.anim->primaryClip < m_clips.Count() && m_clips.Get(j.anim->primaryClip).frameCount > 0;
        // Continuous leg drive follows the actual pose blend, independently
        // of the dominant clip used for discrete VMD switches.
        float ikWeight = 0.f, bestWeight = -1.f, dominantTime = 0.f;
        const ClipAsset* dominant = nullptr;
        auto contribute = [&](const ClipAsset& clip, float time, float weight) {
            if (clip.legDriveMode == LegDriveMode::IK) ikWeight += weight;
            if (weight > bestWeight) { bestWeight = weight; dominant = &clip; dominantTime = time; }
        };
        if (locoActive) {
            float sum = 0.f; for (int i=0; i<j.loco->outCount; ++i) sum += j.loco->outWeight[i];
            for (int i=0; i<j.loco->outCount; ++i)
                contribute(m_clips.Get(j.loco->outClip[i]), j.loco->outTime[i],
                    j.loco->outWeight[i] / std::max(sum, 1e-6f) * (animatorUsable ? 1.f-animW : 1.f));
        }
        if (animatorUsable) {
            const float secondary = j.anim->secondaryClip != kInvalidAnimHandle
                ? std::clamp(j.anim->blendWeight,0.f,1.f) : 0.f;
            contribute(m_clips.Get(j.anim->primaryClip), j.anim->primaryTime, (1.f-secondary)*animW);
            if (secondary > 0.f) contribute(m_clips.Get(j.anim->secondaryClip), j.anim->secondaryTime, secondary*animW);
        }
        if (j.trans && j.trans->active >= 0 && j.trans->weight > 0.f) {
            const auto clip = j.trans->transitions[j.trans->active].clip;
            if (clip != ~0u) {
                ikWeight *= 1.f-j.trans->weight; bestWeight *= 1.f-j.trans->weight;
                contribute(m_clips.Get(clip),j.trans->time,j.trans->weight);
            }
        }
        // Layers affect drive only where they actually write the legs/control
        // bones. An upper-body weapon layer must not disable foot evaluation.
        if (j.layers) for (int li=0;li<j.layers->layerCount && li<AnimLayerComponent::MAX_LAYERS;++li) {
            const auto& layer=j.layers->layers[li];
            if(layer.clip==~0u || layer.weight<=.001f || layer.maskW.size()!=skelAsset.boneCount)continue;
            float coverage=0.f;unsigned samples=0;
            for(auto role:{HumanoidBone::LeftUpperLeg,HumanoidBone::RightUpperLeg,
                           HumanoidBone::LeftLowerLeg,HumanoidBone::RightLowerLeg}) {
                const auto bone=skelAsset.humanoid.Bone(role);
                if(bone>=0) {coverage+=layer.maskW[bone];++samples;}
            }
            if(!samples)for(const auto& chain:skelAsset.ikChains) {
                coverage+=layer.maskW[chain.ikBoneIndex];++samples;
            }
            if(!samples && layer.maskMode==static_cast<uint8_t>(BoneMaskMode::FullBody))coverage=1.f,samples=1;
            const float weight=std::clamp(layer.weight*(samples?coverage/samples:0.f),0.f,1.f);
            ikWeight*=1.f-weight;bestWeight*=1.f-weight;
            contribute(m_clips.Get(layer.clip),layer.time,weight);
        }
        j.anim->legIKWeight = std::clamp(ikWeight,0.f,1.f);
        j.anim->activeLegDrive = static_cast<uint8_t>(ikWeight < 0.5f ? LegDriveMode::FK : LegDriveMode::IK);
        j.anim->ikChainMask = ikWeight > 0.f ? ~uint64_t(0) : 0;
        j.anim->ikChainOverflow.assign(skelAsset.ikChains.size() > 64 ? skelAsset.ikChains.size()-64 : 0, ikWeight > 0.f);
        static const bool s_vmdMaskOff = Sys::CommandLine::Get().Has("-ik_vmd_mask_off");
        if (dominant && !s_vmdMaskOff)
            for (const auto& track : dominant->ikEnableTracks) {
                const auto i = track.chainIndex;
                if (i >= skelAsset.ikChains.size() || track.Sample(dominantTime,dominant->frameRate)) continue;
                if (i < 64) j.anim->ikChainMask &= ~(uint64_t(1)<<i);
                else j.anim->ikChainOverflow[i-64] = 0;
            }
        }
        if (!j.graph && m_poseSource)
            m_poseSource->CaptureExternalPose(j.e, poseVec.data(), j.skel->boneCount);
        Anim::PoseOps::ApplyGrants(skelAsset, poseVec.data(), j.skel->boneCount, false);

        // The virtual bones, after the pre-IK grants and before IK. Off (-vb_off or
        // SetVirtualBonesEnabled(false)): each is put at its rest local.
        if (!skelAsset.virtualBones.empty())
        {
            if (VirtualBonesEnabled()) EvaluateVirtualBones(skelAsset, poseVec.data(), j.skel->boneCount);
            else                       RestVirtualBones(skelAsset, poseVec.data(), j.skel->boneCount);
        }
    };

    if (weighted)
    {
        ZoneScopedN("Anim.Sample.Weighted");
        t_scratch.costs.clear();
        t_scratch.costs.reserve(animJobs.size());
        for (const auto& j : animJobs)
        {
            // Bone traversals approximate sampling/blending cost. The estimate
            // affects ordering only; no simulation timestep or LOD changes.
            uint64_t passes = 1;
            if (j.locoActive) passes += 2 * (std::max)(1, j.loco->outCount);
            if (j.anim->secondaryClip != kInvalidAnimHandle && j.anim->blendWeight > 0.001f) passes += 2;
            if (j.trans && j.trans->active >= 0 && j.trans->weight > 0.001f) passes += 2;
            if (j.layers)
                for (int i = 0; i < j.layers->layerCount && i < AnimLayerComponent::MAX_LAYERS; ++i)
                    if (j.layers->layers[i].clip != ~0u && j.layers->layers[i].weight > 0.001f) passes += 2;
            t_scratch.costs.push_back((std::max)(uint64_t{1}, uint64_t(j.skel->boneCount)) * passes);
        }
        t_scratch.plan.Build(t_scratch.costs, TaskSystem::Get().GetWorkerCount(), 4);
        const auto lanes = t_scratch.plan.Count();
        t_scratch.timings.resize(lanes);
#if WITH_HARNESS
        static const bool s_verifyMaterialCpu = Sys::CommandLine::Get().Has("-materialcpuverify");
        std::vector<std::vector<LocalPose>> before;
        if (s_verifyMaterialCpu)
            for (const auto& j : animJobs) before.push_back(m_localPoseCache[j.e]);
#endif
        // One write per coarse lane replaces a contended atomic per character.
        // ParallelFor safely helps its own lanes, including with one worker.
        auto* laneNs = t_scratch.timings.data();
        const auto* plan = &t_scratch.plan;
        TaskSystem::Get().ParallelFor(0, lanes, [&](uint32_t lane) {
            const auto t0 = std::chrono::steady_clock::now();
            for (const auto index : plan->Items(lane)) sampleEntity(animJobs[index]);
            laneNs[lane].ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
        });
        long long ns = 0;
        for (const auto& timing : t_scratch.timings) ns += timing.ns;
        Anim::AnimProfile::Get().sampleCpuMs = static_cast<float>(ns / 1e6);
#if WITH_HARNESS
        if (s_verifyMaterialCpu)
        {
            ZoneScopedN("Anim.Sample.WeightedOracle");
            for (size_t i = 0; i < animJobs.size(); ++i)
            {
                const auto& j = animJobs[i];
                auto actual = m_localPoseCache[j.e];
                const auto legDrive = j.anim->activeLegDrive;
                m_localPoseCache[j.e] = std::move(before[i]);
                sampleEntity(j); // same initial poses, original entity order
                const auto& reference = m_localPoseCache[j.e];
                static_assert(sizeof(LocalPose) == 10 * sizeof(float));
                if (actual.size() != reference.size() ||
                    (!actual.empty() && std::memcmp(actual.data(), reference.data(), actual.size()*sizeof(LocalPose))) ||
                    legDrive != j.anim->activeLegDrive)
                    throw std::runtime_error("Weighted animation sample mismatch");
                m_localPoseCache[j.e] = std::move(actual);
            }
            LOG_INFO("[ANIM_SAMPLE_BATCH] jobs=%zu lanes=%u match=1", animJobs.size(), lanes);
        }
#endif
    }
    else
    {
        // CPU time summed across whichever thread ran each job; cpu / wall is the
        // parallel speedup achieved.
        std::atomic<long long> _sampleNs{ 0 };
        auto _timedSample = [&](uint32_t i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            sampleEntity(animJobs[i]);
            _sampleNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - t0).count(),
                                std::memory_order_relaxed);
        };
        if (animJobs.size() > 1)
        {
            TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(animJobs.size()),
                _timedSample);
        }
        else if (!animJobs.empty())
        {
            _timedSample(0);
        }
        Anim::AnimProfile::Get().sampleCpuMs =
            static_cast<float>(_sampleNs.load(std::memory_order_relaxed) / 1e6);

    }

    // ---- Second pass: morph-only entities (no animator clip) ----
    auto* pMorph = world.GetPool<MorphComponent>();
    const size_t morphN = pMorph ? pMorph->Data().size() : 0;
    const auto&  morphEnts = EntitiesOf(pMorph);
    for (size_t mi = 0; mi < morphN; ++mi)
    {
        const Entity e = morphEnts[mi];
        auto* morphComp = &pMorph->Data()[mi];
        if (m_poseSource && m_poseSource->OwnsPose(e)) continue;
        if (morphComp->primaryMorphClip == kInvalidAnimHandle) continue;
        if (morphComp->primaryMorphClip >= m_morphClips.Count()) continue;

        // Skip entities the bone pass above samples: an animator with a clip OWNS its
        // morph clip, paused or not, and samples it at its own primaryTime (frozen while
        // paused), so face and body stay in step.
        auto* anim = pAnim ? pAnim->Get(e) : nullptr;
        if (anim && anim->primaryClip != kInvalidAnimHandle) continue;

        if (!morphComp->paused)
        {
            morphComp->time += GameplayTime::EntityDt(pDilation ? pDilation->Get(e) : nullptr, clocks);
            const MorphClipAsset& mc = m_morphClips.Get(morphComp->primaryMorphClip);
            if (morphComp->looping && mc.duration > 0.f)
                morphComp->time = std::fmod(morphComp->time, mc.duration);
            else
                morphComp->time = std::min(morphComp->time, mc.duration);
        }

        const MorphClipAsset& mc = m_morphClips.Get(morphComp->primaryMorphClip);
        SampleMorphClip(mc, morphComp->time, morphComp->looping, morphComp);
    }

    // ---- Third pass: rest-pose fallback for skeletons without a clip ----
    // Keeps GPU skinning running on un-animated characters so MorphComponent weights
    // (editor or scripts) still deform the mesh. Without it LocalToWorldSystem skips
    // -> poseByteOffset stays ~0u -> SkinningPass and morph compute never run.
    auto* pSkelAll = world.GetPool<SkeletonComponent>();
    const size_t skelAllN = pSkelAll ? pSkelAll->Data().size() : 0;
    const auto&  skelAllEnts = EntitiesOf(pSkelAll);
    for (size_t si = 0; si < skelAllN; ++si)
    {
        const Entity e = skelAllEnts[si];
        if (m_poseSource && m_poseSource->OwnsPose(e)) continue;
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;
        auto* skel = &pSkelAll->Data()[si];
        if (skel->assetIndex == kInvalidAnimHandle || skel->boneCount == 0) continue;

        // Skip if a fresh pose was already written above. This MUST cover every base-pose
        // source pass 1 accepts, not just primaryClip: otherwise a character driven only
        // by a blend space has its sampled pose overwritten by rest every frame.
        auto* anim = pAnim ? pAnim->Get(e) : nullptr;
        const bool clipWrotePose = anim
            && anim->primaryClip != kInvalidAnimHandle
            && anim->primaryClip < m_clips.Count();

        // Same predicate pass 1 gates on, so the two cannot disagree about
        // whether locomotion is a pose source (it is suppressed by a Sequencer
        // pose-authority claim).
        auto* locoAll = pLoco ? pLoco->Get(e) : nullptr;
        const AnimPoseOverrideComponent* poseOverrideAll =
            pPoseOverride ? pPoseOverride->Get(e) : nullptr;
        const bool locoWrotePose = LocoIsPoseSource(locoAll, poseOverrideAll);

        // A claimed actor's pose belongs to the claimant (Sequencer): stamping rest over
        // it would fight the shot. Explicit, so an actor claimed WITHOUT a locomotion
        // component is covered too.
        const bool ownedExternally = poseOverrideAll
            && (poseOverrideAll->layers & AnimPoseOverrideComponent::Base) != 0;

        // An AnimLayer is the third source: pass 1 re-stamps rest for a layer-only entity
        // (baseFromRest) and pass 2 blends the layer onto it, so stamping rest here would
        // erase the layer. Requires `anim`: a layer reaches the blend loop only through
        // the AnimationComponent pass.
        const bool layerWrotePose = anim && LayerIsPoseSource(
            pLayer ? pLayer->Get(e) : nullptr);

        if (clipWrotePose || locoWrotePose || layerWrotePose || ownedExternally) continue;

        EnsureCacheSize(e);
        std::vector<LocalPose>& poseVec = m_localPoseCache[e];
        if (poseVec.size() == skel->boneCount && m_lastClipCache[e] == kInvalidAnimHandle - 1u)
            continue; // already at rest pose, no clip churn

        const SkeletonAsset& skelAsset = m_skeletons.Get(skel->assetIndex);
        poseVec.resize(skel->boneCount);
        for (uint32_t b = 0; b < skel->boneCount; ++b)
        {
            XMVECTOR sc, rot, tr;
            // Check the return value: XMMatrixDecompose leaves sc / rot / tr unspecified
            // (typically NaN) on failure, and a NaN travels through the skin matrix and
            // WorldAabb into SceneBVH::BuildRecursive (out-of-range write at (int)NaN). Fall
            // back to translation-only: the bone stays roughly in place and the NaN stops here.
            if (XMMatrixDecompose(&sc, &rot, &tr,
                                  XMLoadFloat4x4(&skelAsset.restPoseLocal[b])))
            {
                XMStoreFloat3(&poseVec[b].pos, tr);
                XMStoreFloat4(&poseVec[b].rot, rot);
                XMStoreFloat3(&poseVec[b].scl, sc);
            }
            else
            {
                const XMFLOAT4X4& m = skelAsset.restPoseLocal[b];
                poseVec[b].pos = { m._41, m._42, m._43 };
                poseVec[b].rot = { 0.f, 0.f, 0.f, 1.f };
                poseVec[b].scl = { 1.f, 1.f, 1.f };
            }
        }
        // Sentinel: rest-pose state. Re-binding a clip later -> pass 1 sees
        // lastClip != primaryClip and re-inits.
        m_lastClipCache[e] = kInvalidAnimHandle - 1u;
    }
}

// ==== ApplyPostIKGrants ====
// D-bones and other grants that need post-IK source data; called after
// IKSystem::Update() so source bones carry their IK-solved rotations.
void AnimationSystem::ApplyPostIKGrants(World& world)
{

    auto* pSkel = world.GetPool<SkeletonComponent>();
    const size_t skelN = pSkel ? pSkel->Data().size() : 0;
    const auto&  skelEnts = EntitiesOf(pSkel);
    for (size_t si = 0; si < skelN; ++si)
    {
        const Entity e = skelEnts[si];
        auto* skel = &pSkel->Data()[si];
        if (skel->boneCount == 0) continue;

        if (e >= m_localPoseCache.size() || m_localPoseCache[e].empty()) continue;
        LocalPose* poseVec = m_localPoseCache[e].data();

        const SkeletonAsset& skelAsset = m_skeletons.Get(skel->assetIndex);

        Anim::PoseOps::ApplyGrants(skelAsset, poseVec, skel->boneCount, true);
    }
}

// The editor preview's temporary pose (see the header). Serial: the Renderer's
// animation stage, between chain physics and LocalToWorld.
void AnimationSystem::ApplyPreviewPoseOverrides(World& world)
{
    auto* pool = world.GetPool<PreviewPoseOverrideComponent>();
    if (!pool || pool->Size() == 0) return;
    const auto& ents = pool->Entities();
    const auto& data = pool->Data();
    for (std::size_t i = 0; i < ents.size(); ++i)
    {
        const Entity e = ents[i];
        if (e >= m_localPoseCache.size() || m_localPoseCache[e].empty()) continue;
        std::vector<LocalPose>& pose = m_localPoseCache[e];
        const uint32_t gen = world.GetGeneration(e);
        for (const PreviewPoseOverrideComponent::Entry& o : data[i].entries)
        {
            if (o.bone >= pose.size()) continue;
            // Undone at the start of the next Update (see OverrideRestore).
            m_overrideRestore.push_back({ e, gen, o.bone, pose[o.bone].rot });
            // In the bone's OWN frame: the delta first, then the pose's rotation
            // (DirectXMath's Multiply(a, b) = a then b).
            XMStoreFloat4(&pose[o.bone].rot, XMQuaternionNormalize(XMQuaternionMultiply(
                XMLoadFloat4(&o.rot), XMLoadFloat4(&pose[o.bone].rot))));
        }
    }
}

bool AnimationSystem::JournalBoneRotation(World& world, Entity e, uint32_t bone)
{
    if (e >= m_localPoseCache.size() || bone >= m_localPoseCache[e].size()) return false;
    m_overrideRestore.push_back({ e, world.GetGeneration(e), bone, m_localPoseCache[e][bone].rot });
    return true;
}

void AnimationSystem::RestorePreviewPoseOverrides(World& world)
{
    // Newest first: a bone overridden twice gets its first saved rotation back.
    for (auto it = m_overrideRestore.rbegin(); it != m_overrideRestore.rend(); ++it)
    {
        if (!world.IsAlive(it->e) || world.GetGeneration(it->e) != it->generation)
        {
            // Gone with the override on it (stage torn down): drop its id's cached pose, so
            // an entity that reuses the id seeds its own rest pose instead of inheriting a
            // turned bone.
            ClearLocalPoseCache(it->e);
            continue;
        }
        if (it->e >= m_localPoseCache.size() || it->bone >= m_localPoseCache[it->e].size()) continue;
        m_localPoseCache[it->e][it->bone].rot = it->rot;
    }
    m_overrideRestore.clear();
}

void AnimationSystem::SampleMorphClip(const MorphClipAsset& mc,
                                       float                 time,
                                       bool                  looping,
                                       MorphComponent*       out) const
{
    if (mc.frameCount == 0 || mc.frameRate <= 0.f || mc.morphCount == 0) return;

    // A looping clip interpolates its last frame into its first. A one-shot is
    // clamped, so its end HOLDS the last frame (a VMD's duration is
    // frameCount / frameRate: wrapping would snap a finished one-shot back to frame 0).
    uint32_t f0 = 0, f1 = 0;
    float    alpha = 0.f;
    if (looping)
    {
        const float rawFrame = time * mc.frameRate;
        f0    = static_cast<uint32_t>(rawFrame) % mc.frameCount;
        f1    = (f0 + 1) % mc.frameCount;
        alpha = rawFrame - std::floorf(rawFrame);
    }
    else
    {
        const float last     = static_cast<float>(mc.frameCount - 1);
        const float rawFrame = std::clamp(time * mc.frameRate, 0.f, last);
        f0    = static_cast<uint32_t>(rawFrame);
        f1    = (std::min)(f0 + 1, mc.frameCount - 1);
        alpha = rawFrame - static_cast<float>(f0);
    }

    out->count = std::min(mc.morphCount, static_cast<uint32_t>(MorphComponent::MAX));
    for (uint32_t m = 0; m < out->count; ++m)
    {
        const uint32_t i0 = m * mc.frameCount + f0;
        const uint32_t i1 = m * mc.frameCount + f1;
        out->weights[m]   = mc.weights[i0] + alpha * (mc.weights[i1] - mc.weights[i0]);
    }
}

const AnimationSystem::LocalPose* AnimationSystem::GetLocalPose(Entity e) const
{
    if (e >= m_localPoseCache.size() || m_localPoseCache[e].empty()) return nullptr;
    return m_localPoseCache[e].data();
}

AnimationSystem::LocalPose* AnimationSystem::GetMutableLocalPose(Entity e)
{
    if (e >= m_localPoseCache.size() || m_localPoseCache[e].empty()) return nullptr;
    return m_localPoseCache[e].data();
}

void AnimationSystem::ClearLocalPoseCache()
{
    m_localPoseCache.clear();
    m_lastClipCache.clear();
}

void AnimationSystem::ClearLocalPoseCache(Entity e)
{
    // Exactly the state EnsureCacheSize gives a never-seen entity: an empty
    // pose (GetLocalPose returns nullptr until the next Update) and a
    // last-clip that matches nothing, so the next Update takes the needsInit
    // / rest-seed path for e and for e alone.
    if (e < m_localPoseCache.size()) m_localPoseCache[e].clear();
    if (e < m_lastClipCache.size())  m_lastClipCache[e] = ~0u;
}

// ==== LocalToWorldSystem ====
void LocalToWorldSystem::Update(World& world, PoseRingBuffer& poseBuffer,
                                const std::unordered_set<Entity>* activeSet)
{
    // Phase 1 (sequential): collect skeleton entities + allocate ring buffer slots.
    // poseBuffer.Alloc is NOT thread-safe, so allocation must be serial.
    struct SkelJob {
        const SkeletonAsset* asset;
        const AnimationSystem::LocalPose* local;
        XMFLOAT4X4* poses;
        uint32_t boneCount;
    };
    std::vector<SkelJob> skelJobs;

    auto* pSkel = world.GetPool<SkeletonComponent>();
    const size_t skelN = pSkel ? pSkel->Data().size() : 0;
    const auto&  skelEnts = EntitiesOf(pSkel);
    for (size_t si = 0; si < skelN; ++si)
    {
        const Entity e = skelEnts[si];
        auto* skel = &pSkel->Data()[si];

        // Mark "no valid pose this frame" BEFORE any skip below. A skipped skeleton (not
        // in the active set, no asset, no local pose) must keep the ~0u sentinel: the
        // skinned draw gate (Renderer_Scene::skinningActive) treats any other offset as
        // "pose ready" and would skin against pose-ring slot 0, another skeleton's
        // matrices (a screen-spanning triangle and a TDR after a delete + reload).
        skel->poseByteOffset = ~0u;

        if (activeSet && activeSet->find(e) == activeSet->end()) continue;
        if (skel->assetIndex == kInvalidAnimHandle) continue;

        const AnimationSystem::LocalPose* local = m_animSys.GetLocalPose(e);
        if (!local) continue;

        const SkeletonAsset& asset = m_skeletons.Get(skel->assetIndex);

        static const bool s_poseHash = Sys::CommandLine::Get().Has("-anim_posehash");
        if (s_poseHash)
        {
            uint64_t hash = 14695981039346656037ull;
            const auto* bytes = reinterpret_cast<const uint8_t*>(local);
            for (size_t n = 0; n < skel->boneCount * sizeof(AnimationSystem::LocalPose); ++n)
                hash = (hash ^ bytes[n]) * 1099511628211ull;
            LOG_INFO("[ANIMHASH] entity=%u bones=%u hash=%016llx", e, skel->boneCount, hash);
        }


        const uint32_t slot = poseBuffer.Alloc(skel->boneCount);
        if (slot == ~0u) continue;   // ring overflow: the sentinel above keeps the rest pose

        XMFLOAT4X4* poses = poseBuffer.MapSlice(slot, skel->boneCount);
        if (!poses) continue;

        // Arm the offset only AFTER the slice is known good: arming before MapSlice
        // succeeds leaves an offset pointing at a slot nothing writes (the TDR above).
        skel->poseByteOffset = slot * static_cast<uint32_t>(sizeof(XMFLOAT4X4));

        skelJobs.push_back({ &asset, local, poses, skel->boneCount });
    }

    // Phase 2 (parallel): accumulate world matrices and fuse with inverseBindPose.
    // Skeletons are independent: different output slots, different assets.
    auto processJob = [](const SkelJob& j)
    {
        Anim::IK::ModelPose model(const_cast<AnimationSystem::LocalPose*>(j.local), *j.asset);
        for (uint32_t b = 0; b < j.boneCount; ++b)
            XMStoreFloat4x4(&j.poses[b], XMLoadFloat4x4(&j.asset->inverseBindPose[b]) * model.Get(b));
    };

    std::atomic<long long> _l2wNs{ 0 };
    auto _timedJob = [&](uint32_t i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        processJob(skelJobs[i]);
        _l2wNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t0).count(),
                         std::memory_order_relaxed);
    };
    if (skelJobs.size() > 1)
    {
        TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(skelJobs.size()),
            _timedJob);
    }
    else if (!skelJobs.empty())
    {
        _timedJob(0);
    }
    Anim::AnimProfile::Get().localToWorldCpuMs =
        static_cast<float>(_l2wNs.load(std::memory_order_relaxed) / 1e6);
}

// ==== SkinMatrixSystem ====
// Skin matrices are finalized in LocalToWorldSystem (once per skeleton); this pass
// only allocates per-mesh vertex-ring output slots. The skeleton entity is the
// SkeletonRef target, else the mesh entity itself (all components on one entity).
void SkinMatrixSystem::Update(World& world, PoseRingBuffer& /*poseBuffer*/,
                               SkinnedVertexRing& vertRing)
{
    // Iterate the MeshSkinnedComponent pool directly: tens of skinned entities
    // against ~22k in the world.
    auto* pMesh    = world.GetPool<MeshSkinnedComponent>();
    auto* pSkinOut = world.GetPool<SkinningOutputComponent>();
    auto* pRef     = world.GetPool<SkeletonRef>();
    auto* pSkel    = world.GetPool<SkeletonComponent>();
    if (!pMesh) return;
    const size_t meshN = pMesh->Data().size();
    const auto&  meshEnts = pMesh->Entities();
    for (size_t mi = 0; mi < meshN; ++mi)
    {
        const Entity e = meshEnts[mi];
        auto* mesh    = &pMesh->Data()[mi];
        auto* skinOut = pSkinOut ? pSkinOut->Get(e) : nullptr;
        if (!skinOut) continue;

        // Sentinel FIRST (mirrors LocalToWorldSystem): every skip below must leave the
        // mesh on the rest-pose fallback, or a skeleton that disappears mid-session
        // (destroyed root, SkeletonRef to a dead entity) leaves last frame's
        // poseByteOffset armed and BuildSkinJobs skins against a slice other entities are
        // rewriting.
        skinOut->poseByteOffset = ~0u;

        // Resolve which entity holds the SkeletonComponent: the SkeletonRef target, else
        // this entity (mesh and skeleton on one entity).
        Entity skelEntity = e;
        const SkeletonRef* ref = pRef ? pRef->Get(e) : nullptr;
        if (ref && ref->entity != NullEntity)
            skelEntity = ref->entity;

        const SkeletonComponent* skel = pSkel ? pSkel->Get(skelEntity) : nullptr;
        if (!skel || skel->assetIndex == kInvalidAnimHandle) continue;

        // Propagate pose validity: ~0u = no active animation this frame; then skip the
        // vertex-ring allocation (BuildSkinJobs skips too).
        skinOut->poseByteOffset = skel->poseByteOffset;
        if (skinOut->poseByteOffset == ~0u) continue;

        // Allocate per-mesh output slices in the skinned vertex ring. Any ring returning
        // the ~0u overflow sentinel drops the mesh back to the rest-pose fallback
        // (poseByteOffset = ~0u, honoured by BuildSkinJobs): never leave a partial
        // allocation armed, or the draw samples a slice no dispatch writes.
        const uint32_t posOff = vertRing.AllocPosition(mesh->vertexCount);
        const uint32_t nrmOff = vertRing.AllocNormal(mesh->vertexCount);
        // Only meshes with an authored tangent stream get a tangent slice; the
        // rest keep ~0u and Skin.cs skips the tangent write entirely.
        const bool     wantsTan = (mesh->restTanSRVHandle != 0);
        const uint32_t tanOff   = wantsTan ? vertRing.AllocTangent(mesh->vertexCount) : ~0u;
        if (posOff == ~0u || nrmOff == ~0u || (wantsTan && tanOff == ~0u))
        {
            skinOut->poseByteOffset = ~0u;
            continue;
        }
        skinOut->outPosByteOffset = posOff;
        skinOut->outNrmByteOffset = nrmOff;
        skinOut->outTanByteOffset = tanOff;
    }
}
